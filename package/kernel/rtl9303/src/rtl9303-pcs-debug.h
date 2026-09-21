/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/jiffies.h>

/* Called under the PCS mutex. One initial sample and subsequent changes only,
 * at least five seconds apart, with a hard cap per provider lifetime.
 */
static void rtl9303_pcs_debug(struct rtpcs_link *link,
			      const struct phylink_link_state *state)
{
	struct rtpcs_ctrl *ctrl = link->ctrl;
	u32 poll, type, force, snapshot;
	bool serdes_link;

	if (link->port != 8 || link->diag_count >= 16 ||
	    (link->diag_count && time_before(jiffies, link->diag_next)))
		return;
	link->diag_next = jiffies + 5 * HZ;
	serdes_link = rtpcs_93xx_sds_10gr_link_up(link->sds);
	snapshot = (state->link << 31) | (serdes_link << 30) |
		   ((u32)state->speed & 0xffff);
	if (link->diag_count && snapshot == link->diag_state)
		return;
	if (regmap_read(ctrl->map, 0xca90, &poll) ||
	    regmap_read(ctrl->map, 0xca04, &type) ||
	    regmap_read(ctrl->map, 0xca3c, &force))
		return;
	link->diag_state = snapshot;
	dev_info(ctrl->dev,
		 "cr1000a-diag PCS port8 sds3: serdes=%u mac=%u speed=%d poll=%08x type=%08x force=%08x (%u/16)\n",
		 serdes_link, state->link, state->speed, poll, type, force,
		 ++link->diag_count);
}
