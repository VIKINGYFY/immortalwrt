/*
 * airscan-fft: aggregate ath10k and ath11k spectral-scan samples into
 * per-window spectra.
 *
 * usage: airscan-fft <file>   (the debugfs relay file spectral_scan0, or a copy)
 *
 * Input is a stream of TLV samples (spectral_common.h), u8 type, be16 length:
 *   ATH_FFT_SAMPLE_ATH11K = 4: u8 chan_width, s8 max_index, u8 max_exp,
 *   be16 freq1, be16 freq2, be16 max_magnitude, be16 rssi, be32 tsf,
 *   be32 noise, u8 bins[length - 19].
 *   ATH_FFT_SAMPLE_ATH10K = 3: u8 chan_width, be16 freq1, be16 freq2,
 *   be16 noise, be16 max_magnitude, be16 total_gain_db, be16 base_pwr_db,
 *   be64 tsf, s8 max_index, u8 rssi, u8 relpwr_db, u8 avgpwr_db, u8 max_exp,
 *   u8 bins[length - 26]. The QCA988x says 22 for a 20 MHz window, so a
 *   width is rounded to 20, 40, 80 or 160.
 *
 * Output is one JSON array, one object per (centre frequency, width, bins):
 *   centre, width, bins, samples, noise (median), pwr_lo, pwr_hist[], avg[], max[]
 * Per-bin power is the fft_eval ath10k estimate,
 *   P_i = noise + rssi + 20 log10(v_i) - 10 log10(sum v^2),  v = max(bin,1) << max_exp,
 * averaged in linear units. pwr_hist counts samples by in-band power
 * (noise + rssi, dBm) in 1 dB steps from pwr_lo; airscan derives busy time
 * from it against a fixed level (-82 dBm per 20 MHz, +3 dB per doubling of
 * width). A sample whose noise or in-band power is out of all reason
 * (corrupt) is dropped: its power would be no number at all.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BINS 512
#define MAX_WIN 64

struct win {
	int centre, width, nbins, samples;
	double lin[MAX_BINS], max[MAX_BINS];
	int *pin;        /* per-sample in-band power, dBm */
	int *noise;
	int cap;
};

static struct win wins[MAX_WIN];
static int nwin;

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static struct win *get_win(int centre, int width, int nbins)
{
	for (int i = 0; i < nwin; i++)
		if (wins[i].centre == centre && wins[i].width == width && wins[i].nbins == nbins)
			return &wins[i];
	if (nwin == MAX_WIN)
		return NULL;
	struct win *w = &wins[nwin++];
	memset(w, 0, sizeof(*w));
	w->centre = centre;
	w->width = width;
	w->nbins = nbins;
	for (int i = 0; i < nbins; i++)
		w->max[i] = -1;
	return w;
}

static int cmp_i(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

static int round_width(int mhz)
{
	return mhz < 30 ? 20 : mhz < 60 ? 40 : mhz < 120 ? 80 : 160;
}

static void add_sample(int type, const uint8_t *s, int len)
{
	int nbins, width, max_exp, centre, rssi, noise;
	const uint8_t *bins;

	if (type == 4) {
		nbins = len - 19;
		width = s[0];
		max_exp = s[2];
		centre = be16(s + 3);
		rssi = (int16_t)be16(s + 9);
		noise = (int32_t)be32(s + 15);
		bins = s + 19;
	} else {
		nbins = len - 26;
		width = round_width(s[0]);
		centre = be16(s + 1);
		noise = (int16_t)be16(s + 5);
		rssi = (int8_t)s[22];
		max_exp = s[25];
		bins = s + 26;
	}
	if (nbins < 1 || nbins > MAX_BINS)
		return;
	if (noise < -150 || noise > 0 || noise + rssi < -150 || noise + rssi > 20 || max_exp > 15)
		return;
	struct win *w = get_win(centre, width, nbins);
	if (!w)
		return;

	double v2[MAX_BINS], sq = 0;
	for (int i = 0; i < nbins; i++) {
		double v = (double)((bins[i] ? bins[i] : 1) << max_exp);
		v2[i] = v * v;
		sq += v2[i];
	}
	double scale = pow(10.0, (noise + rssi) / 10.0) / sq;
	for (int i = 0; i < nbins; i++) {
		double p = scale * v2[i];
		w->lin[i] += p;
		if (p > w->max[i])
			w->max[i] = p;
	}
	if (w->samples == w->cap) {
		w->cap = w->cap ? 2 * w->cap : 256;
		w->pin = realloc(w->pin, w->cap * sizeof(*w->pin));
		w->noise = realloc(w->noise, w->cap * sizeof(*w->noise));
		if (!w->pin || !w->noise)
			exit(1);
	}
	int pin = noise + rssi;   /* clamped: a corrupt sample must not widen the histogram */
	w->pin[w->samples] = pin < -150 ? -150 : pin > 20 ? 20 : pin;
	w->noise[w->samples] = noise;
	w->samples++;
}

static double db(double v)
{
	return v > 0 && isfinite(v) ? 10.0 * log10(v) : -200.0;
}

int main(int argc, char **argv)
{
	FILE *f = argc > 1 ? fopen(argv[1], "rb") : stdin;
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	size_t cap = 1 << 20, len = 0;
	uint8_t *buf = malloc(cap);
	size_t n;
	while (buf && (n = fread(buf + len, 1, cap - len, f)) > 0) {
		len += n;
		if (len == cap)
			buf = realloc(buf, cap *= 2);
	}
	if (!buf)
		return 1;

	size_t off = 0;
	while (off + 3 <= len) {
		int type = buf[off], l = be16(buf + off + 1);
		if (off + 3 + l > len)
			break;
		if ((type == 4 && l > 19) || (type == 3 && l > 26))
			add_sample(type, buf + off + 3, l);
		off += 3 + l;
	}

	printf("[");
	for (int k = 0; k < nwin; k++) {
		struct win *w = &wins[k];
		if (!w->samples)
			continue;
		int lo = w->pin[0], hi = w->pin[0];
		for (int i = 1; i < w->samples; i++) {
			if (w->pin[i] < lo)
				lo = w->pin[i];
			if (w->pin[i] > hi)
				hi = w->pin[i];
		}
		qsort(w->noise, w->samples, sizeof(int), cmp_i);
		printf("%s{\"center\":%d,\"width\":%d,\"bins\":%d,\"samples\":%d,\"noise\":%d,\"pwr_lo\":%d,\"pwr_hist\":[",
		       k ? "," : "", w->centre, w->width, w->nbins, w->samples, w->noise[w->samples / 2], lo);
		for (int p = lo; p <= hi; p++) {
			int c = 0;
			for (int i = 0; i < w->samples; i++)
				c += w->pin[i] == p;
			printf("%s%d", p > lo ? "," : "", c);
		}
		printf("],\"avg\":[");
		for (int i = 0; i < w->nbins; i++)
			printf("%s%.1f", i ? "," : "", db(w->lin[i] / w->samples));
		printf("],\"max\":[");
		for (int i = 0; i < w->nbins; i++)
			printf("%s%.1f", i ? "," : "", db(w->max[i]));
		printf("]}");
	}
	printf("]\n");
	return 0;
}
