/* SPDX-License-Identifier: GPL-2.0-only */
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <syslog.h>

/* TMI7604R V1.3 / TMI7608R V0.3, pp.16-23. The PDFs document event
 * meanings, but not the port-status classification encoding. Never infer
 * Class 4/4+ from 0x44 or from the configured Class4+ enable bit.
 */
enum {
	EV_PGOOD = 0, EV_DETECTION = 2, EV_CLASSIFICATION = 3,
	EV_OVERCURRENT = 4, EV_DISCONNECT = 5, EV_STARTUP = 6,
	EV_CURRENT_LIMIT = 7, EV_SUPPLY = 8,
};

static unsigned int port_mode(const struct tmi_status *s, unsigned int channel)
{
	return (s->modes[channel / 4] >> (2 * (channel % 4))) & 3;
}

static void fault_reason(uint8_t faults, char *text, size_t size)
{
	snprintf(text, size, "%s%s%s", (faults & 1) ? "overcurrent" : "",
		 (faults & 2) ? ((faults & 1) ? ",startup-timeout" : "startup-timeout") : "",
		 (faults & 4) ? ((faults & 3) ? ",current-limit" : "current-limit") : "");
}

void tmi_log_policy(const struct tmi_board *board, const struct tmi_policy *policy,
		    bool enabled, const char *action)
{
	char ports[64] = "";
	unsigned int p;
	size_t used = 0;

	for (p = 0; p < board->ports; p++)
		if (policy->mask & (1U << (board->port_map[p] - 1)))
			used += snprintf(ports + used, sizeof(ports) - used,
					 "%sLAN%u", used ? "," : "", p + 1);
	syslog(LOG_INFO, "PoE %s: switch=%s, ports=%s, budget=%u.%03uW, Class4+=%s, mode=%s",
	       action, enabled ? "enabled" : "disabled", used ? ports : "none",
	       policy->budget_mw / 1000, policy->budget_mw % 1000,
	       policy->class4plus ? "enabled" : "disabled", policy->mask ? "automatic" : "shutdown");
}

void tmi_log_ports_off(const struct tmi_board *board, struct tmi_log *log,
		       uint8_t mask, const char *reason)
{
	unsigned int p, bit;

	/* Called only after the corresponding outputs were confirmed off. */
	for (p = 0; p < board->ports; p++) {
		bit = 1U << (board->port_map[p] - 1);
		if (!(mask & bit))
			continue;
		if ((log->previous.powered | log->previous.good) & bit)
			syslog(LOG_INFO, "LAN%u power off: reason=%s", p + 1, reason);
		memset(&log->ports[p], 0, sizeof(log->ports[p]));
	}
	log->previous.powered &= ~mask;
	log->previous.good &= ~mask;
}

void tmi_log_status(const struct tmi_board *board, struct tmi_log *log,
		    const struct tmi_status *now, bool debug)
{
	const struct tmi_status *old = &log->previous;
	unsigned int p, channel, bit, mode, i;

	if (debug && (!log->initialized || memcmp(old->modes, now->modes, sizeof(now->modes)) ||
	    old->detect != now->detect || old->classify != now->classify ||
	    old->class4plus != now->class4plus || old->budget_raw != now->budget_raw))
		syslog(LOG_DEBUG, "hardware: modes=0x%02x/0x%02x detect=0x%02x classify=0x%02x class4plus=0x%02x budget=0x%04x",
		       now->modes[0], now->modes[1], now->detect, now->classify,
		       now->class4plus, now->budget_raw);
	if (debug)
		for (i = 0; i < TMI_EVENTS; i++)
			if (now->events[i] && (!log->initialized || now->events[i] != old->events[i]))
				syslog(LOG_DEBUG, "event: register=0x%02x value=0x%02x", 0x02 + 2 * i, now->events[i]);
	if (now->events[EV_SUPPLY] & ~log->supply_events) {
		if ((now->events[EV_SUPPLY] & ~log->supply_events) & 0x80)
			syslog(LOG_WARNING, "PoE controller reports a thermal shutdown event");
		if ((now->events[EV_SUPPLY] & ~log->supply_events) & 0x7f)
			syslog(LOG_WARNING, "PoE controller reports a supply event: cause=unconfirmed");
		log->supply_events |= now->events[EV_SUPPLY];
	}
	if (log->supply_events && !now->events[EV_SUPPLY] && now->powered &&
	    now->powered == now->good) {
		syslog(LOG_NOTICE, "PoE controller power recovered");
		log->supply_events = 0;
	}
	for (p = 0; p < board->ports; p++) {
		struct tmi_port_log *port = &log->ports[p];
		bool was_on, on, good, disconnected, lost, automatic;
		uint8_t faults;
		char reason[64];

		channel = board->port_map[p] - 1;
		bit = 1U << channel;
		mode = port_mode(now, channel);
		was_on = port->initialized && (old->powered & bit);
		on = now->powered & bit;
		good = now->good & bit;
		disconnected = now->events[EV_DISCONNECT] & bit;
		lost = was_on && (!on || disconnected);
		automatic = mode == 3 && (now->detect & now->classify & bit);
		faults = (!!(now->events[EV_OVERCURRENT] & bit)) |
			 (!!(now->events[EV_STARTUP] & bit) << 1) |
			 (!!(now->events[EV_CURRENT_LIMIT] & bit) << 2);
		fault_reason(faults, reason, sizeof(reason));
		if (!on && good && (!port->initialized || (old->powered & bit) || !(old->good & bit)))
			syslog(LOG_WARNING, "LAN%u power status inconsistent: output=off, power-good=yes", p + 1);
		if (lost) {
			syslog(LOG_NOTICE, "LAN%u %s: reason=%s", p + 1,
			       on ? "power interruption observed; output is already on" : "power off",
			       faults ? reason : disconnected ? "PD-disconnected" : "unconfirmed");
			port->detection = port->classification = port->waiting = false;
		}
		if (!lost && was_on && on && good == !!(old->good & bit) &&
		    (now->events[EV_PGOOD] & bit))
			syslog(LOG_NOTICE, "LAN%u power-good change occurred between samples: current=%s",
			       p + 1, good ? "good" : "not-good");
		if ((faults & ~port->faults) && !lost)
			syslog(LOG_WARNING, "LAN%u power fault reported: reason=%s, output=%s",
			       p + 1, reason, on ? "on" : "off");
		port->faults |= faults;
		if (!port->initialized || lost || port_mode(old, channel) != mode ||
		    (((old->detect ^ now->detect) | (old->classify ^ now->classify)) & bit)) {
			if (!mode) {
				syslog(LOG_INFO, "LAN%u PoE disabled", p + 1);
				port->detection = port->classification = port->waiting = false;
			} else if (automatic && !on && !port->waiting) {
				syslog(LOG_INFO, "LAN%u waiting for a valid PoE device", p + 1);
				port->waiting = true;
			} else if (!automatic) {
				syslog(LOG_WARNING, "LAN%u automatic PoE detection is not active", p + 1);
			}
		}
		/* Empty/non-PD probes also set DET. Advance the normal log only
		 * when automatic classification or confirmed power establishes that
		 * the controller progressed beyond probing. The raw DET is debug-only.
		 */
		if (automatic && (!was_on || lost) &&
		    ((now->events[EV_CLASSIFICATION] & bit) || (on && good))) {
			if (!port->detection) {
				syslog(LOG_INFO, "LAN%u detection completed", p + 1);
				port->detection = true;
			}
			if (!port->classification) {
				syslog(LOG_INFO, "LAN%u classification completed: class=unconfirmed", p + 1);
				port->classification = true;
			}
		}
		if (on && (!was_on || lost || !port->initialized || (!!(old->good & bit) != good))) {
			if (good) {
				if (port->faults)
					syslog(LOG_NOTICE, "LAN%u power recovered", p + 1);
				syslog(LOG_INFO, "LAN%u power on: negotiation=%s, class=unconfirmed, power-good=yes",
				       p + 1, automatic ? "completed" : "unconfirmed");
				port->faults = 0;
				port->detection = port->classification = true;
			} else {
				syslog(was_on && (old->good & bit) ? LOG_WARNING : LOG_INFO,
				       "LAN%u power %s: power-good=%s", p + 1,
				       was_on && (old->good & bit) ? "unstable" : "starting",
				       was_on && (old->good & bit) ? "lost" : "pending");
			}
			port->waiting = false;
		}
		if (debug && (!port->initialized || old->port_state[p] != now->port_state[p] ||
		    ((old->powered ^ now->powered) & bit) || ((old->good ^ now->good) & bit)))
			syslog(LOG_DEBUG, "LAN%u: pse=%u state=0x%02x powered=%u good=%u voltage-mv=%u current-raw=0x%04x",
			       p + 1, channel + 1, now->port_state[p], on, good,
			       now->voltage_mv[p], now->current_raw[p]);
		port->initialized = true;
	}
	log->previous = *now;
	log->initialized = true;
}
