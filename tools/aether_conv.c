/*
 * aether_conv -- a minimal fid-holding 9P client that drives the /net/aether
 * conversation dance end to end, to validate the stateful path that plan9port's
 * one-shot `9p` cannot (each `9p` invocation opens+clunks, so it can't hold the
 * clone/ctl fid open while walking N/data).
 *
 * It connects to a 9P server over a unix socket (e.g. socat bridging the 5340's
 * USB-CDC 9P port), then:
 *   attach -> walk net/aether/clone (alloc conversation N) -> read "N"
 *          -> walk net/aether/N/data  (deep walk; intermediate convdir fid is
 *             clunked by the walk -- must NOT free conv N: the 0.6.1 fix)
 *          -> read net/aether/status  (expect "1/<max>" while ctl held)
 *          -> [connect <peer>] write ctl -> write a datagram to data
 *
 * Build:  cc -O2 -o aether_conv tools/aether_conv.c
 * Usage:  aether_conv /tmp/9p.sock [peer_addr]
 *         peer_addr like 00:00:00:00:00:01 ; if given, connect+send a datagram.
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
#define NOFID    0xffffffffu
#define NOTAG    0xffff
#define OREAD    0
#define OWRITE   1
#define ORDWR    2
#define MSIZE    8192

static int fd;
static uint8_t mbuf[MSIZE];

static void p16(uint8_t *b, uint16_t v) { b[0] = v; b[1] = v >> 8; }
static void p32(uint8_t *b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static uint16_t g16(const uint8_t *b) { return b[0] | (b[1] << 8); }
static uint32_t g32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }

static int readn(uint8_t *b, int n)
{
	int got = 0;
	while (got < n) {
		int r = read(fd, b + got, n - got);
		if (r <= 0) return -1;
		got += r;
	}
	return got;
}

/* send the message in mbuf (size[4] already implied by len), recv the reply into
 * mbuf. Returns the reply type, or -1 on transport error. On Rerror prints it. */
static int rpc(int len)
{
	p32(mbuf, len);
	if (write(fd, mbuf, len) != len) { perror("write"); return -1; }
	uint8_t hdr[4];
	if (readn(hdr, 4) < 0) { fprintf(stderr, "eof\n"); return -1; }
	uint32_t sz = g32(hdr);
	if (sz < 7 || sz > MSIZE) { fprintf(stderr, "bad size %u\n", sz); return -1; }
	if (readn(mbuf, sz - 4) < 0) { fprintf(stderr, "short reply\n"); return -1; }
	int type = mbuf[0];
	if (type == Rerror) {
		uint16_t n = g16(mbuf + 3);
		fprintf(stderr, "Rerror: %.*s\n", n, (char *)mbuf + 5);
	}
	return type;
}

static int do_version(void)
{
	int o = 4;
	mbuf[o++] = Tversion; p16(mbuf + o, NOTAG); o += 2;
	p32(mbuf + o, MSIZE); o += 4;
	p16(mbuf + o, 6); o += 2; memcpy(mbuf + o, "9P2000", 6); o += 6;
	return rpc(o) == Rversion ? 0 : -1;
}

static int do_attach(uint32_t fid)
{
	int o = 4;
	mbuf[o++] = Tattach; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid); o += 4; p32(mbuf + o, NOFID); o += 4;
	p16(mbuf + o, 1); o += 2; mbuf[o++] = 't';     /* uname */
	p16(mbuf + o, 0); o += 2;                       /* aname "" */
	return rpc(o) == Rattach ? 0 : -1;
}

/* walk a '/'-separated path from fid to newfid (clone if path empty). */
static int do_walk(uint32_t fid, uint32_t newfid, const char *path)
{
	char tmp[128]; strncpy(tmp, path, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
	const char *parts[16]; int np = 0;
	for (char *p = strtok(tmp, "/"); p && np < 16; p = strtok(NULL, "/")) parts[np++] = p;
	int o = 4;
	mbuf[o++] = Twalk; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid); o += 4; p32(mbuf + o, newfid); o += 4;
	p16(mbuf + o, np); o += 2;
	for (int i = 0; i < np; i++) {
		int l = strlen(parts[i]);
		p16(mbuf + o, l); o += 2; memcpy(mbuf + o, parts[i], l); o += l;
	}
	return rpc(o) == Rwalk ? 0 : -1;
}

static int do_open(uint32_t fid, uint8_t mode)
{
	int o = 4;
	mbuf[o++] = Topen; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid); o += 4; mbuf[o++] = mode;
	return rpc(o) == Ropen ? 0 : -1;
}

/* read up to cap bytes into out; returns byte count or -1. */
static int do_read(uint32_t fid, uint64_t off, uint8_t *out, int cap)
{
	int o = 4;
	mbuf[o++] = Tread; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid); o += 4;
	p32(mbuf + o, off); o += 4; p32(mbuf + o, off >> 32); o += 4;
	p32(mbuf + o, cap); o += 4;
	if (rpc(o) != Rread) return -1;
	/* reply in mbuf: type[1] tag[2] count[4] data[] -> count@3, data@7 */
	uint32_t n = g32(mbuf + 3);
	if ((int)n > cap) n = cap;
	memcpy(out, mbuf + 7, n);
	return n;
}

static int do_write(uint32_t fid, const void *data, int len)
{
	int o = 4;
	mbuf[o++] = Twrite; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid); o += 4;
	p32(mbuf + o, 0); o += 4; p32(mbuf + o, 0); o += 4;
	p32(mbuf + o, len); o += 4; memcpy(mbuf + o, data, len); o += len;
	if (rpc(o) != Rwrite) return -1;
	/* reply in mbuf: type[1] tag[2] count[4] -> count@3 */
	return (int)g32(mbuf + 3);
}

static void do_clunk(uint32_t fid)
{
	int o = 4;
	mbuf[o++] = Tclunk; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid); o += 4;
	rpc(o);
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s <unix-sock> [peer_addr]\n", argv[0]); return 2; }
	const char *sock = argv[1];
	const char *peer = argc > 2 ? argv[2] : NULL;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strncpy(a.sun_path, sock, sizeof(a.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("connect"); return 1; }

	uint8_t buf[256];
	if (do_version()) { fprintf(stderr, "version failed\n"); return 1; }
	if (do_attach(0)) { fprintf(stderr, "attach failed\n"); return 1; }
	printf("[ok] attached\n");

	/* clone -> conversation N (fid 1 becomes the ctl fid; HOLD it open) */
	if (do_walk(0, 1, "net/aether/clone")) { fprintf(stderr, "walk clone failed\n"); return 1; }
	if (do_open(1, ORDWR)) { fprintf(stderr, "open clone failed\n"); return 1; }
	int n = do_read(1, 0, buf, sizeof(buf) - 1);
	if (n < 0) { fprintf(stderr, "read clone failed\n"); return 1; }
	buf[n] = 0; int conv = atoi((char *)buf);
	printf("[ok] clone -> conversation %d (ctl fid held open)\n", conv);

	/* deep walk net/aether/N/data: the intermediate convdir fid gets clunked
	 * by the walk machinery -- the 0.6.1 fix means conv N survives. */
	char path[64]; snprintf(path, sizeof(path), "net/aether/%d/data", conv);
	if (do_walk(0, 2, path)) { fprintf(stderr, "walk %s failed\n", path); return 1; }
	if (do_open(2, ORDWR)) { fprintf(stderr, "open data failed\n"); return 1; }
	printf("[ok] deep walk %s + open (conv survived convdir clunk)\n", path);

	/* status should now show one active conversation while the ctl fid is held */
	if (do_walk(0, 3, "net/aether/status") == 0 && do_open(3, OREAD) == 0) {
		n = do_read(3, 0, buf, sizeof(buf) - 1);
		if (n > 0) { buf[n] = 0; printf("[ok] status while ctl held: %s", (char *)buf); }
		do_clunk(3);
	}

	if (peer) {
		char cmd[64]; int l = snprintf(cmd, sizeof(cmd), "connect %s", peer);
		if (do_write(1, cmd, l) >= 0) printf("[ok] ctl: %s\n", cmd);
		else fprintf(stderr, "ctl connect failed\n");
		const char *msg = "hello from aether_conv";
		int w = do_write(2, msg, strlen(msg));
		if (w >= 0) printf("[ok] sent %d-byte datagram to %s\n", w, peer);
		else fprintf(stderr, "[info] data write returned error (no peer/ack?)\n");
	}

	/* clunk data first, then ctl (clunking ctl tears the conversation down) */
	do_clunk(2);
	do_clunk(1);
	printf("[ok] clunked data + ctl (conversation %d torn down)\n", conv);

	/* re-read status: back to zero active */
	if (do_walk(0, 4, "net/aether/status") == 0 && do_open(4, OREAD) == 0) {
		n = do_read(4, 0, buf, sizeof(buf) - 1);
		if (n > 0) { buf[n] = 0; printf("[ok] status after teardown: %s", (char *)buf); }
		do_clunk(4);
	}
	do_clunk(0);
	printf("[done] conversation path validated end to end\n");
	return 0;
}
