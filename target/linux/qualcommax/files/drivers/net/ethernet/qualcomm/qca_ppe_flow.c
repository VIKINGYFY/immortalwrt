// SPDX-License-Identifier: GPL-2.0-or-later OR MIT
/* Copyright (c) 2026 Julius Bairaktaris <julius@bairaktaris.de> */
/* Routed and NAT flow offload for the Qualcomm PPE.
 *
 * The PPE matches a packet's 5-tuple against a hashed flow table and, on a hit,
 * forwards it in hardware. The key is split across two tables: the flow entry
 * carries the destination address, the L4 ports and the protocol, while the
 * source address lives in the host table and the flow entry only references its
 * index. Every entry is staged with the packet source in the host table;
 * keep that key orientation for each direction, including PPPoE ingress.
 *
 * Entries are placed by the hardware hash rather than by the driver: an add
 * stages the entry in the op registers, and the hardware picks the slot, writes
 * the host index into the entry itself, and reports both back.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>

#include "qca_ppe.h"

#define PPE_FLOW_OP_RETRIES	1000

/* The result register reports how many results are queued and a read pops one,
 * so the value has to be captured on the poll that sees it, not re-read after.
 */
static int ppe_flow_op_wait(struct qca_ppe_priv *priv, u32 rslt_reg,
			    u32 cmd_id, u32 *rslt)
{
	u32 val, valid = rslt_reg == PPE_HOST_TBL_OP_RSLT ?
		PPE_HOST_RSLT_VALID_CNT : PPE_FLOW_RSLT_VALID_CNT;
	int i, ret;

	for (i = 0; i < PPE_FLOW_OP_RETRIES; i++) {
		ret = regmap_read(priv->regmap, rslt_reg, &val);
		if (ret)
			return ret;
		if ((val & valid) &&
		    FIELD_GET(PPE_FLOW_RSLT_CMD_ID, val) == cmd_id) {
			*rslt = val;
			return 0;
		}
		udelay(1);
	}

	return -ETIMEDOUT;
}

static u32 ppe_flow_next_cmd_id(struct qca_ppe_priv *priv)
{
	priv->flow_cmd_id = (priv->flow_cmd_id + 1) & PPE_FLOW_RSLT_CMD_ID;

	return priv->flow_cmd_id;
}

/* Add, delete or look up one flow entry by key. @host carries the source
 * address half of the key and is staged alongside, so that an add creates both
 * halves and binds them in one command. On success @index and @host_index name
 * the slots the hardware chose.
 */
int ppe_flow_op(struct qca_ppe_priv *priv, u32 op_type,
		const u32 *entry, int nentry, const u32 *host, int nhost,
		u32 *index, u32 *host_index)
{
	u32 cmd_id;
	int ret, i;

	lockdep_assert_held(&priv->flow_lock);
	if (priv->flow_op_busy || priv->host_retire_pending)
		return -EBUSY;

	for (i = 0; i < nhost; i++) {
		ret = regmap_write(priv->regmap, PPE_FLOW_HOST_TBL_OP_DATA(i),
				   host[i]);
		if (ret)
			return ret;
	}
	ret = regmap_write(priv->regmap, PPE_FLOW_HOST_TBL_OP,
		     FIELD_PREP(PPE_FLOW_HOST_OP_HASH_BLOCK,
				PPE_FLOW_HASH_BLOCKS));
	if (ret)
		return ret;

	for (i = 0; i < nentry; i++) {
		ret = regmap_write(priv->regmap, PPE_FLOW_TBL_OP_DATA(i), entry[i]);
		if (ret)
			return ret;
	}

	cmd_id = ppe_flow_next_cmd_id(priv);
	ret = regmap_write(priv->regmap, PPE_FLOW_TBL_OP,
		     FIELD_PREP(PPE_FLOW_OP_CMD_ID, cmd_id) |
		     FIELD_PREP(PPE_FLOW_OP_TYPE, op_type) |
		     FIELD_PREP(PPE_FLOW_OP_HASH_BLOCK, PPE_FLOW_HASH_BLOCKS) |
		     PPE_FLOW_OP_HOST_EN);
	if (ret)
		return ret;

	priv->flow_op_busy = true;
	priv->flow_op_result_valid = false;
	priv->flow_op_cmd_id = cmd_id;
	priv->flow_op_rslt_reg = PPE_FLOW_TBL_OP_RSLT;
	return ppe_flow_op_finish(priv, index, host_index);
}

/* Finish an issued mutation before another writer can touch its staging area.
 * Preserve a consumed result while its host-index readback is unavailable.
 */
int ppe_flow_op_finish(struct qca_ppe_priv *priv, u32 *index, u32 *host_index)
{
	u32 rslt, val, idx;
	int ret;

	lockdep_assert_held(&priv->flow_lock);
	if (!priv->flow_op_busy)
		return -EINVAL;
	if (!priv->flow_op_result_valid) {
		ret = ppe_flow_op_wait(priv, priv->flow_op_rslt_reg,
				       priv->flow_op_cmd_id, &rslt);
		if (ret)
			return ret;
		priv->flow_op_result = rslt;
		priv->flow_op_result_valid = true;
	}
	rslt = priv->flow_op_result;
	if (rslt & PPE_FLOW_RSLT_FAIL) {
		priv->flow_op_busy = false;
		return -ENOENT;
	}
	idx = FIELD_GET(PPE_FLOW_RSLT_ENTRY_IDX, rslt);
	if ((index || host_index) && idx >= priv->data->num_flow_entries)
		return -EIO;
	if (index)
		*index = idx;
	if (host_index) {
		ret = regmap_read(priv->regmap, PPE_IN_FLOW_TBL(idx), &val);
		if (ret)
			return ret;
		*host_index = ppe_entry_get(&val, PPE_FLOW_E_HOST_IDX_OFF,
					    PPE_FLOW_E_HOST_IDX_LEN);
	}
	priv->flow_op_busy = false;
	return 0;
}

/* Read one entry back by index. An IPv6 entry spans two of the flat table's
 * slots and the hardware distributes its words across them, so the flat mapping
 * cannot be used to read an entry of either family with one layout — the op
 * engine returns the words as they were staged.
 */
int ppe_flow_entry_read(struct qca_ppe_priv *priv, u32 index, u32 *words,
			int nwords)
{
	u32 cmd_id, rslt, read[PPE_FLOW_ENTRY_WORDS_V6];
	int ret, i;

	lockdep_assert_held(&priv->flow_lock);
	if (priv->flow_op_busy)
		return -EBUSY;
	if (nwords <= 0 || nwords > PPE_FLOW_ENTRY_WORDS_V6)
		return -EINVAL;

	cmd_id = ppe_flow_next_cmd_id(priv);
	ret = regmap_write(priv->regmap, PPE_FLOW_TBL_RD_OP,
		     FIELD_PREP(PPE_FLOW_OP_CMD_ID, cmd_id) |
		     FIELD_PREP(PPE_FLOW_OP_TYPE, PPE_TBL_OP_GET) |
		     FIELD_PREP(PPE_FLOW_OP_HASH_BLOCK, PPE_FLOW_HASH_BLOCKS) |
		     PPE_FLOW_OP_INDEX_MODE |
		     FIELD_PREP(PPE_FLOW_OP_ENTRY_IDX, index));
	if (ret)
		return ret;

	ret = ppe_flow_op_wait(priv, PPE_FLOW_TBL_RD_OP_RSLT, cmd_id, &rslt);
	if (ret)
		return ret;

	if (rslt & PPE_FLOW_RSLT_FAIL)
		return -ENOENT;

	for (i = 0; i < nwords; i++) {
		ret = regmap_read(priv->regmap, PPE_FLOW_TBL_RD_RSLT_DATA(i),
				  &read[i]);
		if (ret)
			return ret;
	}
	memcpy(words, read, nwords * sizeof(*words));

	return 0;
}

/* Delete one flow entry by the index the hardware assigned it, so the key does
 * not have to be kept and restaged just to remove it.
 */
int ppe_flow_entry_delete(struct qca_ppe_priv *priv, u32 index)
{
	u32 cmd_id;
	int ret;

	lockdep_assert_held(&priv->flow_lock);
	if (priv->flow_op_busy)
		return -EBUSY;
	cmd_id = ppe_flow_next_cmd_id(priv);
	ret = regmap_write(priv->regmap, PPE_FLOW_TBL_OP,
		     FIELD_PREP(PPE_FLOW_OP_CMD_ID, cmd_id) |
		     FIELD_PREP(PPE_FLOW_OP_TYPE, PPE_TBL_OP_DEL) |
		     FIELD_PREP(PPE_FLOW_OP_HASH_BLOCK, PPE_FLOW_HASH_BLOCKS) |
		     PPE_FLOW_OP_INDEX_MODE |
		     FIELD_PREP(PPE_FLOW_OP_ENTRY_IDX, index));
	if (ret)
		return ret;
	priv->flow_op_busy = true;
	priv->flow_op_result_valid = false;
	priv->flow_op_cmd_id = cmd_id;
	priv->flow_op_rslt_reg = PPE_FLOW_TBL_OP_RSLT;
	return ppe_flow_op_finish(priv, NULL, NULL);
}

/* A host entry is visible in the flat mapping but a valid one cannot be cleared
 * by writing zeroes there, so removal goes through the host op engine.
 */
int ppe_host_del(struct qca_ppe_priv *priv, u32 index)
{
	u32 cmd_id;
	int ret;

	lockdep_assert_held(&priv->flow_lock);
	if (priv->flow_op_busy)
		return -EBUSY;
	cmd_id = ppe_flow_next_cmd_id(priv);
	ret = regmap_write(priv->regmap, PPE_HOST_TBL_OP,
		     FIELD_PREP(PPE_HOST_OP_CMD_ID, cmd_id) |
		     FIELD_PREP(PPE_HOST_OP_TYPE, PPE_TBL_OP_DEL) |
		     FIELD_PREP(PPE_HOST_OP_HASH_BLOCK, PPE_FLOW_HASH_BLOCKS) |
		     PPE_HOST_OP_INDEX_MODE |
		     FIELD_PREP(PPE_HOST_OP_ENTRY_IDX, index));
	if (ret)
		return ret;
	priv->flow_op_busy = true;
	priv->flow_op_result_valid = false;
	priv->flow_op_cmd_id = cmd_id;
	priv->flow_op_rslt_reg = PPE_HOST_TBL_OP_RSLT;
	priv->host_retire_index = index;
	return ppe_flow_op_finish(priv, NULL, NULL);
}

int ppe_flow_counter_read(struct qca_ppe_priv *priv, u32 index, u64 *packets,
			  u64 *bytes)
{
	u32 pkts, lo, hi, hi2;
	int ret, i;

	ret = regmap_read(priv->regmap, PPE_IN_FLOW_CNT_TBL(index), &pkts);
	if (ret)
		goto fail;

	/* 40-bit byte counter. Re-read on a carry between the two halves so a
	 * count crossing the low word's boundary is not torn by 1 << 32.
	 */
	for (i = 0; i < 3; i++) {
		ret = regmap_read(priv->regmap, PPE_IN_FLOW_CNT_TBL(index) + 8, &hi);
		if (ret)
			goto fail;
		ret = regmap_read(priv->regmap, PPE_IN_FLOW_CNT_TBL(index) + 4, &lo);
		if (ret)
			goto fail;
		ret = regmap_read(priv->regmap, PPE_IN_FLOW_CNT_TBL(index) + 8, &hi2);
		if (ret)
			goto fail;
		if ((hi & PPE_FLOW_CNT_BYTES_HI) != (hi2 & PPE_FLOW_CNT_BYTES_HI))
			continue;
		*packets = pkts;
		*bytes = lo | ((u64)FIELD_GET(PPE_FLOW_CNT_BYTES_HI, hi) << 32);
		return 0;
	}
	ret = -EAGAIN;
fail:
	priv->flow_counter_read_failed++;
	return ret;
}

/* Deleting an entry does not reset its counter, so a slot has to be cleared
 * when it is handed to a new flow rather than when the old one goes away.
 */
int ppe_flow_counter_clear(struct qca_ppe_priv *priv, u32 index)
{
	int i, ret;

	for (i = 0; i < PPE_FLOW_CNT_WORDS; i++) {
		ret = regmap_write(priv->regmap, PPE_IN_FLOW_CNT_TBL(index) + i * 4, 0);
		if (ret) {
			priv->flow_counter_clear_failed++;
			return ret;
		}
	}
	return 0;
}

static int ppe_flows_show(struct seq_file *s, void *data)
{
	struct qca_ppe_priv *priv = s->private;
	u32 w[PPE_FLOW_ENTRY_WORDS_V6];
	u64 packets, bytes;
	u32 i;
	int ret;

	seq_puts(s, "index type proto fwd age host  pri packets bytes stats_read\n");

	guard(mutex)(&priv->flow_lock);

	for (i = 0; i < priv->data->num_flow_entries; i++) {
		if (ppe_flow_entry_read(priv, i, w, ARRAY_SIZE(w)))
			continue;
		if (!ppe_entry_get(w, PPE_FLOW_E_VALID_OFF,
				   PPE_FLOW_E_VALID_LEN))
			continue;

		ret = ppe_flow_counter_read(priv, i, &packets, &bytes);

		seq_printf(s, "%-5u %-4s %-5llu %-3llu %-3llu %-5llu %-3llu ",
			   i, (w[0] & PPE_FLOW_E_TYPE_IPV6) ? "ipv6" : "ipv4",
			   ppe_entry_get(w, PPE_FLOW_E_PROTO_OFF,
					 PPE_FLOW_E_PROTO_LEN),
			   ppe_entry_get(w, PPE_FLOW_E_FWD_TYPE_OFF,
					 PPE_FLOW_E_FWD_TYPE_LEN),
			   ppe_entry_get(w, PPE_FLOW_E_AGE_OFF,
					 PPE_FLOW_E_AGE_LEN),
			   ppe_entry_get(w, PPE_FLOW_E_HOST_IDX_OFF,
					 PPE_FLOW_E_HOST_IDX_LEN),
			   ppe_entry_get(w, PPE_FLOW_E_PRI_PROFILE_OFF,
					 PPE_FLOW_E_PRI_PROFILE_LEN));
		if (ret)
			seq_printf(s, "- - %d\n", ret);
		else
			seq_printf(s, "%llu %llu 0\n", packets, bytes);

		/* An IPv6 entry occupies two slots and reads back identically
		 * through either, so it would otherwise be listed twice.
		 */
		if (w[0] & PPE_FLOW_E_TYPE_IPV6)
			i++;
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ppe_flows);

static const char * const ppe_flow_reject_name[] = {
	[PPE_REJECT_INGRESS_PORT]	= "ingress_not_switch_port",
	[PPE_REJECT_INGRESS_VLAN]	= "ingress_vlan_domain",
	[PPE_REJECT_KEY]		= "unsupported_match",
	[PPE_REJECT_PROTO]		= "unsupported_protocol",
	[PPE_REJECT_ACTION]		= "unsupported_action",
	[PPE_REJECT_L2]			= "no_egress_l2_header",
	[PPE_REJECT_EGRESS_PORT]	= "egress_not_switch_port",
	[PPE_REJECT_HAIRPIN]		= "egress_is_ingress",
	[PPE_REJECT_NAT_BOTH]		= "snat_and_dnat",
	[PPE_REJECT_NAT_IPV6]		= "ipv6_nat",
	[PPE_REJECT_RESOURCE]		= "table_full",
	[PPE_REJECT_HW_OP]		= "hardware_op_failed",
};

static int ppe_offload_show(struct seq_file *s, void *data)
{
	struct qca_ppe_priv *priv = s->private;
	int i;

	guard(mutex)(&priv->flow_lock);

	seq_printf(s, "%-24s %u\n", "offloaded", priv->flow_offloaded);
	seq_printf(s, "%-24s %u\n", "reinstalled", priv->flow_reinstalled);
	seq_printf(s, "%-24s %u\n", "destroy_miss", priv->flow_destroy_miss);
	seq_printf(s, "destroy_miss_last cookie=%lx\n", priv->flow_destroy_miss_cookie);
	seq_printf(s, "%-24s %u\n", "stale", priv->flow_stale);
	seq_printf(s, "retire_pending %u\nretire_failed %u\ncounter_read_failed %u\ncounter_clear_failed %u\n",
		   priv->flow_retire_pending, priv->flow_retire_failed,
		   priv->flow_counter_read_failed, priv->flow_counter_clear_failed);
	seq_printf(s, "retire_last cookie=%lx result=%d mutation_pending=%u host_pending=%u\n",
		   priv->flow_retire_cookie, priv->flow_retire_result, priv->flow_op_busy,
		   priv->host_retire_pending);
	seq_printf(s, "%-24s %u\n", "sparse_promoted",
		   priv->flow_sparse_promoted);
	seq_printf(s, "%-24s %u\n", "sparse_demoted",
		   priv->flow_sparse_demoted);
	seq_printf(s, "profile_inplace %u\nprofile_failed %u\nprofile_rollback_failed %u\n",
		   priv->flow_profile_inplace, priv->flow_profile_failed,
		   priv->flow_profile_rollback_failed);
	seq_printf(s, "%-24s %u\n", "live_entries",
		   atomic_read(&priv->flow_table.nelems));
	for (i = 0; i < PPE_REJECT_MAX; i++)
		seq_printf(s, "%-24s %u\n", ppe_flow_reject_name[i],
			   priv->flow_reject[i]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ppe_offload);

static int ppe_offload_rejects_show(struct seq_file *s, void *data)
{
	struct qca_ppe_priv *priv = s->private;
	int i;

	guard(mutex)(&priv->flow_lock);
	seq_puts(s, "reason count iif oif l3 l4 vlan cvlan push actions cookie generation\n");
	for (i = 0; i < PPE_REJECT_MAX; i++) {
		const struct ppe_flow_reject_info *info = &priv->flow_reject_info[i];

		if (!priv->flow_reject[i])
			continue;
		seq_printf(s, "%s %u %d %d %04x %u %04x:%u %04x:%u %04x:%u %016llx %lx %llu\n",
			   ppe_flow_reject_name[i], priv->flow_reject[i],
			   info->ingress_ifindex, info->egress_ifindex,
			   info->n_proto, info->ip_proto,
			   info->vlan_tpid, info->vlan_id,
			   info->cvlan_tpid, info->cvlan_id,
			   info->push_tpid, info->push_vid, info->actions,
			   info->cookie, info->generation);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ppe_offload_rejects);

static int ppe_dsa_services_show(struct seq_file *s, void *data)
{
	struct qca_ppe_priv *priv = s->private;
	int i;

	seq_puts(s, "slot refs port vid vsi xlt\n");
	guard(mutex)(&priv->vlan_lock);
	{
		u32 ingress_qinq, egress_qinq;

		if (regmap_read(priv->regmap, PPE_BRIDGE_CONFIG, &ingress_qinq) ||
		    regmap_read(priv->regmap, PPE_EG_BRIDGE_CONFIG, &egress_qinq))
			return -EIO;
		seq_printf(s, "dsa_qinq ingress=%08x egress=%08x\n",
			   ingress_qinq, egress_qinq);
	}
	for (i = 0; i < QCA_PPE_DSA_SERVICE_MAX; i++) {
		struct ppe_dsa_service *service = &priv->dsa_service[i];

		if (!service->refs)
			continue;
		seq_printf(s, "%d %u %d %u %d %d\n", i, service->refs,
			   service->port, service->vid, service->vsi, service->xlt);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ppe_dsa_services);

static int ppe_pipeline_state_show(struct seq_file *s, void *data)
{
	struct qca_ppe_priv *priv = s->private;
	u32 ctrl0, ctrl1, vsi;
	int type, i;

	if (regmap_read(priv->regmap, PPE_FLOW_CTRL0, &ctrl0))
		return -EIO;
	seq_printf(s, "flow_ctrl0 %08x\n", ctrl0);
	for (type = 0; type < PPE_FLOW_PKT_TYPES; type++) {
		if (regmap_read(priv->regmap, PPE_FLOW_CTRL1(type), &ctrl1))
			return -EIO;
		seq_printf(s, "flow_ctrl1[%d] %08x\n", type, ctrl1);
	}
	for (i = 0; i < 4; i++) {
		if (regmap_read(priv->regmap, PPE_L3_VSI_TBL(i), &vsi))
			return -EIO;
		seq_printf(s, "l3_vsi[%d] %08x\n", i, vsi);
	}

	guard(mutex)(&priv->vlan_lock);
	for (i = 0; i < priv->data->num_ports; i++) {
		u32 ingress, egress;

		if (regmap_read(priv->regmap, PPE_PORT_PARSING(i), &ingress) ||
		    regmap_read(priv->regmap, PPE_PORT_EG_VLAN(i), &egress))
			return -EIO;
		seq_printf(s, "dsa_port_role port=%d base_egress=%u ingress_refs=%u "
			   "egress_refs=%u ingress=%08x egress=%08x\n", i,
			   !!(priv->dsa_core_egress_base & BIT(i)),
			   priv->dsa_core_ingress_refs[i],
			   priv->dsa_core_egress_refs[i], ingress, egress);
	}
	for (i = 0; i < QCA_PPE_DSA_SERVICE_MAX; i++) {
		struct ppe_dsa_service *service = &priv->dsa_service[i];
		u32 rule[2], action[3], eg_rule[2], eg_action[2];
		u32 ingress_role, egress_role, cpu_egress_role;
		u32 count[3], counter_id;
		bool counter_enabled;
		u64 bytes;

		if (!service->refs)
			continue;
		if (regmap_read(priv->regmap, PPE_PORT_PARSING(service->port),
				&ingress_role) ||
		    regmap_read(priv->regmap, PPE_PORT_EG_VLAN(service->port),
				&egress_role) ||
		    regmap_read(priv->regmap, PPE_PORT_EG_VLAN(QCA_PPE_CPU_PORT),
				&cpu_egress_role))
			return -EIO;
		seq_printf(s, "dsa_core_port[%d] ingress=%08x egress=%08x cpu_egress=%08x\n",
			   service->port, ingress_role, egress_role,
			   cpu_egress_role);
		if (regmap_bulk_read(priv->regmap, PPE_EG_XLT_RULE(service->xlt),
				     eg_rule, ARRAY_SIZE(eg_rule)) ||
		    regmap_bulk_read(priv->regmap, PPE_EG_XLT_ACTION(service->xlt),
				     eg_action, ARRAY_SIZE(eg_action)) ||
		    regmap_bulk_read(priv->regmap, PPE_XLT_RULE_TBL(service->xlt),
				     rule, ARRAY_SIZE(rule)) ||
		    regmap_bulk_read(priv->regmap, PPE_XLT_ACTION_TBL(service->xlt),
				     action, ARRAY_SIZE(action)))
			return -EIO;
		seq_printf(s, "dsa_eg_xlt[%d] rule=%08x:%08x action=%08x:%08x\n",
			   i, eg_rule[0], eg_rule[1], eg_action[0], eg_action[1]);
		counter_enabled = action[1] & PPE_XLT_ACTION_W1_CNT_EN;
		counter_id = FIELD_GET(PPE_XLT_ACTION_W1_CNT_ID_LO, action[1]) |
			     FIELD_GET(PPE_XLT_ACTION_W2_CNT_ID_HI, action[2]) << 3;
		seq_printf(s, "dsa_xlt[%d] port=%d vid=%u vsi=%d xlt=%d "
			   "rule=%08x:%08x action=%08x:%08x:%08x "
			   "counter_enabled=%u counter_id=%u", i,
			   service->port, service->vid, service->vsi, service->xlt,
			   rule[0], rule[1], action[0], action[1], action[2],
			   counter_enabled, counter_id);
		if (!counter_enabled) {
			seq_putc(s, '\n');
			continue;
		}
		if (regmap_bulk_read(priv->regmap, PPE_XLT_CNT_TBL(counter_id),
				     count, ARRAY_SIZE(count)))
			return -EIO;
		bytes = count[1] | (u64)FIELD_GET(PPE_XLT_CNT_W2_BYTES_HI, count[2]) << 32;
		seq_printf(s, " packets=%u bytes=%llu\n", count[0],
			   (unsigned long long)bytes);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ppe_pipeline_state);

void ppe_flow_debugfs_init(struct qca_ppe_priv *priv)
{
	debugfs_create_file("flows", 0400, priv->debugfs, priv,
			    &ppe_flows_fops);
	debugfs_create_file("offload", 0400, priv->debugfs, priv,
			    &ppe_offload_fops);
	debugfs_create_file("offload_rejects", 0400, priv->debugfs, priv,
			    &ppe_offload_rejects_fops);
	debugfs_create_file("dsa_services", 0400, priv->debugfs, priv,
			    &ppe_dsa_services_fops);
	debugfs_create_file("pipeline_state", 0400, priv->debugfs, priv,
			    &ppe_pipeline_state_fops);
	ppe_flow_offload_debugfs_init(priv);
}

/* The entry's two-bit age field counts down one step per age period, so an
 * untouched entry survives two to three periods. The hardware turns its own
 * clock into that period using the rate it is told about here, so read the rate
 * from the clock the board actually runs instead of assuming one.
 */
static void ppe_flow_age_timer_set(struct qca_ppe_priv *priv, u32 *ctrl)
{
	unsigned long rate = ppe_clk_rate(priv);

	if (!rate)
		return;

	*ctrl |= FIELD_PREP(PPE_FLOW_CLK_FREQ_MHZ, rate / HZ_PER_MHZ) |
		 FIELD_PREP(PPE_FLOW_AGE_TIMER, PPE_FLOW_AGE_SECS) |
		 FIELD_PREP(PPE_FLOW_AGE_TIMER_UNIT, PPE_FLOW_AGE_UNIT_SEC);
}

void ppe_flow_init(struct qca_ppe_priv *priv)
{
	u32 ctrl;
	int type, dir;


	/* A miss has to forward: with the lookup enabled and no entry matching,
	 * any other action would black-hole traffic the driver never saw.
	 *
	 * Fragments bypass the lookup: only the first fragment carries the L4
	 * ports, so matching it in hardware while the rest miss to the CPU
	 * would leave conntrack's reassembly waiting forever.
	 *
	 * The neighbouring TCP_SPECIAL bypass stays off: on this generation it
	 * takes every TCP packet out of the lookup, not just the flagged ones.
	 * A connection's FIN and RST are therefore forwarded in hardware and its
	 * entry goes on the flowtable's idle timeout, as it does on every driver
	 * whose hardware never shows it the teardown.
	 */
	for (type = 0; type < PPE_FLOW_PKT_TYPES; type++) {
		u32 val = 0;

		for (dir = 0; dir < PPE_FLOW_CTRL1_DIRS; dir++)
			val |= (FIELD_PREP(PPE_FLOW_MISS_ACTION,
					   PPE_FLOW_MISS_FORWARD) |
				PPE_FLOW_FRAG_BYPASS)
			       << (dir * PPE_FLOW_CTRL1_DIR_BITS);

		regmap_write(priv->regmap, PPE_FLOW_CTRL1(type), val);
	}

	/* The flow encoder stores the packet destination in the flow entry and the
	 * packet source in the host entry. Keep the default source-key orientation
	 * for PPPoE as well; both directions have been verified with live PPE flow
	 * counters on CR1000A.
	 */
	ctrl = PPE_FLOW_EN | FIELD_PREP(PPE_FLOW_HASH_MODE1, 1);
	ppe_flow_age_timer_set(priv, &ctrl);
	regmap_write(priv->regmap, PPE_FLOW_CTRL0, ctrl);
}
