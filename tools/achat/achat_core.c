/*
 * achat_core.c -- headless native Aether mesh chat, step 1: open a serial/USB-CDC
 * port, speak 9P over it DIRECTLY via plan9port lib9pclient (no socat, no mount,
 * no FUSE), hold a /net/aether broadcast conversation, and pump stdin<->data.
 *
 * This is the transport/core that a libdraw GUI will later sit on top of. It's a
 * threadmain program because lib9pclient's mux uses ioprocs (libthread).
 *
 *   achat-core /dev/cu.usbmodem1203 [dst-addr]
 *     default dst = ff:ff:ff:ff:ff:ff  (the broadcast party line)
 *
 * Concurrency: a reader thread blocks in fsread(data) for incoming datagrams;
 * threadmain reads stdin via an ioproc and fswrite()s each line to data. Both
 * RPCs ride the one 9P connection concurrently through lib9pclient's tag mux.
 */
#include <u.h>
#include <libc.h>
#include <thread.h>
#include <9pclient.h>

extern int serial_open(char*);

static CFsys *fs;
static CFid  *ctl;    /* net/aether/clone == the ctl fid; HELD for the session */
static CFid  *dataR;  /* net/aether/<conv>/data -- reader fid */
static CFid  *dataW;  /* net/aether/<conv>/data -- writer fid (SEPARATE fid so a
                       * blocked fsread and an fswrite don't serialize on one fid) */
static int    g_bcast; /* broadcast conversation -> reads carry a 6-byte src prefix */
static char   g_dst[24];      /* current target addr, for /connect re-targeting */
static char   myaddrbuf[24];  /* this node's own HONR addr, as a connect string */
static int    switching;      /* /connect in flight: reader re-targets ctl on EOF */
static QLock  convlk;         /* guards g_dst/switching + ctl writes across procs */

/* Format+print one received datagram. §6a broadcast (and announced) reads carry
 * a 6-byte raw source address prefix (see aether_conv.c do_brecv); connected
 * reads are bare payload. */
static void
show(char *buf, long n)
{
	uchar *s = (uchar*)buf;
	if(g_bcast && n >= 6)
		print("[%02x:%02x:%02x:%02x:%02x:%02x] %.*s\n",
			s[0], s[1], s[2], s[3], s[4], s[5], (int)(n-6), buf+6);
	else
		print("%.*s\n", (int)n, buf);
}

/* This node's own durable identity (node_eui / CGA), verbatim from
 * dev/aether/addr -- the stable "aa:bb:cc:dd:ee:ff" a peer types into /connect.
 * The app addresses purely by this durable id; the mesh resolves it to the
 * (churning) HONR routing address internally. */
static void
myaddr(char *out, int outsz)
{
	CFid *f;
	long n;
	int i;

	strecpy(out, out + outsz, "?");
	if((f = fsopen(fs, "net/aether/addr", OREAD)) == nil)
		return;
	n = fsread(f, out, outsz - 1);
	fsclose(f);
	if(n <= 0){ strecpy(out, out + outsz, "?"); return; }
	out[n] = 0;
	for(i = (int)n - 1; i >= 0 && (out[i] == '\n' || out[i] == '\r' || out[i] == ' '); i--)
		out[i] = 0;   /* trim trailing newline/space */
}

/* reader: one datagram per fsread. */
static void
reader(void *a)
{
	char buf[1024], cmd[64];
	long n;
	int w, bc;

	USED(a);
	for(;;){
		n = fsread(dataR, buf, sizeof buf);
		if(n < 0){
			fprint(2, "\n[read error: %r]\n");
			break;
		}
		if(n == 0){
			/* len-0 EOF sentinel. If a /connect is in flight, this is the
			 * "hangup" we asked for -- re-target the same conversation (still
			 * UNCONNECTED after the hangup) and keep reading the same fid, no
			 * fid churn. Otherwise it's a stray/late EOF (e.g. a rapid double
			 * /connect): re-read. Shutdown goes via ^D -> threadexitsall. */
			qlock(&convlk);
			if(!switching){
				qunlock(&convlk);
				continue;
			}
			snprint(cmd, sizeof cmd, "connect %s", g_dst);
			w = fswrite(ctl, cmd, strlen(cmd));
			bc = g_bcast;
			switching = 0;
			qunlock(&convlk);
			if(w < 0){ print("[connect failed -- try /connect again]\n"); continue; }
			if(bc)
				print("[on the party line (broadcast, best-effort)]\n");
			else
				print("[connected (reliable) to %s -- peer must /connect you too]\n", g_dst);
			continue;
		}
		show(buf, n);
	}
	threadexitsall(nil);
}

/* slash-commands typed on stdin. */
static void
docmd(char *s)
{
	if(strcmp(s, "/quit") == 0){
		print("[achat] hangup\n");
		fsclose(ctl);
		threadexitsall(nil);
	}else if(strcmp(s, "/addr") == 0){
		print("[achat] my address: %s   (peer: /connect %s)\n", myaddrbuf, myaddrbuf);
	}else if(strcmp(s, "/who") == 0){
		CFid *f = fsopen(fs, "dev/aether/neighbors", OREAD);
		char buf[2048];
		long n;
		if(f == nil){ print("[who] unavailable\n"); return; }
		n = fsread(f, buf, sizeof buf - 1);
		fsclose(f);
		if(n <= 0){ print("[who] no neighbours\n"); return; }
		buf[n] = 0;
		print("[who] neighbours:\n%s\n", buf);
	}else if(strncmp(s, "/connect", 8) == 0 && (s[8] == ' ' || s[8] == 0)){
		char *p = s + 8, newdst[24];
		while(*p == ' ')
			p++;
		if(*p == 0){
			print("[achat] usage: /connect <addr>   (or /connect bcast for the party line)\n");
			return;
		}
		if(strcmp(p, "bcast") == 0 || strcmp(p, "broadcast") == 0)
			strcpy(newdst, "ff:ff:ff:ff:ff:ff");
		else if(strlen(p) <= 5)
			snprint(newdst, sizeof newdst, "00:00:00:00:%s", p);
		else
			snprint(newdst, sizeof newdst, "%s", p);
		/* Re-target the same conversation in place via hangup+connect (see the
		 * reader's EOF handling): resets to UNCONNECTED and wakes the reader,
		 * which then issues the connect -- no fid churn, and prompt. */
		qlock(&convlk);
		strncpy(g_dst, newdst, sizeof g_dst - 1);
		g_dst[sizeof g_dst - 1] = 0;
		g_bcast = (strcmp(newdst, "ff:ff:ff:ff:ff:ff") == 0);
		switching = 1;
		if(fswrite(ctl, "hangup", 6) < 0){
			switching = 0;
			qunlock(&convlk);
			print("[achat] /connect: hangup failed -- try again\n");
			return;
		}
		qunlock(&convlk);
		print("[achat] switching to %s ...\n", newdst);
	}else if(strcmp(s, "/help") == 0){
		print("[achat] /addr | /who | /connect <addr>|bcast | /quit  (or ^D)\n");
	}else{
		print("[achat] unknown command: %s (try /help)\n", s);
	}
}

void
threadmain(int argc, char **argv)
{
	char *port = "/dev/cu.usbmodem1203";
	char *dst  = "ff:ff:ff:ff:ff:ff";
	char nb[32], cmd[64], path[64], line[1024];
	int fd, conv, rxonly = 0, ai = 1;
	long n;
	Ioproc *io;

	/* -r: single-threaded receive-only (isolates the RX path from the
	 * concurrent-send mux question). */
	if(argc > ai && strcmp(argv[ai], "-r") == 0){ rxonly = 1; ai++; }
	if(argc > ai){ port = argv[ai]; ai++; }
	if(argc > ai){ dst  = argv[ai]; ai++; }

	fd = serial_open(port);
	if(fd < 0)
		sysfatal("open %s: %r", port);
	sleep(400);   /* let the DTR-gated relay session come up before Tversion */

	fs = fsmount(fd, nil);
	if(fs == nil)
		sysfatal("fsmount %s: %r", port);

	/* clone -> conv number; HOLD ctl (clunking it frees the conversation). */
	ctl = fsopen(fs, "net/aether/clone", ORDWR);
	if(ctl == nil)
		sysfatal("open net/aether/clone: %r");
	n = fsread(ctl, nb, sizeof nb - 1);
	if(n <= 0)
		sysfatal("read conv#: %r");
	nb[n] = 0;
	conv = atoi(nb);

	/* join the party line (or dial a peer): write to the ctl fid. */
	g_bcast = (strcmp(dst, "ff:ff:ff:ff:ff:ff") == 0);
	strncpy(g_dst, dst, sizeof g_dst - 1);   /* for /connect re-targeting */
	g_dst[sizeof g_dst - 1] = 0;
	snprint(cmd, sizeof cmd, "connect %s", dst);
	if(fswrite(ctl, cmd, strlen(cmd)) < 0)
		sysfatal("connect %s: %r", dst);

	snprint(path, sizeof path, "net/aether/%d/data", conv);
	dataR = fsopen(fs, path, ORDWR);
	if(dataR == nil)
		sysfatal("open %s (read): %r", path);

	print("[achat] conv %d -- %s -- to %s\n", conv,
		g_bcast ? "party line (broadcast, best-effort)" : "connected (reliable)", dst);

	if(rxonly){
		/* single-threaded receive loop, no writer, no second thread */
		for(;;){
			n = fsread(dataR, line, sizeof line);
			if(n < 0){ fprint(2, "[read error: %r]\n"); break; }
			if(n == 0) break;   /* hangup sentinel */
			show(line, n);
		}
		fsclose(ctl);   /* free the conversation upstream */
		threadexitsall(nil);
	}

	/* separate writer fid on the same data file, so the reader's blocked fsread
	 * and our fswrite don't serialize on a single CFid. */
	dataW = fsopen(fs, path, ORDWR);
	if(dataW == nil)
		sysfatal("open %s (write): %r", path);

	myaddr(myaddrbuf, sizeof myaddrbuf);
	print("[achat] my address: %s   (peer: /connect %s for reliable 1:1)\n", myaddrbuf, myaddrbuf);
	print("[achat] /help for commands, ^D to quit\n");

	/* reader in its OWN proc (real OS thread), not a cooperative thread: its
	 * blocking fsread must not be gated by the main proc's stdin wait, and the
	 * lib9pclient mux is proc-safe (QLock-guarded) so concurrent fsread/fswrite
	 * dispatch by tag across procs. */
	proccreate(reader, nil, 16384);

	/* stdin -> data, one datagram per line. ioread keeps the scheduler alive so
	 * the reader proc runs concurrently. */
	io = ioproc();
	for(;;){
		n = ioread(io, 0, line, sizeof line - 1);
		if(n <= 0)
			break;   /* EOF / error -> hang up */
		while(n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
			n--;
		if(n == 0)
			continue;
		line[n] = 0;
		if(line[0] == '/'){   /* slash-command, not a message */
			docmd(line);
			continue;
		}
		if(fswrite(dataW, line, n) < 0)
			fprint(2, "[send failed: %r]\n");
		else
			print("[me] %.*s\n", (int)n, line);   /* local echo (server suppresses own broadcast) */
	}

	/* ^D / EOF -> clean hangup: clunk the ctl fid so the 9151 frees the
	 * conversation (leaves NO stale conv state, which is what wedges the
	 * inter-chip link on abrupt kills). The freed conversation makes the reader
	 * proc's blocked fsread return the len-0 sentinel, so it unwinds and
	 * threadexitsall then terminates cleanly. */
	closeioproc(io);
	print("[achat] hangup\n");
	fsclose(ctl);
	threadexitsall(nil);
}
