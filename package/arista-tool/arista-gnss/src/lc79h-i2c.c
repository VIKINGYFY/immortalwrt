// SPDX-License-Identifier: GPL-2.0-only
/*
 * lc79h-i2c: the NMEA output of a Quectel LC79H GNSS receiver on I2C, on a
 * pty for gpsd.
 *
 * usage: lc79h-i2c [-v] [-d i2c-device] [-l link] [-i milliseconds]
 *                  [-r gpiochip:line] [-g gpsd-control-socket]
 *
 *   -d  the I2C bus (default /dev/i2c-0)
 *   -l  the link to the pty (default /var/run/lc79h-gnss)
 *   -i  how long to wait between polls (default 300 ms)
 *   -r  the receiver's reset line (high = in reset): reset it at start
 *   -g  gpsd's control socket: if gpsd already reads the link, have it
 *       reopen the link at start
 *   -v  report each poll's output length
 *
 * Notices go to stdout, line by line, and failures to stderr, so that procd
 * logs them as info and as errors.
 *
 * The receiver takes commands at I2C address 0x50 and answers at 0x54.
 * Each poll asks how many bytes its output buffer holds (command
 * 0xAA510008, a 4-byte answer), then reads up to 4095 of them, 128 bytes a
 * read (command 0xAA512000 with the length). As Quectel's own I2C reader
 * does, it waits 10 ms before each I2C message and tries each up to 20
 * times: the receiver refuses a message until it is ready for it. Those two
 * reads are the only commands this sends; the receiver's settings stay as
 * they are.
 *
 * Once its output buffer fills (some 32 KiB, about 70 s of NMEA nobody
 * read), the receiver stops its output, and only a reset brings it back:
 * with -r, this holds it in reset for 100 ms at start and gives it a second
 * to boot. The line then stays requested, low, while this runs.
 *
 * The NMEA goes to a pty. LINK names its slave side, which is group dialout
 * and mode 0660 so that gpsd can open it after dropping privileges; gpsd
 * reopens it by that name when this restarts. This holds the slave open as
 * well, so output nobody reads waits in the pty until that is full and is
 * then dropped, the oldest first. gpsd notices that an old pty is gone only
 * when its main loop next wakes, which can take as long as no client asks
 * it anything; with -g, a restarted reader tells it to reopen the link at
 * once, by removing the link and adding it again, as gpsdctl does.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define ADDR_CMD	0x50
#define ADDR_DATA	0x54
#define CMD_OUT_LEN	0xAA510008u	/* bytes in the output buffer: 4-byte answer */
#define CMD_OUT_READ	0xAA512000u	/* that many bytes of the output buffer */
#define MSG_WAIT_MS	10		/* before each I2C message */
#define MSG_TRIES	20
#define READ_MAX	128		/* bytes a read */
#define POLL_MAX	4095		/* bytes a poll */
#define LEN_SANE	(1u << 20)	/* a longer output buffer is a bad answer */
#define RETRY_MS	1000
#define REPORT_S	600		/* while it fails, say so this often */
#define RESET_MS	100		/* in reset */
#define BOOT_MS		1000		/* out of reset, before the first poll */

static volatile sig_atomic_t stop;
static int failing;
static unsigned long failures;
static struct timespec reported;

struct pty {
	int master;
	int slave;
	char name[64];
};

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void msleep(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) && errno == EINTR && !stop)
		;
}

static void report(const char *fmt, ...)
{
	struct timespec now;
	va_list ap;

	failures++;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (failing && now.tv_sec - reported.tv_sec < REPORT_S)
		return;
	failing = 1;
	reported = now;
	fputs("lc79h-i2c: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, " (%lu failed polls so far)\n", failures);
}

static void answered(void)
{
	if (failing)
		puts("lc79h-i2c: the receiver answers again");
	failing = 0;
}

static int i2c_msg(int fd, uint16_t addr, uint16_t flags, uint8_t *buf, uint16_t len)
{
	struct i2c_msg msg = { .addr = addr, .flags = flags, .len = len, .buf = buf };
	struct i2c_rdwr_ioctl_data data = { .msgs = &msg, .nmsgs = 1 };
	int i;

	for (i = 0; i < MSG_TRIES && !stop; i++) {
		msleep(MSG_WAIT_MS);
		if (ioctl(fd, I2C_RDWR, &data) == 1)
			return 0;
	}
	return -1;
}

/* command CMD with argument ARG to 0x50, then LEN bytes from 0x54 */
static int lc79h_cmd(int fd, uint32_t cmd, uint32_t arg, uint8_t *buf, uint16_t len)
{
	uint8_t w[8];
	int i;

	for (i = 0; i < 4; i++) {
		w[i] = cmd >> (8 * i);
		w[4 + i] = arg >> (8 * i);
	}
	if (i2c_msg(fd, ADDR_CMD, 0, w, sizeof(w)))
		return -1;
	return i2c_msg(fd, ADDR_DATA, I2C_M_RD, buf, len);
}

/*
 * Hold the receiver in reset for 100 ms, then let it run. Returns the line's
 * fd: the line stays requested, and low, while this runs.
 */
static int lc79h_reset(const char *spec)
{
	struct gpio_v2_line_request req;
	struct gpio_v2_line_values v;
	const char *colon;
	unsigned long line;
	char chip[64];
	char *end;
	int fd, ret, err;

	colon = strrchr(spec, ':');
	if (!colon || colon == spec || (size_t)(colon - spec) >= sizeof(chip)) {
		errno = EINVAL;
		return -1;
	}
	memcpy(chip, spec, colon - spec);
	chip[colon - spec] = 0;
	line = strtoul(colon + 1, &end, 10);
	if (*end || end == colon + 1) {
		errno = EINVAL;
		return -1;
	}

	fd = open(chip, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	memset(&req, 0, sizeof(req));
	req.offsets[0] = line;
	req.num_lines = 1;
	snprintf(req.consumer, sizeof(req.consumer), "lc79h reset");
	req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
	req.config.num_attrs = 1;
	req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
	req.config.attrs[0].attr.values = 1;
	req.config.attrs[0].mask = 1;
	ret = ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req);
	err = errno;
	close(fd);
	if (ret < 0) {
		errno = err;
		return -1;
	}
	msleep(RESET_MS);
	memset(&v, 0, sizeof(v));
	v.mask = 1;
	if (ioctl(req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0) {
		err = errno;
		close(req.fd);
		errno = err;
		return -1;
	}
	msleep(BOOT_MS);
	return req.fd;
}

static int pty_open(struct pty *p, const char *link)
{
	char tmp[PATH_MAX];
	struct termios t;
	struct group *g;
	const char *n;

	p->master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (p->master < 0 || grantpt(p->master) || unlockpt(p->master))
		return -1;
	n = ptsname(p->master);
	if (!n || strlen(n) >= sizeof(p->name)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(p->name, n);
	p->slave = open(p->name, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (p->slave < 0)
		return -1;
	if (tcgetattr(p->slave, &t) == 0) {
		cfmakeraw(&t);
		tcsetattr(p->slave, TCSANOW, &t);
	}
	g = getgrnam("dialout");
	if (g && chown(p->name, (uid_t)-1, g->gr_gid))
		return -1;
	if (chmod(p->name, 0660))
		return -1;
	if (snprintf(tmp, sizeof(tmp), "%s.new", link) >= (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	unlink(tmp);
	if (symlink(p->name, tmp) || rename(tmp, link))
		return -1;
	return 0;
}

static void pty_put(struct pty *p, const uint8_t *buf, size_t n)
{
	int flushed = 0;
	ssize_t w;

	while (n) {
		w = write(p->master, buf, n);
		if (w > 0) {
			buf += w;
			n -= w;
		} else if (w < 0 && errno == EINTR) {
			continue;
		} else if (w < 0 && errno == EAGAIN && !flushed) {
			/* nobody reads: drop what waits, keep the newest */
			tcflush(p->slave, TCIFLUSH);
			flushed = 1;
		} else {
			return;
		}
	}
}

/* whatever a reader writes to the pty goes nowhere */
static void pty_drain(struct pty *p)
{
	uint8_t junk[256];

	while (read(p->master, junk, sizeof(junk)) > 0)
		;
}

static void pty_unlink(struct pty *p, const char *link)
{
	char cur[sizeof(p->name)];
	ssize_t l;

	l = readlink(link, cur, sizeof(cur) - 1);
	if (l <= 0)
		return;
	cur[l] = 0;
	if (!strcmp(cur, p->name))
		unlink(link);
}

/*
 * One command to gpsd's control socket ("+path" or "-path"): 1 when gpsd
 * acknowledges it, 0 when it refuses it, -1 when gpsd is not there.
 */
static int gpsd_control(const char *sock, char op, const char *link)
{
	struct sockaddr_un sa;
	struct pollfd p;
	char buf[PATH_MAX + 4];
	ssize_t n;
	int fd, len;

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlen(sock) >= sizeof(sa.sun_path))
		return -1;
	strcpy(sa.sun_path, sock);
	len = snprintf(buf, sizeof(buf), "%c%s\r\n", op, link);
	if (len >= (int)sizeof(buf))
		return -1;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) ||
	    write(fd, buf, len) != len) {
		close(fd);
		return -1;
	}
	p.fd = fd;
	p.events = POLLIN;
	n = poll(&p, 1, 2000) == 1 ? read(fd, buf, sizeof(buf) - 1) : -1;
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	return strstr(buf, "\"ACK\"") != NULL;
}

/* a gpsd that reads LINK reopens it: it still holds the pty this replaced */
static void gpsd_reopen(const char *sock, const char *link)
{
	if (gpsd_control(sock, '-', link) != 1)
		return;
	if (gpsd_control(sock, '+', link) == 1)
		printf("lc79h-i2c: gpsd reopened %s\n", link);
	else
		fprintf(stderr, "lc79h-i2c: gpsd did not take %s back\n", link);
}

/* NMEA is printable ASCII in CR LF lines: keep only that */
static size_t nmea_only(uint8_t *buf, size_t n)
{
	size_t i, k;

	for (i = k = 0; i < n; i++)
		if (buf[i] == '\r' || buf[i] == '\n' || (buf[i] >= 0x20 && buf[i] < 0x7f))
			buf[k++] = buf[i];
	return k;
}

static void usage(void)
{
	fputs("usage: lc79h-i2c [-v] [-d i2c-device] [-l link] [-i milliseconds]"
	      " [-r gpiochip:line] [-g gpsd-control-socket]\n", stderr);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/i2c-0", *link = "/var/run/lc79h-gnss", *reset = NULL;
	const char *gpsd = NULL;
	static uint8_t buf[POLL_MAX];
	unsigned long interval = 300;
	uint32_t n, got, c;
	struct sigaction sa;
	struct pty pty;
	char *end;
	int fd, opt, verbose = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);

	while ((opt = getopt(argc, argv, "vd:l:i:r:g:")) != -1) {
		switch (opt) {
		case 'v':
			verbose = 1;
			break;
		case 'g':
			gpsd = optarg;
			break;
		case 'r':
			reset = optarg;
			break;
		case 'd':
			dev = optarg;
			break;
		case 'l':
			link = optarg;
			break;
		case 'i':
			interval = strtoul(optarg, &end, 10);
			if (*end || !interval || interval > 60000) {
				usage();
				return 2;
			}
			break;
		default:
			usage();
			return 2;
		}
	}
	if (optind != argc) {
		usage();
		return 2;
	}

	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "lc79h-i2c: %s: %s\n", dev, strerror(errno));
		return 1;
	}
	if (pty_open(&pty, link)) {
		fprintf(stderr, "lc79h-i2c: a pty for %s: %s\n", link, strerror(errno));
		return 1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	if (reset && lc79h_reset(reset) < 0)
		fprintf(stderr, "lc79h-i2c: no reset through %s: %s\n", reset, strerror(errno));

	printf("lc79h-i2c: reading the receiver on %s%s; NMEA on %s (%s)\n",
		dev, reset ? ", after a reset" : "", link, pty.name);
	if (gpsd)
		gpsd_reopen(gpsd, link);

	while (!stop) {
		pty_drain(&pty);
		if (lc79h_cmd(fd, CMD_OUT_LEN, 4, buf, 4)) {
			if (!stop) {
				report("asking for the output length: %s", strerror(errno));
				msleep(RETRY_MS);
			}
			continue;
		}
		n = buf[0] | buf[1] << 8 | buf[2] << 16 | (uint32_t)buf[3] << 24;
		if (verbose)
			printf("lc79h-i2c: %u bytes waiting\n", n);
		if (n > LEN_SANE) {
			report("an output length of %u bytes", n);
			msleep(RETRY_MS);
			continue;
		}
		if (n > POLL_MAX)
			n = POLL_MAX;
		for (got = 0; got < n && !stop; got += c) {
			c = n - got < READ_MAX ? n - got : READ_MAX;
			if (lc79h_cmd(fd, CMD_OUT_READ, c, buf + got, c)) {
				if (!stop)
					report("reading %u bytes of output: %s", c, strerror(errno));
				break;
			}
		}
		if (got)
			pty_put(&pty, buf, nmea_only(buf, got));
		if (got < n) {
			if (!stop)
				msleep(RETRY_MS);
			continue;
		}
		answered();
		if (n < POLL_MAX)
			msleep(interval);
	}

	pty_unlink(&pty, link);
	return 0;
}
