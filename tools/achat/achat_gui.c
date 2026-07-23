/*
 * achat_gui.c -- a plan9port libdraw GUI for Aether mesh chat, step 2. Same
 * native transport as achat_core.c (serial -> lib9pclient fsmount -> held
 * /net/aether conversation, separate-fid reader proc), with a windowed UI:
 * a scrollback transcript + an input line.
 *
 *   achat-gui /dev/cu.usbmodem1203 [dst-addr]
 *
 * Runs devdraw (the plan9port window server) for its Cocoa window -- so it must
 * be launched from a GUI session, not a headless shell.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <mouse.h>
#include <keyboard.h>
#include <9pclient.h>

extern int serial_open(char*);
extern char *serial_autodetect(void);
extern void bundle_setup(void);   /* set DEVDRAW/PLAN9 from the .app if bundled */

/* ---- transport core (mirrors achat_core.c) ------------------------------ */
static CFsys *fs;
static CFid  *ctl;     /* clone == ctl fid, held for the conversation */
static CFid  *dataR;   /* reader fid */
static CFid  *dataW;   /* writer fid (separate so blocked fsread != fswrite) */
static int    g_bcast;

/* ---- UI state ----------------------------------------------------------- */
enum { MAXLINES = 400, LINEMAX = 1100, INPUTMAX = 512 };
static char *ring[MAXLINES];
static int   nlines;                 /* monotonic total; line k lives at ring[k%MAXLINES] */
static char  input[INPUTMAX];
static int   ninput;
static Rectangle transr, inputr;
static Image *back, *txt, *mecol;
static Channel *incoming;            /* char* formatted lines from the reader proc */
static Channel *quitc;               /* note handler -> clean shutdown */
static Channel *tickc;               /* watchdog heartbeat -> detect a dead devdraw */
static char nick[64];                /* /nick prefix on outgoing messages */
static char datapath[64];            /* net/aether/<conv>/data, for reader reconnect */
static char g_dst[24];               /* target addr, for re-connecting the conversation */
static char myaddrbuf[24];           /* this node's own HONR addr, as a connect string */
static int  conv;                    /* current conversation number */
static QLock convlk;                 /* guards the conversation fids: reconv vs send */
static int  switching;               /* /connect in flight: reader re-targets ctl on EOF */

/* Re-establish the conversation on the existing mount: the relay wipes held
 * conversations when its 9151 link re-Tattaches destructively under load (a
 * Tversion clunks every fid), so a mid-session read/write starts failing and the
 * data node walks to "file not found". Re-clone -> re-connect -> reopen data, so
 * the chat recovers instead of dying. Caller holds convlk. Returns 0 on success. */
static int
reconv(void)
{
	char nb[32], cmd[64];
	long n;

	if(ctl){ fsclose(ctl); ctl = nil; }
	if(dataR){ fsclose(dataR); dataR = nil; }
	if(dataW){ fsclose(dataW); dataW = nil; }

	ctl = fsopen(fs, "net/aether/clone", ORDWR);
	if(ctl == nil) return -1;
	n = fsread(ctl, nb, sizeof nb - 1);
	if(n <= 0) return -1;
	nb[n] = 0;
	conv = atoi(nb);
	snprint(cmd, sizeof cmd, "connect %s", g_dst);
	if(fswrite(ctl, cmd, strlen(cmd)) < 0) return -1;
	snprint(datapath, sizeof datapath, "net/aether/%d/data", conv);
	dataR = fsopen(fs, datapath, ORDWR);
	dataW = fsopen(fs, datapath, ORDWR);
	if(dataR == nil || dataW == nil) return -1;
	return 0;
}

/* Clunk the ctl fid so the 9151 frees the conversation -- idempotent, and the
 * single choke point every exit path funnels through so the node never wedges. */
static void
hangup(void)
{
	if(ctl){
		fsclose(ctl);
		ctl = nil;
	}
}

/* libdraw error handler: fires when devdraw dies (window closed via the red
 * button), so the close button hangs up cleanly instead of wedging the node. */
static void
drawerr(Display *d, char *m)
{
	USED(d);
	fprint(2, "achat: draw error: %s\n", m);
	hangup();
	threadexitsall(nil);
}

/* Note handler: ctrl-C (interrupt) etc. used to hard-kill us WITHOUT clunking
 * the conversation -> a leaked slot on the 9151 (only 4 exist) -> flaky sessions.
 * Now it wakes the main loop to hang up cleanly. */
static int
noteh(void *v, char *note)
{
	USED(v);
	if(strstr(note, "interrupt") || strstr(note, "hangup")
	|| strstr(note, "term") || strstr(note, "kill")){
		nbsendul(quitc, 1);
		return 1;
	}
	return 0;
}

/* Watchdog: closing the window makes devdraw terminate (mac-screen.m:116), but a
 * quiet event loop would just block forever (hence the ctrl-C). Tick the main
 * loop so it flushimage()s periodically and notices the dead display -> clean exit. */
static void
watchdog(void *a)
{
	USED(a);
	for(;;){
		sleep(1000);
		nbsendul(tickc, 1);
	}
}

static void
addline(char *s)
{
	int i = nlines % MAXLINES;
	if(ring[i])
		free(ring[i]);
	ring[i] = strdup(s);
	nlines++;
}

static void
layout(void)
{
	Rectangle r = screen->r;
	int ih = font->height + 6;
	inputr = Rect(r.min.x, r.max.y - ih, r.max.x, r.max.y);
	transr = Rect(r.min.x, r.min.y, r.max.x, inputr.min.y);
}

/* How many leading chars of s fit in pixel width w (>=1 to guarantee progress). */
static int
fitchars(char *s, int w)
{
	char tmp[LINEMAX];
	int i;

	for(i = 0; s[i] && i < LINEMAX - 1; i++){
		tmp[i] = s[i];
		tmp[i+1] = 0;
		if(stringwidth(font, tmp) > w)
			return i > 0 ? i : 1;
	}
	return i;
}

struct wrow { char *s; int n; Image *col; };

static void
redraw(void)
{
	static struct wrow rows[512];   /* main-proc-only; keep it off the thread stack */
	int nfit, avail, nrows, start, i, firstline;
	Point p, after;
	char pbuf[INPUTMAX + 4], tmp[LINEMAX];

	draw(screen, transr, back, nil, ZP);
	avail = Dx(transr) - 6;
	if(avail < font->height) avail = font->height;
	nfit = Dy(transr) / font->height;

	/* wrap the last nfit logical lines into display rows (each >=1 row, so nfit
	 * lines yield >= nfit rows -- enough to fill the transcript). */
	nrows = 0;
	firstline = nlines > nfit ? nlines - nfit : 0;
	for(i = firstline; i < nlines && nrows < nelem(rows); i++){
		char *ln = ring[i % MAXLINES];
		Image *col = (strncmp(ln, "[me]", 4) == 0) ? mecol : txt;
		int off = 0, L = strlen(ln);
		do {
			int fit = fitchars(ln + off, avail);
			if(off + fit > L) fit = L - off;
			rows[nrows].s = ln + off;
			rows[nrows].n = fit;
			rows[nrows].col = col;
			nrows++;
			off += fit;
		} while(off < L && nrows < nelem(rows));
		if(L == 0){ rows[nrows-1].s = ln; rows[nrows-1].n = 0; }  /* keep blank lines */
	}

	/* draw the newest nfit rows, top-to-bottom */
	start = nrows > nfit ? nrows - nfit : 0;
	p = addpt(transr.min, Pt(3, 2));
	for(i = start; i < nrows; i++){
		int L = rows[i].n;
		if(L > LINEMAX - 1) L = LINEMAX - 1;
		memmove(tmp, rows[i].s, L);
		tmp[L] = 0;
		string(screen, p, rows[i].col, ZP, font, tmp);
		p.y += font->height;
	}

	/* input line: separator + prompt + block cursor */
	draw(screen, inputr, back, nil, ZP);
	line(screen, Pt(inputr.min.x, inputr.min.y), Pt(inputr.max.x, inputr.min.y),
		0, 0, 0, txt, ZP);
	snprint(pbuf, sizeof pbuf, "> %s", input);
	after = string(screen, addpt(inputr.min, Pt(3, 3)), txt, ZP, font, pbuf);
	draw(screen, Rect(after.x, after.y, after.x + 2, after.y + font->height), txt, nil, ZP);

	flushimage(display, 1);
}

/* This node's own HONR routing address, formatted as a connect string
 * (00:00:00:00:XX:XX) -- what a peer types into /connect to reach us reliably.
 * Read from dev/aether/addr (the 9151's HONR short address, ~4 hex digits). */
static void
myaddr(char *out, int outsz)
{
	CFid *f;
	char b[32], h[8];
	long n;
	int i, hi = 0;

	strecpy(out, out + outsz, "?");
	if((f = fsopen(fs, "dev/aether/addr", OREAD)) == nil)
		return;
	n = fsread(f, b, sizeof b - 1);
	fsclose(f);
	if(n <= 0)
		return;
	b[n] = 0;
	for(i = 0; b[i] && hi < 4; i++)
		if(strchr("0123456789abcdefABCDEF", b[i]))
			h[hi++] = b[i];
	while(hi < 4)              /* left-pad short addrs to 4 hex digits */
		h[hi++] = '0';
	snprint(out, outsz, "00:00:00:00:%c%c:%c%c", h[0], h[1], h[2], h[3]);
}

/* reader proc: one datagram per fsread -> format -> hand to the UI proc. */
static void
reader(void *a)
{
	char buf[LINEMAX], out[LINEMAX];
	long n;

	int fails = 0;
	vlong t0;

	USED(a);
	for(;;){
		t0 = nsec();
		n = fsread(dataR, buf, sizeof buf);
		if(n < 0){
			int r;
			vlong dt = (nsec() - t0) / 1000000;   /* ms blocked before failing */

			/* A /net/aether/data read legitimately BLOCKS until the next broadcast.
			 * The relay caps that block (~timeout) and returns an error -- but on a
			 * quiet party line that's NOT a real failure, just "no message yet". So
			 * if we blocked a long time before failing, silently re-read (no spam,
			 * no re-clone). A FAST failure means the conversation was actually wiped
			 * (relay re-Tattach under load clunked our fids) -> re-clone to recover. */
			if(dt > 8000)
				continue;

			if(fails++ == 0){
				snprint(out, sizeof out, "[rx hiccup: %r -- reconnecting]");
				sendp(incoming, strdup(out));
			}
			sleep(500);
			qlock(&convlk);
			r = reconv();
			qunlock(&convlk);
			if(r < 0)
				continue;   /* couldn't re-clone yet; back off (the sleep) and retry */
			sendp(incoming, strdup("[reconnected]"));
			fails = 0;
			continue;
		}
		fails = 0;
		if(n == 0){
			/* EOF sentinel. If a /connect is in flight, this is the "hangup"
			 * we asked for -- re-target the same conversation (still UNCONNECTED
			 * after the hangup) and keep reading. Otherwise it's a real hangup. */
			int w, bc;
			char cmd[64];

			qlock(&convlk);
			if(!switching){
				/* No /connect in flight: this is a stray/late EOF (e.g. a rapid
				 * double /connect queued a second one). Nothing else produces a
				 * len-0 datagram, so just re-read -- shutdown goes via ^D /
				 * window-close (threadexitsall), never via a data EOF. */
				qunlock(&convlk);
				continue;
			}
			snprint(cmd, sizeof cmd, "connect %s", g_dst);
			w = fswrite(ctl, cmd, strlen(cmd));
			bc = g_bcast;
			switching = 0;
			qunlock(&convlk);
			if(w < 0){
				sendp(incoming, strdup("[connect failed -- try /connect again]"));
				continue;
			}
			if(bc)
				sendp(incoming, strdup("[on the party line (broadcast, best-effort)]"));
			else{
				snprint(out, sizeof out, "[connected (reliable) to %s -- peer must /connect you too]", g_dst);
				sendp(incoming, strdup(out));
			}
			continue;
		}
		if(g_bcast && n >= 6){
			uchar *s = (uchar*)buf;
			snprint(out, sizeof out, "[%02x:%02x:%02x:%02x:%02x:%02x] %.*s",
				s[0], s[1], s[2], s[3], s[4], s[5], (int)(n - 6), buf + 6);
		}else
			snprint(out, sizeof out, "%.*s", (int)n, buf);
		sendp(incoming, strdup(out));
	}
}

/* slash-commands typed in the input line. */
static void
docmd(char *s)
{
	char m[400];

	if(strncmp(s, "/nick", 5) == 0 && (s[5] == ' ' || s[5] == 0)){
		char *p = s + 5;
		while(*p == ' ')
			p++;
		strncpy(nick, p, sizeof nick - 1);
		nick[sizeof nick - 1] = 0;
		snprint(m, sizeof m, "[achat] nick set to '%s'", nick);
		addline(m);
	}else if(strcmp(s, "/who") == 0){
		/* the node's live neighbour table, read straight off dev/aether. */
		CFid *f = fsopen(fs, "dev/aether/neighbors", OREAD);
		char buf[2048], *p, *nl;
		long n;
		if(f == nil){ addline("[who] unavailable"); return; }
		n = fsread(f, buf, sizeof buf - 1);
		fsclose(f);
		if(n <= 0){ addline("[who] no neighbours"); return; }
		buf[n] = 0;
		addline("[who] neighbours:");
		for(p = buf; (nl = strchr(p, '\n')) != nil; p = nl + 1){
			*nl = 0;
			if(*p){ snprint(m, sizeof m, "  %s", p); addline(m); }
		}
		if(*p){ snprint(m, sizeof m, "  %s", p); addline(m); }
	}else if(strcmp(s, "/addr") == 0){
		snprint(m, sizeof m, "[achat] my address: %s   (peer: /connect %s)", myaddrbuf, myaddrbuf);
		addline(m);
	}else if(strncmp(s, "/connect", 8) == 0 && (s[8] == ' ' || s[8] == 0)){
		char *p = s + 8, newdst[24];
		while(*p == ' ')
			p++;
		if(*p == 0){
			addline("[achat] usage: /connect <addr>   (or /connect bcast for the party line)");
			return;
		}
		/* bcast keyword -> broadcast; short "HH:LL" -> full 00:00:00:00:HH:LL;
		 * anything longer is taken as a full 6-group address. */
		if(strcmp(p, "bcast") == 0 || strcmp(p, "broadcast") == 0)
			strcpy(newdst, "ff:ff:ff:ff:ff:ff");
		else if(strlen(p) <= 5)
			snprint(newdst, sizeof newdst, "00:00:00:00:%s", p);
		else
			snprint(newdst, sizeof newdst, "%s", p);
		/* Re-target the SAME conversation in place: "hangup" resets it to
		 * UNCONNECTED (the server only accepts "connect" from there) AND wakes
		 * the reader's blocked data read with an EOF sentinel. The reader then
		 * issues "connect <newdst>" and keeps reading the same data fid -- no
		 * fid churn (so no use-after-free vs the blocked reader), and prompt. */
		qlock(&convlk);
		strncpy(g_dst, newdst, sizeof g_dst - 1);
		g_dst[sizeof g_dst - 1] = 0;
		g_bcast = (strcmp(newdst, "ff:ff:ff:ff:ff:ff") == 0);
		switching = 1;
		if(fswrite(ctl, "hangup", 6) < 0){
			switching = 0;
			qunlock(&convlk);
			addline("[achat] /connect: hangup failed -- try again");
			return;
		}
		qunlock(&convlk);
		snprint(m, sizeof m, "[achat] switching to %s ...", newdst);
		addline(m);
	}else if(strcmp(s, "/quit") == 0){
		hangup();
		threadexitsall(nil);
	}else if(strcmp(s, "/help") == 0){
		addline("[achat] /nick NAME | /who | /addr | /connect <addr>|bcast | /quit  (or ^D)");
		addline("[achat]   /connect <peer-addr> = reliable 1:1 (both sides connect); bcast = party line");
	}else{
		snprint(m, sizeof m, "[achat] unknown command: %s (try /help)", s);
		addline(m);
	}
}

static void
sendline(void)
{
	char payload[INPUTMAX + 80], echo[INPUTMAX + 8];

	if(ninput == 0)
		return;
	input[ninput] = 0;
	if(input[0] == '/'){
		docmd(input);
		ninput = 0;
		input[0] = 0;
		return;
	}
	if(nick[0])
		snprint(payload, sizeof payload, "%s: %s", nick, input);
	else
		snprint(payload, sizeof payload, "%s", input);
	qlock(&convlk);   /* don't send on dataW while the reader is re-cloning it */
	long w = fswrite(dataW, payload, strlen(payload));
	qunlock(&convlk);
	if(w < 0)
		addline("[send failed -- reconnecting]");   /* the reader will re-clone; retry the line */
	else{
		snprint(echo, sizeof echo, "[me] %s", input);
		addline(echo);
	}
	ninput = 0;
	input[0] = 0;
}

static void
key(Rune r)
{
	switch(r){
	case '\n':
	case '\r':
		sendline();
		break;
	case Kbs:
		if(ninput > 0)
			ninput--;
		break;
	case Kesc:
		ninput = 0;
		break;
	case Keof:            /* ^D: clean hangup + quit */
		hangup();
		threadexitsall(nil);
	default:
		if(r >= 0x20 && r < 0x7f && ninput < INPUTMAX - 1)
			input[ninput++] = r;   /* ASCII-only input for v1 */
		break;
	}
	input[ninput] = 0;
	redraw();
}

void
threadmain(int argc, char **argv)
{
	char *port = nil;
	char *dst  = "ff:ff:ff:ff:ff:ff";
	char nb[32], cmd[64], banner[128];
	int fd, ai = 1;
	long n;
	Mousectl *mc;
	Keyboardctl *kc;
	Rune r;
	char *msg;
	ulong qv;

	if(argc > ai){ port = argv[ai]; ai++; }
	if(argc > ai){ dst  = argv[ai]; ai++; }
	if(port == nil && (port = serial_autodetect()) == nil)
		sysfatal("no 9P port found (/dev/cu.usbmodem*03); pass one explicitly");

	bundle_setup();   /* if running inside achat.app, point libdraw at bundled devdraw+fonts */

	/* --- UI FIRST --- initdraw spawns devdraw via fork() (libdraw drawclient.c).
	 * It MUST run before fsmount creates the lib9pclient mux's ioproc threads:
	 * forking a process that already has those threads corrupts the mux, and the
	 * reader's first fsread then fails immediately ([read error], one-sided chat). */
	atexit(hangup);   /* free the conversation on ANY exit path */
	if(initdraw(drawerr, nil, "achat") < 0)
		sysfatal("initdraw: %r");
	if((mc = initmouse(nil, screen)) == nil)
		sysfatal("initmouse: %r");
	if((kc = initkeyboard(nil)) == nil)
		sysfatal("initkeyboard: %r");
	back  = display->black;
	txt   = display->white;
	mecol = allocimage(display, Rect(0,0,1,1), screen->chan, 1, DPalegreen);
	if(mecol == nil) mecol = txt;
	incoming = chancreate(sizeof(char*), 32);
	quitc = chancreate(sizeof(ulong), 2);
	tickc = chancreate(sizeof(ulong), 2);
	threadnotify(noteh, 1);   /* catch ctrl-C etc. -> clean hangup */
	layout();
	addline("[achat] connecting...");
	redraw();

	/* --- transport (identical to achat_core.c), AFTER devdraw is forked --- */
	fd = serial_open(port);
	if(fd < 0)
		sysfatal("open %s: %r", port);
	sleep(400);
	fs = fsmount(fd, nil);
	if(fs == nil)
		sysfatal("fsmount %s: %r", port);
	ctl = fsopen(fs, "net/aether/clone", ORDWR);
	if(ctl == nil)
		sysfatal("open net/aether/clone: %r");
	n = fsread(ctl, nb, sizeof nb - 1);
	if(n <= 0)
		sysfatal("read conv#: %r");
	nb[n] = 0;
	conv = atoi(nb);
	g_bcast = (strcmp(dst, "ff:ff:ff:ff:ff:ff") == 0);
	strncpy(g_dst, dst, sizeof g_dst - 1);   /* for reconv() */
	snprint(cmd, sizeof cmd, "connect %s", dst);
	if(fswrite(ctl, cmd, strlen(cmd)) < 0)
		sysfatal("connect %s: %r", dst);
	snprint(datapath, sizeof datapath, "net/aether/%d/data", conv);
	dataR = fsopen(fs, datapath, ORDWR);
	dataW = fsopen(fs, datapath, ORDWR);
	if(dataR == nil || dataW == nil)
		sysfatal("open %s: %r", datapath);

	myaddr(myaddrbuf, sizeof myaddrbuf);
	snprint(banner, sizeof banner, "[achat] %s conv %d -- %s -- /help, ^D quits", port, conv,
		g_bcast ? "party line (broadcast, best-effort)" : "connected (reliable)");
	addline(banner);
	snprint(banner, sizeof banner, "[achat] my address: %s   (peer: /connect %s for reliable 1:1)",
		myaddrbuf, myaddrbuf);
	addline(banner);
	redraw();

	proccreate(reader, nil, 16384);
	proccreate(watchdog, nil, 4096);

	enum { AKEY, ARESIZE, AMSG, AQUIT, ATICK, AEND };
	Alt alts[] = {
		[AKEY]    = { kc->c,       &r,   CHANRCV },
		[ARESIZE] = { mc->resizec, nil,  CHANRCV },
		[AMSG]    = { incoming,    &msg, CHANRCV },
		[AQUIT]   = { quitc,       &qv,  CHANRCV },
		[ATICK]   = { tickc,       &qv,  CHANRCV },
		[AEND]    = { nil,         nil,  CHANEND },
	};
	for(;;){
		switch(alt(alts)){
		case AKEY:
			key(r);
			break;
		case ARESIZE:
			if(getwindow(display, Refnone) < 0){   /* display gone -> hang up */
				hangup();
				threadexitsall(nil);
			}
			layout();
			redraw();
			break;
		case AMSG:
			addline(msg);
			free(msg);
			redraw();
			break;
		case AQUIT:            /* ctrl-C / term note */
			hangup();
			threadexitsall(nil);
		case ATICK:            /* heartbeat: notice a closed window (dead devdraw) */
			if(flushimage(display, 1) < 0){
				hangup();
				threadexitsall(nil);
			}
			break;
		}
	}
}
