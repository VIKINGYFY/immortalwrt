/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TMI_POE_LOG_SINK_H
#define TMI_POE_LOG_SINK_H

#include <stdbool.h>

int tmi_log_init(bool debug);
int tmi_log_clear(void);
void tmi_log_set_debug(bool debug);
void tmi_log_close(void);
#ifdef syslog
#define tmi_log_message syslog
#endif

void tmi_log_message(int priority, const char *format, ...);

#endif
