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
#include <time.h>
#include <signal.h>

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

/* SIGINT/SIGTERM set this; the receive loops (do_recv/do_crecv) check it and the
 * blocked read() returns EINTR (handler installed WITHOUT SA_RESTART), so a killed
 * receiver falls through to its do_clunk() instead of leaking the conversation.
 * The /net/aether pool is only 4 deep and an orphaned ctl fid survives a socat
 * restart (the shared DTR session never drops), so leak-on-kill exhausts it fast. */
static volatile sig_atomic_t g_stop = 0;
static void on_term(int s) { (void)s; g_stop = 1; }

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

static int do_pwrite(uint32_t fid, uint64_t off, const void *data, int len)
{
	int o = 4;
	mbuf[o++] = Twrite; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid); o += 4;
	p32(mbuf + o, off); o += 4; p32(mbuf + o, off >> 32); o += 4;
	p32(mbuf + o, len); o += 4; memcpy(mbuf + o, data, len); o += len;
	if (rpc(o) != Rwrite) return -1;
	/* reply in mbuf: type[1] tag[2] count[4] -> count@3 */
	return (int)g32(mbuf + 3);
}

static int do_write(uint32_t fid, const void *data, int len)
{
	return do_pwrite(fid, 0, data, len);
}

static void do_clunk(uint32_t fid)
{
	int o = 4;
	mbuf[o++] = Tclunk; p16(mbuf + o, 1); o += 2;
	p32(mbuf + o, fid); o += 4;
	rpc(o);
}

/* ---- tag-aware primitives for the pipelined concurrency demo ---- */

static int send_only(int len)
{
	p32(mbuf, len);
	return write(fd, mbuf, len) == len ? 0 : -1;
}

static uint8_t stash[MSIZE];
static uint16_t stash_tag;
static int have_stash;

/* read one reply into dst (dst[0]=type, dst[1..2]=tag); returns type or -1 */
static int recv_one(uint8_t *dst, uint16_t *tag)
{
	uint8_t hdr[4];
	if (readn(hdr, 4) < 0) return -1;
	uint32_t sz = g32(hdr);
	if (sz < 7 || sz > MSIZE) return -1;
	if (readn(dst, sz - 4) < 0) return -1;
	*tag = g16(dst + 1);
	return dst[0];
}

/* wait for the reply bearing `want`, stashing one out-of-order reply */
static int recv_tag(uint16_t want, uint8_t *dst)
{
	if (have_stash && stash_tag == want) {
		memcpy(dst, stash, MSIZE);
		have_stash = 0;
		return dst[0];
	}
	for (;;) {
		uint16_t t;
		int ty = recv_one(dst, &t);
		if (ty < 0) return -1;
		if (t == want) return ty;
		memcpy(stash, dst, MSIZE);
		stash_tag = t;
		have_stash = 1;
	}
}

/* build helpers that take an explicit tag, returning the framed length */
static int b_walk(uint16_t tag, uint32_t fid, uint32_t nf, const char *path)
{
	char tmp[128]; strncpy(tmp, path, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
	const char *parts[16]; int np = 0;
	for (char *p = strtok(tmp, "/"); p && np < 16; p = strtok(NULL, "/")) parts[np++] = p;
	int o = 4; mbuf[o++] = Twalk; p16(mbuf + o, tag); o += 2;
	p32(mbuf + o, fid); o += 4; p32(mbuf + o, nf); o += 4; p16(mbuf + o, np); o += 2;
	for (int i = 0; i < np; i++) { int l = strlen(parts[i]); p16(mbuf + o, l); o += 2; memcpy(mbuf + o, parts[i], l); o += l; }
	return o;
}
static int b_open(uint16_t tag, uint32_t fid, uint8_t mode)
{ int o = 4; mbuf[o++] = Topen; p16(mbuf + o, tag); o += 2; p32(mbuf + o, fid); o += 4; mbuf[o++] = mode; return o; }
static int b_read(uint16_t tag, uint32_t fid, uint32_t count)
{ int o = 4; mbuf[o++] = Tread; p16(mbuf + o, tag); o += 2; p32(mbuf + o, fid); o += 4; p32(mbuf + o, 0); o += 4; p32(mbuf + o, 0); o += 4; p32(mbuf + o, count); o += 4; return o; }
static int b_clunk(uint16_t tag, uint32_t fid)
{ int o = 4; mbuf[o++] = Tclunk; p16(mbuf + o, tag); o += 2; p32(mbuf + o, fid); o += 4; return o; }

static long now_ms(void)
{ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L; }

/* Prove the server stays responsive while a data read blocks: open a
 * conversation's data fid, fire a blocking Tread (no datagram will come), then
 * WITHOUT waiting do a status read and show it returns in milliseconds -- only
 * possible if the blocking read was dispatched off the processing thread. */
static int demo_concurrent(void)
{
	uint8_t rb[256];
	if (do_version() || do_attach(0)) { fprintf(stderr, "setup failed\n"); return 1; }
	printf("[ok] attached\n");
	if (do_walk(0, 1, "net/aether/clone") || do_open(1, ORDWR)) { fprintf(stderr, "clone failed\n"); return 1; }
	int n = do_read(1, 0, rb, sizeof(rb) - 1); rb[n < 0 ? 0 : n] = 0; int conv = atoi((char *)rb);
	char path[64]; snprintf(path, sizeof(path), "net/aether/%d/data", conv);
	if (do_walk(0, 2, path) || do_open(2, ORDWR)) { fprintf(stderr, "data open failed\n"); return 1; }
	printf("[ok] conversation %d open; data fid ready\n", conv);

	/* fire a BLOCKING read on data (tag 10) and DO NOT wait for it, then
	 * immediately -- no client-side pacing -- pipeline a status read behind
	 * it. The server's ring-buffered RX frames both without dropping (9P is
	 * multiplexed; a client may have many requests outstanding). */
	send_only(b_read(10, 2, 256));
	printf("[ok] blocking Tread(data) in flight (no datagram will arrive)\n");

	/* now interleave a status read on the SAME connection, timed */
	long t0 = now_ms();
	send_only(b_walk(1, 0, 3, "net/aether/status")); recv_tag(1, rb);
	send_only(b_open(1, 3, OREAD)); recv_tag(1, rb);
	send_only(b_read(1, 3, sizeof(rb) - 1)); int ty = recv_tag(1, rb);
	long dt = now_ms() - t0;
	if (ty == Rread) {
		uint32_t c = g32(rb + 3); rb[7 + (c > 200 ? 200 : c)] = 0;
		printf("[ok] status read returned in %ld ms WHILE data read blocked: %.*s",
		       dt, (int)c, (char *)rb + 7);
	}
	send_only(b_clunk(1, 3)); recv_tag(1, rb);
	if (dt < 2000)
		printf("[PASS] server stayed responsive (status in %ldms, not stalled behind the blocked read)\n", dt);
	else
		printf("[FAIL] status took %ldms -- looks stalled behind the blocked read\n", dt);

	/* tear down: clunk ctl -> conversation frees -> the blocked data read gets EOF */
	send_only(b_clunk(1, 1)); recv_tag(1, rb);          /* clunk ctl (tag 1) */
	int dty = recv_tag(10, rb);                          /* now the deferred data reply */
	if (dty == Rread) printf("[ok] blocked data read woke with %u bytes (EOF on hangup)\n", g32(rb + 3));
	send_only(b_clunk(1, 2)); recv_tag(1, rb);
	do_clunk(0);
	printf("[done] concurrent-read demo complete\n");
	return 0;
}

/* Stream a file to a 9P path in small chunks. Each Twrite carries `chunk` data
 * bytes (default 1024), so even a server whose RX ring is small (e.g. an old
 * build before the ring was sized to msize) accepts it without overflow -- the
 * resilient way to push a corrected firmware image (e.g. /dev/fw5340) when a
 * full-size write would stall. */
static int do_put(const char *path, const char *file, int chunk)
{
	if (chunk <= 0 || chunk > (int)sizeof(mbuf) - 64) chunk = 1024;
	FILE *f = fopen(file, "rb");
	if (!f) { perror("fopen"); return 1; }

	if (do_version() || do_attach(0)) { fprintf(stderr, "setup failed\n"); return 1; }
	if (do_walk(0, 1, path)) { fprintf(stderr, "walk %s failed\n", path); return 1; }
	if (do_open(1, OWRITE)) { fprintf(stderr, "open %s (OWRITE) failed\n", path); return 1; }
	printf("[ok] %s open for write; streaming %s in %d-byte chunks\n", path, file, chunk);

	uint8_t *blk = malloc(chunk);
	uint64_t off = 0;
	size_t r;
	long t0 = now_ms();
	while ((r = fread(blk, 1, chunk, f)) > 0) {
		int w = do_pwrite(1, off, blk, (int)r);
		if (w < 0) { fprintf(stderr, "\nwrite failed at offset %llu\n", off); free(blk); fclose(f); return 1; }
		off += w;
		if ((off & 0x3fff) == 0 || (size_t)w < r) printf("\r  %llu bytes", off);
	}
	printf("\r[ok] streamed %llu bytes in %ld ms\n", off, now_ms() - t0);
	free(blk); fclose(f);
	do_clunk(1);
	do_clunk(0);
	printf("[done] wrote %s\n", path);
	return 0;
}

/* Open a conversation, fire a blocking data read, and HOLD it (never tear down)
 * so the caller can kill us mid-read -- the test for the async-worker epoch
 * guard: on disconnect the read's worker must not inject a stale reply into a
 * reused session. */
static int do_hold(void)
{
	uint8_t rb[256];
	if (do_version() || do_attach(0)) { fprintf(stderr, "setup failed\n"); return 1; }
	if (do_walk(0, 1, "net/aether/clone") || do_open(1, ORDWR)) { fprintf(stderr, "clone failed\n"); return 1; }
	int n = do_read(1, 0, rb, sizeof(rb) - 1); rb[n < 0 ? 0 : n] = 0; int conv = atoi((char *)rb);
	char path[64]; snprintf(path, sizeof(path), "net/aether/%d/data", conv);
	if (do_walk(0, 2, path) || do_open(2, ORDWR)) { fprintf(stderr, "data open failed\n"); return 1; }
	send_only(b_read(10, 2, 256));   /* blocking read, never replied (no datagram) */
	printf("[ok] conv %d: blocking data read held open -- kill me now\n", conv);
	for (;;) pause();   /* hold until killed */
	return 0;
}

/* Receiver: clone a conversation, ANNOUNCE (receive any datagram, source-
 * prefixed), then blocking-read one datagram and print its source + payload.
 * Pair with a sender (`aether_conv <peer-sock> <this-node-MAC>`). */
static int do_recv(int count)
{
	uint8_t rb[600];
	if (do_version() || do_attach(0)) { fprintf(stderr, "setup failed\n"); return 1; }
	if (do_walk(0, 1, "net/aether/clone") || do_open(1, ORDWR)) { fprintf(stderr, "clone failed\n"); return 1; }
	int n = do_read(1, 0, rb, sizeof(rb) - 1); rb[n < 0 ? 0 : n] = 0; int conv = atoi((char *)rb);
	if (do_write(1, "announce", 8) < 0) { fprintf(stderr, "announce failed\n"); do_clunk(1); do_clunk(0); return 1; }
	char path[64]; snprintf(path, sizeof(path), "net/aether/%d/data", conv);
	if (do_walk(0, 2, path) || do_open(2, ORDWR)) { fprintf(stderr, "data open failed\n"); do_clunk(1); do_clunk(0); return 1; }
	/* ONE announced session reads up to `count` datagrams back-to-back (the conv
	 * stays announced; the rxq buffers bursts). This avoids respawning a receiver
	 * per datagram -- which raced announce-vs-arrival and churned the DTR session
	 * pool -- so a sequential-delivery (reliability) check reads cleanly. */
	printf("[recv] conv %d announced; reading up to %d datagram(s) in one session...\n", conv, count);
	for (int k = 0; k < count && !g_stop; k++) {
		n = do_read(2, 0, rb, sizeof(rb) - 1);
		if (n < 0) { if (!g_stop) fprintf(stderr, "[recv] read failed: %d\n", n); break; }
		if (n == 0) { printf("[recv] EOF (hangup)\n"); break; }
		if (n >= 6) {
			printf("[RECV] %d bytes from %02x:%02x:%02x:%02x:%02x:%02x : %.*s\n",
			       n - 6, rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], n - 6, (char *)rb + 6);
		} else {
			printf("[recv] %d bytes (no src prefix): %.*s\n", n, n, (char *)rb);
		}
	}
	do_clunk(2); do_clunk(1); do_clunk(0);
	printf("[recv] done\n");
	return 0;
}

/* Held sender: ONE session -- clone, connect <peer>, send <count> datagrams
 * spaced <gap_ms> apart, then exit. No per-send respawn, so the sender side
 * does not churn the link either (pairs with a held `--recv <count>` receiver,
 * so a reliability measurement reflects pure mesh delivery, not harness churn). */
static int do_sendn(const char *peer, int count, int gap_ms)
{
	uint8_t rb[64];
	if (do_version() || do_attach(0)) { fprintf(stderr, "setup failed\n"); return 1; }
	if (do_walk(0, 1, "net/aether/clone") || do_open(1, ORDWR)) { fprintf(stderr, "clone failed\n"); return 1; }
	int n = do_read(1, 0, rb, sizeof(rb) - 1); rb[n < 0 ? 0 : n] = 0; int conv = atoi((char *)rb);
	char cmd[64]; int l = snprintf(cmd, sizeof(cmd), "connect %s", peer);
	if (do_write(1, cmd, l) < 0) { fprintf(stderr, "connect failed\n"); do_clunk(1); do_clunk(0); return 1; }
	char path[64]; snprintf(path, sizeof(path), "net/aether/%d/data", conv);
	if (do_walk(0, 2, path) || do_open(2, ORDWR)) { fprintf(stderr, "data open failed\n"); do_clunk(1); do_clunk(0); return 1; }
	for (int k = 0; k < count && !g_stop; k++) {
		char msg[32]; int ml = snprintf(msg, sizeof(msg), "sn-%d", k);
		if (do_write(2, msg, ml) < 0) fprintf(stderr, "[sendn] write %d failed\n", k);
		else printf("[SENT] %d : %s\n", k, msg);
		if (gap_ms > 0) usleep((useconds_t)gap_ms * 1000);
	}
	do_clunk(2); do_clunk(1); do_clunk(0);
	printf("[sendn] done (%d sent)\n", count);
	return 0;
}

/* Big-datagram sender: send <count> datagrams of <nbytes> each (filled with a
 * verifiable pattern: byte i = 'A' + (i % 26), with a 4-char "bNNN" tag at the
 * front), spaced <gap_ms> apart. Validates that the DECT per-frame TBS sizing
 * carries a full payload end-to-end -- the suite only sends ~13-byte frames that
 * fit even the old fixed 69-byte block, so they never exercised large frames. */
static int do_sendbig(const char *peer, int nbytes, int count, int gap_ms)
{
	uint8_t rb[64];
	static uint8_t payload[600];
	if (nbytes < 1) nbytes = 1;
	if (nbytes > (int)sizeof(payload)) nbytes = sizeof(payload);
	if (do_version() || do_attach(0)) { fprintf(stderr, "setup failed\n"); return 1; }
	if (do_walk(0, 1, "net/aether/clone") || do_open(1, ORDWR)) { fprintf(stderr, "clone failed\n"); return 1; }
	int n = do_read(1, 0, rb, sizeof(rb) - 1); rb[n < 0 ? 0 : n] = 0; int conv = atoi((char *)rb);
	char cmd[64]; int l = snprintf(cmd, sizeof(cmd), "connect %s", peer);
	if (do_write(1, cmd, l) < 0) { fprintf(stderr, "connect failed\n"); do_clunk(1); do_clunk(0); return 1; }
	char path[64]; snprintf(path, sizeof(path), "net/aether/%d/data", conv);
	if (do_walk(0, 2, path) || do_open(2, ORDWR)) { fprintf(stderr, "data open failed\n"); do_clunk(1); do_clunk(0); return 1; }
	for (int k = 0; k < count && !g_stop; k++) {
		for (int i = 0; i < nbytes; i++) payload[i] = 'A' + (i % 26);
		int hl = snprintf((char *)payload, nbytes, "b%03d-", k);
		if (hl > 0 && hl < nbytes) payload[hl] = 'A';   /* undo snprintf's NUL */
		if (do_write(2, payload, nbytes) < 0) fprintf(stderr, "[sendbig] write %d failed\n", k);
		else printf("[SENTBIG] %d : %d bytes\n", k, nbytes);
		if (gap_ms > 0) usleep((useconds_t)gap_ms * 1000);
	}
	do_clunk(2); do_clunk(1); do_clunk(0);
	printf("[sendbig] done (%d x %d bytes)\n", count, nbytes);
	return 0;
}

/* Connected-mode receiver: clone, connect to a specific peer, then blocking-read
 * one datagram -- the connected path returns the bare payload (NO src prefix,
 * and only datagrams from the bound peer are delivered). */
static int do_crecv(const char *peer)
{
	uint8_t rb[600];
	if (do_version() || do_attach(0)) { fprintf(stderr, "setup failed\n"); return 1; }
	if (do_walk(0, 1, "net/aether/clone") || do_open(1, ORDWR)) { fprintf(stderr, "clone failed\n"); return 1; }
	int n = do_read(1, 0, rb, sizeof(rb) - 1); rb[n < 0 ? 0 : n] = 0; int conv = atoi((char *)rb);
	char cmd[64]; int l = snprintf(cmd, sizeof(cmd), "connect %s", peer);
	if (do_write(1, cmd, l) < 0) { fprintf(stderr, "connect failed\n"); do_clunk(1); do_clunk(0); return 1; }
	char path[64]; snprintf(path, sizeof(path), "net/aether/%d/data", conv);
	if (do_walk(0, 2, path) || do_open(2, ORDWR)) { fprintf(stderr, "data open failed\n"); do_clunk(1); do_clunk(0); return 1; }
	printf("[crecv] conv %d connected to %s; blocking read (no src prefix expected)...\n", conv, peer);
	n = do_read(2, 0, rb, sizeof(rb) - 1);
	if (n < 0) { if (!g_stop) fprintf(stderr, "[crecv] read failed: %d\n", n);
		do_clunk(2); do_clunk(1); do_clunk(0); return 1; }
	printf("[crecv] raw %d bytes:", n);
	for (int i = 0; i < n; i++) printf(" %02x", rb[i]);
	printf("\n[CRECV] %d bytes (connected, no prefix) : %.*s\n", n, n, (char *)rb);
	do_clunk(2); do_clunk(1); do_clunk(0);
	return 0;
}

/* Isolation: hold THREE conversations in ONE session -- c1 connected to <real>,
 * c2 connected to <fake>, c3 announced -- and fire a concurrent blocking read on
 * each data fid (distinct tags). A datagram from <real> must reach c1 (connected
 * peer match) and c3 (announced), but NOT c2 (connected to a different peer).
 * Prints [ISO cN] per delivery so the harness can assert c1+c3 received and c2
 * did not. Held until killed (c2's read never completes). Proves peer-filtering
 * + announced-receives-all + per-conversation isolation under concurrent traffic. */
static int do_iso_recv(const char *real, const char *fake)
{
	uint8_t rb[700];
	struct { uint32_t ctl, data; const char *mode, *peer; } cv[3] = {
		{ 1, 11, "connect", real }, { 2, 12, "connect", fake }, { 3, 13, "announce", NULL },
	};
	if (do_version() || do_attach(0)) { fprintf(stderr, "setup failed\n"); return 1; }
	for (int i = 0; i < 3; i++) {
		char cmd[64], path[64];
		if (do_walk(0, cv[i].ctl, "net/aether/clone") || do_open(cv[i].ctl, ORDWR)) {
			fprintf(stderr, "clone c%d failed\n", i + 1); return 1;
		}
		int n = do_read(cv[i].ctl, 0, rb, sizeof(rb) - 1); rb[n < 0 ? 0 : n] = 0;
		int conv = atoi((char *)rb);
		int l = cv[i].peer ? snprintf(cmd, sizeof(cmd), "%s %s", cv[i].mode, cv[i].peer)
				   : snprintf(cmd, sizeof(cmd), "%s", cv[i].mode);
		if (do_write(cv[i].ctl, cmd, l) < 0) { fprintf(stderr, "ctl c%d failed\n", i + 1); return 1; }
		snprintf(path, sizeof(path), "net/aether/%d/data", conv);
		if (do_walk(0, cv[i].data, path) || do_open(cv[i].data, ORDWR)) {
			fprintf(stderr, "data c%d failed\n", i + 1); return 1;
		}
	}
	for (int i = 0; i < 3; i++) send_only(b_read(cv[i].data, cv[i].data, 512));
	printf("[iso] armed: c1=connect(%s) c2=connect(%s) c3=announce; waiting...\n", real, fake);
	for (;;) {
		uint16_t tag; int ty = recv_one(rb, &tag);
		if (ty < 0) { fprintf(stderr, "[iso] recv error\n"); return 1; }
		if (ty != Rread) continue;
		uint32_t n = g32(rb + 3);                       /* Rread: type[1] tag[2] count[4] data */
		int c = (tag == 11) ? 1 : (tag == 12) ? 2 : (tag == 13) ? 3 : 0;
		printf("[ISO c%d] %u bytes : %.*s\n", c, n, (int)n, (char *)rb + 7);
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <unix-sock> [peer_addr | --concurrent | --hold | --recv | --put <path> <file> [chunk]]\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: see progress even if a recv blocks */
	/* Interrupt (don't SA_RESTART) a blocked read on SIGINT/SIGTERM so receivers
	 * clunk their conversation on kill instead of leaking it. */
	struct sigaction sa = { .sa_handler = on_term, .sa_flags = 0 };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	const char *sock = argv[1];
	const char *arg2 = argc > 2 ? argv[2] : NULL;
	int concurrent = arg2 && strcmp(arg2, "--concurrent") == 0;
	int hold = arg2 && strcmp(arg2, "--hold") == 0;
	int recv = arg2 && strcmp(arg2, "--recv") == 0;
	int crecv = arg2 && strcmp(arg2, "--crecv") == 0;
	int iso = arg2 && strcmp(arg2, "--iso") == 0;
	int sendn = arg2 && strcmp(arg2, "--sendn") == 0;
	int sendbig = arg2 && strcmp(arg2, "--sendbig") == 0;
	int put = arg2 && strcmp(arg2, "--put") == 0;
	const char *peer = (concurrent || put || hold || recv || crecv || iso || sendn || sendbig) ? NULL : arg2;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strncpy(a.sun_path, sock, sizeof(a.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("connect"); return 1; }

	if (put) {
		if (argc < 5) { fprintf(stderr, "usage: %s <sock> --put <path> <file> [chunk]\n", argv[0]); return 2; }
		return do_put(argv[3], argv[4], argc > 5 ? atoi(argv[5]) : 1024);
	}
	if (hold) {
		return do_hold();
	}
	if (recv) {
		return do_recv(argc > 3 ? atoi(argv[3]) : 1);   /* --recv [count] */
	}
	if (crecv) {
		if (argc < 4) { fprintf(stderr, "usage: %s <sock> --crecv <peer_addr>\n", argv[0]); return 2; }
		return do_crecv(argv[3]);
	}
	if (iso) {
		if (argc < 5) { fprintf(stderr, "usage: %s <sock> --iso <real_peer> <fake_peer>\n", argv[0]); return 2; }
		return do_iso_recv(argv[3], argv[4]);
	}
	if (sendn) {
		if (argc < 5) { fprintf(stderr, "usage: %s <sock> --sendn <peer> <count> [gap_ms]\n", argv[0]); return 2; }
		return do_sendn(argv[3], atoi(argv[4]), argc > 5 ? atoi(argv[5]) : 1500);
	}
	if (sendbig) {
		if (argc < 5) { fprintf(stderr, "usage: %s <sock> --sendbig <peer> <nbytes> [count] [gap_ms]\n", argv[0]); return 2; }
		return do_sendbig(argv[3], atoi(argv[4]), argc > 5 ? atoi(argv[5]) : 1, argc > 6 ? atoi(argv[6]) : 1500);
	}
	if (concurrent) {
		return demo_concurrent();
	}

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
		const char *msg = argc > 3 ? argv[3] : "hello from aether_conv";
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
