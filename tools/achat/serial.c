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
