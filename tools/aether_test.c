/*
 * aether_test -- a /net/aether conformance harness. A fid-holding 9P client that
 * drives the conversation state machine and asserts behavior against the spec
 * (doc/NET_AETHER_SPEC.md): clone/status accounting, per-conversation files
 * (local/remote/status), the ctl grammar (connect/announce/hangup) and its error
 * cases, datagram-write error paths (ENOTCONN, EMSGSIZE), conversation
 * exhaustion (AETHER_MAX_CONNS), and hangup-wakes-a-blocked-reader.
 *
 * Single-node: every check here targets one node's /net/aether server, so it
 * needs no peer. (Cross-node datagram delivery/length is covered separately.)
 *
 * Build:  cc -O2 -o aether_test tools/aether_test.c
 * Usage:  aether_test /tmp/9p.sock
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

enum {
	Tversion = 100, Rversion = 101, Tattach = 104, Rattach = 105,
	Rerror = 107, Twalk = 110, Rwalk = 111, Topen = 112, Ropen = 113,
	Tread = 116, Rread = 117, Twrite = 118, Rwrite = 119,
	Tclunk = 120, Rclunk = 121,
};
#define NOFID  0xffffffffu
#define NOTAG  0xffff
#define OREAD  0
#define OWRITE 1
#define ORDWR  2
#define MSIZE  8192

static int fd;
static uint8_t mbuf[MSIZE];
static char last_err[160];   /* last Rerror text, for asserting error paths */

static void p16(uint8_t *b, uint16_t v) { b[0] = v; b[1] = v >> 8; }
static void p32(uint8_t *b, uint32_t v) { b[0]=v; b[1]=v>>8; b[2]=v>>16; b[3]=v>>24; }
static uint16_t g16(const uint8_t *b) { return b[0] | (b[1] << 8); }
static uint32_t g32(const uint8_t *b) { return b[0]|(b[1]<<8)|(b[2]<<16)|((uint32_t)b[3]<<24); }

static int readn(uint8_t *b, int n)
{
	int got = 0;
	while (got < n) { int r = read(fd, b + got, n - got); if (r <= 0) return -1; got += r; }
	return got;
}

/* one synchronous RPC (tag 1 unless built otherwise); returns reply type or -1.
 * On Rerror captures the text in last_err. */
static int rpc(int len)
{
	last_err[0] = 0;
	p32(mbuf, len);
	if (write(fd, mbuf, len) != len) return -1;
	uint8_t hdr[4];
	if (readn(hdr, 4) < 0) return -1;
	uint32_t sz = g32(hdr);
	if (sz < 7 || sz > MSIZE) return -1;
	if (readn(mbuf, sz - 4) < 0) return -1;
	if (mbuf[0] == Rerror) {
		uint16_t n = g16(mbuf + 3);
		snprintf(last_err, sizeof(last_err), "%.*s", (int)n, (char *)mbuf + 5);
	}
	return mbuf[0];
}

static int do_version(void)
{
	int o = 4; mbuf[o++] = Tversion; p16(mbuf + o, NOTAG); o += 2;
	p32(mbuf + o, MSIZE); o += 4; p16(mbuf + o, 6); o += 2;
	memcpy(mbuf + o, "9P2000", 6); o += 6;
	return rpc(o) == Rversion ? 0 : -1;
}
static int do_attach(uint32_t fid)
{
	int o = 4; mbuf[o++] = Tattach; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid); o += 4; p32(mbuf + o, NOFID); o += 4;
	p16(mbuf + o, 1); o += 2; mbuf[o++] = 't'; p16(mbuf + o, 0); o += 2;
	return rpc(o) == Rattach ? 0 : -1;
}
static int do_walk(uint32_t fid, uint32_t nf, const char *path)
{
	char tmp[128]; strncpy(tmp, path, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
	const char *parts[16]; int np = 0;
	for (char *p = strtok(tmp, "/"); p && np < 16; p = strtok(NULL, "/")) parts[np++] = p;
	int o = 4; mbuf[o++] = Twalk; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid); o += 4; p32(mbuf + o, nf); o += 4; p16(mbuf + o, np); o += 2;
	for (int i = 0; i < np; i++) {
		int l = strlen(parts[i]); p16(mbuf + o, l); o += 2; memcpy(mbuf + o, parts[i], l); o += l;
	}
	return rpc(o) == Rwalk ? 0 : -1;
}
static int do_open(uint32_t fid, uint8_t mode)
{
	int o = 4; mbuf[o++] = Topen; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid); o += 4; mbuf[o++] = mode;
	return rpc(o) == Ropen ? 0 : -1;
}
static int do_read(uint32_t fid, uint64_t off, uint8_t *out, int cap)
{
	int o = 4; mbuf[o++] = Tread; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid); o += 4; p32(mbuf + o, off); o += 4; p32(mbuf + o, off >> 32); o += 4;
	p32(mbuf + o, cap); o += 4;
	if (rpc(o) != Rread) return -1;
	uint32_t n = g32(mbuf + 3); if ((int)n > cap) n = cap;
	memcpy(out, mbuf + 7, n); return n;
}
static int do_pwrite(uint32_t fid, uint64_t off, const void *data, int len)
{
	int o = 4; mbuf[o++] = Twrite; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid); o += 4; p32(mbuf + o, off); o += 4; p32(mbuf + o, off >> 32); o += 4;
	p32(mbuf + o, len); o += 4; memcpy(mbuf + o, data, len); o += len;
	if (rpc(o) != Rwrite) return -1;
	return (int)g32(mbuf + 3);
}
static int do_write(uint32_t fid, const void *d, int l) { return do_pwrite(fid, 0, d, l); }
static void do_clunk(uint32_t fid)
{
	int o = 4; mbuf[o++] = Tclunk; p16(mbuf + o, 1); o += 2; p32(mbuf + o, fid); o += 4; rpc(o);
}

/* tag-aware send (for the hangup wakeup test) */
static int send_only(int len) { p32(mbuf, len); return write(fd, mbuf, len) == len ? 0 : -1; }
static int b_read(uint16_t tag, uint32_t fid, uint32_t count)
{
	int o = 4; mbuf[o++] = Tread; p16(mbuf + o, tag); o += 2; p32(mbuf + o, fid); o += 4;
	p32(mbuf + o, 0); o += 4; p32(mbuf + o, 0); o += 4; p32(mbuf + o, count); o += 4; return o;
}
static int b_write(uint16_t tag, uint32_t fid, const void *d, int l)
{
	int o = 4; mbuf[o++] = Twrite; p16(mbuf + o, tag); o += 2; p32(mbuf + o, fid); o += 4;
	p32(mbuf + o, 0); o += 4; p32(mbuf + o, 0); o += 4; p32(mbuf + o, l); o += 4;
	memcpy(mbuf + o, d, l); o += l; return o;
}
static int recv_one(uint8_t *dst, uint16_t *tag)
{
	uint8_t hdr[4]; if (readn(hdr, 4) < 0) return -1;
	uint32_t sz = g32(hdr); if (sz < 7 || sz > MSIZE) return -1;
	if (readn(dst, sz - 4) < 0) return -1; *tag = g16(dst + 1); return dst[0];
}

/* read a fresh path into out[cap] (NUL-terminated); returns len or -1 */
static int read_path(uint32_t fid, const char *path, char *out, int cap)
{
	if (do_walk(0, fid, path)) return -1;
	if (do_open(fid, OREAD)) { do_clunk(fid); return -1; }
	int n = do_read(fid, 0, (uint8_t *)out, cap - 1);
	do_clunk(fid);
	if (n < 0) return -1;
	out[n] = 0; return n;
}
/* clone -> conversation number, holding the ctl fid open at `ctlfid`. -1 fail. */
static int sclone(uint32_t ctlfid)
{
	char b[32];
	if (do_walk(0, ctlfid, "net/aether/clone")) return -1;
	if (do_open(ctlfid, ORDWR)) return -1;
	int n = do_read(ctlfid, 0, (uint8_t *)b, sizeof(b) - 1);
	if (n < 0) return -1; b[n] = 0; return atoi(b);
}

static int g_pass, g_fail;
static void chk(int cond, const char *desc, const char *extra)
{
	printf("  [%s] %s%s%s\n", cond ? "PASS" : "FAIL", desc,
	       extra && extra[0] ? " :: " : "", extra ? extra : "");
	if (cond) g_pass++; else g_fail++;
}
/* trim a trailing newline for tidy one-line reporting */
static char *oneline(char *s) { char *nl = strchr(s, '\n'); if (nl) *nl = 0; return s; }

/* Isolation probe: step-by-step, to find what breaks the data walk / 2nd clone. */
static int do_iso(void)
{
	char b[256]; int n, A, B;
	if (do_version() || do_attach(0)) { fprintf(stderr, "setup failed\n"); return 1; }
	printf("== isolation probe ==\n");
	A = sclone(10);
	printf("  clone A -> conv %d (%s)\n", A, A >= 0 ? "ok" : last_err);

	snprintf(b, sizeof(b), "net/aether/%d/data", A);
	int w1 = do_walk(0, 20, b); int o1 = w1 ? -1 : do_open(20, ORDWR);
	printf("  T1 walk+open A/data (immediate, like aether_conv): walk=%s open=%s %s\n",
	       w1 ? "FAIL" : "ok", w1 ? "-" : (o1 ? "FAIL" : "ok"), last_err);

	snprintf(b, sizeof(b), "net/aether/%d/status", A);
	n = read_path(31, b, b, sizeof(b));
	printf("  T2 read A/status (one per-conv read): %s\n", n >= 0 ? "ok" : "FAIL");

	snprintf(b, sizeof(b), "net/aether/%d/data", A);
	int w3 = do_walk(0, 22, b);
	printf("  T3 walk A/data AGAIN (after a per-conv read): %s %s\n",
	       w3 ? "FAIL" : "ok", w3 ? last_err : "");

	snprintf(b, sizeof(b), "net/aether/%d/local", A);  (void)read_path(32, b, b, sizeof(b));
	snprintf(b, sizeof(b), "net/aether/%d/remote", A); (void)read_path(33, b, b, sizeof(b));
	snprintf(b, sizeof(b), "net/aether/%d/data", A);
	int w4 = do_walk(0, 23, b);
	printf("  T4 walk A/data (after 3 per-conv reads): %s %s\n",
	       w4 ? "FAIL" : "ok", w4 ? last_err : "");

	B = sclone(11);
	printf("  T5 clone B (2nd conversation): conv %d (%s)\n", B, B >= 0 ? "ok" : last_err);
	do_clunk(0);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s <unix-sock> [--iso]\n", argv[0]); return 2; }
	setvbuf(stdout, NULL, _IONBF, 0);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strncpy(a.sun_path, argv[1], sizeof(a.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("connect"); return 1; }
	if (argc > 2 && strcmp(argv[2], "--iso") == 0) return do_iso();
	if (do_version() || do_attach(0)) { fprintf(stderr, "9P setup failed\n"); return 1; }

	char b[256], path[64];
	int n, w, A, B;

	printf("== /net/aether conformance (single node) ==\n");

	/* --- top-level files --- */
	n = read_path(30, "net/aether/addr", b, sizeof(b));
	chk(n > 0 && strchr(b, ':'), "addr: colon-hex 6-byte address", n > 0 ? oneline(b) : "read failed");

	n = read_path(30, "net/aether/status", b, sizeof(b));
	chk(n > 0 && strchr(b, '/'), "status: \"<active>/<max>\" form", n > 0 ? oneline(b) : "read failed");
	int max_conns = 0; if (n > 0) { char *sl = strchr(b, '/'); if (sl) max_conns = atoi(sl + 1); }

	/* --- clone + accounting --- */
	A = sclone(10);
	chk(A >= 0, "clone allocates a conversation (ctl fid held)", A >= 0 ? "" : last_err);
	n = read_path(30, "net/aether/status", b, sizeof(b));
	chk(n > 0 && b[0] != '0', "status shows >=1 active while ctl held", oneline(b));

	/* --- per-conversation files, fresh (unconnected) --- */
	snprintf(path, sizeof(path), "net/aether/%d/status", A);
	n = read_path(31, path, b, sizeof(b));
	chk(n > 0 && strstr(b, "unconnected"), "N/status == unconnected (fresh)", oneline(b));
	snprintf(path, sizeof(path), "net/aether/%d/local", A);
	n = read_path(31, path, b, sizeof(b));
	chk(n > 0 && strchr(b, ':'), "N/local == this node's address", oneline(b));
	snprintf(path, sizeof(path), "net/aether/%d/remote", A);
	n = read_path(31, path, b, sizeof(b));
	chk(n == 0, "N/remote empty when unbound", n == 0 ? "(empty)" : oneline(b));

	/* --- ctl error grammar --- */
	w = do_write(10, "connect zz:zz:zz", 16);
	chk(w < 0, "ctl connect <bad-addr> rejected", last_err);
	w = do_write(10, "frobnicate", 10);
	chk(w < 0, "ctl unknown verb rejected", last_err);

	/* --- connect + bound state --- */
	w = do_write(10, "connect 00:00:00:00:00:99", 25);
	chk(w >= 0, "ctl connect <addr> accepted", w >= 0 ? "" : last_err);
	snprintf(path, sizeof(path), "net/aether/%d/status", A);
	n = read_path(31, path, b, sizeof(b));
	chk(n > 0 && strstr(b, "connected"), "N/status == connected after connect", oneline(b));
	snprintf(path, sizeof(path), "net/aether/%d/remote", A);
	n = read_path(31, path, b, sizeof(b));
	chk(n > 0 && strstr(b, "00:00:00:00:00:99"), "N/remote shows bound peer", oneline(b));
	w = do_write(10, "connect 00:00:00:00:00:98", 25);
	chk(w < 0, "second connect on a connected conv rejected", last_err);

	/* --- data write error paths (on conv A, connected) --- */
	snprintf(path, sizeof(path), "net/aether/%d/data", A);
	if (do_walk(0, 20, path) || do_open(20, ORDWR)) { fprintf(stderr, "open A/data failed\n"); }
	static uint8_t big[600]; memset(big, 'x', sizeof(big));
	w = do_pwrite(20, 0, big, 600);
	chk(w < 0, "data write >512 rejected (EMSGSIZE)", last_err);

	/* --- ENOTCONN: write data on an unconnected conv B --- */
	B = sclone(11);
	chk(B >= 0, "clone a second conversation", B >= 0 ? "" : last_err);
	snprintf(path, sizeof(path), "net/aether/%d/data", B);
	if (do_walk(0, 21, path) || do_open(21, ORDWR)) { fprintf(stderr, "open B/data failed\n"); }
	w = do_write(21, "hi", 2);
	chk(w < 0, "data write on unconnected conv rejected (ENOTCONN)", last_err);

	/* --- announce + its error case --- */
	w = do_write(11, "announce", 8);
	chk(w >= 0, "ctl announce accepted", w >= 0 ? "" : last_err);
	snprintf(path, sizeof(path), "net/aether/%d/status", B);
	n = read_path(31, path, b, sizeof(b));
	chk(n > 0 && strstr(b, "announced"), "N/status == announced", oneline(b));
	w = do_write(11, "connect 00:00:00:00:00:97", 25);
	chk(w < 0, "connect on an announced conv rejected", last_err);

	/* --- hangup wakes a blocked data reader with EOF (0 bytes) --- */
	send_only(b_read(40, 21, 64));          /* blocking read on B/data (announced), tag 40 */
	send_only(b_write(1, 11, "hangup", 6)); /* hangup write, tag 1 */
	int got_ack = 0, got_eof = 0;            /* both replies, either order */
	for (int i = 0; i < 2; i++) {
		uint8_t rb[128]; uint16_t tg = 0;
		int ty = recv_one(rb, &tg);
		if (tg == 1 && ty == Rwrite) got_ack = 1;
		if (tg == 40 && ty == Rread && g32(rb + 3) == 0) got_eof = 1;
	}
	chk(got_ack && got_eof, "hangup wakes blocked data read with EOF",
	    got_eof ? "EOF (Rread count=0)" : "no EOF");

	/* --- conversation exhaustion: fill to max, next clone fails --- */
	int held = 2; uint32_t cf = 12;          /* A(10), B(11) already held */
	while (held < max_conns && cf < 12 + 8) { if (sclone(cf) < 0) break; held++; cf++; }
	snprintf(path, sizeof(path), "net/aether/status");
	n = read_path(31, "net/aether/status", b, sizeof(b));
	chk(held == max_conns, "can hold MAX concurrent conversations", oneline(b));
	int over = sclone(cf);                    /* one past max */
	chk(over < 0, "clone past MAX is refused", over < 0 ? last_err : "unexpectedly succeeded");

	/* --- teardown: clunk data fids + all ctls, status returns toward baseline --- */
	do_clunk(20); do_clunk(21);
	for (uint32_t f = 10; f < cf; f++) do_clunk(f);
	n = read_path(31, "net/aether/status", b, sizeof(b));
	chk(n > 0 && b[0] == '0', "status back to 0 active after clunking all ctls", oneline(b));

	do_clunk(0);
	printf("== result: %d passed, %d failed ==\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
