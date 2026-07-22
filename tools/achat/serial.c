/*
 * serial.c -- open a serial / USB-CDC port in raw mode, the way `socat ...,rawer`
 * does, and hand back a plain fd. Kept in its own translation unit compiled with
 * the SYSTEM headers (termios etc.), separate from the plan9port core, so the two
 * header worlds never meet.
 */
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <libgen.h>
#include <mach-o/dyld.h>

/* Open @path raw (8N1, no echo/canon, no flow control, CLOCAL so we don't wait
 * on modem control lines). Returns the fd, or -1 with errno set. The nRF5340
 * relay's 9P session pool is DTR-gated; opening /dev/cu.* asserts DTR, which
 * brings the session up. */
int
serial_open(const char *path)
{
	int fd = open(path, O_RDWR | O_NOCTTY);
	if(fd < 0)
		return -1;

	struct termios t;
	if(tcgetattr(fd, &t) < 0){
		close(fd);
		return -1;
	}
	cfmakeraw(&t);
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cflag &= ~CRTSCTS;          /* no hardware flow control */
	t.c_cc[VMIN] = 1;               /* block until at least 1 byte */
	t.c_cc[VTIME] = 0;
	cfsetispeed(&t, B115200);
	cfsetospeed(&t, B115200);       /* CDC ignores baud, but set it anyway */

	if(tcsetattr(fd, TCSANOW, &t) < 0){
		close(fd);
		return -1;
	}
	tcflush(fd, TCIOFLUSH);
	return fd;
}

/* Find the relay's 9P CDC port with no args (for the double-clicked .app): a
 * /dev/cu.usbmodem* whose name ends in "03" (the 9P endpoint; *01=9151 console,
 * *05=5340 console). Ending in "03" also skips a connected Nordic DK, whose
 * J-Link serial ends in "93". Returns a malloc'd path, or nil if none. */
char *
serial_autodetect(void)
{
	DIR *d = opendir("/dev");
	struct dirent *e;
	char *best = 0;

	if(d == 0)
		return 0;
	while((e = readdir(d)) != 0){
		const char *n = e->d_name;
		size_t L = strlen(n);
		if(strncmp(n, "cu.usbmodem", 11) != 0 || L < 2)
			continue;
		if(n[L-2] == '0' && n[L-1] == '3'){
			char path[128];
			snprintf(path, sizeof path, "/dev/%s", n);
			/* prefer the shortest name (a Thingy "cu.usbmodem1203" over any
			 * longer serial) for a stable default. */
			if(best == 0 || strlen(path) < strlen(best)){
				free(best);
				best = strdup(path);
			}
		}
	}
	closedir(d);
	return best;
}

/* If we're running from inside achat.app, point libdraw at the bundled devdraw
 * (via $DEVDRAW) so the app is self-contained -- no plan9port install needed.
 * The built-in "*default*" font means no font files are required. No-op when
 * run normally from a plan9port tree. */
void
bundle_setup(void)
{
	char exe[1024], dir[1024];
	uint32_t sz = sizeof exe;
	char *d, devdraw[1100];

	if(_NSGetExecutablePath(exe, &sz) != 0)
		return;
	if(strstr(exe, "/Contents/MacOS/") == 0)
		return;                        /* not in a bundle */
	strncpy(dir, exe, sizeof dir - 1);
	dir[sizeof dir - 1] = 0;
	d = dirname(dir);                  /* .../Contents/MacOS */
	snprintf(devdraw, sizeof devdraw, "%s/devdraw", d);
	setenv("DEVDRAW", devdraw, 1);
}
