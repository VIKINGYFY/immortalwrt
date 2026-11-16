/*
 * airscan-ble: Bluetooth LE survey through the kernel's management interface.
 *
 * usage: airscan-ble [-i index] [-d seconds]
 *
 * Opens an HCI_CHANNEL_CONTROL (mgmt) socket, starts LE discovery (public and
 * random addresses), collects MGMT_EV_DEVICE_FOUND for the given time,
 * restarting discovery when the kernel ends a round early, then stops
 * discovery and prints one JSON object:
 *
 *   {"index", "duration", "rounds", "error", "devices": [ ... ]}
 *
 * Per device: address, address type (and the kind of random address), RSSI
 * last/avg/max, times seen, and the decoded advertising data: flags, name,
 * TX power, appearance, manufacturer (company ID + data), 16/128-bit service
 * UUIDs, iBeacon / Eddystone beacons, and a heuristic tracker label (Apple
 * Find My, Samsung SmartTag, Tile, Google Find My Device network).
 *
 * The controller must already be powered; the helper never changes its
 * power, address or advertising state.
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_HCI 1
#define HCI_DEV_NONE 0xffff
#define HCI_CHANNEL_CONTROL 3

struct sockaddr_hci {
	sa_family_t hci_family;
	unsigned short hci_dev;
	unsigned short hci_channel;
};

#define MGMT_OP_START_DISCOVERY 0x0023
#define MGMT_OP_STOP_DISCOVERY 0x0024
#define MGMT_EV_CMD_COMPLETE 0x0001
#define MGMT_EV_CMD_STATUS 0x0002
#define MGMT_EV_DEVICE_FOUND 0x0012
#define MGMT_EV_DISCOVERING 0x0013
#define DISCOV_LE 0x06                  /* LE public | LE random */

#define MAX_DEV 512

struct dev {
	uint8_t addr[6];                /* as received: little-endian */
	uint8_t type;                   /* 1 LE public, 2 LE random */
	int seen, rssi_last, rssi_max, rssi_n;   /* rssi_n: reports with an RSSI */
	long rssi_sum;
	char name[64];
	int name_len, name_complete;
	int has_flags, adflags;
	int has_tx, tx;
	int has_app, appearance;
	int company;                    /* -1: none */
	uint8_t mfr[32];
	int mfr_len;
	uint16_t uuid16[8];
	int n16;
	uint8_t uuid128[2][16];
	int n128;
	char beacon[200];               /* JSON object text, or "" */
	const char *tracker;
};

static struct dev devs[MAX_DEV];
static int ndev;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int send_cmd(int fd, uint16_t op, uint16_t index, const void *param, uint16_t len)
{
	uint8_t buf[64];

	buf[0] = op & 0xff; buf[1] = op >> 8;
	buf[2] = index & 0xff; buf[3] = index >> 8;
	buf[4] = len & 0xff; buf[5] = len >> 8;
	memcpy(buf + 6, param, len);
	return write(fd, buf, 6 + len) == 6 + len ? 0 : -1;
}

static struct dev *find_dev(const uint8_t *addr, uint8_t type)
{
	for (int i = 0; i < ndev; i++)
		if (devs[i].type == type && !memcmp(devs[i].addr, addr, 6))
			return &devs[i];
	if (ndev == MAX_DEV)
		return NULL;
	struct dev *d = &devs[ndev++];
	memset(d, 0, sizeof(*d));
	memcpy(d->addr, addr, 6);
	d->type = type;
	d->company = -1;
	d->rssi_max = -128;
	return d;
}

static void add16(struct dev *d, uint16_t u)
{
	for (int i = 0; i < d->n16; i++)
		if (d->uuid16[i] == u)
			return;
	if (d->n16 < 8)
		d->uuid16[d->n16++] = u;
}

static void add128(struct dev *d, const uint8_t *u)
{
	for (int i = 0; i < d->n128; i++)
		if (!memcmp(d->uuid128[i], u, 16))
			return;
	if (d->n128 < 2)
		memcpy(d->uuid128[d->n128++], u, 16);
}

/* 128-bit UUIDs arrive little-endian; print in the usual big-endian form. */
static void uuid128_str(char *out, const uint8_t *u)
{
	uint8_t b[16];

	for (int i = 0; i < 16; i++)
		b[i] = u[15 - i];
	sprintf(out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
		b[13], b[14], b[15]);
}

static void parse_ad(struct dev *d, const uint8_t *p, int len)
{
	int i = 0;

	while (i < len) {
		int l = p[i];
		if (l == 0 || i + 1 + l > len)
			break;
		int t = p[i + 1];
		const uint8_t *v = p + i + 2;
		int vl = l - 1;

		switch (t) {
		case 0x01:                      /* flags */
			if (vl >= 1) { d->has_flags = 1; d->adflags = v[0]; }
			break;
		case 0x02: case 0x03:           /* 16-bit service UUIDs */
			for (int k = 0; k + 1 < vl; k += 2)
				add16(d, le16(v + k));
			break;
		case 0x06: case 0x07:           /* 128-bit service UUIDs */
			for (int k = 0; k + 15 < vl; k += 16)
				add128(d, v + k);
			break;
		case 0x08: case 0x09:           /* short / complete name */
			if (vl > 0 && (t == 0x09 || !d->name_complete)) {
				int n = vl < (int)sizeof(d->name) ? vl : (int)sizeof(d->name);
				memcpy(d->name, v, n);
				d->name_len = n;
				d->name_complete = t == 0x09;
			}
			break;
		case 0x0a:                      /* TX power level */
			if (vl >= 1) { d->has_tx = 1; d->tx = (int8_t)v[0]; }
			break;
		case 0x16:                      /* service data, 16-bit UUID */
			if (vl >= 2) {
				uint16_t u = le16(v);
				add16(d, u);
				if (u == 0xfeaa && vl >= 3) {           /* Eddystone */
					const char *fr = v[2] == 0x00 ? "uid" : v[2] == 0x10 ? "url" :
						v[2] == 0x20 ? "tlm" : v[2] == 0x30 ? "eid" :
						(v[2] == 0x40 || v[2] == 0x41) ? "fmdn" : "other";
					snprintf(d->beacon, sizeof(d->beacon),
						 "{\"type\":\"eddystone\",\"frame\":\"%s\"}", fr);
					if (v[2] == 0x40 || v[2] == 0x41)
						d->tracker = "google_find_my_device";
				}
				if (u == 0xfd5a)
					d->tracker = "samsung_smarttag";
			}
			break;
		case 0x19:                      /* appearance */
			if (vl >= 2) { d->has_app = 1; d->appearance = le16(v); }
			break;
		case 0xff:                      /* manufacturer specific */
			if (vl >= 2) {
				d->company = le16(v);
				d->mfr_len = vl - 2 < (int)sizeof(d->mfr) ? vl - 2 : (int)sizeof(d->mfr);
				memcpy(d->mfr, v + 2, d->mfr_len);
				if (d->company == 0x004c && vl >= 25 && v[2] == 0x02 && v[3] == 0x15) {
					/* iBeacon: 16-byte UUID, major, minor (big-endian), TX at 1 m */
					char u[40];
					uint8_t le[16];
					for (int k = 0; k < 16; k++)
						le[k] = v[4 + 15 - k];          /* uuid128_str reverses */
					uuid128_str(u, le);
					snprintf(d->beacon, sizeof(d->beacon),
						 "{\"type\":\"ibeacon\",\"uuid\":\"%s\",\"major\":%d,\"minor\":%d,\"tx_1m\":%d}",
						 u, v[20] << 8 | v[21], v[22] << 8 | v[23], (int8_t)v[24]);
				}
				if (d->company == 0x004c && vl >= 3 && v[2] == 0x12)
					d->tracker = "apple_find_my";
			}
			break;
		}
		i += 1 + l;
	}
	for (int k = 0; k < d->n16; k++)
		if (d->uuid16[k] == 0xfeed || d->uuid16[k] == 0xfeec)
			d->tracker = "tile";
		else if (d->uuid16[k] == 0xfd5a)
			d->tracker = "samsung_smarttag";
}

static void on_device_found(const uint8_t *p, int len)
{
	if (len < 14)
		return;
	uint8_t type = p[6];
	if (type != 1 && type != 2)             /* LE only */
		return;
	int8_t rssi = (int8_t)p[7];
	int eir_len = le16(p + 12);
	if (14 + eir_len > len)
		eir_len = len - 14;
	struct dev *d = find_dev(p, type);
	if (!d)
		return;
	d->seen++;
	if (rssi != 127) {                      /* 127: RSSI not available */
		d->rssi_last = rssi;
		d->rssi_sum += rssi;
		d->rssi_n++;
		if (rssi > d->rssi_max)
			d->rssi_max = rssi;
	}
	parse_ad(d, p + 14, eir_len);
}

static const char *company_name(int c)
{
	switch (c) {
	case 0x0006: return "Microsoft";
	case 0x000d: return "Texas Instruments";
	case 0x004c: return "Apple";
	case 0x0059: return "Nordic Semiconductor";
	case 0x0075: return "Samsung";
	case 0x0087: return "Garmin";
	case 0x00e0: return "Google";
	case 0x0171: return "Amazon";
	case 0x02e5: return "Espressif";
	case 0x038f: return "Xiaomi";
	case 0x0499: return "Ruuvi Innovations";
	case 0x05f1: return "The Linux Foundation";
	}
	return NULL;
}

/* JSON string: escapes quotes, backslashes and controls; keeps valid UTF-8,
 * replaces invalid bytes with U+FFFD. */
static void json_str(const uint8_t *s, int n)
{
	putchar('"');
	for (int i = 0; i < n; i++) {
		uint8_t c = s[i];
		if (c == '"' || c == '\\') {
			printf("\\%c", c);
		} else if (c < 0x20) {
			printf("\\u%04x", c);
		} else if (c < 0x80) {
			putchar(c);
		} else {
			int k = c >= 0xf0 && c < 0xf5 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 && c < 0xe0 ? 1 : -1;
			if (k > 0 && c < 0xf5 && i + k < n) {
				int ok = 1;
				for (int j = 1; j <= k; j++)
					ok &= (s[i + j] & 0xc0) == 0x80;
				if (ok) {
					fwrite(s + i, 1, k + 1, stdout);
					i += k;
					continue;
				}
			}
			fputs("\\ufffd", stdout);
		}
	}
	putchar('"');
}

static void print_json(int index, int secs, int rounds, const char *err)
{
	printf("{\"index\":%d,\"duration\":%d,\"rounds\":%d,\"error\":", index, secs, rounds);
	if (err)
		printf("\"%s\"", err);
	else
		fputs("null", stdout);
	fputs(",\"devices\":[", stdout);
	for (int i = 0; i < ndev; i++) {
		struct dev *d = &devs[i];
		const uint8_t *a = d->addr;
		const char *rnd = "";
		if (d->type == 2)
			rnd = (a[5] >> 6) == 3 ? "static" : (a[5] >> 6) == 1 ? "resolvable" :
			      (a[5] >> 6) == 0 ? "non-resolvable" : "reserved";
		printf("%s{\"addr\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"type\":\"%s\"",
		       i ? "," : "", a[5], a[4], a[3], a[2], a[1], a[0],
		       d->type == 1 ? "public" : "random");
		if (*rnd)
			printf(",\"random\":\"%s\"", rnd);
		printf(",\"seen\":%d", d->seen);
		if (d->rssi_max > -128) {
			int n = d->rssi_n ? d->rssi_n : 1;
			printf(",\"rssi\":%d,\"rssi_avg\":%.1f,\"rssi_max\":%d", d->rssi_last,
			       (double)d->rssi_sum / n, d->rssi_max);
		}
		if (d->name_len) {
			fputs(",\"name\":", stdout);
			json_str((const uint8_t *)d->name, d->name_len);
		}
		if (d->has_flags)
			printf(",\"flags\":%d", d->adflags);
		if (d->has_tx)
			printf(",\"tx_power\":%d", d->tx);
		if (d->has_app)
			printf(",\"appearance\":%d", d->appearance);
		if (d->company >= 0) {
			const char *cn = company_name(d->company);
			printf(",\"company\":%d", d->company);
			if (cn)
				printf(",\"company_name\":\"%s\"", cn);
			fputs(",\"mfr_data\":\"", stdout);
			for (int k = 0; k < d->mfr_len; k++)
				printf("%02x", d->mfr[k]);
			putchar('"');
		}
		if (d->n16) {
			fputs(",\"uuid16\":[", stdout);
			for (int k = 0; k < d->n16; k++)
				printf("%s\"%04x\"", k ? "," : "", d->uuid16[k]);
			putchar(']');
		}
		if (d->n128) {
			fputs(",\"uuid128\":[", stdout);
			for (int k = 0; k < d->n128; k++) {
				char u[40];
				uuid128_str(u, d->uuid128[k]);
				printf("%s\"%s\"", k ? "," : "", u);
			}
			putchar(']');
		}
		if (*d->beacon)
			printf(",\"beacon\":%s", d->beacon);
		if (d->tracker)
			printf(",\"tracker\":\"%s\"", d->tracker);
		putchar('}');
	}
	puts("]}");
}

static const char *status_str(int st)
{
	switch (st) {
	case 0x0a: return "busy";
	case 0x0f: return "not powered";
	case 0x11: return "invalid index";
	case 0x14: return "permission denied";
	}
	return "start discovery failed";
}

int main(int argc, char **argv)
{
	int index = 0, secs = 10, rounds = 0, opt;
	const char *err = NULL;

	while ((opt = getopt(argc, argv, "i:d:")) != -1) {
		if (opt == 'i')
			index = atoi(optarg);
		else if (opt == 'd')
			secs = atoi(optarg);
		else {
			fprintf(stderr, "usage: %s [-i index] [-d seconds]\n", argv[0]);
			return 2;
		}
	}
	if (secs < 1 || secs > 120)
		secs = 10;

	int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	struct sockaddr_hci sa = { AF_BLUETOOTH, HCI_DEV_NONE, HCI_CHANNEL_CONTROL };
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("bind");
		return 1;
	}

	uint8_t dtype = DISCOV_LE;
	long deadline = now_ms() + secs * 1000L;
	int retried_busy = 0, discovering = 0;

	send_cmd(fd, MGMT_OP_START_DISCOVERY, index, &dtype, 1);
	rounds = 1;

	uint8_t buf[1024];
	for (;;) {
		long left = deadline - now_ms();
		if (left <= 0 || err)
			break;
		struct pollfd pfd = { fd, POLLIN, 0 };
		if (poll(&pfd, 1, (int)left) <= 0)
			continue;
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n < 6)
			continue;
		uint16_t ev = le16(buf), idx = le16(buf + 2), len = le16(buf + 4);
		if (idx != index || 6 + len > n)
			continue;
		const uint8_t *p = buf + 6;

		if ((ev == MGMT_EV_CMD_COMPLETE || ev == MGMT_EV_CMD_STATUS) && len >= 3 &&
		    le16(p) == MGMT_OP_START_DISCOVERY && p[2] != 0) {
			if (p[2] == 0x0a && !retried_busy) {    /* a previous run left it on */
				/*
				 * Stop it, and start again once the kernel says
				 * discovery has stopped (MGMT_EV_DISCOVERING 0,
				 * below): a start sent while it is still stopping
				 * is busy too.
				 */
				retried_busy = 1;
				rounds = 0;
				send_cmd(fd, MGMT_OP_STOP_DISCOVERY, index, &dtype, 1);
			} else {
				err = status_str(p[2]);
			}
		} else if ((ev == MGMT_EV_CMD_COMPLETE || ev == MGMT_EV_CMD_STATUS) && len >= 3 &&
			   le16(p) == MGMT_OP_STOP_DISCOVERY && p[2] != 0 && retried_busy && !rounds) {
			err = status_str(0x0a);         /* what was left on could not be stopped */
		} else if (ev == MGMT_EV_DISCOVERING && len >= 2) {
			discovering = p[1];
			if (!discovering && deadline - now_ms() > 1500) {
				send_cmd(fd, MGMT_OP_START_DISCOVERY, index, &dtype, 1);
				rounds++;
			}
		} else if (ev == MGMT_EV_DEVICE_FOUND) {
			on_device_found(p, len);
		}
	}

	if (!err) {
		/* stop, and wait briefly for the kernel to confirm */
		send_cmd(fd, MGMT_OP_STOP_DISCOVERY, index, &dtype, 1);
		long stop_by = now_ms() + 1000;
		while (discovering && now_ms() < stop_by) {
			struct pollfd pfd = { fd, POLLIN, 0 };
			if (poll(&pfd, 1, (int)(stop_by - now_ms())) <= 0)
				break;
			ssize_t n = read(fd, buf, sizeof(buf));
			if (n >= 8 && le16(buf) == MGMT_EV_DISCOVERING && le16(buf + 2) == index)
				discovering = buf[7];
		}
	}
	close(fd);
	print_json(index, secs, rounds, err);
	return 0;
}
