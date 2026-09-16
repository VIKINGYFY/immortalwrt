/* SPDX-License-Identifier: GPL-2.0-only */
#include "tmi.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* Mode/reset/event semantics: TMI7604R V1.3 and TMI7608R V0.3, pp.14-23.
 * Board wiring, ADC packing and undocumented setup values: RP01/RP02 pse_ctl.
 * These are board-specific settings, not a generic TMI register specification.
 */
const struct tmi_board tmi_boards[2] = {
	{ "xiaomi,be3600-pro-wired-p5", "TMI7604R", 0x50, 4, 4, 60000,
	  { 2, 1, 4, 3 } },
	{ "xiaomi,be3600-pro-wired-p8", "TMI7608R", 0x70, 8, 7, 105000,
	  { 2, 1, 4, 3, 6, 5, 8 } },
};

static int read_reg(struct tmi_io *io, uint8_t reg, uint8_t *value)
{
	io->failed_reg = reg;
	io->operation = "read";
	return io->read(io->ctx, reg, value);
}

static int write_reg(struct tmi_io *io, uint8_t reg, uint8_t value)
{
	io->failed_reg = reg;
	io->operation = "write";
	return io->write(io->ctx, reg, value);
}

static int verify_reg(struct tmi_io *io, uint8_t reg, uint8_t expected)
{
	uint8_t value;
	int ret = read_reg(io, reg, &value);

	if (ret)
		return ret;
	if (value == expected)
		return 0;
	io->operation = "verify";
	io->expected = expected;
	io->actual = value;
	return -EIO;
}

static int write_verify(struct tmi_io *io, uint8_t reg, uint8_t value)
{
	int ret = write_reg(io, reg, value);

	return ret ? ret : verify_reg(io, reg, value);
}

static int read_word(struct tmi_io *io, uint8_t reg, unsigned int *value)
{
	uint8_t lo, hi, check;
	int ret;
	unsigned int attempt;

	/* Retry if the high byte changes during the factory two-byte ADC read.
	 * This detects rollover; the PDFs do not specify an atomic sample latch.
	 */
	for (attempt = 0; attempt < 3; attempt++) {
		ret = read_reg(io, reg + 1, &hi);
		if (ret)
			return ret;
		ret = read_reg(io, reg, &lo);
		if (ret)
			return ret;
		ret = read_reg(io, reg + 1, &check);
		if (ret)
			return ret;
		if (hi == check) {
			*value = lo | ((unsigned int)hi << 8);
			return 0;
		}
	}
	return -EAGAIN;
}

static unsigned int voltage_mv(unsigned int raw)
{
	/* 124.5 mV/LSB (datasheet); factory word packing has five fraction bits. */
	return raw * 1245U / 320U;
}

uint8_t tmi_board_mask(const struct tmi_board *board)
{
	uint8_t mask = 0;
	unsigned int port;

	for (port = 0; port < board->ports; port++)
		mask |= 1U << (board->port_map[port] - 1);
	return mask;
}

int tmi_validate(const struct tmi_board *board, const struct tmi_policy *policy)
{
	if (policy->budget_mw < 1000 || policy->budget_mw > board->max_budget_mw ||
	    (policy->mask & ~tmi_board_mask(board)))
		return -EINVAL;
	return 0;
}

static uint8_t port_modes(uint8_t automatic, uint8_t semi, unsigned int group)
{
	uint8_t result = 0;
	unsigned int i, bit;

	for (i = 0; i < 4; i++) {
		bit = 1U << (group * 4 + i);
		/* 00=shutdown, 10=semiautomatic, 11=automatic; never manual. */
		if (automatic & bit)
			result |= 3U << (2 * i);
		else if (semi & bit)
			result |= 2U << (2 * i);
	}
	return result;
}

static int set_modes(struct tmi_io *io, const struct tmi_board *board,
		     uint8_t automatic, uint8_t semi)
{
	unsigned int group;
	int ret;

	/* The four-channel part documents only R1Fh. */
	for (group = 0; group < board->channels / 4; group++) {
		ret = write_verify(io, 0x1f + group, port_modes(automatic, semi, group));
		if (ret)
			return ret;
	}
	return 0;
}

static unsigned int budget_value(unsigned int budget_mw)
{
	/* Factory compatibility: trunc((budget_mw / 53 integer division) / 1.956).
	 * A nominal-53-V current threshold, not a calibrated wattmeter limit.
	 */
	return (budget_mw / 53U) * 1000U / 1956U;
}

static int set_budget(struct tmi_io *io, unsigned int budget_mw)
{
	unsigned int value = budget_value(budget_mw);
	int ret = write_verify(io, 0x77, value & 0xff);

	return ret ? ret : write_verify(io, 0x78, value >> 8);
}

static int check_supply(struct tmi_io *io)
{
	unsigned int attempt, raw, mv;
	int ret;

	io->stage = "supply-check";
	for (attempt = 0; attempt < 20; attempt++) {
		ret = read_word(io, 0x89, &raw);
		if (ret)
			return ret;
		mv = voltage_mv(raw);
		if (mv >= TMI_INPUT_MIN_MV && mv <= TMI_INPUT_MAX_MV)
			return 0;
		io->delay(io->ctx, 100);
	}
	/* No 12/24 V mode or factory 45 V display clamp on these boards. */
	return -ERANGE;
}

static int confirm_off(struct tmi_io *io, uint8_t mask)
{
	unsigned int attempt;
	uint8_t powered, good;
	int ret;

	io->stage = "shutdown-confirm";
	for (attempt = 0; attempt < 20; attempt++) {
		ret = read_reg(io, 0x1c, &powered);
		if (ret)
			return ret;
		ret = read_reg(io, 0x1d, &good);
		if (ret)
			return ret;
		if (!((powered | good) & mask))
			return 0;
		io->delay(io->ctx, 100);
	}
	return -ETIMEDOUT;
}

int tmi_disable(struct tmi_io *io, const struct tmi_board *board)
{
	int ret, first = 0;
	unsigned int group;
	uint8_t expected = 0, actual = 0;
	int first_reg = -1;
	const char *operation = NULL;

	io->stage = "shutdown-modes";
	/* Try every mode group despite bus errors, preserving the first error. */
	for (group = 0; group < board->channels / 4; group++) {
		ret = write_verify(io, 0x1f + group, 0);
		if (ret && !first) {
			first = ret;
			first_reg = io->failed_reg;
			operation = io->operation;
			expected = io->expected;
			actual = io->actual;
		}
	}
	if (first) {
		io->failed_reg = first_reg;
		io->operation = operation;
		io->expected = expected;
		io->actual = actual;
		return first;
	}
	return confirm_off(io, (1U << board->channels) - 1);
}

static int enable_detection(struct tmi_io *io, const struct tmi_board *board,
			    uint8_t retained, uint8_t mask)
{
	int ret;

	/* Stage new ports in semi mode: detection cannot power them on. */
	io->stage = "detection-prepare";
	ret = set_modes(io, board, retained, mask & ~retained);
	if (ret)
		return ret;
	ret = write_verify(io, 0x23, mask);
	if (ret)
		return ret;
	ret = write_verify(io, 0x22, mask);
	if (ret)
		return ret;
	/* Automatic mode powers only valid PDs. Do not issue PWR_ON (R28h). */
	io->stage = "automatic-mode";
	return set_modes(io, board, mask, 0);
}

int tmi_initialize(struct tmi_io *io, const struct tmi_board *board,
		   const struct tmi_policy *policy)
{
	static const uint8_t setup[][2] = {
		{ 0x01, 0xe7 }, { 0x54, 0xe7 }, { 0x32, 0xff }, { 0x76, 0x23 },
		{ 0x2e, 0x33 }, { 0x2f, 0x33 }, { 0x30, 0x33 }, { 0x31, 0x33 },
	};
	unsigned int i;
	int ret = tmi_validate(board, policy);

	io->stage = "validate";
	io->failed_reg = -1;
	if (ret)
		return ret;
	/* Reset pin straps can select auto mode. Quiesce before programming. */
	ret = tmi_disable(io, board);
	if (ret)
		return ret;
	io->stage = "clear-interrupts";
	ret = write_reg(io, 0x2b, 0x80); /* INT_CLR, not RESET_IC (bit 4). */
	if (ret)
		return ret;
	io->stage = "factory-setup";
	for (i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
		ret = write_verify(io, setup[i][0], setup[i][1]);
		if (ret)
			return ret;
	}
	io->stage = "budget";
	ret = set_budget(io, policy->budget_mw);
	if (ret)
		return ret;
	ret = write_verify(io, 0x88, 0x3a);
	if (ret)
		return ret;
	io->stage = "protection";
	/* Retain automatic detection; Class4+ extends classification, not PWR_ON. */
	ret = write_verify(io, 0x2d, policy->class4plus ? tmi_board_mask(board) : 0);
	if (ret)
		return ret;
	ret = write_verify(io, 0x21, tmi_board_mask(board)); /* DC disconnect */
	if (ret)
		return ret;
	ret = check_supply(io);
	if (ret)
		return ret;
	ret = enable_detection(io, board, 0, policy->mask);
	if (ret)
		return ret;
	return tmi_verify_policy(io, board, policy);
}

int tmi_set_policy(struct tmi_io *io, const struct tmi_board *board,
		   const struct tmi_policy *old, const struct tmi_policy *policy)
{
	uint8_t retained = old->mask & policy->mask;
	int ret = tmi_validate(board, policy);

	io->stage = "validate";
	io->failed_reg = -1;
	if (ret)
		return ret;
	if (old->budget_mw == policy->budget_mw && old->mask == policy->mask &&
	    old->class4plus == policy->class4plus)
		return policy->mask ? 0 : confirm_off(io, (1U << board->channels) - 1);
	if (old->budget_mw != policy->budget_mw || old->class4plus != policy->class4plus) {
		/* Update two budget bytes with outputs off, never at a transient limit. */
		ret = tmi_disable(io, board);
		if (ret)
			return ret;
		io->stage = "budget";
		ret = set_budget(io, policy->budget_mw);
		if (ret)
			return ret;
		io->stage = "protection";
		ret = write_verify(io, 0x2d, policy->class4plus ? tmi_board_mask(board) : 0);
		if (ret)
			return ret;
		retained = 0;
	}
	if (policy->mask & ~retained) {
		ret = check_supply(io);
		if (ret)
			return ret;
	}
	ret = enable_detection(io, board, retained, policy->mask);
	if (ret)
		return ret;
	if (!policy->mask || (old->mask & ~policy->mask)) {
		ret = confirm_off(io, !policy->mask ? (1U << board->channels) - 1 :
				  old->mask & ~policy->mask);
		if (ret)
			return ret;
	}
	return tmi_verify_policy(io, board, policy);
}

int tmi_verify_policy(struct tmi_io *io, const struct tmi_board *board,
		      const struct tmi_policy *policy)
{
	unsigned int group, value = budget_value(policy->budget_mw);
	int ret;

	io->stage = "configuration-monitor";
	for (group = 0; group < board->channels / 4; group++) {
		ret = verify_reg(io, 0x1f + group, port_modes(policy->mask, 0, group));
		if (ret)
			return ret;
	}
	ret = verify_reg(io, 0x77, value & 0xff);
	if (ret)
		return ret;
	ret = verify_reg(io, 0x78, value >> 8);
	if (ret)
		return ret;
	return verify_reg(io, 0x2d, policy->class4plus ? tmi_board_mask(board) : 0);
}

int tmi_read_status(struct tmi_io *io, const struct tmi_board *board,
		    struct tmi_status *status)
{
	struct tmi_status sample = { 0 };
	unsigned int raw, port, channel, i;
	int ret;

	io->stage = "status";
	for (i = 0; i < board->channels / 4; i++) {
		ret = read_reg(io, 0x1f + i, &sample.modes[i]);
		if (ret)
			return ret;
	}
	ret = read_reg(io, 0x22, &sample.detect);
	if (ret)
		return ret;
	ret = read_reg(io, 0x23, &sample.classify);
	if (ret)
		return ret;
	ret = read_word(io, 0x77, &sample.budget_raw);
	if (ret)
		return ret;
	ret = read_reg(io, 0x2d, &sample.class4plus);
	if (ret)
		return ret;
	ret = read_reg(io, 0x00, &sample.summary);
	if (ret)
		return ret;
	ret = read_reg(io, 0x1c, &sample.powered);
	if (ret)
		return ret;
	ret = read_reg(io, 0x1d, &sample.good);
	if (ret)
		return ret;
	/* Even addresses are read-only event latches; odd addresses clear them. */
	for (i = 0; i < TMI_EVENTS; i++) {
		ret = read_reg(io, 0x02 + 2 * i, &sample.events[i]);
		if (ret)
			return ret;
	}
	ret = read_word(io, 0x89, &raw);
	if (ret)
		return ret;
	sample.input_mv = voltage_mv(raw);
	for (port = 0; port < board->ports; port++) {
		channel = board->port_map[port] - 1;
		ret = read_reg(io, 0x14 + channel, &sample.port_state[port]);
		if (ret)
			return ret;
		ret = read_word(io, 0x33 + 4 * channel, &raw);
		if (ret)
			return ret;
		/* Preserve raw ADC: Class4+ uses twice the af/at current scale.
		 * The supplied PDFs do not define the active-range status encoding;
		 * an enabled Class4+ bit alone does not identify the detected PD class.
		 */
		sample.current_raw[port] = raw;
		sample.current_ma[port] = raw * 1956U / 16000U;
		ret = read_word(io, 0x35 + 4 * channel, &raw);
		if (ret)
			return ret;
		sample.voltage_mv[port] = voltage_mv(raw);
	}
	/* Never publish a partly updated snapshot after an I2C failure. */
	*status = sample;
	return 0;
}

int tmi_poll_status(struct tmi_io *io, const struct tmi_board *board,
		    struct tmi_status *status)
{
	struct tmi_status sample;
	unsigned int i;
	uint8_t event;
	int ret;

	/* Only the owning daemon consumes the clear-on-read aliases. Never
	 * retry these reads in the transport: a failed transfer may have cleared
	 * the latch already. Keep every successful read across later failures.
	 */
	io->stage = "events";
	for (i = 0; i < TMI_EVENTS; i++) {
		ret = read_reg(io, 0x03 + 2 * i, &event);
		if (ret)
			return ret;
		io->pending_events[i] |= event;
	}
	ret = tmi_read_status(io, board, &sample);
	if (ret)
		return ret;
	memcpy(sample.events, io->pending_events, sizeof(sample.events));
	memset(io->pending_events, 0, sizeof(io->pending_events));
	*status = sample;
	return 0;
}
