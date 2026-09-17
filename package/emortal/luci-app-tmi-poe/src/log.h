/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TMI_POE_LOG_H
#define TMI_POE_LOG_H

#include "tmi.h"

struct tmi_port_log {
	bool initialized;
	bool detection;
	bool classification;
	bool classification_event;
	bool waiting;
	uint8_t faults;
};

struct tmi_log {
	struct tmi_status previous;
	struct tmi_port_log ports[TMI_MAX_PORTS];
	uint8_t supply_events;
	uint8_t detection_events;
	bool initialized;
	bool debug;
};

void tmi_log_status(const struct tmi_board *board, struct tmi_log *log,
		    const struct tmi_status *now, bool debug);
void tmi_log_ports_off(const struct tmi_board *board, struct tmi_log *log,
		       uint8_t mask, const char *reason);
void tmi_log_policy(const struct tmi_board *board, const struct tmi_policy *policy,
		    bool enabled, const char *action);

#endif
