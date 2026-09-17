/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TMI_POE_H
#define TMI_POE_H

#include <stdbool.h>
#include <stdint.h>

#define TMI_MAX_PORTS 8
#define TMI_EVENTS 9
#define TMI_INPUT_MIN_MV 44000
#define TMI_INPUT_MAX_MV 57000

struct tmi_board {
	const char *compatible;
	const char *chip;
	unsigned int address;
	unsigned int channels;
	unsigned int ports;
	unsigned int max_budget_mw;
	uint8_t port_map[TMI_MAX_PORTS]; /* LAN number -> one-based PSE channel */
};

struct tmi_io {
	void *ctx;
	int (*read)(void *ctx, uint8_t reg, uint8_t *value);
	int (*write)(void *ctx, uint8_t reg, uint8_t value);
	void (*delay)(void *ctx, unsigned int ms);
	const char *stage;
	int failed_reg; /* -1 when the failing stage has not accessed a register. */
	const char *operation;
	uint8_t expected;
	uint8_t actual;
	/* Preserve consumed events until a complete sample can be delivered. */
	uint8_t pending_events[TMI_EVENTS];
};

struct tmi_policy {
	unsigned int budget_mw;
	uint8_t mask;
	bool class4plus;
	bool debug;
};

struct tmi_status {
	uint8_t modes[2];
	uint8_t detect;
	uint8_t classify;
	uint8_t disconnect;
	uint8_t class4plus;
	unsigned int budget_raw;
	uint8_t summary;
	uint8_t powered;
	uint8_t good;
	uint8_t events[TMI_EVENTS];
	uint8_t port_state[TMI_MAX_PORTS];
	unsigned int input_mv;
	unsigned int current_ma[TMI_MAX_PORTS];
	unsigned int current_raw[TMI_MAX_PORTS];
	unsigned int voltage_mv[TMI_MAX_PORTS];
};

extern const struct tmi_board tmi_boards[2];
uint8_t tmi_board_mask(const struct tmi_board *board);
int tmi_validate(const struct tmi_board *board, const struct tmi_policy *policy);
int tmi_initialize(struct tmi_io *io, const struct tmi_board *board,
		   const struct tmi_policy *policy);
int tmi_set_policy(struct tmi_io *io, const struct tmi_board *board,
		   const struct tmi_policy *old, const struct tmi_policy *policy);
int tmi_disable(struct tmi_io *io, const struct tmi_board *board);
int tmi_verify_policy(struct tmi_io *io, const struct tmi_board *board,
		      const struct tmi_policy *policy);
int tmi_check_policy_status(struct tmi_io *io, const struct tmi_board *board,
			    const struct tmi_policy *policy, const struct tmi_status *status);
int tmi_read_status(struct tmi_io *io, const struct tmi_board *board,
		    struct tmi_status *status);
int tmi_poll_status(struct tmi_io *io, const struct tmi_board *board,
		    struct tmi_status *status);

#endif
