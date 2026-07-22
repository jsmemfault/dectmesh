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
static char nick[64];                /* /nick prefix on outgoing messages */

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

/* reader proc: one datagram per fsread -> format -> hand to the UI proc. */
static void
reader(void *a)
{
	char buf[LINEMAX], out[LINEMAX];
	long n;

	USED(a);
	for(;;){
		n = fsread(dataR, buf, sizeof buf);
		if(n < 0){ sendp(incoming, strdup("[read error]")); break; }
		if(n == 0){ sendp(incoming, strdup("[hangup]")); break; }
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
	}else if(strcmp(s, "/quit") == 0){
		hangup();
		threadexitsall(nil);
	}else if(strcmp(s, "/help") == 0){
		addline("[achat] /nick NAME | /who | /quit  (or ^D)");
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
	if(fswrite(dataW, payload, strlen(payload)) < 0)
		addline("[send failed]");
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
	char nb[32], cmd[64], path[64], banner[128];
	int fd, conv, ai = 1;
	long n;
	Mousectl *mc;
	Keyboardctl *kc;
	Rune r;
	char *msg;

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
	snprint(cmd, sizeof cmd, "connect %s", dst);
	if(fswrite(ctl, cmd, strlen(cmd)) < 0)
		sysfatal("connect %s: %r", dst);
	snprint(path, sizeof path, "net/aether/%d/data", conv);
	dataR = fsopen(fs, path, ORDWR);
	dataW = fsopen(fs, path, ORDWR);
	if(dataR == nil || dataW == nil)
		sysfatal("open %s: %r", path);

	snprint(banner, sizeof banner, "[achat] conv %d on %s -- /help for commands, ^D quits", conv, dst);
	addline(banner);
	redraw();

	proccreate(reader, nil, 16384);

	enum { AKEY, ARESIZE, AMSG, AEND };
	Alt alts[] = {
		[AKEY]    = { kc->c,       &r,   CHANRCV },
		[ARESIZE] = { mc->resizec, nil,  CHANRCV },
		[AMSG]    = { incoming,    &msg, CHANRCV },
		[AEND]    = { nil,         nil,  CHANEND },
	};
	for(;;){
		switch(alt(alts)){
		case AKEY:
			key(r);
			break;
		case ARESIZE:
			if(getwindow(display, Refnone) < 0)
				sysfatal("resize: %r");
			layout();
			redraw();
			break;
		case AMSG:
			addline(msg);
			free(msg);
			redraw();
			break;
		}
	}
}
