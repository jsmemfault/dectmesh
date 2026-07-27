/*
 * aether_forwarder -- hardened, continuously-running Memfault telemetry
 * forwarder for the Aether / DECT NR+ mesh.
 *
 * Replaces the socat + bash prototype (mflt_forward.sh / field_monitor.sh) with
 * a single long-lived process that OWNS the gateway's USB-CDC 9P port directly
 * (termios raw -- no socat, so the macOS CDC driver is never churned/wedged) and
 * speaks 9P over it. Every cycle it drains the gateway's own chunks
 * (dev/mflt5340 + dev/mflt9151), discovers the mesh peers currently in range
 * from the gateway's neighbor table, tunnels a 9P session to each peer's mesh 9P
 * server to drain its dev/mflt over the air, and POSTs every chunk to Memfault.
 *
 * Built to run 24/7 with devices coming and going:
 *   - per-read timeouts (poll + deadline): a wedged uart or slow mesh read fails
 *     fast and is retried next cycle; nothing ever hangs the loop.
 *   - auto-reconnect: if the gateway power-cycles / the port drops, it re-opens
 *     and re-attaches, surviving device resets.
 *   - graceful come/go: peers are rediscovered each cycle by durable CGA; the
 *     device table logs "+ appeared" / "- silent" transitions -- roaming is the
 *     normal case, not an error.
 *   - uart-wedge detection: neighbor/9151 reads failing while relay reads succeed
 *     is logged as a degraded link (the known inter-chip limit) instead of
 *     silently going relay-only.
 *
 * The 9P client + mesh-conversation tunnel mirror tools/aether_conv.c; the only
 * new transport work is opening the serial directly and bounding every read.
 *
 * Build:  cc -O2 -o tools/aether_forwarder tools/aether_forwarder.c
 * Usage:  MEMFAULT_PROJECT_KEY=<key> tools/aether_forwarder <port|dev> [interval_s]
 *           port  = a /dev/cu.usbmodem* suffix (e.g. 1203) or a full device path
 *           interval_s defaults to 30
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <glob.h>
#include <stdarg.h>

enum {
	Tversion = 100, Rversion = 101, Tattach = 104, Rattach = 105,
	Rerror = 107, Twalk = 110, Rwalk = 111, Topen = 112, Ropen = 113,
	Tread = 116, Rread = 117, Twrite = 118, Rwrite = 119,
	Tclunk = 120, Rclunk = 121,
};
#define NOFID   0xffffffffu
#define NOTAG   0xffff
#define OREAD   0
#define OWRITE  1
#define ORDWR   2
#define MSIZE   8192

/* Distinct return codes so callers can tell "the link stalled, skip and retry"
 * (RC_TIMEOUT) from "the port dropped, reconnect" (RC_ERR). */
#define RC_TIMEOUT (-2)
#define RC_ERR     (-1)

/* fids: root=0 (attach), local file read=5, mesh ctl=1 / data=2. */
#define FID_ROOT 0
#define FID_FILE 5
#define FID_CTL  1
#define FID_DATA 2

static int fd = -1;
static uint8_t mbuf[MSIZE];
static char lerr[128];
static volatile sig_atomic_t g_stop = 0;
static char g_where[80] = "?";   /* diagnostic: what op last hit a transport error */

/* ---- config ---- */
static const char *g_dev_arg;      /* suffix or full path as given */
static char g_dev[256];            /* resolved device path in use */
static const char *g_key;          /* Memfault project key */
static int g_interval = 30;
#define API_BASE "https://chunks.memfault.com/api/v0/chunks"

static void on_term(int s) { (void)s; g_stop = 1; }

static void p16(uint8_t *b, uint16_t v) { b[0] = v; b[1] = v >> 8; }
static void p32(uint8_t *b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static uint16_t g16(const uint8_t *b) { return b[0] | (b[1] << 8); }
static uint32_t g32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }

static void ts(char *out, size_t n)
{
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(out, n, "%H:%M:%S", &tm);
}

static void logline(const char *fmt, ...)
{
	char tb[16];
	ts(tb, sizeof(tb));
	va_list ap;
	printf("%s  ", tb);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

/* ---- serial transport ---------------------------------------------------- */

/* Resolve the device: a full path (starts with '/') is used as-is; otherwise it
 * is treated as a /dev/cu.usbmodem<suffix> glob (stable across re-enumeration). */
static int resolve_dev(void)
{
	if (g_dev_arg[0] == '/') {
		snprintf(g_dev, sizeof(g_dev), "%s", g_dev_arg);
		return access(g_dev, F_OK) == 0 ? 0 : -1;
	}
	char pat[256];
	snprintf(pat, sizeof(pat), "/dev/cu.usbmodem%s", g_dev_arg);
	glob_t gl;
	if (glob(pat, 0, NULL, &gl) == 0 && gl.gl_pathc > 0) {
		snprintf(g_dev, sizeof(g_dev), "%s", gl.gl_pathv[0]);
		globfree(&gl);
		return 0;
	}
	globfree(&gl);
	return -1;
}

static int open_serial(void)
{
	if (resolve_dev() < 0) return -1;
	int f = open(g_dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (f < 0) return -1;
	/* Clear O_NONBLOCK now that it's open; we gate reads with poll() instead. */
	fcntl(f, F_SETFL, 0);
	struct termios t;
	if (tcgetattr(f, &t) == 0) {
		cfmakeraw(&t);
		t.c_cflag |= CLOCAL | CREAD;
		t.c_cflag &= ~CRTSCTS;
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = 0;
		cfsetispeed(&t, B115200);
		cfsetospeed(&t, B115200);
		tcsetattr(f, TCSANOW, &t);
	}
	tcflush(f, TCIOFLUSH);
	fd = f;
	return 0;
}

static void close_serial(void)
{
	if (fd >= 0) close(fd);
	fd = -1;
}

static long now_ms(void)
{
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return (long)tp.tv_sec * 1000 + tp.tv_nsec / 1000000;
}

/* Read exactly n bytes with a wall-clock deadline. RC_TIMEOUT on deadline,
 * RC_ERR on EOF/hangup/error. */
static int readn_to(uint8_t *b, int n, int timeout_ms)
{
	long deadline = now_ms() + timeout_ms;
	int got = 0;
	while (got < n) {
		long rem = deadline - now_ms();
		if (rem <= 0) return RC_TIMEOUT;
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = poll(&pfd, 1, (int)rem);
		if (pr == 0) return RC_TIMEOUT;
		if (pr < 0) { if (errno == EINTR) continue; return RC_ERR; }
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return RC_ERR;
		int r = (int)read(fd, b + got, n - got);
		if (r < 0) { if (errno == EINTR || errno == EAGAIN) continue; return RC_ERR; }
		if (r == 0) return RC_ERR;
		got += r;
	}
	return got;
}

/* Send mbuf[0..len) (size prefix stamped here), receive reply into mbuf.
 * Returns reply type, or RC_TIMEOUT / RC_ERR. */
static int rpc_to(int len, int timeout_ms)
{
	p32(mbuf, len);
	if (write(fd, mbuf, len) != len) { snprintf(g_where, sizeof(g_where), "write"); return RC_ERR; }
	uint8_t hdr[4];
	int r = readn_to(hdr, 4, timeout_ms);
	if (r == RC_ERR) { snprintf(g_where, sizeof(g_where), "readhdr-eof"); return RC_ERR; }
	if (r < 0) return r;
	uint32_t sz = g32(hdr);
	if (sz < 7 || sz > MSIZE) { snprintf(g_where, sizeof(g_where), "badsize=%u", sz); return RC_ERR; }
	r = readn_to(mbuf, sz - 4, timeout_ms);
	if (r == RC_ERR) { snprintf(g_where, sizeof(g_where), "readbody-eof"); return RC_ERR; }
	if (r < 0) return r;
	int type = mbuf[0];
	if (type == Rerror) {
		uint16_t n = g16(mbuf + 3);
		if (n > sizeof(lerr) - 1) n = sizeof(lerr) - 1;
		memcpy(lerr, mbuf + 5, n);
		lerr[n] = 0;
	}
	return type;
}

#define TMO 4000   /* default per-op timeout, ms */

static int do_version(void)
{
	int o = 4;
	mbuf[o++] = Tversion; p16(mbuf + o, NOTAG); o += 2;
	p32(mbuf + o, MSIZE); o += 4;
	p16(mbuf + o, 6); o += 2; memcpy(mbuf + o, "9P2000", 6); o += 6;
	return rpc_to(o, TMO) == Rversion ? 0 : -1;
}

static int do_attach(uint32_t fid_)
{
	int o = 4;
	mbuf[o++] = Tattach; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid_); o += 4; p32(mbuf + o, NOFID); o += 4;
	p16(mbuf + o, 1); o += 2; mbuf[o++] = 't';
	p16(mbuf + o, 0); o += 2;
	return rpc_to(o, TMO) == Rattach ? 0 : -1;
}

/* Returns 0 / RC_TIMEOUT / RC_ERR / -3(Rerror-or-wrong-type). */
static int do_walk(uint32_t fid_, uint32_t newfid, const char *path)
{
	char tmp[128]; strncpy(tmp, path, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
	const char *parts[16]; int np = 0;
	for (char *p = strtok(tmp, "/"); p && np < 16; p = strtok(NULL, "/")) parts[np++] = p;
	int o = 4;
	mbuf[o++] = Twalk; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid_); o += 4; p32(mbuf + o, newfid); o += 4;
	p16(mbuf + o, np); o += 2;
	for (int i = 0; i < np; i++) {
		int l = strlen(parts[i]);
		p16(mbuf + o, l); o += 2; memcpy(mbuf + o, parts[i], l); o += l;
	}
	int t = rpc_to(o, TMO);
	if (t < 0) return t;
	return t == Rwalk ? 0 : -3;
}

static int do_open(uint32_t fid_, uint8_t mode)
{
	int o = 4;
	mbuf[o++] = Topen; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid_); o += 4; mbuf[o++] = mode;
	int t = rpc_to(o, TMO);
	if (t < 0) return t;
	return t == Ropen ? 0 : -3;
}

/* Read into out (cap). Returns byte count, or RC_TIMEOUT / RC_ERR / -3. */
static int do_read(uint32_t fid_, uint64_t off, uint8_t *out, int cap, int timeout_ms)
{
	int o = 4;
	mbuf[o++] = Tread; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid_); o += 4;
	p32(mbuf + o, off); o += 4; p32(mbuf + o, off >> 32); o += 4;
	p32(mbuf + o, cap); o += 4;
	int t = rpc_to(o, timeout_ms);
	if (t < 0) return t;
	if (t != Rread) return -3;
	uint32_t n = g32(mbuf + 3);
	if ((int)n > cap) n = cap;
	memcpy(out, mbuf + 7, n);
	return n;
}

static int do_write_fid(uint32_t fid_, const void *data, int len)
{
	int o = 4;
	mbuf[o++] = Twrite; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid_); o += 4;
	p32(mbuf + o, 0); o += 4; p32(mbuf + o, 0); o += 4;
	p32(mbuf + o, len); o += 4; memcpy(mbuf + o, data, len); o += len;
	int t = rpc_to(o, TMO);
	if (t < 0) return t;
	if (t != Rwrite) return -3;
	return (int)g32(mbuf + 3);
}

static void do_clunk(uint32_t fid_)
{
	int o = 4;
	mbuf[o++] = Tclunk; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid_); o += 4;
	rpc_to(o, TMO);
}

/* ---- local file read ----------------------------------------------------- */

/* Walk+open+drain a local file into out (cap). Returns bytes read (>=0), or the
 * negative RC on transport error/timeout. Drains via a short read loop. */
static int read_file(const char *path, char *out, int cap)
{
	int w = do_walk(FID_ROOT, FID_FILE, path);
	if (w < 0) return w;                 /* propagate TIMEOUT/ERR */
	if (w == -3) return 0;               /* no such file -> empty, not fatal */
	if (do_open(FID_FILE, OREAD) != 0) { do_clunk(FID_FILE); return 0; }
	int total = 0;
	for (int i = 0; i < 8 && total < cap - 8; i++) {
		int r = do_read(FID_FILE, 0, (uint8_t *)out + total, cap - 1 - total, TMO);
		if (r == RC_ERR) { do_clunk(FID_FILE); return RC_ERR; }
		if (r <= 0) break;               /* drained (0) or timeout -> stop */
		total += r;
	}
	out[total] = 0;
	do_clunk(FID_FILE);
	return total;
}

/* ---- mesh conversation tunnel (a peer's dev/mflt over the air) ------------ */

static uint8_t pbuf[600];   /* built peer-side T message */
static uint8_t rbuf[600];   /* peer-side R message */
static uint16_t g_ptag = 1; /* peer 9P tag counter */

/* Open a conversation and connect it to <cga>. Leaves FID_CTL + FID_DATA open.
 * Returns 0, or negative RC. */
static int mesh_connect(const char *cga)
{
	char nb[32];
	if (do_walk(FID_ROOT, FID_CTL, "net/aether/clone") != 0) return RC_ERR;
	if (do_open(FID_CTL, ORDWR) != 0) { do_clunk(FID_CTL); return RC_ERR; }
	int n = do_read(FID_CTL, 0, (uint8_t *)nb, sizeof(nb) - 1, TMO);
	if (n <= 0) { do_clunk(FID_CTL); return RC_ERR; }
	nb[n] = 0;
	int conv = atoi(nb);
	char path[64];
	snprintf(path, sizeof(path), "net/aether/%d/data", conv);
	if (do_walk(FID_ROOT, FID_DATA, path) != 0) { do_clunk(FID_CTL); return RC_ERR; }
	if (do_open(FID_DATA, ORDWR) != 0) { do_clunk(FID_DATA); do_clunk(FID_CTL); return RC_ERR; }
	char cmd[64];
	int l = snprintf(cmd, sizeof(cmd), "connect %s", cga);
	if (do_write_fid(FID_CTL, cmd, l) < 0) { do_clunk(FID_DATA); do_clunk(FID_CTL); return -3; }
	return 0;
}

static void mesh_close(void)
{
	do_clunk(FID_DATA);
	do_clunk(FID_CTL);
}

/* Send one framed peer-side 9P message (pbuf[0..len)) as a mesh datagram and
 * return the tag-matched reply in rbuf. Retries a transient mesh drop of THIS
 * request and discards stale/duplicate replies (the datagram channel is untagged
 * at the mesh layer). Returns reply length, or <=0 on failure. */
static int mesh_rpc(int len)
{
	p32(pbuf, (uint32_t)len);   /* stamp the peer 9P size prefix (we build these) */
	uint16_t want = (uint16_t)(pbuf[5] | (pbuf[6] << 8));
	for (int attempt = 0; attempt < 4; attempt++) {
		if (do_write_fid(FID_DATA, pbuf, len) < 0) return -1;
		for (int reads = 0; reads < 6; reads++) {
			int r = do_read(FID_DATA, 0, rbuf, sizeof(rbuf), 6000);
			if (r == RC_ERR) return RC_ERR;
			if (r <= 0) break;           /* no reply this attempt -> resend */
			if (r >= 7 && (uint16_t)(rbuf[5] | (rbuf[6] << 8)) == want) return r;
			/* else stale tag: keep reading within this attempt */
		}
	}
	return -1;
}

/* Peer-side 9P ops, tunneled. Each builds a T in pbuf, sends via mesh_rpc,
 * checks the R type. Peer fids live on the PEER's server (independent space). */
static int peer_version(void)
{
	int o = 4;
	pbuf[o++] = Tversion; p16(pbuf + o, NOTAG); o += 2;
	p32(pbuf + o, 512); o += 4;
	p16(pbuf + o, 6); o += 2; memcpy(pbuf + o, "9P2000", 6); o += 6;
	int r = mesh_rpc(o);
	/* reply is a FRAMED datagram (size[4] kept): type@4, tag@5, count@7, data@11. */
	return (r > 4 && rbuf[4] == Rversion) ? 0 : -1;
}

static int peer_attach(uint32_t fid_)
{
	int o = 4;
	pbuf[o++] = Tattach; p16(pbuf + o, g_ptag++); o += 2;
	p32(pbuf + o, fid_); o += 4; p32(pbuf + o, NOFID); o += 4;
	p16(pbuf + o, 1); o += 2; pbuf[o++] = 't';
	p16(pbuf + o, 0); o += 2;
	int r = mesh_rpc(o);
	return (r > 4 && rbuf[4] == Rattach) ? 0 : -1;
}

static int peer_walk(uint32_t fid_, uint32_t newfid, const char *name)
{
	int o = 4;
	pbuf[o++] = Twalk; p16(pbuf + o, g_ptag++); o += 2;
	p32(pbuf + o, fid_); o += 4; p32(pbuf + o, newfid); o += 4;
	/* single-element walk (name has no '/') */
	p16(pbuf + o, 1); o += 2;
	int l = strlen(name);
	p16(pbuf + o, l); o += 2; memcpy(pbuf + o, name, l); o += l;
	int r = mesh_rpc(o);
	return (r > 4 && rbuf[4] == Rwalk) ? 0 : -1;
}

static int peer_open(uint32_t fid_, uint8_t mode)
{
	int o = 4;
	pbuf[o++] = Topen; p16(pbuf + o, g_ptag++); o += 2;
	p32(pbuf + o, fid_); o += 4; pbuf[o++] = mode;
	int r = mesh_rpc(o);
	return (r > 4 && rbuf[4] == Ropen) ? 0 : -1;
}

static int peer_read(uint32_t fid_, uint64_t off, char *out, int cap)
{
	int o = 4;
	pbuf[o++] = Tread; p16(pbuf + o, g_ptag++); o += 2;
	p32(pbuf + o, fid_); o += 4;
	p32(pbuf + o, off); o += 4; p32(pbuf + o, off >> 32); o += 4;
	p32(pbuf + o, cap > 400 ? 400 : cap); o += 4;
	int r = mesh_rpc(o);
	if (r <= 4 || rbuf[4] != Rread) return -1;
	uint32_t n = g32(rbuf + 7);           /* Rread count, past size[4]+type[1]+tag[2] */
	if ((int)n > cap) n = cap;
	if ((int)n > r - 11) n = r - 11;      /* guard against a short datagram */
	memcpy(out, rbuf + 11, n);            /* data starts past count[4] */
	return n;
}

/* Drain a peer's dev/mflt over the mesh into out (cap). Bounded by a wall-clock
 * deadline so a distant/slow peer can't stall the cycle. Returns bytes (>=0), or
 * RC_ERR if the underlying serial link dropped. */
static int read_peer_mflt(const char *cga, char *out, int cap)
{
	long deadline = now_ms() + 20000;
	int rc = mesh_connect(cga);
	if (rc == RC_ERR) return RC_ERR;
	if (rc != 0) { logline("    peer %s: connect failed", cga); return 0; }
	int total = 0;
	const char *step = "version";
	if (peer_version() != 0 || (step = "attach", peer_attach(FID_ROOT) != 0) ||
	    (step = "walk/dev", peer_walk(FID_ROOT, 1, "dev") != 0) ||
	    (step = "walk/mflt", peer_walk(1, 2, "mflt") != 0) ||
	    (step = "open", peer_open(2, OREAD) != 0)) {
		logline("    peer %s: session failed at %s", cga, step);
		mesh_close();
		return 0;                       /* peer session didn't establish this cycle */
	}
	for (int i = 0; i < 12 && total < cap - 8; i++) {
		if (now_ms() > deadline) break;
		int r = peer_read(2, 0, out + total, cap - 1 - total);
		if (r <= 0) break;
		total += r;
	}
	out[total] = 0;
	mesh_close();
	return total;
}

/* ---- Memfault POST ------------------------------------------------------- */

static int b64dec(const char *in, int inlen, uint8_t *out, int outcap)
{
	int8_t tbl[256];
	memset(tbl, -1, sizeof(tbl));
	const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	for (int i = 0; i < 64; i++) tbl[(unsigned char)a[i]] = i;
	int val = 0, bits = 0, n = 0;
	for (int i = 0; i < inlen; i++) {
		unsigned char c = in[i];
		if (c == '=') break;
		int8_t d = tbl[c];
		if (d < 0) continue;
		val = (val << 6) | d; bits += 6;
		if (bits >= 8) { bits -= 8; if (n < outcap) out[n++] = (val >> bits) & 0xff; }
	}
	return n;
}

/* POST one raw chunk to Memfault via curl (synchronous, reaped by pclose).
 * --fail makes an HTTP >= 400 a nonzero exit. Returns 0 on success. */
static int post_chunk(const char *serial, const uint8_t *data, int len)
{
	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		 "curl -sS --fail --max-time 20 -X POST '%s/%s' "
		 "-H 'Memfault-Project-Key: %s' "
		 "-H 'Content-Type: application/octet-stream' --data-binary @- >/dev/null 2>&1",
		 API_BASE, serial, g_key);
	FILE *p = popen(cmd, "w");
	if (!p) return -1;
	fwrite(data, 1, len, p);
	int st = pclose(p);
	return (st == 0) ? 0 : -1;
}

/* ---- device come/go tracking --------------------------------------------- */

struct dev { char serial[40]; time_t last; unsigned chunks; int active; };
static struct dev devs[64];
static int ndev;

static struct dev *dev_find(const char *serial)
{
	for (int i = 0; i < ndev; i++)
		if (strcmp(devs[i].serial, serial) == 0) return &devs[i];
	if (ndev < (int)(sizeof(devs) / sizeof(devs[0]))) {
		struct dev *d = &devs[ndev++];
		snprintf(d->serial, sizeof(d->serial), "%s", serial);
		d->last = 0; d->chunks = 0; d->active = 0;
		return d;
	}
	return NULL;
}

/* Walk the DEV:/MC: token stream, POST each chunk to the current device, and
 * update the device table. Returns chunks POSTed. */
static int forward_stream(const char *text)
{
	int posted = 0;
	char serial[40] = "";
	const char *p = text;
	while (*p) {
		const char *dev_ = strstr(p, "DEV:");
		const char *mc = strstr(p, "MC:");
		if (mc && (!dev_ || mc < dev_)) {
			const char *b = mc + 3;
			const char *e = strchr(b, ':');
			if (!e) break;
			if (serial[0]) {
				uint8_t raw[512];
				int rl = b64dec(b, (int)(e - b), raw, sizeof(raw));
				if (rl > 0 && post_chunk(serial, raw, rl) == 0) {
					posted++;
					struct dev *d = dev_find(serial);
					if (d) {
						if (!d->active) { logline("  + device %s appeared", serial); d->active = 1; }
						d->last = time(NULL); d->chunks++;
					}
				}
			}
			p = e + 1;
		} else if (dev_) {
			const char *b = dev_ + 4;
			const char *e = strchr(b, ':');
			if (!e) break;
			int l = (int)(e - b);
			if (l > (int)sizeof(serial) - 1) l = sizeof(serial) - 1;
			memcpy(serial, b, l); serial[l] = 0;
			p = e + 1;
		} else break;
	}
	return posted;
}

/* Age out devices that have not forwarded a chunk in a while -> "went silent". */
static void dev_age(void)
{
	time_t now = time(NULL);
	for (int i = 0; i < ndev; i++) {
		if (devs[i].active && now - devs[i].last > (time_t)(g_interval * 4 + 30)) {
			logline("  - device %s silent (no chunks in %lds)",
				devs[i].serial, (long)(now - devs[i].last));
			devs[i].active = 0;
		}
	}
}

/* ---- peer discovery ------------------------------------------------------ */

/* Parse "<honr> identity <CGA> rssi ..." tuples from a neighbor dump into cgas[].
 * Skips the null self-entry. Returns count. */
static int parse_peers(const char *nbrs, char cgas[][24], int max)
{
	int n = 0;
	const char *p = nbrs;
	while ((p = strstr(p, "identity ")) && n < max) {
		p += 9;
		char cga[24]; int i = 0;
		while (*p && (isxdigit((unsigned char)*p) || *p == ':') && i < 23) cga[i++] = *p++;
		cga[i] = 0;
		if (strcmp(cga, "00:00:00:00:00:00") != 0 && strlen(cga) == 17) {
			int dup = 0;
			for (int k = 0; k < n; k++) if (strcmp(cgas[k], cga) == 0) dup = 1;
			if (!dup) snprintf(cgas[n++], 24, "%s", cga);
		}
	}
	return n;
}

/* ---- one forwarding cycle ------------------------------------------------ */
/* Returns 0 normally, RC_ERR if the serial link dropped (-> reconnect). */
static int cycle(void)
{
	static char buf[16384];
	int total = 0, relay_ok = 0;

	/* Gateway's own two chips (over the held serial). */
	int r = read_file("dev/mflt5340", buf, sizeof(buf));
	if (r == RC_ERR) return RC_ERR;
	if (r > 0) { total += forward_stream(buf); relay_ok = 1; }

	r = read_file("dev/mflt9151", buf, sizeof(buf));
	if (r == RC_ERR) return RC_ERR;
	int nine_ok = (r != RC_TIMEOUT);
	if (r > 0) total += forward_stream(buf);

	/* Every in-range mesh peer, drained ON the 9151 (in-process, over the air)
	 * and returned in ONE read -- no host-driven per-peer 9P tunnel over uart1.
	 * This is the on-device peer-drain: the mesh session cost stays on the mesh,
	 * only the result crosses the inter-chip link, once. Self-describing, so it
	 * forwards through the same generic loop. This read can block a few seconds
	 * while the 9151 drains peers (the relay's Tread timeout is 5 min). */
	snprintf(g_where, sizeof(g_where), "dev/mflt_mesh");
	r = read_file("dev/mflt_mesh", buf, sizeof(buf));
	if (r == RC_ERR) return RC_ERR;
	if (r == RC_TIMEOUT) nine_ok = 0;
	int peer_chunks = (r > 0) ? forward_stream(buf) : 0;
	total += peer_chunks;

	/* uart-wedge signal: the 9151 side is unreadable while the relay answered. */
	if (relay_ok && !nine_ok)
		logline("  ! 9151/mesh side not responding (uart link degraded) -- relay-only this cycle");

	dev_age();

	int active = 0;
	for (int i = 0; i < ndev; i++) if (devs[i].active) active++;
	logline("cycle: %d chunk(s) posted (%d from mesh peers), %d device(s) active",
		total, peer_chunks, active);
	return 0;
}

static void sleep_interruptible(int secs)
{
	for (int i = 0; i < secs && !g_stop; i++) sleep(1);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: MEMFAULT_PROJECT_KEY=<key> %s <port|dev> [interval_s]\n", argv[0]);
		return 2;
	}
	g_dev_arg = argv[1];
	if (argc >= 3) g_interval = atoi(argv[2]);
	if (g_interval < 5) g_interval = 5;
	g_key = getenv("MEMFAULT_PROJECT_KEY");
	if (!g_key || !g_key[0]) { fprintf(stderr, "set MEMFAULT_PROJECT_KEY\n"); return 2; }

	struct sigaction sa = { .sa_handler = on_term };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	logline("aether_forwarder up: dev=%s interval=%ds -> Memfault", g_dev_arg, g_interval);

	while (!g_stop) {
		if (fd < 0) {
			if (open_serial() != 0) {
				logline("waiting for gateway port (%s)...", g_dev_arg);
				sleep_interruptible(3);
				continue;
			}
			if (do_version() != 0 || do_attach(FID_ROOT) != 0) {
				logline("9P attach failed, retrying");
				close_serial();
				sleep_interruptible(2);
				continue;
			}
			logline("connected: %s (9P attached)", g_dev);
		}
		if (cycle() == RC_ERR) {
			logline("serial link dropped [at: %s] -- reconnecting", g_where);
			close_serial();
			sleep_interruptible(2);
			continue;
		}
		sleep_interruptible(g_interval);
	}
	if (fd >= 0) { do_clunk(FID_ROOT); close_serial(); }
	logline("aether_forwarder stopped");
	return 0;
}
