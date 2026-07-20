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
#include <sys/time.h>
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
/* §6a status surface: clone, connect ff:ff:ff:ff:ff:ff (CONV_BCAST), read the
 * conversation's `status` -- spec §6a requires it read "broadcast best-effort".
 * Distinct from "connected ..."/"announced"/"unconnected" so a client can tell
 * it has joined the party line. */
static int do_bstatus(void)
{
	uint8_t rb[256];
	if (do_version() || do_attach(0)) { fprintf(stderr, "setup failed\n"); return 1; }
	if (do_walk(0, 1, "net/aether/clone") || do_open(1, ORDWR)) { fprintf(stderr, "clone failed\n"); return 1; }
	int n = do_read(1, 0, rb, sizeof(rb) - 1); rb[n < 0 ? 0 : n] = 0; int conv = atoi((char *)rb);
	if (do_write(1, "connect ff:ff:ff:ff:ff:ff", 25) < 0) { fprintf(stderr, "connect bcast failed\n"); do_clunk(1); do_clunk(0); return 1; }
	char path[64]; snprintf(path, sizeof(path), "net/aether/%d/status", conv);
	if (do_walk(0, 2, path) || do_open(2, OREAD)) { fprintf(stderr, "status open failed\n"); do_clunk(1); do_clunk(0); return 1; }
	n = do_read(2, 0, rb, sizeof(rb) - 1); if (n < 0) n = 0; rb[n] = 0;
	/* strip trailing newline for a clean single-line print */
	while (n > 0 && (rb[n - 1] == '\n' || rb[n - 1] == '\r')) rb[--n] = 0;
	printf("[bstatus] conv %d status: %s\n", conv, (char *)rb);
	do_clunk(2); do_clunk(1); do_clunk(0);
	return 0;
}

/* §6a broadcast receiver: clone, connect ff:ff:ff:ff:ff:ff (CONV_BCAST), then
 * hold one session and read up to `count` source-prefixed broadcast datagrams.
 * Mirrors do_recv but joins the party line instead of announcing -- this is the
 * exact mode the deck chat client uses, so it isolates the §6a *receive* path. */
static int do_brecv(int count)
{
	uint8_t rb[600];
	if (do_version() || do_attach(0)) { fprintf(stderr, "setup failed\n"); return 1; }
	if (do_walk(0, 1, "net/aether/clone") || do_open(1, ORDWR)) { fprintf(stderr, "clone failed\n"); return 1; }
	int n = do_read(1, 0, rb, sizeof(rb) - 1); rb[n < 0 ? 0 : n] = 0; int conv = atoi((char *)rb);
	if (do_write(1, "connect ff:ff:ff:ff:ff:ff", 25) < 0) { fprintf(stderr, "connect bcast failed\n"); do_clunk(1); do_clunk(0); return 1; }
	char path[64]; snprintf(path, sizeof(path), "net/aether/%d/data", conv);
	if (do_walk(0, 2, path) || do_open(2, ORDWR)) { fprintf(stderr, "data open failed\n"); do_clunk(1); do_clunk(0); return 1; }
	printf("[brecv] conv %d joined party line (connect ff:ff); reading up to %d broadcast(s)...\n", conv, count);
	for (int k = 0; k < count && !g_stop; k++) {
		n = do_read(2, 0, rb, sizeof(rb) - 1);
		if (n < 0) { if (!g_stop) fprintf(stderr, "[brecv] read failed: %d\n", n); break; }
		if (n == 0) { printf("[brecv] EOF (hangup)\n"); break; }
		if (n >= 6) {
			printf("[BRECV] %d bytes from %02x:%02x:%02x:%02x:%02x:%02x : %.*s\n",
			       n - 6, rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], n - 6, (char *)rb + 6);
		} else {
			printf("[brecv] %d bytes (no src prefix): %.*s\n", n, n, (char *)rb);
		}
	}
	do_clunk(2); do_clunk(1); do_clunk(0);
	printf("[brecv] done\n");
	return 0;
}

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
	/* The K_DATA 9P write blocks in aether_mesh_send_reliable until the datagram
	 * is ACKed (or fails after retries), so with gap_ms=0 the loop is naturally
	 * stop-and-wait paced and its wall time IS the delivery time -> throughput. */
	struct timeval t0, t1;
	int ok = 0;
	gettimeofday(&t0, NULL);
	for (int k = 0; k < count && !g_stop; k++) {
		for (int i = 0; i < nbytes; i++) payload[i] = 'A' + (i % 26);
		int hl = snprintf((char *)payload, nbytes, "b%03d-", k);
		if (hl > 0 && hl < nbytes) payload[hl] = 'A';   /* undo snprintf's NUL */
		if (do_write(2, payload, nbytes) < 0) fprintf(stderr, "[sendbig] write %d failed\n", k);
		else { ok++; printf("[SENTBIG] %d : %d bytes\n", k, nbytes); }
		if (gap_ms > 0) usleep((useconds_t)gap_ms * 1000);
	}
	gettimeofday(&t1, NULL);
	do_clunk(2); do_clunk(1); do_clunk(0);
	double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_usec - t0.tv_usec) / 1000.0;
	double bps = ms > 0 ? ok * (double)nbytes * 1000.0 / ms : 0;
	printf("[sendbig] %d/%d x %d B in %.0f ms = %.0f B/s (%.1f kbit/s), %.1f ms/datagram\n",
	       ok, count, nbytes, ms, bps, bps * 8 / 1000.0, ok ? ms / ok : 0);
	return 0;
}

/* Parse "aa:bb:cc:dd:ee:ff" -> 6 bytes. */
static int parse6(const char *s, uint8_t out[6])
{
	unsigned int b[6];
	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return -1;
	for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
	return 0;
}

/* Announced-mode sender: clone, ANNOUNCE, then write [dst(6)][payload] -- exercises
 * the announced-write (destination-prefixed) reply path (spec §4/§8), i.e. a 9P
 * server replying to a requester. */
static int do_asend(const char *dstaddr, const char *msg)
{
	uint8_t rb[64];
	static uint8_t out[6 + 512];
	if (parse6(dstaddr, out) < 0) { fprintf(stderr, "bad dst addr\n"); return 1; }
	if (do_version() || do_attach(0)) { fprintf(stderr, "setup failed\n"); return 1; }
	if (do_walk(0, 1, "net/aether/clone") || do_open(1, ORDWR)) { fprintf(stderr, "clone failed\n"); return 1; }
	int n = do_read(1, 0, rb, sizeof(rb) - 1); rb[n < 0 ? 0 : n] = 0; int conv = atoi((char *)rb);
	if (do_write(1, "announce", 8) < 0) { fprintf(stderr, "announce failed\n"); do_clunk(1); do_clunk(0); return 1; }
	char path[64]; snprintf(path, sizeof(path), "net/aether/%d/data", conv);
	if (do_walk(0, 2, path) || do_open(2, ORDWR)) { fprintf(stderr, "data open failed\n"); do_clunk(1); do_clunk(0); return 1; }
	int ml = (int)strlen(msg); if (ml > 512) ml = 512;
	memcpy(out + 6, msg, ml);
	if (do_write(2, out, 6 + ml) < 0) fprintf(stderr, "[asend] write failed\n");
	else printf("[ASEND] announced-write to %s, %d-byte payload: %s\n", dstaddr, ml, msg);
	do_clunk(2); do_clunk(1); do_clunk(0);
	return 0;
}

/* one datagram -- the connected path returns the bare payload (NO src prefix,
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

/* ---- 9P-over-mesh bridge -------------------------------------------------
 * --bridge <peer> <unix-path>: serve a byte-stream 9P endpoint on a unix
 * socket, relaying each framed 9P message as ONE reliable mesh datagram
 * through THIS node's conversation layer to <peer>'s mesh 9P server -- and
 * each reply datagram back as the R-message. Strictly request-response (9P is
 * client-driven), so no polling is needed: read a T from the client, write it
 * to the conversation, block on the conversation read for the R, hand it back.
 * Point plan9port at it:  9p -a 'unix!<path>' ls /   -- and you are listing a
 * far node's filesystem across the mesh, through relays.
 */
static int readn_fd(int cfd, uint8_t *buf, int n)
{
	int got = 0;

	while (got < n) {
		int r = (int)read(cfd, buf + got, n - got);
		if (r <= 0) return -1;
		got += r;
	}
	return n;
}

static int do_bridge(const char *peer, const char *lpath)
{
	uint8_t rb[600], msg[600];

	if (do_version() || do_attach(0)) { fprintf(stderr, "setup failed\n"); return 1; }
	if (do_walk(0, 1, "net/aether/clone") || do_open(1, ORDWR)) { fprintf(stderr, "clone failed\n"); return 1; }
	int n = do_read(1, 0, rb, sizeof(rb) - 1);
	if (n < 0) { fprintf(stderr, "clone read failed\n"); return 1; }
	rb[n] = 0;
	int conv = atoi((char *)rb);
	char path[64]; snprintf(path, sizeof(path), "net/aether/%d/data", conv);
	if (do_walk(0, 2, path) || do_open(2, ORDWR)) { fprintf(stderr, "data open failed\n"); return 1; }
	char cmd[64]; int l = snprintf(cmd, sizeof(cmd), "connect %s", peer);
	if (do_write(1, cmd, l) < 0) { fprintf(stderr, "ctl connect failed\n"); return 1; }
	fprintf(stderr, "[bridge] conv %d connected to %s; 9P endpoint on %s\n", conv, peer, lpath);

	unlink(lpath);
	int ls = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un la = { .sun_family = AF_UNIX };
	strncpy(la.sun_path, lpath, sizeof(la.sun_path) - 1);
	if (bind(ls, (struct sockaddr *)&la, sizeof(la)) < 0 || listen(ls, 4) < 0) { perror("listen"); return 1; }

	/* Serve clients sequentially, keeping the ONE mesh conversation open the
	 * whole time -- plan9port's 9p opens a fresh connection per command. */
	for (;;) {
		int cfd = accept(ls, NULL, NULL);
		if (cfd < 0) break;
		fprintf(stderr, "[bridge] client attached; relaying (one reliable datagram per 9P message)\n");
		for (;;) {
			uint8_t hdr[4];
			if (readn_fd(cfd, hdr, 4) < 0) break;
			uint32_t sz = (uint32_t)hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) |
				      ((uint32_t)hdr[3] << 24);
			if (sz < 7 || sz > sizeof(msg)) { fprintf(stderr, "[bridge] bad msg size %u\n", sz); break; }
			memcpy(msg, hdr, 4);
			if (readn_fd(cfd, msg + 4, (int)sz - 4) < 0) break;
			uint16_t want = (uint16_t)(msg[5] | (msg[6] << 8));  /* this T's 9P tag */
			/* Retry a transient mesh drop of THIS request, then TAG-MATCH the
			 * reply: a resend can produce a duplicate/late R, and the datagram
			 * conversation is untagged at the mesh layer, so read replies until
			 * one bears this T's 9P tag -- discard stale ones. Without this the
			 * reply stream desyncs (a Tread getting a leftover Ropen). */
			int rn = -1;
			for (int attempt = 0; attempt < 4 && rn <= 0; attempt++) {
				if (do_write(2, msg, (int)sz) < 0) { continue; }
				for (int reads = 0; reads < 6; reads++) {
					int r = do_read(2, 0, rb, sizeof(rb));
					if (r <= 0) break;                  /* no reply this attempt; resend */
					if (r >= 7 && (uint16_t)(rb[5] | (rb[6] << 8)) == want) { rn = r; break; }
					fprintf(stderr, "[bridge] discarding stale R tag=%u (want %u)\n",
						(unsigned)(rb[5] | (rb[6] << 8)), want);
				}
			}
			if (rn <= 0) { fprintf(stderr, "[bridge] mesh round-trip failed after retries (T type=%u tag=%u)\n", msg[4], want); break; }
			if (write(cfd, rb, rn) != rn) break;
			fprintf(stderr, "[bridge] T%u (%u B) -> R%u (%d B) across the mesh\n",
				msg[4], sz, rn >= 5 ? rb[4] : 0, rn);
		}
		close(cfd);
		fprintf(stderr, "[bridge] client detached (mesh conversation stays open)\n");
	}
	close(ls);
	do_clunk(2); do_clunk(1); do_clunk(0);
	return 0;
}

/* ---- raw-channel reliability probe ----------------------------------------
 * Runs STRICT request-response 9P over ONE held /net/aether conversation to
 * <peer>'s mesh 9P server: send each T ONCE (no resend), read the FIRST reply
 * datagram (bounded poll, NO tag-match), and classify. Separates the two
 * failure modes do_bridge's shim defends against:
 *   loss     = no reply arrived without resending the T   -> would need RETRY
 *   mismatch = a reply arrived but wrong tag/type          -> would need TAG-MATCH
 * A dumb transport_nsfile behaves exactly like this (first reply == the answer),
 * so a high clean% means the conversation channel is mount-clean as a raw pipe;
 * frequent anomalies mean the deck transport needs the shim.
 */
static int inner_send_recv(const uint8_t *t, int tlen, uint8_t *rb, int cap,
			   int *rn, int poll_budget)
{
	*rn = 0;
	if (do_write(2, t, tlen) < 0) return -1;         /* local/outer error */
	for (int i = 0; i < poll_budget; i++) {
		int n = do_read(2, 0, rb, cap);
		if (n > 0) { *rn = n; return 1; }            /* got a datagram */
		usleep(30000);
	}
	return 0;                                         /* no reply (loss) */
}

static int do_probe(const char *peer, int count)
{
	uint8_t rb[600], t[600];
	int rn, o;

	/* outer: open a conversation to the peer (same setup as do_bridge) */
	if (do_version() || do_attach(0)) { fprintf(stderr, "outer setup failed\n"); return 1; }
	if (do_walk(0, 1, "net/aether/clone") || do_open(1, ORDWR)) { fprintf(stderr, "clone failed\n"); return 1; }
	int n = do_read(1, 0, rb, sizeof(rb) - 1);
	if (n < 0) { fprintf(stderr, "clone read failed\n"); return 1; }
	rb[n] = 0; int conv = atoi((char *)rb);
	char path[64]; snprintf(path, sizeof(path), "net/aether/%d/data", conv);
	if (do_walk(0, 2, path) || do_open(2, ORDWR)) { fprintf(stderr, "data open failed\n"); return 1; }
	char cmd[64]; int l = snprintf(cmd, sizeof(cmd), "connect %s", peer);
	if (do_write(1, cmd, l) < 0) { fprintf(stderr, "connect failed\n"); return 1; }
	fprintf(stderr, "[probe] conv %d connected to %s\n", conv, peer);
	for (int i = 0; i < 8; i++) { if (do_read(2, 0, rb, sizeof(rb)) <= 0) break; }  /* drain stale */

	/* inner 9P session to the PEER's mesh server, tunneled through the data fid */
	o = 4; t[o++] = Tversion; p16(t + o, NOTAG); o += 2; p32(t + o, 480); o += 4;
	p16(t + o, 6); o += 2; memcpy(t + o, "9P2000", 6); o += 6; p32(t, o);
	if (inner_send_recv(t, o, rb, sizeof(rb), &rn, 40) != 1 || rb[4] != Rversion) {
		fprintf(stderr, "[probe] inner Tversion failed (rn=%d)\n", rn); return 1; }
	o = 4; t[o++] = Tattach; p16(t + o, 1); o += 2; p32(t + o, 0); o += 4; p32(t + o, NOFID); o += 4;
	p16(t + o, 1); o += 2; t[o++] = 'p'; p16(t + o, 0); o += 2; p32(t, o);
	if (inner_send_recv(t, o, rb, sizeof(rb), &rn, 40) != 1 || rb[4] != Rattach) {
		fprintf(stderr, "[probe] inner Tattach failed\n"); return 1; }
	o = 4; t[o++] = Twalk; p16(t + o, 2); o += 2; p32(t + o, 0); o += 4; p32(t + o, 1); o += 4;
	{ const char *w[] = { "dev", "aether", "addr" }; p16(t + o, 3); o += 2;
	  for (int i = 0; i < 3; i++) { int wl = strlen(w[i]); p16(t + o, wl); o += 2; memcpy(t + o, w[i], wl); o += wl; } }
	p32(t, o);
	if (inner_send_recv(t, o, rb, sizeof(rb), &rn, 40) != 1 || rb[4] != Rwalk) {
		fprintf(stderr, "[probe] inner Twalk failed (type=%d)\n", rn > 4 ? rb[4] : -1); return 1; }
	o = 4; t[o++] = Topen; p16(t + o, 3); o += 2; p32(t + o, 1); o += 4; t[o++] = OREAD; p32(t, o);
	if (inner_send_recv(t, o, rb, sizeof(rb), &rn, 40) != 1 || rb[4] != Ropen) {
		fprintf(stderr, "[probe] inner Topen failed\n"); return 1; }
	fprintf(stderr, "[probe] inner session open; strict Tread x%d (one send, first reply, no tag-match)\n", count);

	int clean = 0, loss = 0, mism = 0;
	for (int i = 0; i < count; i++) {
		uint16_t tag = (uint16_t)(1000 + i);
		o = 4; t[o++] = Tread; p16(t + o, tag); o += 2; p32(t + o, 1); o += 4;
		p32(t + o, 0); o += 4; p32(t + o, 0); o += 4; p32(t + o, 200); o += 4; p32(t, o);
		int r = inner_send_recv(t, o, rb, sizeof(rb), &rn, 60);
		if (r != 1) { loss++; continue; }
		uint16_t rtag = g16(rb + 5);
		if (rb[4] == Rread && rtag == tag) { clean++; }
		else { mism++; if (mism <= 10) fprintf(stderr, "[probe] mismatch #%d: type=%d tag=%u (want %u)\n", mism, rb[4], rtag, tag); }
	}
	int tot = clean + loss + mism; if (tot == 0) tot = 1;
	fprintf(stderr, "\n===== RAW-CHANNEL PROBE (%s, %d Treads, strict single-send, no tag-match) =====\n", peer, count);
	fprintf(stderr, "  clean    : %d (%.1f%%)\n", clean, 100.0 * clean / tot);
	fprintf(stderr, "  loss     : %d (%.1f%%)   <- would need RETRY\n", loss, 100.0 * loss / tot);
	fprintf(stderr, "  mismatch : %d (%.1f%%)   <- would need TAG-MATCH\n", mism, 100.0 * mism / tot);
	fprintf(stderr, "  verdict  : %s\n", (loss == 0 && mism == 0)
		? "RAW-CLEAN -> transport_nsfile can be a dumb pipe"
		: "NEEDS SHIM -> deck transport needs retry/tag-match");
	do_clunk(2); do_clunk(1); do_clunk(0);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <unix-sock> [peer_addr | --concurrent | --hold | --recv | --put <path> <file> [chunk] | --bridge <peer> <listen-path> | --probe <peer> [count]]\n", argv[0]);
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
	int brecv = arg2 && strcmp(arg2, "--brecv") == 0;
	int bstatus = arg2 && strcmp(arg2, "--bstatus") == 0;
	int crecv = arg2 && strcmp(arg2, "--crecv") == 0;
	int iso = arg2 && strcmp(arg2, "--iso") == 0;
	int sendn = arg2 && strcmp(arg2, "--sendn") == 0;
	int sendbig = arg2 && strcmp(arg2, "--sendbig") == 0;
	int asend = arg2 && strcmp(arg2, "--asend") == 0;
	int put = arg2 && strcmp(arg2, "--put") == 0;
	int bridge = arg2 && strcmp(arg2, "--bridge") == 0;
	int probe = arg2 && strcmp(arg2, "--probe") == 0;
	const char *peer = (concurrent || put || hold || recv || brecv || bstatus || crecv || iso || sendn || sendbig || asend || bridge || probe) ? NULL : arg2;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strncpy(a.sun_path, sock, sizeof(a.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("connect"); return 1; }

	if (put) {
		if (argc < 5) { fprintf(stderr, "usage: %s <sock> --put <path> <file> [chunk]\n", argv[0]); return 2; }
		return do_put(argv[3], argv[4], argc > 5 ? atoi(argv[5]) : 1024);
	}
	if (bridge) {
		if (argc < 5) { fprintf(stderr, "usage: %s <sock> --bridge <peer_addr> <unix_listen_path>\n", argv[0]); return 2; }
		return do_bridge(argv[3], argv[4]);
	}
	if (probe) {
		if (argc < 4) { fprintf(stderr, "usage: %s <sock> --probe <peer> [count]\n", argv[0]); return 2; }
		return do_probe(argv[3], argc > 4 ? atoi(argv[4]) : 100);
	}
	if (hold) {
		return do_hold();
	}
	if (recv) {
		return do_recv(argc > 3 ? atoi(argv[3]) : 1);   /* --recv [count] */
	}
	if (brecv) {
		return do_brecv(argc > 3 ? atoi(argv[3]) : 1);  /* --brecv [count] */
	}
	if (bstatus) {
		return do_bstatus();                            /* --bstatus */
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
	if (asend) {
		if (argc < 5) { fprintf(stderr, "usage: %s <sock> --asend <dst_addr> <payload>\n", argv[0]); return 2; }
		return do_asend(argv[3], argv[4]);
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
