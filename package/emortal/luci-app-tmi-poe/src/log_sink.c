/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "log_sink.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define LOG_PATH "/var/log/tmi-poe.log"
#define LOG_ROTATED_PATH "/var/log/tmi-poe.log.1"
#define LOG_LOCK_PATH "/var/run/tmi-poe-log.lock"
#define LOG_MAX_BYTES (128U * 1024U)

static FILE *log_file;
static bool debug_enabled;

static const char *priority_name(int priority)
{
	switch (priority) {
	case LOG_ERR: return "error";
	case LOG_WARNING: return "warning";
	case LOG_NOTICE: return "notice";
	case LOG_DEBUG: return "debug";
	default: return "info";
	}
}

/* Serialize clearing with writes and rotation, without taking the PSE lock. */
static int lock_log(void)
{
	int fd = open(LOG_LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	int error;

	if (fd < 0)
		return -errno;
	while (flock(fd, LOCK_EX)) {
		if (errno == EINTR)
			continue;
		error = errno;
		close(fd);
		return -error;
	}
	return fd;
}

static int open_log(void)
{
	int fd;

	fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -errno;
	if (lseek(fd, 0, SEEK_END) > LOG_MAX_BYTES) {
		close(fd);
		(void)rename(LOG_PATH, LOG_ROTATED_PATH);
		fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd < 0)
			return -errno;
	}
	log_file = fdopen(fd, "a");
	if (!log_file) {
		int error = errno;
		close(fd);
		return -error;
	}
	setvbuf(log_file, NULL, _IOLBF, 0);
	return 0;
}

int tmi_log_init(bool debug)
{
	int lock = lock_log(), ret;

	debug_enabled = debug;
	if (lock < 0)
		return lock;
	tmi_log_close();
	ret = open_log();
	close(lock);
	return ret;
}

int tmi_log_clear(void)
{
	struct stat st;
	int lock = lock_log(), fd, ret = 0;

	if (lock < 0)
		return lock;
	/* Keep the active inode: the daemon continues appending to the same file. */
	fd = open(LOG_PATH, O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd >= 0) {
		if (fstat(fd, &st))
			ret = -errno;
		else if (!S_ISREG(st.st_mode))
			ret = -EINVAL;
		else if (ftruncate(fd, 0))
			ret = -errno;
		close(fd);
	} else if (errno != ENOENT) {
		ret = -errno;
	}
	if (!ret && unlink(LOG_ROTATED_PATH) && errno != ENOENT)
		ret = -errno;
	close(lock);
	return ret;
}

void tmi_log_set_debug(bool debug)
{
	debug_enabled = debug;
}

void tmi_log_close(void)
{
	if (log_file) {
		fclose(log_file);
		log_file = NULL;
	}
}

void tmi_log_message(int priority, const char *format, ...)
{
	char timestamp[32];
	struct timespec now;
	struct tm tm;
	struct stat st;
	va_list ap;
	int lock;

	if (priority == LOG_DEBUG && !debug_enabled)
		return;
	if (!log_file)
		return;
	if (clock_gettime(CLOCK_REALTIME, &now) || !localtime_r(&now.tv_sec, &tm))
		return;
	lock = lock_log();
	if (lock < 0)
		return;
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(log_file, "%s.%03ld [%s] ", timestamp, now.tv_nsec / 1000000L,
		priority_name(priority));
	va_start(ap, format);
	vfprintf(log_file, format, ap);
	va_end(ap);
	fputc('\n', log_file);
	fflush(log_file);
	if (!fstat(fileno(log_file), &st) && st.st_size > LOG_MAX_BYTES) {
		fclose(log_file);
		log_file = NULL;
		(void)rename(LOG_PATH, LOG_ROTATED_PATH);
		(void)open_log();
	}
	close(lock);
}
