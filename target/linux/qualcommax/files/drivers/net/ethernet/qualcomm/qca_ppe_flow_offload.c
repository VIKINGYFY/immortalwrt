// SPDX-License-Identifier: GPL-2.0-or-later OR MIT
/* Copyright (c) 2026 Julius Bairaktaris <julius@bairaktaris.de> */
/* netfilter flowtable offload for the Qualcomm PPE.
 *
 * The kernel hands us one rule per direction of a connection; each becomes one
 * PPE flow entry, and the two are unrelated as far as this driver is concerned.
 * A rule that rewrites the source address is an SNAT entry, one that rewrites
 * the destination is a DNAT entry, and one that rewrites neither is a plain
 * route. The hardware overlays the SNAT and DNAT fields on the same bits, so a
 * rule that rewrites both is declined and stays in software.
 * Transparent untagged HPPE/CPPE bridge flows use native L2 forwarding, which
 * preserves both Ethernet addresses and TTL and owns no routing side tables.
 *
 * Three side tables carry what does not fit in the flow entry: the egress L3
 * interface holds the source MAC and the PPPoE session, the nexthop holds the
 * destination MAC, the egress port, the VLAN tag and the DNAT address, and the
 * public-address table holds the SNAT address. All three are shared between
 * flows that need the same contents and are reference counted.
 */

#include <linux/bitfield.h>
#include <linux/dsa/8021q.h>
#include <linux/debugfs.h>
#include <linux/if_arp.h>
#include <linux/if_bridge.h>
#include <linux/qca_edma.h>
#include <linux/hash.h>
#include <linux/if_pppox.h>
#include <linux/if_vlan.h>
#include <linux/module.h>
#include <linux/hashtable.h>
#include <linux/jhash.h>
#include <linux/ktime.h>
#include <linux/in6.h>
#include <linux/ipv6.h>
#include <linux/socket.h>
#include <net/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/netfilter.h>
#include <linux/netfilter_netdev.h>
#include <linux/soc/qcom/qca_ppe.h>
#include <linux/netdevice.h>
#include <linux/ppp_defs.h>
#include <linux/rhashtable.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/unaligned.h>
#include <net/dsa.h>
#include <net/cfg80211.h>
#include <net/flow_offload.h>
#include <net/dsfield.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_flow_table.h>
#include <net/pkt_cls.h>
#include <net/tcp.h>
#include <net/switchdev.h>

#include "qca_ppe.h"

static int ppe_dsa_core_port_get(struct qca_ppe_priv *priv, int port, bool ingress);
static void ppe_dsa_core_port_put(struct qca_ppe_priv *priv, int port, bool ingress);

struct ppe_flow_data {
	struct ethhdr eth;
	u16 addr_type;
	u8 l4proto;

	__be32 v4_src, v4_dst, v4_src_new, v4_dst_new;
	struct in6_addr v6_src, v6_dst;
	__be16 sport, dport, sport_new, dport_new;

	u16 vlan_id;
	bool vlan_valid;
	u16 ingress_svid;
	bool ingress_svid_valid;
	u8 ingress_mac[ETH_ALEN];
	bool ingress_mac_valid;
	u16 egress_svid;
	bool egress_svid_valid;
	u16 ivid;
	u16 pppoe_sid;
	bool pppoe_valid;

	struct net_device *odev;
	bool bridge;
	/* The profile the entry is given; filled only where the flowtable
	 * reports a priority, and zero means DSCP still decides.
	 */
	u8 priority;
};

enum ppe_flow_state {
	PPE_FLOW_LIVE,
	PPE_FLOW_RETIRING,
	PPE_FLOW_PENDING_ADD,
	PPE_FLOW_PENDING_DELETE,
};

struct ppe_flow_entry {
	enum ppe_flow_state state;
	struct rhash_head node;
	struct list_head list;
	struct flow_block *block;
	unsigned long cookie;
	u32 index;
	u32 words[PPE_FLOW_ENTRY_WORDS_V6];
	u8 nwords;
	u32 hwords[PPE_HOST_ENTRY_WORDS_V6];
	u8 nhwords;
	u8 profile;
	u8 quiet;
	bool sparse;
	u8 src_if;
	u8 ingress_vsi;
	bool vsi_ref;
	bool bridge;
	int bridge_ifindex;
	int bridge_iif, bridge_oif;
	bool steer;
	int fw_ifindex;
	u32 host_index;
	u32 previous_host;
	int nexthop;
	int my_mac;
	int l3_if;
	int eg_l3_if;
	int pub_ip;
	int uplink;
	int in_uplink;
	int icasc;
	int ocasc;
	int session;
	int xdev;
	unsigned long ct;
	u16 vid;
	u8 xport;
	u8 dscp;
	u8 iport;
	u8 oport;
	bool xdev_bound;

	s8 dsa_service;
	s8 dsa_egress_port;
	u16 ivid;
	u16 ovid;
	u64 packets;
	u64 bytes;
	u64 unread_packets;
	u64 unread_bytes;
	unsigned long last_used;
};

/* One bound flowtable block. Entries remember which block installed them,
 * because a dying flowtable unbinds from every port before the core flushes
 * its flows - the FLOW_CLS_DESTROY commands never arrive, and the block
 * callback's release, which the core invokes after the last unbind, is the
 * one point where that block's flows are known dead.
 */
struct ppe_flow_block {
	struct qca_ppe_priv *priv;
	struct flow_block *block;
};

/* Keep the software-side flow identity and the exact nexthop image together
 * in one readout. The normal `flows` file only exposes hardware counters, so
 * it cannot distinguish a LAN-to-WAN entry from its reverse or show whether
 * the RTL9303 TX service tag was actually put in the nexthop.
 */
static int ppe_flow_entry_age(struct qca_ppe_priv *priv,
			      struct ppe_flow_entry *entry);

static int ppe_offload_entries_show(struct seq_file *s, void *data)
{
	struct qca_ppe_priv *priv = s->private;
	struct ppe_flow_entry *entry;

	seq_puts(s, "cookie index src_if iport oport ivid ovid dsa_service dsa_vid "
		 "nexthop nh_type nh_port nh_stag_fmt nh_svid nh_ctag_fmt nh_cvid "
		 "l3_if eg_l3_if uplink in_uplink packets bytes nh_words "
		 "sw_sc hw_read hw_sc fwd port_valid stats_read "
		 "bridge bridge_ifindex hw_fwd bridge_port "
		 "hw_bridge_port hw_src_if_valid hw_src_if\n");

	guard(mutex)(&priv->flow_lock);
	list_for_each_entry(entry, &priv->flow_list, list) {
		u32 *words = NULL;
		u16 dsa_vid = 0;
		u64 packets, bytes;
		u32 hw[PPE_FLOW_ENTRY_WORDS_V6] = {};
		int hw_ret, stats_ret;
		int i;

		if (entry->nexthop >= 0 &&
		    entry->nexthop < priv->data->num_nexthop_entries) {
			words = priv->nexthop[entry->nexthop].words;
			if (entry->dsa_service >= 0 &&
			    entry->dsa_service < QCA_PPE_DSA_SERVICE_MAX)
				dsa_vid = priv->dsa_service[entry->dsa_service].vid;
		}

		stats_ret = ppe_flow_entry_age(priv, entry);
		if (stats_ret >= 0)
			stats_ret = ppe_flow_counter_read(priv, entry->index, &packets, &bytes);
		seq_printf(s, "%lx %u %u %u %u %u %u %d %u %d ",
			   entry->cookie, entry->index, entry->src_if,
			   entry->iport, entry->oport,
			   entry->ivid, entry->ovid,
			   entry->dsa_service, dsa_vid, entry->nexthop);
		if (!words) {
			seq_puts(s, "- - - - - - ");
		} else {
			seq_printf(s, "%u %u %u %u %u %u ",
				   (unsigned int)ppe_entry_get(words, PPE_NEXTHOP_TYPE_OFF,
						 PPE_NEXTHOP_TYPE_LEN),
				   (unsigned int)ppe_entry_get(words, PPE_NEXTHOP_PORT_OFF,
						 PPE_NEXTHOP_PORT_LEN),
				   (unsigned int)ppe_entry_get(words, PPE_NEXTHOP_STAG_FMT_OFF,
						 PPE_NEXTHOP_STAG_FMT_LEN),
				   (unsigned int)ppe_entry_get(words, PPE_NEXTHOP_SVID_OFF,
						 PPE_NEXTHOP_SVID_LEN),
				   (unsigned int)ppe_entry_get(words, PPE_NEXTHOP_CTAG_FMT_OFF,
						 PPE_NEXTHOP_CTAG_FMT_LEN),
				   (unsigned int)ppe_entry_get(words, PPE_NEXTHOP_CVID_OFF,
						 PPE_NEXTHOP_CVID_LEN));
		}
		seq_printf(s, "%d %d %d %d ", entry->l3_if,
			   entry->eg_l3_if, entry->uplink, entry->in_uplink);
		if (stats_ret)
			seq_puts(s, "- - ");
		else
			seq_printf(s, "%llu %llu ", packets, bytes);
		if (!words)
			seq_putc(s, '-');
		else
			for (i = 0; i < PPE_NEXTHOP_WORDS; i++)
				seq_printf(s, "%s%08x", i ? ":" : "", words[i]);
		hw_ret = entry->bridge ?
			 (stats_ret ? stats_ret :
			  ppe_flow_entry_read(priv, entry->index, hw, entry->nwords)) :
			 -EOPNOTSUPP;
		seq_printf(s, " %u %d %u %u %u",
			   (u32)ppe_entry_get(entry->words, PPE_FLOW_E_SERVICE_CODE_OFF,
					 PPE_FLOW_E_SERVICE_CODE_LEN), hw_ret,
			   hw_ret ? 0 : (u32)ppe_entry_get(hw, PPE_FLOW_E_SERVICE_CODE_OFF,
					 PPE_FLOW_E_SERVICE_CODE_LEN),
			   (u32)ppe_entry_get(entry->words, PPE_FLOW_E_FWD_TYPE_OFF,
					      PPE_FLOW_E_FWD_TYPE_LEN),
			   (u32)ppe_entry_get(entry->words, PPE_FLOW_E_PORT_VALID_OFF,
					      PPE_FLOW_E_PORT_VALID_LEN));
		seq_printf(s, " %d", stats_ret);
		seq_printf(s, " %u %d %u %u %u %u %u\n", entry->bridge,
			   entry->bridge_ifindex,
			   hw_ret ? 0 : (u32)ppe_entry_get(hw, PPE_FLOW_E_FWD_TYPE_OFF,
							 PPE_FLOW_E_FWD_TYPE_LEN),
			   entry->bridge ? entry->oport : 0,
			   hw_ret || !entry->bridge ? 0 :
			   (u32)ppe_entry_get(hw, PPE_FLOW_E_BRIDGE_PORT_OFF,
					     PPE_FLOW_E_BRIDGE_PORT_LEN),
			   hw_ret ? 0 : (u32)ppe_entry_get(hw, PPE_FLOW_E_SRC_IF_VALID_OFF,
						 PPE_FLOW_E_SRC_IF_VALID_LEN),
			   hw_ret ? 0 : (u32)ppe_entry_get(hw, PPE_FLOW_E_SRC_IF_OFF,
						 PPE_FLOW_E_SRC_IF_LEN));
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ppe_offload_entries);

static int ppe_active_flows_show(struct seq_file *s, void *data)
{
	struct qca_ppe_priv *priv = s->private;
	struct ppe_flow_entry *entry;
	u32 ipv4 = 0, ipv6 = 0;

	/* Count published, directional rules without touching hardware or aging
	 * entries. Failed additions and unpublished retirement are not active.
	 */
	guard(mutex)(&priv->flow_lock);
	list_for_each_entry(entry, &priv->flow_list, list) {
		if (entry->words[0] & PPE_FLOW_E_TYPE_IPV6)
			ipv6++;
		else
			ipv4++;
	}

	seq_printf(s, "%u %u %u/%u\n", ipv4, ipv6, ipv4 + ipv6,
		   priv->data->num_flow_entries);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ppe_active_flows);

/* The downlink service retains L2/L3 editing and publishes its marker. */

void ppe_flow_offload_debugfs_init(struct qca_ppe_priv *priv)
{
	debugfs_create_file("active_flows", 0400, priv->debugfs, priv,
			    &ppe_active_flows_fops);
	debugfs_create_file("offload_entries", 0400, priv->debugfs, priv,
			    &ppe_offload_entries_fops);
}

static const struct rhashtable_params ppe_flow_ht_params = {
	.head_offset = offsetof(struct ppe_flow_entry, node),
	.key_offset = offsetof(struct ppe_flow_entry, cookie),
	.key_len = sizeof(unsigned long),
	.automatic_shrinking = true,
};

/* Reference-counted allocation over one hardware side table. Entries are
 * compared by content so that every flow leaving by the same nexthop, or
 * sourced from the same MAC, shares one slot; without that, the nexthops would
 * run out long before the flow entries do.
 */
static int ppe_res_get(struct ppe_res *tbl, u32 count, const u32 *words,
		       int nwords)
{
	int free_idx = -1;
	u32 i;

	for (i = 0; i < count; i++) {
		if (!tbl[i].refcount) {
			if (free_idx < 0)
				free_idx = i;
			continue;
		}
		if (!memcmp(tbl[i].words, words, nwords * sizeof(*words))) {
			tbl[i].refcount++;
			return i;
		}
	}

	if (free_idx < 0)
		return -ENOSPC;

	memcpy(tbl[free_idx].words, words, nwords * sizeof(*words));
	tbl[free_idx].refcount = 1;

	return free_idx;
}

static bool ppe_res_put(struct ppe_res *tbl, int idx)
{
	if (idx < 0)
		return false;

	return --tbl[idx].refcount == 0;
}

/* The hardware find-or-creates a host entry from the source address, so every
 * flow from one host lands on the same slot and the slot outlives any single
 * flow that referenced it.
 */
static void ppe_host_ref_get(struct qca_ppe_priv *priv, u32 index)
{
	priv->host_ref[index]++;
}

static void ppe_host_ref_put(struct qca_ppe_priv *priv, u32 index)
{
	int ret;

	if (--priv->host_ref[index] || priv->flow_stopping)
		return;
	if (!test_and_set_bit(index, priv->host_retired))
		priv->host_retire_pending++;
	ret = ppe_host_del(priv, index);
	if (!ret || ret == -ENOENT) {
		clear_bit(index, priv->host_retired);
		priv->host_retire_pending--;
	}
}

/* The age of the entry in @index, or -ENOENT if the slot no longer holds this
 * flow. Word 0 on its own is not an identity - its host index, protocol and
 * forwarding type are shared by every flow of one client - so a slot the
 * hardware aged out and handed to a sibling would read back as still ours.
 */
static int ppe_flow_entry_age(struct qca_ppe_priv *priv,
			      struct ppe_flow_entry *entry)
{
	u32 w[PPE_FLOW_ENTRY_WORDS_V6], expected[PPE_FLOW_ENTRY_WORDS_V6];
	u32 age;
	int ret;

	/* Only a read that says the slot is empty means the flow is gone; any
	 * other failure leaves it unknown, and reporting "gone" would strand a
	 * live entry pointing at side-table slots about to be handed on.
	 */
	ret = ppe_flow_entry_read(priv, entry->index, w, entry->nwords);
	if (ret)
		return ret;

	age = FIELD_GET(PPE_FLOW_E_AGE_MASK, w[0]);
	w[0] &= ~PPE_FLOW_E_AGE_MASK;

	memcpy(expected, entry->words, entry->nwords * sizeof(*w));
	/* QoS is not identity. A failed priority restore must not strand the
	 * owned slot while teardown frees its unchanged forwarding resources.
	 */
	w[1] &= ~BIT(PPE_FLOW_E_PRI_PROFILE_OFF % 32);
	expected[1] &= ~BIT(PPE_FLOW_E_PRI_PROFILE_OFF % 32);
	w[2] &= ~GENMASK(PPE_FLOW_E_PRI_PROFILE_LEN - 2, 0);
	expected[2] &= ~GENMASK(PPE_FLOW_E_PRI_PROFILE_LEN - 2, 0);
	if (memcmp(w, expected, entry->nwords * sizeof(*w)))
		return -ENOENT;

	return age;
}

static void ppe_tbl_write(struct qca_ppe_priv *priv, u32 reg, const u32 *words,
			  int nwords)
{
	int i;

	for (i = 0; i < nwords; i++)
		regmap_write(priv->regmap, reg + i * 4, words[i]);
}

static void ppe_tbl_clear(struct qca_ppe_priv *priv, u32 reg, int nwords)
{
	int i;

	for (i = 0; i < nwords; i++)
		regmap_write(priv->regmap, reg + i * 4, 0);
}

/* An entry of a multi-word table takes the write to its last word as the
 * commit: a word 0 written on its own is staged and reads back unchanged.
 *
 * MRU is also how an interface is retired: teardown clears the entry, leaving
 * MRU 0, so a frame still matching a flow whose interface is gone fails the
 * MRU check and L3_ROUTE_CTRL decides what happens to it. The driver never
 * writes that register and depends on its reset, which redirects to the CPU;
 * writing it is how teardown would become a black hole.
 */
static void ppe_l3_if_mtu_set(struct qca_ppe_priv *priv, u32 vsi, u32 mtu)
{
	u32 words[PPE_L3_IF_WORDS];
	int i;

	/* A bridge takes an mtu wider than the field holds, and the value that
	 * survives the truncation reads back as a retired interface.
	 */
	mtu = min_t(u32, mtu, FIELD_MAX(PPE_L3_IF_MRU));

	for (i = 0; i < PPE_L3_IF_WORDS; i++)
		regmap_read(priv->regmap, PPE_IN_L3_IF_TBL(vsi) + i * 4,
			    &words[i]);

	words[0] &= ~(PPE_L3_IF_MRU | PPE_L3_IF_MTU);
	words[0] |= FIELD_PREP(PPE_L3_IF_MRU, mtu) |
		    FIELD_PREP(PPE_L3_IF_MTU, mtu);

	ppe_tbl_write(priv, PPE_IN_L3_IF_TBL(vsi), words, PPE_L3_IF_WORDS);
}

/* The egress half of an L3 interface answers the mtu check and nothing else:
 * no route enables and no my-mac bitmap, because no VSI resolves to it.
 */
static void ppe_eg_l3_if_mtu_set(struct qca_ppe_priv *priv, u32 idx, u32 mtu)
{
	u32 words[PPE_L3_IF_WORDS] = {};

	words[0] = FIELD_PREP(PPE_L3_IF_MRU, mtu) |
		   FIELD_PREP(PPE_L3_IF_MTU, mtu);

	ppe_tbl_write(priv, PPE_IN_L3_IF_TBL(idx), words, PPE_L3_IF_WORDS);
}

/* Ports outside a bridge sit on VSI 0 since setup. */
static u32 ppe_port_l3_vsi(struct qca_ppe_priv *priv, int port)
{
	return priv->port_vsi[port] == PPE_VSI_INVALID ? 0 :
	       priv->port_vsi[port];
}

static void ppe_entry_set_addr6(u32 *words, u32 offset,
				const struct in6_addr *addr)
{
	int i;

	/* The hardware stores the address least significant word first. */
	for (i = 3; i >= 0; i--)
		ppe_entry_set(words, offset + (3 - i) * 32, 32,
			      ntohl(addr->s6_addr32[i]));
}

/* Remove a stacked DSA egress translation before releasing its slot. */
static void ppe_eg_xlt_clear(struct qca_ppe_priv *priv, int xlt)
{
	/* Invalidate the key before clearing its action, as a miss must never see
	 * an incomplete restore operation during service teardown.
	 */
	regmap_write(priv->regmap, PPE_EG_XLT_RULE(xlt), 0);
	regmap_write(priv->regmap, PPE_EG_XLT_RULE_W1(xlt), 0);
	regmap_write(priv->regmap, PPE_EG_XLT_ACTION(xlt), 0);
	regmap_write(priv->regmap, PPE_EG_XLT_ACTION_W1(xlt), 0);
}



/* GRE is the one protocol the flowtable offers whose tuple has no ports: the
 * entry keys on the IP protocol instead, which the hardware selects by naming
 * no L4 protocol at all.
 */
static int ppe_flow_proto(u8 l4proto)
{
	switch (l4proto) {
	case IPPROTO_TCP:
		return PPE_FLOW_PROTO_TCP;
	case IPPROTO_UDP:
		return PPE_FLOW_PROTO_UDP;
	case IPPROTO_GRE:
		return PPE_FLOW_PROTO_OTHER;
	default:
		return -EOPNOTSUPP;
	}
}

static int ppe_flow_mangle_eth(const struct flow_action_entry *act,
			       struct ethhdr *eth)
{
	void *dest = (void *)eth + act->mangle.offset;
	const void *src = &act->mangle.val;

	/* The core writes the egress header as mangles at byte offsets into
	 * struct ethhdr, so the destination follows from the offset alone.
	 */
	if (act->mangle.offset > 8)
		return -EOPNOTSUPP;

	if (act->mangle.mask == 0xffff) {
		src += 2;
		dest += 2;
	}

	memcpy(dest, src, act->mangle.mask ? 2 : 4);

	return 0;
}

static int ppe_flow_mangle_ports(const struct flow_action_entry *act,
				 struct ppe_flow_data *data)
{
	u32 val = ntohl(act->mangle.val);

	switch (act->mangle.offset) {
	case 0:
		if (act->mangle.mask == ~htonl(0xffff))
			data->dport_new = cpu_to_be16(val);
		else
			data->sport_new = cpu_to_be16(val >> 16);
		break;
	case 2:
		data->dport_new = cpu_to_be16(val);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int ppe_flow_mangle_ipv4(const struct flow_action_entry *act,
				struct ppe_flow_data *data)
{
	__be32 *dest;

	switch (act->mangle.offset) {
	case offsetof(struct iphdr, saddr):
		dest = &data->v4_src_new;
		break;
	case offsetof(struct iphdr, daddr):
		dest = &data->v4_dst_new;
		break;
	default:
		return -EOPNOTSUPP;
	}

	memcpy(dest, &act->mangle.val, sizeof(u32));

	return 0;
}

static struct net_device *ppe_flow_vlan_upper(struct net_device *dev, u16 vid)
{
	struct net_device *upper;

	upper = __vlan_find_dev_deep_rcu(dev, htons(ETH_P_8021Q), vid);

	return upper && vlan_dev_real_dev(upper) == dev ? upper : NULL;
}

/* A port behind a QCA8K switch reaches the PPE as one 802.1Q VLAN. In a
 * VLAN-aware bridge the frame's VLAN names the domain; an untagged frame uses
 * the port PVID. A VLAN-unaware bridge and a standalone port use their DSA
 * tag_8021q VID. */
static u16 ppe_flow_8021q_vid(struct dsa_port *dp, u16 flow_vid)
{
	struct net_device *br;
	struct bridge_vlan_info vinfo;
	u16 pvid;

	guard(rcu)();
	br = dsa_port_bridge_dev_get(dp);
	if (!br || !br_vlan_enabled(br)) {
		if (flow_vid)
			return ppe_flow_vlan_upper(dp->user, flow_vid) ?
			       flow_vid : 0;
		if (!br)
			return dsa_tag_8021q_standalone_vid(dp);
		return dsa_tag_8021q_bridge_vid(dsa_port_bridge_num_get(dp));
	}
	if (flow_vid)
		return flow_vid;

	if (br_vlan_get_pvid_rcu(dp->user, &pvid) || !pvid ||
	    br_vlan_get_info_rcu(dp->user, pvid, &vinfo) ||
	    !(vinfo.flags & BRIDGE_VLAN_INFO_UNTAGGED))
		return 0;

	return pvid;
}

/* Resolve a QCA8K user port to the PPE conduit and the VLAN tag that the
 * qca-8021q tagger carries across it. */
static int ppe_flow_qca_port_by_ifindex(struct qca_ppe_priv *priv,
						int ifindex, u16 flow_vid, u16 *vid)
{
	struct dsa_port *dp, *cpu_dp, *cdp;

	*vid = 0;
	dsa_switch_for_each_user_port(dp, &priv->ds) {
		if (!dp->user || !netdev_uses_dsa(dp->user))
			continue;

		cpu_dp = dp->user->dsa_ptr;
		if (!cpu_dp || !cpu_dp->tag_ops ||
		    cpu_dp->tag_ops->proto != DSA_TAG_PROTO_QCA_8021Q)
			continue;

		dsa_tree_for_each_user_port(cdp, cpu_dp->dst) {
			if (cdp->cpu_dp != cpu_dp || !cdp->user ||
			    cdp->user->ifindex != ifindex)
				continue;

			*vid = ppe_flow_8021q_vid(cdp, flow_vid);
			return *vid ? dp->index : -EOPNOTSUPP;
		}
	}

	return -EOPNOTSUPP;
}

/* The RTL9303 DSA user netdev is not a user port of this PPE's DSA switch.
 * Its conduit is the PPE port 5 (lan-cpu), and the tagger identifies the
 * external socket in a private 802.1ad service VID. Return both pieces so a
 * flow can use the conduit for hardware lookup while retaining socket
 * identity in the VLAN editor/nexthop.
 */
static int ppe_flow_port_by_ifindex(struct qca_ppe_priv *priv, int ifindex,
				    u16 *service_vid, u8 *ingress_mac,
				    bool egress)
{
	struct dsa_port *dp;
	struct net_device *dev, *conduit;
	struct dsa_port *cp;
	int ret = -EOPNOTSUPP;

	if (service_vid)
		*service_vid = 0;
	if (ingress_mac)
		eth_zero_addr(ingress_mac);

	dsa_switch_for_each_user_port(dp, &priv->ds)
		if (dp->user && dp->user->ifindex == ifindex)
			return dp->index;

	rcu_read_lock();
	dev = __dev_get_by_index(&init_net, ifindex);
	if (!dev || !dsa_user_dev_check(dev))
		goto out;

	dp = dsa_port_from_netdev(dev);
	if (IS_ERR(dp) || !dp->cpu_dp || !dp->cpu_dp->tag_ops ||
	    dp->cpu_dp->tag_ops->proto != DSA_TAG_PROTO_RTL9303_8021AD)
		goto out;

	conduit = dsa_port_to_conduit(dp);
	if (!conduit)
		goto out;

	dsa_switch_for_each_user_port(cp, &priv->ds) {
		if (cp->user != conduit)
			continue;

		if (service_vid)
			*service_vid = (egress ? QCA_PPE_DSA_TX_VID_BASE :
					QCA_PPE_DSA_RX_VID_BASE) |
				       (dp->ds->index << 5) | dp->index;
		if (ingress_mac) {
			ether_addr_copy(ingress_mac, dp->user->dev_addr);
		}
		ret = cp->index;
		break;
	}
out:
	rcu_read_unlock();
	return ret;
}

/* The conduit role outlives offloaded flows. In particular, DHCP and ARP
 * emitted by the CPU must retain the external switch's private S-tag even
 * when the last hardware egress lease has been released.
 */
int ppe_dsa_core_port_set_base(struct qca_ppe_priv *priv, int port, bool core)
{
	int ret;

	if (WARN_ON_ONCE(port < 0 || port >= QCA_PPE_MAX_PORTS))
		return -EINVAL;
	mutex_lock(&priv->vlan_lock);
	ret = regmap_update_bits(priv->regmap, PPE_PORT_EG_VLAN(port),
				 PPE_PORT_EG_VLAN_CORE,
				 core || priv->dsa_core_egress_refs[port] ?
				 PPE_PORT_EG_VLAN_CORE : 0);
	if (!ret) {
		if (core)
			priv->dsa_core_egress_base |= BIT(port);
		else
			priv->dsa_core_egress_base &= ~BIT(port);
	}
	mutex_unlock(&priv->vlan_lock);
	return ret;
}

static int ppe_dsa_core_port_get(struct qca_ppe_priv *priv, int port,
				  bool ingress)
{
	u16 *refs;
	int ret;

	lockdep_assert_held(&priv->vlan_lock);
	if (WARN_ON_ONCE(port < 0 || port >= QCA_PPE_MAX_PORTS))
		return -EINVAL;

	refs = ingress ? &priv->dsa_core_ingress_refs[port] :
			 &priv->dsa_core_egress_refs[port];
	if (WARN_ON_ONCE(*refs == U16_MAX))
		return -EOVERFLOW;
	if ((*refs)++)
		return 0;

	ret = ingress ?
		regmap_update_bits(priv->regmap, PPE_PORT_PARSING(port),
				   PPE_PORT_PARSING_CORE, PPE_PORT_PARSING_CORE) :
		regmap_update_bits(priv->regmap, PPE_PORT_EG_VLAN(port),
				   PPE_PORT_EG_VLAN_CORE, PPE_PORT_EG_VLAN_CORE);
	if (ret)
		(*refs)--;
	return ret;
}

static void ppe_dsa_core_port_put(struct qca_ppe_priv *priv, int port,
				   bool ingress)
{
	u16 *refs;
	int ret;

	lockdep_assert_held(&priv->vlan_lock);
	if (WARN_ON_ONCE(port < 0 || port >= QCA_PPE_MAX_PORTS))
		return;
	refs = ingress ? &priv->dsa_core_ingress_refs[port] :
			 &priv->dsa_core_egress_refs[port];
	if (WARN_ON_ONCE(!*refs) || --(*refs))
		return;

	ret = ingress ?
		regmap_update_bits(priv->regmap, PPE_PORT_PARSING(port),
				   PPE_PORT_PARSING_CORE, 0) :
		regmap_update_bits(priv->regmap, PPE_PORT_EG_VLAN(port),
				   PPE_PORT_EG_VLAN_CORE,
				   priv->dsa_core_egress_base & BIT(port) ?
				   PPE_PORT_EG_VLAN_CORE : 0);
	if (ret) {
		*refs = 1;
		dev_err(priv->ds.dev, "failed to restore DSA port %d %s role: %d\n",
			port, ingress ? "ingress" : "egress", ret);
	}
}

static int ppe_dsa_service_get(struct qca_ppe_priv *priv, int port, u16 vid)
{
	struct ppe_dsa_service *service;
	u32 rule, rule_w1, action, action_w1;
	int i, vsi, xlt, ret;

	lockdep_assert_held(&priv->vlan_lock);
	for (i = 0; i < QCA_PPE_DSA_SERVICE_MAX; i++) {
		service = &priv->dsa_service[i];
		if (service->refs && service->port == port && service->vid == vid) {
			service->refs++;
			return i;
		}
	}

	for (i = 0; i < QCA_PPE_DSA_SERVICE_MAX; i++)
		if (!priv->dsa_service[i].refs)
			break;
	if (i == QCA_PPE_DSA_SERVICE_MAX)
		return -ENOSPC;

	vsi = ppe_vsi_alloc(priv);
	if (vsi < 0)
		return vsi;
	xlt = ppe_xlt_idx_alloc(priv);
	if (xlt < 0) {
		ppe_vsi_free(priv, vsi);
		return xlt;
	}
	ret = ppe_dsa_core_port_get(priv, port, true);
	if (ret) {
		ppe_xlt_idx_free(priv, &xlt);
		ppe_vsi_free(priv, vsi);
		return ret;
	}
	/* A flow miss leaves through the CPU port. It must emit the private
	 * service tag as an S-tag before the RTL9303 receive tagger sees it.
	 */
	ret = ppe_dsa_core_port_get(priv, QCA_PPE_CPU_PORT, false);
	if (ret) {
		ppe_dsa_core_port_put(priv, port, true);
		ppe_xlt_idx_free(priv, &xlt);
		ppe_vsi_free(priv, vsi);
		return ret;
	}

	service = &priv->dsa_service[i];
	service->port = port;
	service->vid = vid;
	service->vsi = vsi;
	service->xlt = xlt;
	service->refs = 1;

	/* The routed flow sees the frame after the private S-tag is removed.
	 * Match only this conduit and VID, require an absent customer tag, and
	 * assign the dedicated VSI before making the rule visible.
	 */
	action = FIELD_PREP(PPE_XLT_SVID_CMD, PPE_XLT_VID_DELETE);
	regmap_write(priv->regmap, PPE_XLT_ACTION_TBL(xlt), action);
	/* The routed path must carry the L3 interface selected by the XLT. The
	 * VSI assignment alone is enough for ordinary VLAN bridge traffic, but
	 * the CR1000A private RTL9303 service tag otherwise reaches the flow
	 * lookup without an L3 interface and never matches LAN-to-WAN flows. */
	action_w1 = PPE_XLT_VSI_CMD | FIELD_PREP(PPE_XLT_VSI, vsi) |
		    PPE_XLT_SRC_INFO_VALID | PPE_XLT_SRC_INFO_L3 |
		    FIELD_PREP(PPE_XLT_SRC_INFO, vsi);
	regmap_write(priv->regmap, PPE_XLT_ACTION_W1(xlt), action_w1);

	rule = PPE_XLT_VALID | FIELD_PREP(PPE_XLT_PORT_BMP, BIT(port)) |
	       FIELD_PREP(PPE_XLT_SKEY_FMT, PPE_XLT_SKEY_TAGGED) |
	       PPE_XLT_SKEY_VID_INCL | FIELD_PREP(PPE_XLT_SKEY_VID, vid) |
	       PPE_XLT_CKEY_FMT_0;
	/* cfmt=untagged|priority-tagged is three bits split across the two
	 * hardware rule words. The low bit is CKEY_FMT_0; the high bit belongs in
	 * RULE_W1 and must not be lost by the final word write. */
	rule_w1 = FIELD_PREP(PPE_XLT_CKEY_FMT_1, 1);
	regmap_write(priv->regmap, PPE_XLT_RULE_TBL(xlt), rule);
	regmap_write(priv->regmap, PPE_XLT_RULE_W1(xlt), rule_w1);
	regmap_write(priv->regmap, PPE_XLT_RULE_TBL(xlt) + 8, 0);

	/* A flow miss still has to reach the RTL9303 DSA receive path with its
	 * RX service tag intact. The tagger accepts only the e00 RX namespace
	 * from the switch; routed hardware hits use the f00 TX namespace in the
	 * nexthop STAG field below.
	 */
	action = FIELD_PREP(PPE_EG_XLT_SVID_CMD, PPE_EG_XLT_SVID_ADD) |
		  FIELD_PREP(PPE_EG_XLT_SVID,
			     QCA_PPE_DSA_RX_VID_BASE | (vid & 0xff));
	regmap_write(priv->regmap, PPE_EG_XLT_ACTION(xlt), action);
	regmap_write(priv->regmap, PPE_EG_XLT_ACTION_W1(xlt), 0);
	rule = PPE_EG_XLT_VALID | FIELD_PREP(PPE_EG_XLT_PORT_BMP,
					     BIT(QCA_PPE_CPU_PORT)) |
	       PPE_EG_XLT_VSI_INCL | FIELD_PREP(PPE_EG_XLT_VSI, vsi) |
	       PPE_EG_XLT_VSI_VALID | FIELD_PREP(PPE_EG_XLT_SKEY_FMT,
						  PPE_XLT_SKEY_UNTAGGED);
	regmap_write(priv->regmap, PPE_EG_XLT_RULE(xlt), rule);
	regmap_write(priv->regmap, PPE_EG_XLT_RULE_W1(xlt),
		     FIELD_PREP(PPE_EG_XLT_CKEY_FMT, PPE_XLT_SKEY_UNTAGGED));

	ppe_vsi_member_set(priv, vsi, BIT(port) | BIT(QCA_PPE_CPU_PORT));
	return i;
}

static void ppe_dsa_service_put(struct qca_ppe_priv *priv, int index)
{
	struct ppe_dsa_service *service = &priv->dsa_service[index];
	int port, xlt;

	lockdep_assert_held(&priv->vlan_lock);
	if (--service->refs)
		return;

	xlt = service->xlt;
	port = service->port;
	ppe_eg_xlt_clear(priv, xlt);
	ppe_xlt_idx_free(priv, &xlt);
	ppe_vsi_free(priv, service->vsi);
	ppe_dsa_core_port_put(priv, QCA_PPE_CPU_PORT, false);
	ppe_dsa_core_port_put(priv, port, true);
	memset(service, 0, sizeof(*service));
	service->port = -1;
	service->vsi = -1;
	service->xlt = -1;
}

static int ppe_uplink_find(struct qca_ppe_priv *priv, int port, u16 vid)
{
	int i;

	for (i = 0; i < PPE_PPPOE_SESSIONS; i++)
		if (priv->uplink[i].ref && priv->uplink[i].port == port &&
		    priv->uplink[i].vid == vid)
			return i;

	return -ENOENT;
}

/* Make an uplink route its ingress traffic in hardware, so the download
 * direction of an offloaded connection reaches the flow lookup on its inner
 * tuple.
 *
 * A dedicated VSI keeps the uplink out of any L2 domain: the ingress VLAN is
 * classified into it with the 802.1Q tag stripped, and the VSI carries a routing
 * L3 interface holding the router's MAC. A frame that misses the flow table is
 * still forwarded to the CPU, with the tag the hardware stripped re-added on
 * that path, so the software stack sees on-wire frames throughout. The MRU
 * leaves room for the tag and a PPPoE header whether or not either is there.
 */
static int ppe_uplink_get(struct qca_ppe_priv *priv, int port, u16 vid,
			  const u8 *mac, u32 mtu)
{
	u32 words[PPE_MY_MAC_WORDS] = {};
	struct ppe_uplink *up;
	int i, vsi, xlt = -1, ret;

	i = ppe_uplink_find(priv, port, vid);
	if (i >= 0) {
		priv->uplink[i].ref++;
		return i;
	}

	for (i = 0; i < PPE_PPPOE_SESSIONS && priv->uplink[i].ref; i++)
		;
	if (i == PPE_PPPOE_SESSIONS)
		return -ENOSPC;
	up = &priv->uplink[i];

	/* The translation rule shares one table with the bridge VLANs, so its
	 * index comes from the allocator they share.
	 */
	if (vid) {
		xlt = ppe_routed_xlt_idx_alloc(priv);
		if (xlt < 0)
			return xlt;
	}

	vsi = ppe_vsi_alloc(priv);
	if (vsi < 0) {
		ret = vsi;
		goto err_xlt;
	}
	ppe_vsi_member_set(priv, vsi, BIT(port) | BIT(QCA_PPE_CPU_PORT));

	ppe_entry_set(words, PPE_MY_MAC_ADDR_OFF, PPE_MY_MAC_ADDR_LEN,
		      ether_addr_to_u64(mac));
	ppe_entry_set(words, PPE_MY_MAC_VALID_OFF, PPE_MY_MAC_VALID_LEN, 1);
	ret = ppe_res_get(priv->my_mac, PPE_MY_MAC_ENTRIES, words,
			  PPE_MY_MAC_WORDS);
	if (ret < 0)
		goto err_vsi;
	up->mymac = ret;
	if (priv->my_mac[ret].refcount == 1)
		ppe_tbl_write(priv, PPE_MY_MAC_TBL(ret), words, PPE_MY_MAC_WORDS);

	regmap_write(priv->regmap, PPE_IN_L3_IF_TBL(vsi),
		     PPE_L3_IF_IPV4_ROUTE_EN | PPE_L3_IF_IPV6_ROUTE_EN);
	regmap_write(priv->regmap, PPE_IN_L3_IF_TBL(vsi) + 4,
		     FIELD_PREP(PPE_L3_IF_TTL_EXCEED_CMD,
				PPE_L3_IF_TTL_EXCEED_TO_CPU) |
		     FIELD_PREP(PPE_L3_IF_MAC_BITMAP, GENMASK(7, 0)));
	ppe_l3_if_mtu_set(priv, vsi, mtu + VLAN_ETH_HLEN + PPPOE_SES_HLEN);
	regmap_write(priv->regmap, PPE_L3_VSI_TBL(vsi),
		     PPE_L3_VSI_IF_VALID | FIELD_PREP(PPE_L3_VSI_IF_INDEX, vsi));

	if (vid) {
		/* The L3 stage does not parse through a residual 802.1Q tag:
		 * a PPPoE session is only recognised, and the inner tuple
		 * only reaches the flow lookup, once the rule also strips the
		 * tag. The hardware re-adds it toward the CPU (below), so a
		 * frame that misses the flow table still reaches the software
		 * stack in its on-wire form. Action and re-tag are in place
		 * before the rule goes live.
		 */
		regmap_write(priv->regmap, PPE_XLT_ACTION_TBL(xlt),
			     FIELD_PREP(PPE_XLT_CVID_CMD, PPE_XLT_CVID_DEL));
		regmap_write(priv->regmap, PPE_XLT_ACTION_W1(xlt),
			     PPE_XLT_VSI_CMD | FIELD_PREP(PPE_XLT_VSI, vsi));

		/* An egress translation rule, keyed on the VSI, puts the tag
		 * back on everything leaving toward the CPU port. Nothing else
		 * uses the egress table, so the ingress index names its entry
		 * too rather than needing a second allocator.
		 */
		regmap_write(priv->regmap, PPE_EG_XLT_ACTION(xlt),
			     FIELD_PREP(PPE_EG_XLT_CVID_CMD,
					PPE_EG_XLT_CVID_ADD) |
			     FIELD_PREP(PPE_EG_XLT_CVID, vid));
		regmap_write(priv->regmap, PPE_EG_XLT_ACTION_W1(xlt), 0);
		regmap_write(priv->regmap, PPE_EG_XLT_RULE(xlt),
			     PPE_EG_XLT_VALID |
			     FIELD_PREP(PPE_EG_XLT_PORT_BMP,
					BIT(QCA_PPE_CPU_PORT)) |
			     PPE_EG_XLT_VSI_INCL |
			     FIELD_PREP(PPE_EG_XLT_VSI, vsi) |
			     PPE_EG_XLT_VSI_VALID |
			     FIELD_PREP(PPE_EG_XLT_SKEY_FMT,
					PPE_XLT_SKEY_UNTAGGED));
		regmap_write(priv->regmap, PPE_EG_XLT_RULE_W1(xlt),
			     FIELD_PREP(PPE_EG_XLT_CKEY_FMT,
					PPE_XLT_SKEY_UNTAGGED));

		/* The frame-format fields are match bitmaps: a zero matches no
		 * frame at all. The uplink is single (C-)tagged, so the S-tag
		 * side must accept the untagged format for the rule to hit.
		 */
		regmap_write(priv->regmap, PPE_XLT_RULE_TBL(xlt),
			     PPE_XLT_VALID |
			     FIELD_PREP(PPE_XLT_PORT_BMP, BIT(port)) |
			     FIELD_PREP(PPE_XLT_SKEY_FMT,
					PPE_XLT_SKEY_UNTAGGED));
		regmap_write(priv->regmap, PPE_XLT_RULE_W1(xlt),
			     FIELD_PREP(PPE_XLT_CKEY_FMT_1,
					PPE_XLT_CKEY_TAGGED >> 1) |
			     PPE_XLT_CKEY_VID_INCL |
			     FIELD_PREP(PPE_XLT_CKEY_VID, vid));
		regmap_write(priv->regmap, PPE_XLT_RULE_TBL(xlt) + 8, 0);
	}

	up->ref = 1;
	up->port = port;
	up->vid = vid;
	up->vsi = vsi;
	up->xlt = xlt;

	return i;

err_vsi:
	ppe_vsi_free(priv, vsi);
err_xlt:
	if (xlt >= 0)
		ppe_xlt_idx_free(priv, &xlt);
	return ret;
}

static void ppe_uplink_put(struct qca_ppe_priv *priv, int i)
{
	struct ppe_uplink *up = &priv->uplink[i];
	int xlt = up->xlt;

	if (--up->ref)
		return;

	if (xlt >= 0) {
		/* Ingress first: the egress rule is what puts the tag back on
		 * a frame the ingress rule stripped, so taking it down first
		 * would surface stripped frames on the port device. Within the
		 * ingress rule the allocator clears the three key words before
		 * the action's, since a key left live over a zeroed action
		 * blackholes every frame it matches.
		 */
		ppe_xlt_idx_free(priv, &up->xlt);
		ppe_eg_xlt_clear(priv, xlt);
	}
	regmap_write(priv->regmap, PPE_L3_VSI_TBL(up->vsi), 0);
	ppe_tbl_clear(priv, PPE_IN_L3_IF_TBL(up->vsi), PPE_L3_IF_WORDS);
	if (ppe_res_put(priv->my_mac, up->mymac))
		ppe_tbl_clear(priv, PPE_MY_MAC_TBL(up->mymac),
			      PPE_MY_MAC_WORDS);
	ppe_vsi_free(priv, up->vsi);
}

/* Recognise a PPPoE session on its uplink, so the header is parsed through to
 * the inner IP. A re-dialled session has a new id and gets an entry of its own;
 * the old one goes with the last flow that used it. The uplink terminates PPPoE
 * from its first session on. Like the kernel's session lookup, the entry
 * matches only frames from the server's address.
 */
static int ppe_pppoe_session_get(struct qca_ppe_priv *priv, int up, u16 sid,
				 const u8 *smac)
{
	int port = priv->uplink[up].port;
	u32 vsi = priv->uplink[up].vsi;
	u32 w[PPE_L3_IF_WORDS];
	int i, free = -1;

	for (i = 0; i < PPE_PPPOE_SESSIONS; i++) {
		struct ppe_pppoe_session *s = &priv->pppoe[i];

		if (!s->ref) {
			if (free < 0)
				free = i;
			continue;
		}
		if (s->sid != sid || priv->uplink[s->uplink].port != port ||
		    !ether_addr_equal(s->smac, smac))
			continue;
		/* The same id on another VLAN of the port would match the
		 * same entry.
		 */
		if (s->uplink != up)
			return -ENOSPC;
		s->ref++;
		return i;
	}

	if (free < 0)
		return -ENOSPC;

	for (i = 0; i < PPE_L3_IF_WORDS; i++)
		regmap_read(priv->regmap, PPE_IN_L3_IF_TBL(vsi) + i * 4, &w[i]);
	w[1] |= PPE_L3_IF_PPPOE_EN;
	ppe_tbl_write(priv, PPE_IN_L3_IF_TBL(vsi), w, PPE_L3_IF_WORDS);

	priv->pppoe[free].ref = 1;
	priv->pppoe[free].sid = sid;
	priv->pppoe[free].uplink = up;
	ether_addr_copy(priv->pppoe[free].smac, smac);
	regmap_write(priv->regmap, PPE_PPPOE_SESSION(free),
		     FIELD_PREP(PPE_PPPOE_SESSION_ID, sid) |
		     FIELD_PREP(PPE_PPPOE_SESSION_PORT_BMP, BIT(port)) |
		     FIELD_PREP(PPE_PPPOE_SESSION_L3_IF, vsi));
	regmap_write(priv->regmap, PPE_PPPOE_SESSION_EXT1(free),
		     get_unaligned_be32(smac));
	regmap_write(priv->regmap, PPE_PPPOE_SESSION_EXT(free),
		     PPE_PPPOE_EXT_L3_IF_VALID | PPE_PPPOE_EXT_UC_VALID |
		     PPE_PPPOE_EXT_SMAC_VALID |
		     FIELD_PREP(PPE_PPPOE_EXT_SMAC_LO,
				get_unaligned_be16(smac + 4)));

	return free;
}

static void ppe_pppoe_session_put(struct qca_ppe_priv *priv, int i)
{
	if (--priv->pppoe[i].ref)
		return;

	regmap_write(priv->regmap, PPE_PPPOE_SESSION(i), 0);
	regmap_write(priv->regmap, PPE_PPPOE_SESSION_EXT(i), 0);
	regmap_write(priv->regmap, PPE_PPPOE_SESSION_EXT1(i), 0);
}

/* A port's PVID only classifies while the bridge enforces its VLANs; until
 * then the bridge VLANs are recorded and name no port.
 */
static u16 ppe_port_pvid(struct qca_ppe_priv *priv, int port)
{
	return priv->vlan_filtering & BIT(port) ? priv->port_pvid[port] : 0;
}

/* The VSI the ingress classification puts this rule's packets in - the routing
 * domain the flow belongs to, and the one ingress identifier its hardware entry
 * can carry. A domain that cannot be named is declined rather than encoded as
 * the bare tuple, which would let the flow forward traffic from another VLAN.
 */
static int ppe_flow_ingress_vsi(struct qca_ppe_priv *priv, int iport, u16 vid)
{
	struct qca_ppe_vlan_entry *vlan;

	lockdep_assert_held(&priv->vlan_lock);

	/* A VLAN-filtering bridge classifies a tagged frame into its VLAN's
	 * VSI and an untagged one into the PVID's, so the port's own VSI
	 * answers only without either.
	 */
	if (!vid)
		vid = ppe_port_pvid(priv, iport);
	if (!vid)
		return ppe_port_l3_vsi(priv, iport);

	vlan = ppe_vlan_find(priv, priv->port_br_dev[iport], vid);

	return vlan && (vlan->ports & priv->vlan_filtering & BIT(iport)) ?
	       (int)vlan->vsi : -EOPNOTSUPP;
}

/* The VSI a flow from this port and tag is keyed on: the uplink's, or the one
 * the port classifies into.
 */


/* A VLAN device on a port that no bridge VLAN classifies, such as lan1.20 on a
 * port of its own or on a port of a bridge that does not filter VLANs, routes
 * on an uplink of its own, as the WAN's VLAN does. Its frames are addressed to
 * the device, or to the bridge it is a port of.
 */
static int ppe_flow_vlan_uplink_get(struct qca_ppe_priv *priv, int iport,
				    u16 vid)
{
	struct dsa_port *dp = dsa_to_port(&priv->ds, iport);
	struct net_device *vdev, *br;
	u8 mac[ETH_ALEN];

	rcu_read_lock();
	vdev = __vlan_find_dev_deep_rcu(dp->user, htons(ETH_P_8021Q), vid);
	if (!vdev) {
		rcu_read_unlock();
		return -EOPNOTSUPP;
	}
	br = netif_is_bridge_port(vdev) ?
	     netdev_master_upper_dev_get_rcu(vdev) : NULL;
	ether_addr_copy(mac, (br ?: vdev)->dev_addr);
	rcu_read_unlock();

	return ppe_uplink_get(priv, iport, vid, mac, dp->user->mtu);
}

/* The flow lookup only runs on packets the L3 stage accepted, and nothing in
 * the L2 half of this driver sets that up: the ingress interface has to route,
 * and the frame's destination address has to match a MY_MAC entry. Both are
 * built here from the ports the flow actually uses, and torn down with the last
 * flow that needed them.
 *
 * One ingress L3 interface per VSI is enough, and using the VSI number as its
 * index keeps the two in step without a second allocator.
 */
static int ppe_flow_alloc_ingress(struct qca_ppe_priv *priv, int iport,
				  u16 vid, bool dsa_service,
				  const u8 *ingress_mac,
				  struct ppe_flow_entry *entry)
{
	struct dsa_port *dp = dsa_to_port(&priv->ds, iport);
	u32 words[PPE_MY_MAC_WORDS] = {};
	struct net_device *l3dev;
	u32 mtu;
	int vsi, ret;

	lockdep_assert_held(&priv->vlan_lock);

	/* An uplink's ingress interface belongs to the uplink, so take a
	 * reference rather than programming anything: a sibling flow going away
	 * must not pull the classification out from under this one. A tag no
	 * bridge VLAN classifies gets an uplink here, before any flow egresses
	 * it.
	 */
	ret = dsa_service ? -ENOENT : ppe_uplink_find(priv, iport, vid);
	if (ret >= 0) {
		priv->uplink[ret].ref++;
	} else if (!dsa_service && vid &&
		   ppe_flow_ingress_vsi(priv, iport, vid) < 0) {
		ret = ppe_flow_vlan_uplink_get(priv, iport, vid);
		if (ret < 0)
			return ret;
	}
	if (ret >= 0) {
		entry->in_uplink = ret;
		entry->src_if = priv->uplink[ret].vsi;
		entry->ivid = vid;
		return 0;
	}

	if (dsa_service) {
		entry->dsa_service = ppe_dsa_service_get(priv, iport, vid);
		if (entry->dsa_service < 0)
			return entry->dsa_service;
		vsi = priv->dsa_service[entry->dsa_service].vsi;
	} else {
		vsi = ppe_flow_ingress_vsi(priv, iport, vid);
		if (vsi < 0)
			return vsi;
	}

	entry->src_if = vsi;
	entry->ivid = dsa_service ? 0 :
		      vid ? vid : ppe_port_pvid(priv, iport);

	/* The address the packet is sent to is the address of the device that
	 * routes for this port, which is the bridge when there is one. That
	 * pointer is this driver's own, kept in step under this lock; DSA's is
	 * only safe to follow under rtnl, which the flowtable does not hold.
	 */
	l3dev = priv->port_br_dev[iport];
	if (!l3dev)
		l3dev = dp->user;

	ppe_entry_set(words, PPE_MY_MAC_ADDR_OFF, PPE_MY_MAC_ADDR_LEN,
		      ether_addr_to_u64(dsa_service && ingress_mac ?
					 ingress_mac : l3dev->dev_addr));
	ppe_entry_set(words, PPE_MY_MAC_VALID_OFF, PPE_MY_MAC_VALID_LEN, 1);

	ret = ppe_res_get(priv->my_mac, PPE_MY_MAC_ENTRIES, words,
			  PPE_MY_MAC_WORDS);
	if (ret < 0) {
		if (dsa_service)
			ppe_dsa_service_put(priv, entry->dsa_service);
		entry->dsa_service = -1;
		return ret;
	}
	entry->my_mac = ret;
	if (priv->my_mac[ret].refcount == 1)
		ppe_tbl_write(priv, PPE_MY_MAC_TBL(ret), words,
			      PPE_MY_MAC_WORDS);

	entry->l3_if = vsi;
	if (priv->l3_if_ref[vsi]++)
		return 0;

	mtu = l3dev->mtu + ETH_HLEN;

	/* Accept any of our addresses rather than tracking which MY_MAC entry
	 * belongs to which interface: the table only holds our own addresses.
	 */
	regmap_write(priv->regmap, PPE_IN_L3_IF_TBL(vsi),
		     PPE_L3_IF_IPV4_ROUTE_EN | PPE_L3_IF_IPV6_ROUTE_EN);
	regmap_write(priv->regmap, PPE_IN_L3_IF_TBL(vsi) + 4,
		     FIELD_PREP(PPE_L3_IF_TTL_EXCEED_CMD,
				PPE_L3_IF_TTL_EXCEED_TO_CPU) |
		     FIELD_PREP(PPE_L3_IF_MAC_BITMAP, GENMASK(7, 0)));
	ppe_l3_if_mtu_set(priv, vsi, mtu);
	regmap_write(priv->regmap, PPE_L3_VSI_TBL(vsi),
		     PPE_L3_VSI_IF_VALID | FIELD_PREP(PPE_L3_VSI_IF_INDEX, vsi));

	return 0;
}

static int ppe_flow_entry_destroy(struct qca_ppe_priv *priv,
				  struct ppe_flow_entry *entry);

static void ppe_flow_retire(struct qca_ppe_priv *priv,
			    struct ppe_flow_entry *entry)
{
	int ret = ppe_flow_entry_destroy(priv, entry);

	if (!ret) {
		kfree(entry);
		return;
	}
	/* No packet path or dying flowtable may retain the entry or a netdev.
	 * Its hardware-side leases stay allocated until mutation completion. */
	entry->block = NULL;
	if (entry->state == PPE_FLOW_LIVE)
		entry->state = PPE_FLOW_RETIRING;
	list_add_tail(&entry->list, &priv->flow_retire_list);
	priv->flow_retire_pending++;
	priv->flow_retire_failed++;
	priv->flow_retire_cookie = entry->cookie;
	priv->flow_retire_result = ret;
}

static void ppe_flow_unpublish(struct qca_ppe_priv *priv,
			       struct ppe_flow_entry *entry)
{
	rhashtable_remove_fast(&priv->flow_table, &entry->node,
			       ppe_flow_ht_params);
	list_del(&entry->list);
	ppe_flow_retire(priv, entry);
}

static void ppe_flow_drop(struct qca_ppe_priv *priv,
			  struct ppe_flow_entry *entry)
{
	priv->flow_stale++;
	ppe_flow_unpublish(priv, entry);
}


/* The ingress L3 interface holds its own MTU and is programmed with the first
 * flow that needs it, so a routing domain whose MTU changes while flows are
 * live keeps being forwarded to the old limit unless the change reaches it.
 */
static void ppe_flow_l3_mtu_set(struct qca_ppe_priv *priv, int port, int mtu)
{
	u32 vsi;
	int i;

	lockdep_assert_held(&priv->vlan_lock);

	for (i = 0; i < PPE_PPPOE_SESSIONS; i++)
		if (priv->uplink[i].ref && priv->uplink[i].port == port)
			ppe_l3_if_mtu_set(priv, priv->uplink[i].vsi,
					  mtu + VLAN_ETH_HLEN + PPPOE_SES_HLEN);

	vsi = ppe_port_l3_vsi(priv, port);
	if (priv->l3_if_ref[vsi])
		ppe_l3_if_mtu_set(priv, vsi, mtu + ETH_HLEN);

	for (i = 0; i < PPE_VSI_MAX; i++) {
		struct qca_ppe_vlan_entry *vlan = &priv->vlans[i];

		if (vlan->br_dev && vlan->ports & BIT(port) &&
		    priv->l3_if_ref[vlan->vsi])
			ppe_l3_if_mtu_set(priv, vlan->vsi, mtu + ETH_HLEN);
	}

	for (i = 0; i < QCA_PPE_DSA_SERVICE_MAX; i++) {
		struct ppe_dsa_service *service = &priv->dsa_service[i];

		if (service->refs && service->port == port &&
		    priv->l3_if_ref[service->vsi])
			ppe_l3_if_mtu_set(priv, service->vsi, mtu + ETH_HLEN);
	}
}

/* A MAC the entries were built against is changing: it is the address the
 * ingress accepts and the source the egress writes, so neither half can match
 * any more. Drop them and let the flowtable rebuild against the new one.
 */
static void ppe_flow_drop_port(struct qca_ppe_priv *priv, int port)
{
	struct ppe_flow_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &priv->flow_list, list)
		if (entry->iport == port ||
		    entry->oport == port)
			ppe_flow_drop(priv, entry);
}

void ppe_flow_purge_bridge_port(struct qca_ppe_priv *priv, int port)
{
	struct ppe_flow_entry *entry, *tmp;

	lockdep_assert_held(&priv->flow_lock);
	list_for_each_entry_safe(entry, tmp, &priv->flow_list, list)
		if (entry->bridge && (entry->iport == port || entry->oport == port))
			ppe_flow_drop(priv, entry);
}

static void ppe_flow_drop_bridge_dev(struct qca_ppe_priv *priv,
				     struct net_device *dev)
{
	struct ppe_flow_entry *entry, *tmp;

	lockdep_assert_held(&priv->flow_lock);
	list_for_each_entry_safe(entry, tmp, &priv->flow_list, list)
		if (entry->bridge && (entry->bridge_ifindex == dev->ifindex ||
				      entry->bridge_iif == dev->ifindex ||
				      entry->bridge_oif == dev->ifindex))
			ppe_flow_drop(priv, entry);
}

static int ppe_flow_switchdev_event(struct notifier_block *nb,
				    unsigned long event, void *ptr)
{
	struct qca_ppe_priv *priv = container_of(nb, struct qca_ppe_priv,
						 flow_switchdev_nb);
	struct switchdev_notifier_port_attr_info *info = ptr;
	struct net_device *dev = switchdev_notifier_info_to_dev(ptr);

	if (event != SWITCHDEV_PORT_ATTR_SET)
		return NOTIFY_DONE;
	switch (info->attr->id) {
	case SWITCHDEV_ATTR_ID_PORT_STP_STATE:
	case SWITCHDEV_ATTR_ID_PORT_BRIDGE_FLAGS:
	case SWITCHDEV_ATTR_ID_BRIDGE_VLAN_FILTERING:
		break;
	default:
		return NOTIFY_DONE;
	}
	guard(mutex)(&priv->flow_lock);
	guard(mutex)(&priv->vlan_lock);
	ppe_flow_drop_bridge_dev(priv, dev);
	return NOTIFY_DONE;
}

/* Only the flows of one direction change path when its shaper does: those
 * already handed to a shaper of it, and those it would now take, by the rules
 * of ppe_flow_shape().
 */
static void ppe_flow_shaper_drop(struct qca_ppe_priv *priv, bool upload)
{
	struct ppe_flow_entry *entry, *tmp;

	lockdep_assert_held(&priv->flow_lock);

	list_for_each_entry_safe(entry, tmp, &priv->flow_list, list) {
		bool hit;

		if (upload)
			hit = entry->xdev & PPE_XDEV_UL ||
			      (entry->oport != QCA_PPE_CPU_PORT &&
			       !priv->port_br_dev[entry->oport]);
		else
			hit = entry->xdev & PPE_XDEV_DL ||
			      entry->in_uplink >= 0 ||
			      !priv->port_br_dev[entry->iport];
		if (hit)
			ppe_flow_drop(priv, entry);
	}
}

/* Find where the uplink device - or, unnamed, the upload shaper's - meets the
 * switch, the way the flowtable resolves a device: down through PPPoE and
 * VLAN to a port of this switch. A path that does not reach one leaves
 * ul_port at -1. Returns whether it changed.
 */
static bool ppe_flow_shaper_path(struct qca_ppe_priv *priv)
{
	int port = -1, vid = 0, sid = 0, i;
	u64 h_dest = 0, h_source = 0;
	struct net_device_path_ctx ctx = {};
	struct net_device_path_stack stack;
	struct net_device *dev;
	bool changed;

	lockdep_assert_held(&priv->flow_lock);

	rcu_read_lock();
	i = priv->uplink_dev ?: priv->ul_shaper;
	dev = i ? dev_get_by_index_rcu(&init_net, i) : NULL;
	ctx.dev = dev;
	if (dev && !dev_fill_forward_path(&ctx, &stack)) {
		for (i = 0; i < stack.num_paths; i++) {
			const struct net_device_path *path = &stack.path[i];
			struct dsa_port *dp;

			switch (path->type) {
			case DEV_PATH_PPPOE:
				sid = path->encap.id;
				h_dest = ether_addr_to_u64(path->encap.h_dest);
				break;
			case DEV_PATH_VLAN:
				if (!vid) {
					vid = path->encap.id;
					h_source = ether_addr_to_u64(path->dev->dev_addr);
				}
				break;
			case DEV_PATH_DSA:
				dp = dsa_port_from_netdev((struct net_device *)path->dev);
				if (!IS_ERR(dp) && dp->ds == &priv->ds)
					port = path->dsa.port;
				if (!h_source)
					h_source = ether_addr_to_u64(path->dev->dev_addr);
				break;
			default:
				break;
			}
		}
	}
	rcu_read_unlock();

	if (port < 0)
		vid = sid = 0;
	changed = port != priv->ul_port || vid != priv->ul_vid ||
		  sid != priv->ul_sid;
	WRITE_ONCE(priv->ul_port, port);
	WRITE_ONCE(priv->ul_vid, vid);
	WRITE_ONCE(priv->ul_sid, sid);
	WRITE_ONCE(priv->ul_h_dest, h_dest);
	WRITE_ONCE(priv->ul_h_source, h_source);

	return changed;
}

/* A shaper is named, and the device of that name can go and come back with
 * another ifindex: a PPPoE session that reconnects brings up a new ppp
 * device. While it is gone that direction is not shaped rather than sent
 * to an ifindex that no longer exists, and a device that takes the name
 * becomes the shaper again.
 */
static void ppe_flow_shaper_track(struct qca_ppe_priv *priv,
				  struct net_device *dev, unsigned long event)
{
	int i;

	lockdep_assert_held(&priv->flow_lock);

	if (!net_eq(dev_net(dev), &init_net))
		return;

	for (i = 0; i < 3; i++) {
		bool upload = i == 1, uplink = i == 2;
		int *shaper = uplink ? &priv->uplink_dev :
			      upload ? &priv->ul_shaper : &priv->dl_shaper;
		const char *name = uplink ? priv->uplink_name :
				   upload ? priv->ul_shaper_name :
				   priv->dl_shaper_name;
		int now = *shaper;

		if (!*name)
			continue;

		if (now == dev->ifindex &&
		    (event == NETDEV_UNREGISTER || strcmp(dev->name, name)))
			now = 0;
		else if (event != NETDEV_UNREGISTER && !strcmp(dev->name, name) &&
			 dev->ifindex <= PPE_XDEV_IFINDEX)
			now = dev->ifindex;

		if (now == *shaper) {
			/* A PPPoE device is up before its session is. */
			if ((upload || uplink) && now && now == dev->ifindex &&
			    event == NETDEV_UP && ppe_flow_shaper_path(priv)) {
				ppe_flow_shaper_drop(priv, true);
				ppe_flow_shaper_drop(priv, false);
			}
			continue;
		}

		WRITE_ONCE(*shaper, now);
		if (upload || uplink) {
			ppe_flow_shaper_path(priv);
			ppe_flow_shaper_drop(priv, false);
		}
		ppe_flow_shaper_drop(priv, upload || uplink);
	}
}

/* Each half of the size check follows the device that owns it: the routing
 * domain's MRU follows the device that routes for the port, and the egress size
 * a flow was built with follows the port itself.
 *
 * Neither can be taken from DSA's port_change_mtu. That runs before the bridge
 * recomputes its own MTU, so a bridged port reads the previous one there, and
 * `ip link set br-lan mtu N` never reaches it at all. By the NETDEV_CHANGEMTU
 * of the device the value belongs to, both have settled - whichever of the two
 * the user set, and however the bridge chose to answer it.
 *
 * The egress size is part of the key that picks the interface entry, and that
 * slot is shared by every flow leaving the same way at the old size, so a live
 * flow cannot be moved to the new one. Drop those and let the flowtable rebuild
 * them against the size it has now.
 */
static int ppe_flow_netdev_event(struct notifier_block *nb, unsigned long event,
				 void *ptr)
{
	struct qca_ppe_priv *priv = container_of(nb, struct qca_ppe_priv,
						 netdev_nb);
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct ppe_flow_entry *entry, *tmp;
	struct dsa_port *dp;
	int i;

	if (event == NETDEV_REGISTER || event == NETDEV_CHANGENAME ||
	    event == NETDEV_UP) {
		guard(mutex)(&priv->flow_lock);
		guard(mutex)(&priv->vlan_lock);
		ppe_flow_shaper_track(priv, dev, event);
		return NOTIFY_DONE;
	}

	if (event != NETDEV_CHANGEMTU && event != NETDEV_CHANGEADDR &&
	    event != NETDEV_UNREGISTER && event != NETDEV_DOWN &&
	    event != NETDEV_CHANGEUPPER && event != NETDEV_CHANGEINFODATA)
		return NOTIFY_DONE;

	guard(mutex)(&priv->flow_lock);
	guard(mutex)(&priv->vlan_lock);

	ppe_flow_shaper_track(priv, dev, event);
	ppe_flow_drop_bridge_dev(priv, dev);
	if (event == NETDEV_CHANGEINFODATA)
		return NOTIFY_DONE;

	/* A stacked switch port invalidates the flow when its MTU, address or
	 * bridge membership changes. The next software packet rebuilds it with
	 * the new qca-8021q VID and egress size. */
	list_for_each_entry_safe(entry, tmp, &priv->flow_list, list)
		if (entry->icasc == dev->ifindex ||
		    entry->ocasc == dev->ifindex)
			ppe_flow_drop(priv, entry);

	/* WLAN is an egress endpoint rather than a DSA port.  Remove its
	 * bindings before the netdev is released; software flowtable processing
	 * will continue when no PPE entry can be rebuilt. */
	list_for_each_entry_safe(entry, tmp, &priv->flow_list, list)
		if ((entry->steer && entry->fw_ifindex == dev->ifindex) ||
		    (entry->xdev && (entry->xdev & PPE_XDEV_IFINDEX) == dev->ifindex))
			ppe_flow_drop(priv, entry);

	if (event == NETDEV_UNREGISTER || event == NETDEV_DOWN ||
	    event == NETDEV_CHANGEUPPER)
		return NOTIFY_DONE;

	/* Reached through the netdev itself rather than by walking the switch:
	 * this notifier is live before the switch is registered, and every
	 * netdev in the system passes through it. An uplink's MY_MAC is the
	 * address of the VLAN device on the port, so that one counts too.
	 */
	if (event == NETDEV_CHANGEADDR && is_vlan_dev(dev))
		dp = dsa_port_from_netdev(vlan_dev_real_dev(dev));
	else
		dp = dsa_port_from_netdev(dev);
	if (!IS_ERR(dp) && dp->ds == &priv->ds) {
		if (event == NETDEV_CHANGEADDR) {
			ppe_flow_drop_port(priv, dp->index);
		} else {
			list_for_each_entry_safe(entry, tmp, &priv->flow_list,
						 list)
				if (entry->oport == dp->index)
					ppe_flow_drop(priv, entry);

			if (!priv->port_br_dev[dp->index])
				ppe_flow_l3_mtu_set(priv, dp->index, dev->mtu);
		}
	}

	for (i = 0; i < QCA_PPE_MAX_PORTS; i++) {
		if (priv->port_br_dev[i] != dev)
			continue;

		if (event == NETDEV_CHANGEADDR)
			ppe_flow_drop_port(priv, i);
		else
			ppe_flow_l3_mtu_set(priv, i, dev->mtu);
	}

	return NOTIFY_DONE;
}

static void ppe_flow_free_ingress(struct qca_ppe_priv *priv,
				  struct ppe_flow_entry *entry)
{
	lockdep_assert_held(&priv->vlan_lock);

	if (entry->in_uplink >= 0) {
		ppe_uplink_put(priv, entry->in_uplink);
	} else {
		if (entry->l3_if >= 0 && !--priv->l3_if_ref[entry->l3_if]) {
			regmap_write(priv->regmap, PPE_L3_VSI_TBL(entry->l3_if), 0);
			ppe_tbl_clear(priv, PPE_IN_L3_IF_TBL(entry->l3_if),
				      PPE_L3_IF_WORDS);
		}
		if (ppe_res_put(priv->my_mac, entry->my_mac))
			ppe_tbl_clear(priv, PPE_MY_MAC_TBL(entry->my_mac), PPE_MY_MAC_WORDS);
		if (entry->dsa_service >= 0)
			ppe_dsa_service_put(priv, entry->dsa_service);
	}
	if (entry->vsi_ref && !--priv->vsi_flow_refs[entry->ingress_vsi] &&
	    test_bit(entry->ingress_vsi, priv->vsi_retired))
		ppe_vsi_free(priv, entry->ingress_vsi);
}


/* The routing domain a set of flows was built for is going away. Their entries
 * name its VSI as the ingress they match on, and that number is about to be
 * handed to another domain, so they cannot be left behind: they would match the
 * new domain's traffic, and their own teardown would later clear the new
 * owner's L3 interface out from under it. The flowtable reinstalls whatever is
 * still live, against whatever domain it belongs to then.
 */
void ppe_flow_purge_vsi(struct qca_ppe_priv *priv, u32 vsi)
{
	struct ppe_flow_entry *entry, *tmp;

	lockdep_assert_held(&priv->flow_lock);

	list_for_each_entry_safe(entry, tmp, &priv->flow_list, list) {
		if (!entry->vsi_ref || entry->ingress_vsi != vsi)
			continue;

		ppe_flow_drop(priv, entry);
	}
}

/* A flow that ingresses on the uplink's port and VLAN and was installed before
 * the uplink had a classification named the port's plain VSI, and can never
 * match now that one exists. Drop those entries: the flowtable reinstalls
 * whatever is still live.
 */
static void ppe_flow_purge_ingress(struct qca_ppe_priv *priv,
				   const struct ppe_uplink *up)
{
	struct ppe_flow_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &priv->flow_list, list) {
		if (entry->bridge || entry->iport != up->port || entry->ivid != up->vid ||
		    entry->src_if == up->vsi)
			continue;

		ppe_flow_drop(priv, entry);
	}
}

static void ppe_flow_free_egress(struct qca_ppe_priv *priv,
				 struct ppe_flow_entry *entry)
{

	if (ppe_res_put(priv->nexthop, entry->nexthop))
		ppe_tbl_clear(priv, PPE_IN_NEXTHOP_TBL(entry->nexthop),
			      PPE_NEXTHOP_WORDS);
	if (ppe_res_put(priv->pub_ip, entry->pub_ip))
		regmap_write(priv->regmap, PPE_PUB_IP_TBL(entry->pub_ip), 0);
	if (ppe_res_put(priv->eg_l3_if, entry->eg_l3_if)) {
		ppe_tbl_clear(priv, PPE_EG_L3_IF_TBL(entry->eg_l3_if),
			      PPE_EG_L3_IF_WORDS);
		ppe_tbl_clear(priv, PPE_IN_L3_IF_TBL(entry->eg_l3_if),
			      PPE_L3_IF_WORDS);
	}

	if (entry->session >= 0)
		ppe_pppoe_session_put(priv, entry->session);
	if (entry->uplink >= 0)
		ppe_uplink_put(priv, entry->uplink);
	if (entry->dsa_egress_port >= 0)
		ppe_dsa_core_port_put(priv, entry->dsa_egress_port, false);
}

static bool ppe_cpu_xmit = true;
module_param_named(cpu_xmit, ppe_cpu_xmit, bool, 0644);
MODULE_PARM_DESC(cpu_xmit,
		 "Route flows to devices outside the switch in hardware and pass them out through the CPU port");

/* A flow out of a wired device the switch cannot reach, such as a tunnel,
 * is still routed by the PPE. Its frames come to the CPU finished, and the
 * driver only queues them on the device. The device has to take Ethernet
 * frames as the nexthop builds them, with no tag or session to push.
 */
static bool ppe_dev_is_wifi(const struct net_device *dev)
{
#if IS_ENABLED(CONFIG_CFG80211_HEADERS)
	return dev->ieee80211_ptr;
#else
	return false;
#endif
}

static bool ppe_cpu_xmit_ok(const struct ppe_flow_data *data)
{
	return ppe_cpu_xmit && !ppe_dev_is_wifi(data->odev) &&
	       net_eq(dev_net(data->odev), &init_net) &&
	       netif_running(data->odev) &&
	       data->odev->type == ARPHRD_ETHER &&
	       !data->vlan_valid && !data->pppoe_valid;
}

/* Whether a flow's side of the switch is the shaped uplink: the port, VLAN
 * and, upstream, the PPPoE session the upload shaper's device sits on. A
 * second routed port, a DMZ or another VLAN on the WAN port is not, and its
 * flows stay in hardware. Until the path is known, any routed port is.
 */
static bool ppe_flow_on_uplink(struct qca_ppe_priv *priv, int port, u16 vid,
			       u16 sid, bool upload)
{
	if (priv->ul_port < 0)
		return !priv->port_br_dev[port];

	return port == priv->ul_port && vid == priv->ul_vid &&
	       (!upload || sid == priv->ul_sid);
}

/* The PPE has no queue that can run CAKE or an L4S AQM: it drops on a
 * buffer count and cannot mark. Where the shaping is done by a qdisc
 * instead, the flows that cross the shaped link still have their lookup,
 * NAT and nexthop done in hardware, and only their frames come to the CPU,
 * finished, to pass the qdisc on the way out: a download flow through the
 * ifb that carries the download qdisc, an upload flow through the uplink
 * device, whose own qdisc, PPPoE and VLAN stack then do the rest. The
 * uplink is where the upload shaper's device meets the switch; a download
 * enters by it. Returns the port the frames leave the switch by.
 */
static int ppe_flow_shape(struct qca_ppe_priv *priv, struct ppe_flow_data *data,
			  struct ppe_flow_entry *entry, int port)
{
	int ul = READ_ONCE(priv->ul_shaper), dl = READ_ONCE(priv->dl_shaper);

	if (ul && port != QCA_PPE_CPU_PORT &&
	    ppe_flow_on_uplink(priv, port,
			       data->vlan_valid ? data->vlan_id : 0,
			       data->pppoe_valid ? data->pppoe_sid : 0, true)) {
		entry->xdev = ul | PPE_XDEV_UL;
		entry->xport = port + 1;
		return QCA_PPE_CPU_PORT;
	}

	/* A download to a tagged LAN VLAN comes to the CPU untagged: the tag
	 * is added after the shaper, as the VLAN stack would.
	 */
	if (dl && !data->pppoe_valid &&
	    ppe_flow_on_uplink(priv, entry->iport, data->ivid, 0, false)) {
		if (port != QCA_PPE_CPU_PORT) {
			entry->xdev = data->odev->ifindex;
			entry->xport = port + 1;
		}
		entry->xdev |= PPE_XDEV_DL;
		entry->vid = data->vlan_valid ? data->vlan_id : 0;
		return QCA_PPE_CPU_PORT;
	}

	return port;
}

/* An upload frame arrives NATed, without the conntrack the stack would have
 * attached, so the qdisc could not tell the LAN host behind it: CAKE's nat
 * mode finds the connection by the frame's own tuple, which after NAT is
 * neither of the connection's. The conntrack goes with the frame as it would
 * on the forward path. The entry holds a reference until it is destroyed; a
 * conntrack is freed by RCU and reused only as one, so taking a reference
 * that is not zero is safe here.
 */
static void ppe_flow_ct_attach(struct qca_ppe_priv *priv, struct sk_buff *skb,
			       u32 flow)
{
	unsigned long v = READ_ONCE(priv->flow_ct[flow]);
	struct nf_conn *ct = (struct nf_conn *)(v & ~1UL);

	if (ct && refcount_inc_not_zero(&ct->ct_general.use))
		nf_ct_set(skb, ct, v & 1 ? IP_CT_ESTABLISHED_REPLY :
				   IP_CT_ESTABLISHED);
}

/* The flowtable's cookie is the tuple of the offloaded direction. */
static unsigned long ppe_flow_ct_get(unsigned long cookie)
{
	const struct flow_offload_tuple *tuple = (void *)cookie;
	struct flow_offload_tuple_rhash *th;
	struct flow_offload *flow;

	th = container_of(tuple, struct flow_offload_tuple_rhash, tuple);
	flow = container_of(th, struct flow_offload, tuplehash[tuple->dir]);
	nf_conntrack_get(&flow->ct->ct_general);

	return (unsigned long)flow->ct | (tuple->dir == FLOW_OFFLOAD_DIR_REPLY);
}

/* The DSCP a firewall rule stamped into the conntrack mark on the flow's
 * first packets, as act_ctinfo reads it: bit 7 says it is set, bits 0-5 are
 * the value. An offloaded flow's later packets pass no such rule, so its
 * frames are given it when they are handed over.
 */
static u8 ppe_flow_ct_dscp(unsigned long cookie)
{
#if IS_ENABLED(CONFIG_NF_CONNTRACK_MARK)
	const struct flow_offload_tuple *tuple = (void *)cookie;
	struct flow_offload_tuple_rhash *th;
	struct flow_offload *flow;
	u32 mark;

	th = container_of(tuple, struct flow_offload_tuple_rhash, tuple);
	flow = container_of(th, struct flow_offload, tuplehash[tuple->dir]);
	mark = READ_ONCE(flow->ct->mark);
	if (mark & 0x80)
		return 0x80 | (mark & 0x3f);
#endif
	return 0;
}

/* Only the DSCP bits: the ECN field stays as the sender and the path set it. */
static void ppe_flow_dscp_apply(struct sk_buff *skb, u8 dscp)
{
	if (skb->protocol == htons(ETH_P_IP))
		ipv4_change_dsfield(ip_hdr(skb), INET_ECN_MASK, dscp << 2);
	else if (skb->protocol == htons(ETH_P_IPV6))
		ipv6_change_dsfield(ipv6_hdr(skb), INET_ECN_MASK, dscp << 2);
}

static void ppe_flow_slot_set(struct qca_ppe_priv *priv,
			      struct ppe_flow_entry *entry, u32 index)
{
	WRITE_ONCE(priv->flow_dscp[index], entry->dscp);
	WRITE_ONCE(priv->flow_vid[index], entry->vid);
	WRITE_ONCE(priv->flow_port[index], entry->xport);
	WRITE_ONCE(priv->flow_ct[index], entry->ct);
	WRITE_ONCE(priv->flow_xdev[index], entry->xdev);
	entry->xdev_bound = true;
}

static void ppe_flow_slot_clear(struct qca_ppe_priv *priv, u32 index)
{
	WRITE_ONCE(priv->flow_xdev[index], 0);
	WRITE_ONCE(priv->flow_ct[index], 0);
	WRITE_ONCE(priv->flow_dscp[index], 0);
	WRITE_ONCE(priv->flow_vid[index], 0);
	WRITE_ONCE(priv->flow_port[index], 0);
}

/* The shaper device. It stands where an ifb would, in front of the devices
 * a direction's flows leave by, and carries that direction's qdisc. What the
 * host forwards or sends itself is redirected to it, as to an ifb, and it
 * hands that on to the device the redirect came from, or back to the receive
 * path. A frame the PPE built for a flow is handed to it by ppe_flow_xmit()
 * instead: one for a switch port goes straight into the conduit's ring once
 * the qdisc lets it go, with no other device or qdisc on its way; one for a
 * wired device outside the switch, such as a tunnel, to that device.
 */
struct ppe_shaper {
	struct qca_ppe_priv *priv;
	struct net_device *conduit;
};

static const struct net_device_ops ppe_shaper_ops;

static bool ppe_shaper_is(const struct net_device *dev)
{
	return dev->netdev_ops == &ppe_shaper_ops;
}

/* Whether upload flows are built whole by the PPE: their shaper is this
 * driver's device, which sends them to the conduit.
 */
static bool ppe_flow_ul_push(struct qca_ppe_priv *priv)
{
	struct net_device *sh = priv->shaper_dev[1];

	lockdep_assert_held(&priv->flow_lock);

	return sh && priv->ul_shaper == sh->ifindex;
}

/* Into the conduit's ring for switch port @port, as the conduit's own
 * transmit would be called from its qdisc, on the queue of the ring the
 * frame arrived on. A full ring leaves the frame in the shaper's qdisc,
 * which offers it again.
 */
static netdev_tx_t ppe_shaper_conduit_xmit(struct sk_buff *skb,
					   struct net_device *dev,
					   unsigned int port)
{
	struct ppe_shaper *sh = netdev_priv(dev);
	struct net_device *conduit = sh->conduit;
	struct netdev_queue *txq;
	unsigned int len = skb->len;
	netdev_tx_t rc;
	u16 qi;

	qi = skb_get_queue_mapping(skb);
	if (unlikely(qi >= conduit->real_num_tx_queues))
		qi %= conduit->real_num_tx_queues;
	txq = netdev_get_tx_queue(conduit, qi);

	skb->dev = conduit;
	__netif_tx_lock(txq, smp_processor_id());
	if (unlikely(netif_xmit_frozen_or_stopped(txq)))
		rc = NETDEV_TX_BUSY;
	else
		rc = qca_edma_port_xmit(skb, conduit, port);
	__netif_tx_unlock(txq);

	if (unlikely(rc == NETDEV_TX_BUSY)) {
		skb->dev = dev;
		atomic_inc(&sh->priv->shaper_busy);
		return NETDEV_TX_BUSY;
	}
	dev_sw_netstats_tx_add(dev, 1, len);

	return NETDEV_TX_OK;
}

/* What the host routed to a PPPoE uplink, redirected here from its egress:
 * built the way the switch builds an offloaded upload frame, with the
 * session, the VLAN tag and the port the uplink's own path resolves to, so
 * that it too goes straight to the conduit rather than through the PPP,
 * PPPoE, VLAN and switch port devices. Anything else goes the ifb way.
 */
static int ppe_shaper_ul_frame(struct qca_ppe_priv *priv, struct sk_buff *skb,
			       struct net_device *to, struct net_device *conduit)
{
	int port = READ_ONCE(priv->ul_port);
	u16 sid = READ_ONCE(priv->ul_sid), vid = READ_ONCE(priv->ul_vid);
	struct pppoe_hdr *ph;
	struct ethhdr *eth;
	__be16 proto;
	int len;

	if (to->ifindex != READ_ONCE(priv->uplink_dev) ||
	    to->type != ARPHRD_PPP || port < 0 || !sid || skb_is_gso(skb) ||
	    skb->from_ingress || skb_network_offset(skb))
		return -1;

	if (skb->protocol == htons(ETH_P_IP))
		proto = htons(PPP_IP);
	else if (skb->protocol == htons(ETH_P_IPV6))
		proto = htons(PPP_IPV6);
	else
		return -1;

	if (skb->ip_summed == CHECKSUM_PARTIAL && skb_checksum_help(skb))
		return -1;
	if (skb_cow_head(skb, ETH_HLEN + PPPOE_SES_HLEN +
			 conduit->needed_headroom))
		return -1;

	len = skb->len;
	*(__be16 *)__skb_push(skb, sizeof(proto)) = proto;
	ph = __skb_push(skb, sizeof(*ph));
	ph->ver = 1;
	ph->type = 1;
	ph->code = 0;
	ph->sid = htons(sid);
	ph->length = htons(len + sizeof(proto));
	eth = __skb_push(skb, ETH_HLEN);
	u64_to_ether_addr(READ_ONCE(priv->ul_h_dest), eth->h_dest);
	u64_to_ether_addr(READ_ONCE(priv->ul_h_source), eth->h_source);
	eth->h_proto = htons(ETH_P_PPP_SES);
	skb_reset_mac_header(skb);
	skb->protocol = htons(ETH_P_PPP_SES);
	if (vid)
		__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), vid);

	return port;
}

/* A frame ppe_flow_xmit() built for a switch port is not marked redirected
 * and carries one more than that port in skb_iif; everything else names the
 * device to hand it on to there, as for an ifb.
 */
static netdev_tx_t ppe_shaper_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct ppe_shaper *sh = netdev_priv(dev);
	struct net_device *to;
	int port;

	if (!skb->redirected)
		return ppe_shaper_conduit_xmit(skb, dev, skb->skb_iif - 1);

	to = dev_get_by_index_rcu(dev_net(dev), skb->skb_iif);
	if (unlikely(!to)) {
		dev_core_stats_tx_dropped_inc(dev);
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	if (dev == READ_ONCE(sh->priv->shaper_dev[1])) {
		port = ppe_shaper_ul_frame(sh->priv, skb, to, sh->conduit);
		if (port >= 0)
			return ppe_shaper_conduit_xmit(skb, dev, port);
	}

	/* As an ifb hands a frame on: tc and netfilter are skipped on the
	 * way out, so that the redirect that brought it is not taken again.
	 */
	dev_sw_netstats_tx_add(dev, 1, skb->len);
	skb->redirected = 0;
#ifdef CONFIG_NET_CLS_ACT
	skb->tc_skip_classify = 1;
#endif
	nf_skip_egress(skb, true);
	skb->dev = to;
	skb->skb_iif = dev->ifindex;

	if (skb->from_ingress) {
		skb_pull_rcsum(skb, skb->mac_len);
		netif_rx(skb);
	} else {
		dev_queue_xmit(skb);
	}

	return NETDEV_TX_OK;
}

static const struct net_device_ops ppe_shaper_ops = {
	.ndo_start_xmit = ppe_shaper_xmit,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_get_stats64 = dev_get_tstats64,
};

#define PPE_SHAPER_FEATURES	(NETIF_F_HW_CSUM | NETIF_F_SG | \
				 NETIF_F_FRAGLIST | NETIF_F_TSO_ECN | \
				 NETIF_F_TSO | NETIF_F_TSO6 | \
				 NETIF_F_GSO_ENCAP_ALL | NETIF_F_HIGHDMA | \
				 NETIF_F_HW_VLAN_CTAG_TX | \
				 NETIF_F_HW_VLAN_STAG_TX)

static void ppe_shaper_setup(struct net_device *dev)
{
	dev->netdev_ops = &ppe_shaper_ops;
	ether_setup(dev);
	dev->tx_queue_len = 32;
	dev->features |= PPE_SHAPER_FEATURES;
	dev->hw_features |= dev->features;
	dev->hw_enc_features |= dev->features;
	dev->vlan_features |= PPE_SHAPER_FEATURES & ~(NETIF_F_HW_VLAN_CTAG_TX |
						      NETIF_F_HW_VLAN_STAG_TX);
	dev->flags |= IFF_NOARP;
	dev->flags &= ~IFF_MULTICAST;
	dev->priv_flags &= ~IFF_TX_SKB_SHARING;
	dev->lltx = true;
	dev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;
	netif_keep_dst(dev);
	eth_hw_addr_random(dev);
	dev->needs_free_netdev = true;
	dev->min_mtu = 0;
	dev->max_mtu = 0;
	netif_set_tso_max_size(dev, GSO_MAX_SIZE);
}

static struct net_device *ppe_shaper_create(struct qca_ppe_priv *priv,
					    const char *name)
{
	struct net_device *dev;
	struct ppe_shaper *sh;
	int ret;

	dev = alloc_netdev_mqs(sizeof(*sh), name, NET_NAME_USER,
			       ppe_shaper_setup, num_possible_cpus(), 1);
	if (!dev)
		return ERR_PTR(-ENOMEM);
	sh = netdev_priv(dev);
	sh->priv = priv;
	sh->conduit = dsa_to_port(&priv->ds, QCA_PPE_CPU_PORT)->conduit;

	ret = register_netdev(dev);
	if (ret) {
		free_netdev(dev);
		return ERR_PTR(ret);
	}

	return dev;
}

/* An upload frame the PPE built for the shaper device carries its PPPoE
 * header already; the qdisc classifies and marks by the IP header behind it,
 * which the network header points at. The uplink's VLAN tag, @vid, is in the
 * frame or, when the engine took it off on the way to the CPU, added on the
 * way out as the host would.
 */
static bool ppe_flow_push_parse(struct sk_buff *skb, u16 vid)
{
	unsigned int off = ETH_HLEN;
	__be16 type = eth_hdr(skb)->h_proto;

	if (eth_type_vlan(type)) {
		if (skb_headlen(skb) < off + VLAN_HLEN)
			return false;
		type = ((struct vlan_hdr *)(skb->data + off))->h_vlan_encapsulated_proto;
		off += VLAN_HLEN;
	} else if (vid) {
		__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), vid);
	}

	if (type == htons(ETH_P_PPP_SES)) {
		__be16 ppp;

		if (skb_headlen(skb) < off + PPPOE_SES_HLEN)
			return false;
		ppp = *(__be16 *)(skb->data + off + sizeof(struct pppoe_hdr));
		type = ppp == htons(PPP_IP) ? htons(ETH_P_IP) :
		       ppp == htons(PPP_IPV6) ? htons(ETH_P_IPV6) : 0;
		off += PPPOE_SES_HLEN;
	}

	if (type != htons(ETH_P_IP) && type != htons(ETH_P_IPV6))
		return false;

	skb->protocol = type;
	skb_set_network_header(skb, off);

	return true;
}

/* A frame the PPE routed to the CPU port for a flow whose egress is a
 * device outside the switch, or behind a software shaper. It arrives
 * finished, so it goes straight out.
 */
bool ppe_flow_xmit(struct dsa_switch *ds, struct sk_buff *skb, u32 flow)
{
	struct qca_ppe_priv *priv = ds_to_priv(ds);
	struct net_device *dev, *shaper, *uplink;
	int xdev, up;
	u8 dscp, xport;

	if (flow >= priv->data->num_flow_entries)
		return false;
	xdev = READ_ONCE(priv->flow_xdev[flow]);
	if (!xdev)
		return false;

	dev = dev_get_by_index_rcu(dev_net(skb->dev), xdev & PPE_XDEV_IFINDEX);
	if (unlikely(!dev || !netif_running(dev)))
		goto drop;

	/* An upload shaper may sit on an ifb in front of the uplink, as the
	 * download one does: the frame is then built for the uplink and the
	 * ifb hands it on there once its qdisc lets it go. A frame the PPE
	 * built whole needs no uplink.
	 */
	uplink = dev;
	up = READ_ONCE(priv->uplink_dev);
	if ((xdev & (PPE_XDEV_UL | PPE_XDEV_PUSH)) == PPE_XDEV_UL && up &&
	    up != dev->ifindex) {
		uplink = dev_get_by_index_rcu(dev_net(dev), up);
		if (unlikely(!uplink || !netif_running(uplink)))
			goto drop;
	}

	/* What the egress classifies on - tc, the Wi-Fi access category -
	 * reads the IP header and the flow hash, so both are set as the
	 * receive path would have. The flow index is unique per flow.
	 */
	skb->dev = dev;
	skb_reset_mac_header(skb);
	skb_set_network_header(skb, ETH_HLEN);
	skb->protocol = eth_hdr(skb)->h_proto;
	skb_set_hash(skb, hash_32(flow, 32), PKT_HASH_TYPE_L4);

	if (xdev & PPE_XDEV_PUSH) {
		if (unlikely(!ppe_flow_push_parse(skb, READ_ONCE(priv->ul_vid))))
			goto drop;
	} else if ((xdev & PPE_XDEV_UL) && uplink->type != ARPHRD_ETHER) {
		/* An uplink that is not Ethernet, PPPoE's ppp device, takes
		 * the IP packet and builds its own link header.
		 */
		__skb_pull(skb, ETH_HLEN);
		skb_reset_network_header(skb);
		skb_reset_mac_header(skb);
	}

	dscp = READ_ONCE(priv->flow_dscp[flow]);
	if (dscp)
		ppe_flow_dscp_apply(skb, dscp & 0x3f);

	xport = READ_ONCE(priv->flow_port[flow]);

	/* Into the download shaper the way act_mirred sends an egress frame
	 * to an ifb: the ifb sends it on to skb_iif once its qdisc lets it go.
	 * The driver's own shaper device sends a frame for a switch port to
	 * the conduit instead. A shaper that has gone away leaves the frame
	 * unshaped, not lost.
	 */
	if (xdev & PPE_XDEV_DL) {
		shaper = dev_get_by_index_rcu(dev_net(dev),
					      READ_ONCE(priv->dl_shaper));
		if (likely(shaper && netif_running(shaper))) {
			u16 vid = READ_ONCE(priv->flow_vid[flow]);

			if (vid)
				__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q),
						       vid);
			skb->dev = shaper;
			if (xport && ppe_shaper_is(shaper)) {
				skb->skb_iif = xport;
			} else {
				skb->skb_iif = dev->ifindex;
				skb_set_redirected(skb, false);
			}
		} else {
			atomic_inc(&priv->flow_shaper_miss);
		}
	} else if (xdev & PPE_XDEV_UL) {
		ppe_flow_ct_attach(priv, skb, flow);
		if (xdev & PPE_XDEV_PUSH) {
			if (unlikely(!xport))
				goto drop;
			skb->skb_iif = xport;
		} else if (uplink != dev) {
			skb->skb_iif = uplink->ifindex;
			skb_set_redirected(skb, false);
		}
	}

	dev_queue_xmit(skb);
	return true;

drop:
	atomic_inc(&priv->flow_xmit_drop);
	kfree_skb(skb);
	return true;
}

/* Flows already handed over keep the DSCP they were built with, so a change
 * rebuilds them.
 */
void ppe_flow_ct_dscp_set(struct qca_ppe_priv *priv, bool on)
{
	guard(mutex)(&priv->flow_lock);
	guard(mutex)(&priv->vlan_lock);

	if (priv->ct_dscp == on)
		return;
	priv->ct_dscp = on;
	ppe_flow_shaper_drop(priv, false);
	ppe_flow_shaper_drop(priv, true);
}

void ppe_flow_uplink_get(struct qca_ppe_priv *priv, char *name)
{
	guard(mutex)(&priv->flow_lock);

	strscpy(name, priv->uplink_name, IFNAMSIZ);
}

/* Name the WAN's L3 device, or clear it with an empty name. It decides which
 * flows are the shaped ones in both directions, so the flows of both are
 * rebuilt.
 */
int ppe_flow_uplink_set(struct qca_ppe_priv *priv, const char *name)
{
	struct net_device *dev;
	int ifindex = 0;

	if (*name) {
		dev = dev_get_by_name(&init_net, name);
		if (!dev)
			return -ENODEV;
		ifindex = dev->ifindex;
		dev_put(dev);
	}

	guard(mutex)(&priv->flow_lock);
	guard(mutex)(&priv->vlan_lock);

	priv->uplink_dev = ifindex;
	strscpy(priv->uplink_name, name, IFNAMSIZ);
	ppe_flow_shaper_path(priv);
	ppe_flow_shaper_drop(priv, false);
	ppe_flow_shaper_drop(priv, true);

	return 0;
}

void ppe_flow_shaper_get(struct qca_ppe_priv *priv, bool upload, char *name)
{
	guard(mutex)(&priv->flow_lock);

	strscpy(name, upload ? priv->ul_shaper_name : priv->dl_shaper_name,
		IFNAMSIZ);
}

/* Name the device whose qdisc shapes one direction, or clear it with an
 * empty name. A name no device has yet gets the driver's own shaper device,
 * which goes again once the direction is cleared or named another device.
 * The flows of that direction already in the table were built for the old
 * path, so they are dropped and the flowtable offloads them again on the new
 * one.
 */
int ppe_flow_shaper_set(struct qca_ppe_priv *priv, bool upload,
			const char *name)
{
	struct net_device *dev, *own = NULL, *old;
	int ifindex = 0;

	if (*name) {
		dev = dev_get_by_name(&init_net, name);
		if (!dev) {
			own = ppe_shaper_create(priv, name);
			if (IS_ERR(own))
				return PTR_ERR(own);
			dev = own;
			dev_hold(dev);
		}
		ifindex = dev->ifindex;
		dev_put(dev);
		if (ifindex > PPE_XDEV_IFINDEX) {
			if (own)
				unregister_netdev(own);
			return -ERANGE;
		}
	}

	scoped_guard(mutex, &priv->flow_lock) {
		guard(mutex)(&priv->vlan_lock);

		old = priv->shaper_dev[upload];
		if (old && old->ifindex == ifindex)
			old = NULL;
		else
			priv->shaper_dev[upload] = own;

		if (upload) {
			WRITE_ONCE(priv->ul_shaper, ifindex);
			strscpy(priv->ul_shaper_name, name, IFNAMSIZ);
			/* The uplink decides which downloads are shaped as well. */
			ppe_flow_shaper_path(priv);
			ppe_flow_shaper_drop(priv, false);
		} else {
			WRITE_ONCE(priv->dl_shaper, ifindex);
			strscpy(priv->dl_shaper_name, name, IFNAMSIZ);
		}

		ppe_flow_shaper_drop(priv, upload);
	}

	/* Frames on their way to it found it by ifindex under RCU, which the
	 * unregistration waits out.
	 */
	if (old)
		unregister_netdev(old);

	return 0;
}

/* One path's egress L3 interface and nexthop words: out of @port with the
 * headers pushed in hardware (@push), or handed over through the CPU port for
 * the host to build them. Returns the egress interface index.
 */
static int ppe_flow_egress_path(struct qca_ppe_priv *priv,
				struct ppe_flow_data *data,
				struct ppe_flow_entry *entry, int port,
				int nh_port, bool push, bool snat, bool dnat,
				u32 *nh)
{
	u32 words[PPE_NEXTHOP_WORDS] = {};
	struct dsa_port *odp;
	int eg_l3_if, ret;
	u32 eg_mtu;
	u64 mac;

	mac = ether_addr_to_u64(data->eth.h_source);
	ppe_entry_set(words, PPE_EG_L3_IF_MAC_OFF, PPE_EG_L3_IF_MAC_LEN, mac);
	if (data->pppoe_valid && push) {
		ppe_entry_set(words, PPE_EG_L3_IF_SESSION_OFF,
			      PPE_EG_L3_IF_SESSION_LEN, data->pppoe_sid);
		ppe_entry_set(words, PPE_EG_L3_IF_PPPOE_EN_OFF,
			      PPE_EG_L3_IF_PPPOE_EN_LEN, 1);
	}
	/* The size a routed frame leaves at, which the hardware compares before
	 * it egresses and sends the frame to the CPU when it does not fit. The
	 * port's mtu carries one mac header and the vlan tag the nexthop pushes;
	 * a pppoe session header rides inside that mtu rather than on top of it.
	 * It joins the key because two interfaces sharing a source address need
	 * separate entries when they do not share a size.
	 */
	odp = dsa_to_port(&priv->ds, port);
	eg_mtu = (entry->ocasc || port == QCA_PPE_CPU_PORT ?
		  data->odev->mtu : odp->user->mtu) + ETH_HLEN +
		 ((data->vlan_valid && push) +
		  (data->egress_svid_valid && push)) * VLAN_HLEN;
	/* A flow handed to the uplink device leaves at that device's size,
	 * which for PPPoE is under the port's; a larger frame then takes the
	 * host's path, which fragments it or answers with ICMP.
	 */
	if (!push) {
		int ifindex = entry->xdev & PPE_XDEV_IFINDEX;
		struct net_device *ul;

		/* An upload shaper on an ifb hands the frame on to the uplink,
		 * whose size is the one that counts.
		 */
		if ((entry->xdev & PPE_XDEV_UL) && READ_ONCE(priv->uplink_dev))
			ifindex = READ_ONCE(priv->uplink_dev);

		rcu_read_lock();
		ul = dev_get_by_index_rcu(&init_net, ifindex);
		if (ul)
			eg_mtu = min_t(u32, eg_mtu, ul->mtu + ETH_HLEN);
		rcu_read_unlock();
	}
	words[PPE_EG_L3_IF_WORDS] = eg_mtu;

	/* An L3 interface is one index with an ingress half and an egress half.
	 * ppe_flow_alloc_ingress() keys the ingress half by VSI, so an egress
	 * interface allocated below PPE_VSI_MAX would answer the size check out
	 * of some VSI's ingress mtu, or overwrite it. Allocating above that
	 * range is what keeps the two halves from sharing an entry.
	 */
	ret = ppe_res_get(priv->eg_l3_if + PPE_VSI_MAX,
			  PPE_EG_L3_IF_ENTRIES - PPE_VSI_MAX, words,
			  PPE_EG_L3_IF_WORDS + 1);
	if (ret < 0)
		return ret;
	eg_l3_if = ret + PPE_VSI_MAX;
	if (priv->eg_l3_if[eg_l3_if].refcount == 1) {
		ppe_tbl_write(priv, PPE_EG_L3_IF_TBL(eg_l3_if), words,
			      PPE_EG_L3_IF_WORDS);
		ppe_eg_l3_if_mtu_set(priv, eg_l3_if, eg_mtu);
	}

	memset(nh, 0, PPE_NEXTHOP_WORDS * sizeof(*nh));
	/* Type selects how the port field is read: as a physical port (1) or,
	 * left at 0, as a VSI whose FDB resolves the port. The egress port is
	 * known exactly here, so name it directly; left at 0 the port number
	 * would be read as a VSI.
	 */
	ppe_entry_set(nh, PPE_NEXTHOP_TYPE_OFF, PPE_NEXTHOP_TYPE_LEN,
		      PPE_NEXTHOP_TYPE_PORT);
	ppe_entry_set(nh, PPE_NEXTHOP_PORT_OFF, PPE_NEXTHOP_PORT_LEN, nh_port);
	ppe_entry_set(nh, PPE_NEXTHOP_POST_L3_IF_OFF,
		      PPE_NEXTHOP_POST_L3_IF_LEN, eg_l3_if);
	if (data->egress_svid_valid && push) {
		ppe_entry_set(nh, PPE_NEXTHOP_STAG_FMT_OFF,
			      PPE_NEXTHOP_STAG_FMT_LEN, 1);
		ppe_entry_set(nh, PPE_NEXTHOP_SVID_OFF,
			      PPE_NEXTHOP_SVID_LEN, data->egress_svid);
	}
	if (data->vlan_valid && push) {
		ppe_entry_set(nh, PPE_NEXTHOP_CTAG_FMT_OFF,
			      PPE_NEXTHOP_CTAG_FMT_LEN, 1);
		ppe_entry_set(nh, PPE_NEXTHOP_CVID_OFF,
			      PPE_NEXTHOP_CVID_LEN, data->vlan_id);
	}
	if (snat)
		ppe_entry_set(nh, PPE_NEXTHOP_PUB_IP_IDX_OFF,
			      PPE_NEXTHOP_PUB_IP_IDX_LEN, entry->pub_ip);
	ppe_entry_set(nh, PPE_NEXTHOP_MAC_OFF, PPE_NEXTHOP_MAC_LEN,
		      ether_addr_to_u64(data->eth.h_dest));
	if (dnat)
		ppe_entry_set(nh, PPE_NEXTHOP_DNAT_IP_OFF,
			      PPE_NEXTHOP_DNAT_IP_LEN, ntohl(data->v4_dst_new));

	return eg_l3_if;
}

static int ppe_flow_alloc_egress(struct qca_ppe_priv *priv,
				 struct ppe_flow_data *data, bool snat,
				 bool dnat, int iport,
				 struct ppe_flow_entry *entry)
{
	u32 words[PPE_NEXTHOP_WORDS] = {};
	struct dsa_port *odp;
	bool casc = false;
	int port, nh_port, ret;
	bool push;
	u16 qca_vid;

	qca_vid = data->vlan_valid ? data->vlan_id : 0;
	port = ppe_flow_qca_port_by_ifindex(priv, data->odev->ifindex,
					   qca_vid, &qca_vid);
	if (port >= 0) {
		data->vlan_id = qca_vid;
		data->vlan_valid = true;
		entry->ocasc = data->odev->ifindex;
		casc = true;
	} else {
		port = ppe_flow_port_by_ifindex(priv, data->odev->ifindex,
						&data->egress_svid, NULL, true);
		if (port < 0 && ppe_cpu_xmit_ok(data)) {
			port = QCA_PPE_CPU_PORT;
			entry->xdev = data->odev->ifindex;
		}
		if (port < 0)
			return port;
	}
	data->egress_svid_valid = !!data->egress_svid;

	/* A frame sent back out the port it arrived on is discarded by source
	 * port filtering, so offloading it would black-hole what the CPU would
	 * otherwise have forwarded.
	 */
	if (data->bridge && port == iport)
		return -EBUSY;
	entry->oport = port;
	/* An untagged egress is credited to the port's PVID VLAN, the one an
	 * untagged frame on that port belongs to.
	 */
	entry->ovid = data->vlan_valid ? data->vlan_id :
		      ppe_port_pvid(priv, port);
	/* Native L2 mode preserves Ethernet addresses and selects a port in
	 * the flow itself. It owns no routing interface or next-hop entry.
	 */
	if (data->bridge)
		return 0;

	nh_port = ppe_flow_shape(priv, data, entry, port);
	push = !(entry->xdev & (PPE_XDEV_UL | PPE_XDEV_DL));
	if ((entry->xdev & PPE_XDEV_UL) && ppe_flow_ul_push(priv)) {
		push = true;
		entry->xdev |= PPE_XDEV_PUSH;
	}
	entry->oport = nh_port;
	entry->ovid = data->vlan_valid && !(entry->xdev & PPE_XDEV_UL) ?
		      data->vlan_id : ppe_port_pvid(priv, nh_port);

	if (snat) {
		u32 pub = ntohl(data->v4_src_new);

		ret = ppe_res_get(priv->pub_ip, PPE_PUB_IP_ENTRIES, &pub, 1);
		if (ret < 0)
			goto err_eg_l3_if;
		entry->pub_ip = ret;
		if (priv->pub_ip[ret].refcount == 1)
			regmap_write(priv->regmap, PPE_PUB_IP_TBL(ret), pub);
	}

	ret = ppe_flow_egress_path(priv, data, entry, port, nh_port, push,
				   snat, dnat, words);
	if (ret < 0)
		goto err_pub_ip;
	entry->eg_l3_if = ret;

	ret = ppe_res_get(priv->nexthop, priv->data->num_nexthop_entries, words,
			  PPE_NEXTHOP_WORDS);
	if (ret < 0)
		goto err_pub_ip;
	entry->nexthop = ret;
	if (priv->nexthop[ret].refcount == 1)
		ppe_tbl_write(priv, PPE_IN_NEXTHOP_TBL(ret), words,
			      PPE_NEXTHOP_WORDS);

	/* The reverse of a flow that egresses PPPoE, or a VLAN of a port no
	 * bridge classifies, arrives the same way on this port; set the port up
	 * to route it so that direction offloads too.
	 */
	if (data->pppoe_valid ||
	    (data->vlan_valid && !priv->port_br_dev[port])) {
		odp = dsa_to_port(&priv->ds, port);
		ret = ppe_uplink_get(priv, port,
				     data->vlan_valid ? data->vlan_id : 0,
				     casc ? data->eth.h_source : odp->user->dev_addr,
				     casc ? data->odev->mtu : odp->user->mtu);
		if (ret < 0)
			goto err_nexthop;
		entry->uplink = ret;
	}

	if (data->pppoe_valid) {
		ret = ppe_pppoe_session_get(priv, entry->uplink,
					    data->pppoe_sid, data->eth.h_dest);
		if (ret < 0)
			goto err_uplink;
		entry->session = ret;
	}

	if (data->egress_svid_valid) {
		ret = ppe_dsa_core_port_get(priv, port, false);
		if (ret)
			goto err_session;
		entry->dsa_egress_port = port;
	}

	if (entry->uplink >= 0 && priv->uplink[entry->uplink].ref == 1)
		ppe_flow_purge_ingress(priv, &priv->uplink[entry->uplink]);

	return 0;

err_session:
	if (entry->dsa_egress_port >= 0)
		ppe_dsa_core_port_put(priv, entry->dsa_egress_port, false);
	if (entry->session >= 0)
		ppe_pppoe_session_put(priv, entry->session);
err_uplink:
	if (entry->uplink >= 0)
		ppe_uplink_put(priv, entry->uplink);
err_nexthop:
	if (ppe_res_put(priv->nexthop, entry->nexthop))
		ppe_tbl_clear(priv, PPE_IN_NEXTHOP_TBL(entry->nexthop),
			      PPE_NEXTHOP_WORDS);
err_pub_ip:
	if (ppe_res_put(priv->pub_ip, entry->pub_ip))
		regmap_write(priv->regmap, PPE_PUB_IP_TBL(entry->pub_ip), 0);
err_eg_l3_if:
	if (ppe_res_put(priv->eg_l3_if, entry->eg_l3_if)) {
		ppe_tbl_clear(priv, PPE_EG_L3_IF_TBL(entry->eg_l3_if),
			      PPE_EG_L3_IF_WORDS);
		ppe_tbl_clear(priv, PPE_IN_L3_IF_TBL(entry->eg_l3_if),
			      PPE_L3_IF_WORDS);
	}

	return ret;
}

static int ppe_flow_counter_delta(struct qca_ppe_priv *priv,
				  struct ppe_flow_entry *entry, u32 *pkts,
				  u64 *bytes)
{
	u64 packets, total;
	int ret;

	ret = ppe_flow_counter_read(priv, entry->index, &packets, &total);
	if (ret)
		return ret;
	*pkts = (u32)(packets - entry->packets);
	*bytes = (total - entry->bytes) & PPE_FLOW_CNT_BYTES;
	entry->packets = packets;
	entry->bytes = total;
	return 0;
}

static void ppe_flow_dev_add(struct net_device *dev, bool rx, u32 pkts,
			     u64 bytes)
{
	local_bh_disable();
	if (is_vlan_dev(dev)) {
		struct vlan_pcpu_stats *s;

		s = this_cpu_ptr(vlan_dev_priv(dev)->vlan_pcpu_stats);
		u64_stats_update_begin(&s->syncp);
		u64_stats_add(rx ? &s->rx_packets : &s->tx_packets, pkts);
		u64_stats_add(rx ? &s->rx_bytes : &s->tx_bytes, bytes);
		u64_stats_update_end(&s->syncp);
	} else if (netif_is_bridge_master(dev)) {
		struct pcpu_sw_netstats *s = this_cpu_ptr(dev->tstats);

		u64_stats_update_begin(&s->syncp);
		u64_stats_add(rx ? &s->rx_packets : &s->tx_packets, pkts);
		u64_stats_add(rx ? &s->rx_bytes : &s->tx_bytes, bytes);
		u64_stats_update_end(&s->syncp);
	}
	local_bh_enable();
}

static void ppe_flow_account_side(struct qca_ppe_priv *priv, int port,
				  u16 vid, int casc, bool rx, u32 pkts,
				  u64 bytes)
{
	struct net_device *dev = priv->port_br_dev[port];

	/* A stacked switch port counts its own frames; account the flow on its
	 * VLAN upper or bridge instead of on the PPE conduit. */
	if (casc) {
		struct net_device *upper;

		dev = dsa_to_port(&priv->ds, port)->user;
		if (!dev)
			return;
		dev = dev_get_by_index_rcu(dev_net(dev), casc);
		if (!dev)
			return;
		upper = ppe_flow_vlan_upper(dev, vid);
		if (upper) {
			ppe_flow_dev_add(upper, rx, pkts, bytes);
			return;
		}
		dev = netdev_master_upper_dev_get_rcu(dev);
		if (dev && netif_is_bridge_master(dev))
			ppe_flow_dev_add(dev, rx, pkts, bytes);
		return;
	}

	if (dev)
		ppe_flow_dev_add(dev, rx, pkts, bytes);
	else
		dev = dsa_to_port(&priv->ds, port)->user;

	if (dev && vid) {
		dev = __vlan_find_dev_deep_rcu(dev, htons(ETH_P_8021Q), vid);
		if (dev)
			ppe_flow_dev_add(dev, rx, pkts, bytes);
	}
}

/* What the hardware forwarded never crossed the software interfaces on the
 * way: the bridge a port is in and the VLAN interface of the VLAN the frame
 * carries, received on the ingress side and sent on the egress side. Their
 * counters get the flow's deltas so they read as without offload. The port
 * itself is left alone; its MIB already counts the frames.
 *
 * A delta can be read only once, because the read moves the baseline, so the
 * flowtable is answered from what this banks rather than from the counter.
 *
 * The bridge pointer is written under the flow lock by the bridge join and
 * leave ops, and a VLAN interface is found under RCU rather than held.
 */
static void ppe_flow_account(struct qca_ppe_priv *priv,
			     struct ppe_flow_entry *entry, u32 pkts, u64 bytes)
{
	lockdep_assert_held(&priv->flow_lock);

	/* The firmware forwards a steered flow and reports it itself. */
	if (!pkts || entry->steer)
		return;

	entry->unread_packets += pkts;
	entry->unread_bytes += bytes;

	rcu_read_lock();
	ppe_flow_account_side(priv, entry->iport, entry->ivid, entry->icasc,
			      true, pkts, bytes);
	ppe_flow_account_side(priv, entry->oport, entry->ovid, entry->ocasc,
			      false, pkts, bytes);
	rcu_read_unlock();
}

/* Retire only after the issued mutation completed and the owned identity
 * disappeared. Unknown reads never authorize deletion of a sibling index.
 */
static int ppe_flow_entry_destroy(struct qca_ppe_priv *priv,
				  struct ppe_flow_entry *entry)
{
	u64 bytes;
	u32 pkts;
	int ret, age;

	lockdep_assert_held(&priv->flow_lock);
	lockdep_assert_held(&priv->vlan_lock);
	if (priv->flow_stopping) {
		if (entry->xdev_bound)
			ppe_flow_slot_clear(priv, entry->index);
		goto free;
	}
	if (entry->state == PPE_FLOW_PENDING_ADD) {
		ret = ppe_flow_op_finish(priv, &entry->index, &entry->host_index);
		if (ret == -ENOENT)
			goto free;
		if (ret)
			return ret;
		ppe_host_ref_get(priv, entry->host_index);
		if (entry->previous_host < priv->data->num_host_entries) {
			ppe_host_ref_put(priv, entry->previous_host);
			entry->previous_host = U32_MAX;
		}
		entry->words[0] = (entry->words[0] & ~PPE_FLOW_E_HOST_IDX_MASK) |
				 (entry->host_index << PPE_FLOW_E_HOST_IDX_OFF);
		entry->state = PPE_FLOW_RETIRING;
	} else if (entry->state == PPE_FLOW_PENDING_DELETE) {
		ret = ppe_flow_op_finish(priv, NULL, NULL);
		if (priv->flow_op_busy)
			return ret;
		entry->state = PPE_FLOW_RETIRING;
		if (!ret)
			goto free;
	}
	age = ppe_flow_entry_age(priv, entry);
	if (age == -ENOENT)
		goto free;
	if (age < 0)
		return age;
	if (entry->xdev_bound) {
		ppe_flow_slot_clear(priv, entry->index);
		entry->xdev_bound = false;
	}
	if (entry->state == PPE_FLOW_LIVE &&
	    !ppe_flow_counter_delta(priv, entry, &pkts, &bytes))
		ppe_flow_account(priv, entry, pkts, bytes);
	if (priv->flow_op_busy)
		return -EBUSY;
	ret = ppe_flow_entry_delete(priv, entry->index);
	if (priv->flow_op_busy) {
		entry->state = PPE_FLOW_PENDING_DELETE;
		return ret;
	}
	if (ret) {
		age = ppe_flow_entry_age(priv, entry);
		if (age != -ENOENT)
			return age < 0 ? age : ret;
	}
free:
	if (entry->ct)
		nf_conntrack_put(&((struct nf_conn *)(entry->ct & ~1UL))->ct_general);
	if (entry->previous_host < priv->data->num_host_entries)
		ppe_host_ref_put(priv, entry->previous_host);
	if (entry->host_index < priv->data->num_host_entries)
		ppe_host_ref_put(priv, entry->host_index);
	ppe_flow_free_egress(priv, entry);
	ppe_flow_free_ingress(priv, entry);
	return 0;
}

/* A firmware restart changes who receives CPU-port frames. Retire both
 * firmware companions and flows that need the host EDMA transmit callback.
 */
void ppe_flow_fw_purge(struct qca_ppe_priv *priv)
{
	struct ppe_flow_entry *entry, *tmp;

	guard(mutex)(&priv->flow_lock);
	guard(mutex)(&priv->vlan_lock);
	list_for_each_entry_safe(entry, tmp, &priv->flow_list, list)
		if (entry->steer || entry->xdev)
			ppe_flow_drop(priv, entry);
}

/* Whether the NSS firmware forwards to @dev, which is then not a switch port. */
static bool ppe_flow_fw_carries(struct qca_ppe_priv *priv,
				struct net_device *dev)
{
	bool (*carries)(int ifindex);

	if (ppe_flow_port_by_ifindex(priv, dev->ifindex, NULL, NULL, false) >= 0)
		return false;

	guard(rcu)();
	carries = rcu_dereference(priv->fw_carries);

	return carries && carries(dev->ifindex);
}

/* Encode the flow entry and its host half. The key is always the tuple as the
 * packet arrives: the destination address in the flow entry, the source address
 * in the host entry.
 */
static void ppe_flow_encode(struct ppe_flow_data *data, bool v6, bool snat,
				    bool dnat, bool steer,
				    u32 destination,
				    u32 src_if, u32 *fw, u32 *hw)
{
	u32 fwd;

	ppe_entry_set(fw, PPE_FLOW_E_VALID_OFF, PPE_FLOW_E_VALID_LEN, 1);

	/* The key is a bare 5-tuple otherwise, so the same addresses on two
	 * VLANs - or on two ports that route separately - would alias. The
	 * ingress L3 interface is the one ingress identifier the entry can
	 * hold; a packet that resolves another one misses to the CPU.
	 */
	ppe_entry_set(fw, PPE_FLOW_E_SRC_IF_VALID_OFF,
		      PPE_FLOW_E_SRC_IF_VALID_LEN, 1);
	ppe_entry_set(fw, PPE_FLOW_E_SRC_IF_OFF, PPE_FLOW_E_SRC_IF_LEN, src_if);
	ppe_entry_set(fw, PPE_FLOW_E_TYPE_OFF, PPE_FLOW_E_TYPE_LEN, v6);
	ppe_entry_set(fw, PPE_FLOW_E_PROTO_OFF, PPE_FLOW_E_PROTO_LEN,
		      ppe_flow_proto(data->l4proto));
	ppe_entry_set(fw, PPE_FLOW_E_AGE_OFF, PPE_FLOW_E_AGE_LEN,
		      PPE_FLOW_AGE_MAX);
	ppe_entry_set(fw, PPE_FLOW_E_PRI_PROFILE_OFF,
		      PPE_FLOW_E_PRI_PROFILE_LEN, data->priority);

	/* NSS performs translation for a steered flow; the native switch
	 * delivers it unchanged. Wired NAT retains the hardware edit type. */
	fwd = (data->bridge || steer) ? PPE_FLOW_FWD_BRIDGE :
		 snat ? PPE_FLOW_FWD_SNAT : dnat ? PPE_FLOW_FWD_DNAT :
						PPE_FLOW_FWD_ROUTE;
	ppe_entry_set(fw, PPE_FLOW_E_FWD_TYPE_OFF, PPE_FLOW_E_FWD_TYPE_LEN, fwd);
	/* HPPE and CPPE share the native bridge layout: PORT_VP2 occupies
	 * only the low eight bits of the routed next-hop field. It selects
	 * the physical egress directly, without MAC editing or a TTL change.
	 */
	if (steer)
		ppe_entry_set(fw, PPE_FLOW_E_BRIDGE_PORT_OFF,
			      PPE_FLOW_E_BRIDGE_PORT_LEN, QCA_PPE_CPU_PORT);
	else if (data->bridge)
		ppe_entry_set(fw, PPE_FLOW_E_BRIDGE_PORT_OFF,
			      PPE_FLOW_E_BRIDGE_PORT_LEN, destination);
	else
		ppe_entry_set(fw, PPE_FLOW_E_NEXTHOP_OFF,
			      PPE_FLOW_E_NEXTHOP_LEN, destination);
	if (snat)
		ppe_entry_set(fw, PPE_FLOW_E_NEW_PORT_OFF,
			      PPE_FLOW_E_NEW_PORT_LEN, ntohs(data->sport_new));
	else if (dnat)
		ppe_entry_set(fw, PPE_FLOW_E_NEW_PORT_OFF,
			      PPE_FLOW_E_NEW_PORT_LEN, ntohs(data->dport_new));

	if (ppe_flow_proto(data->l4proto) == PPE_FLOW_PROTO_OTHER) {
		ppe_entry_set(fw, PPE_FLOW_E_IP_PROTO_OFF,
			      PPE_FLOW_E_IP_PROTO_LEN, data->l4proto);
	} else {
		ppe_entry_set(fw, PPE_FLOW_E_SPORT_OFF, PPE_FLOW_E_SPORT_LEN,
			      ntohs(data->sport));
		ppe_entry_set(fw, PPE_FLOW_E_DPORT_OFF, PPE_FLOW_E_DPORT_LEN,
			      ntohs(data->dport));
	}

	ppe_entry_set(hw, PPE_HOST_E_VALID_OFF, PPE_HOST_E_VALID_LEN, 1);

	if (v6) {
		ppe_entry_set_addr6(fw, PPE_FLOW_E_IPV6_OFF, &data->v6_dst);
		ppe_entry_set(hw, PPE_HOST_E_KEY_TYPE_OFF,
			      PPE_HOST_E_KEY_TYPE_LEN, PPE_HOST_KEY_IPV6);
		ppe_entry_set_addr6(hw, PPE_HOST_E_IPV6_OFF, &data->v6_src);
	} else {
		ppe_entry_set(fw, PPE_FLOW_E_IPV4_OFF, PPE_FLOW_E_IPV4_LEN,
			      ntohl(data->v4_dst));
		ppe_entry_set(hw, PPE_HOST_E_KEY_TYPE_OFF,
			      PPE_HOST_E_KEY_TYPE_LEN, PPE_HOST_KEY_IPV4);
		ppe_entry_set(hw, PPE_HOST_E_IPV4_OFF, PPE_HOST_E_IPV4_LEN,
			      ntohl(data->v4_src));
	}
}

static int ppe_flow_reject(struct qca_ppe_priv *priv, struct flow_rule *rule,
			   enum ppe_flow_reject why)
{
	struct ppe_flow_reject_info *info = &priv->flow_reject_info[why];
	const struct flow_action_entry *act;
	int i;

	lockdep_assert_held(&priv->flow_lock);
	priv->flow_reject[why]++;
	memset(info, 0, sizeof(*info));
	info->cookie = priv->flow_request_cookie;
	info->generation = priv->flow_request_generation;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META)) {
		struct flow_match_meta match;

		flow_rule_match_meta(rule, &match);
		info->ingress_ifindex = match.key->ingress_ifindex;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		info->n_proto = ntohs(match.key->n_proto);
		info->ip_proto = match.key->ip_proto;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		info->vlan_tpid = ntohs(match.key->vlan_tpid);
		info->vlan_id = match.key->vlan_id;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_cvlan(rule, &match);
		info->cvlan_tpid = ntohs(match.key->vlan_tpid);
		info->cvlan_id = match.key->vlan_id;
	}
	flow_action_for_each(i, act, &rule->action) {
		if (act->id < 64)
			info->actions |= BIT_ULL(act->id);
		if (act->id == FLOW_ACTION_REDIRECT && act->dev) {
			info->egress_ifindex = act->dev->ifindex;
		}
		if (act->id == FLOW_ACTION_VLAN_PUSH) {
			info->push_tpid = ntohs(act->vlan.proto);
			info->push_vid = act->vlan.vid;
		}
	}

	return -EOPNOTSUPP;
}

static void ppe_flow_reject_hw(struct qca_ppe_priv *priv,
			       struct flow_rule *rule, int error)
{
	struct ppe_flow_failure *event;
	u64 sequence = priv->flow_failure_sequence++;
	int bucket;

	ppe_flow_reject(priv, rule, PPE_REJECT_HW_OP);
	switch (error) {
	case -ENOENT: bucket = 0; break;
	case -ENOSPC: bucket = 1; break;
	case -ENOMEM: bucket = 2; break;
	case -EIO: bucket = 3; break;
	case -ETIMEDOUT: bucket = 4; break;
	default: bucket = 5; break;
	}
	priv->flow_hw_errors[bucket]++;
	event = &priv->flow_failures[sequence % PPE_FLOW_FAILURE_HISTORY];
	event->rule = priv->flow_reject_info[PPE_REJECT_HW_OP];
	event->sequence = sequence;
	event->boottime_ns = ktime_get_boottime_ns();
	event->error = error;
	event->command_id = priv->flow_op_cmd_id;
	event->result_reg = priv->flow_op_rslt_reg;
	event->result_valid = priv->flow_op_result_valid;
	event->result = event->result_valid ? priv->flow_op_result : 0;
	event->pending = priv->flow_op_busy;
	event->expected_index = priv->flow_op_expected_index;
	event->index_mode = priv->flow_op_index_mode;
	event->result_rejected = priv->flow_op_result_rejected;
		event->op_type = priv->flow_op_type;
		event->request_issued = priv->flow_op_request_issued;
		event->entry_words = priv->flow_op_nentry;
		event->host_words = priv->flow_op_nhost;
		memcpy(event->entry, priv->flow_op_entry, sizeof(event->entry));
		memcpy(event->host, priv->flow_op_host, sizeof(event->host));
}

/* A routed rule writes a local source MAC; a bridge preserves a peer MAC. */
static bool ppe_flow_local_smac(const u8 *addr)
{
	guard(rcu)();
	return dev_getbyhwaddr_rcu(&init_net, ARPHRD_ETHER, addr);
}

/* Direct bridge forwarding has no VLAN editor or tunnel context. Only a
 * common VLAN-unaware bridge with forwarding, unlocked ports is eligible.
 * Hold RTNL through publication so topology changes cannot miss a new flow.
 */
static bool ppe_flow_bridge_endpoint(struct qca_ppe_priv *priv,
				     struct net_device *dev)
{
	struct dsa_port *dp;

	#if IS_ENABLED(CONFIG_CFG80211_HEADERS)
	if (dev->ieee80211_ptr)
		return dev->ieee80211_ptr->iftype == NL80211_IFTYPE_AP;
#endif
	dp = dsa_port_from_netdev(dev);
	return !IS_ERR(dp) && dp->ds == &priv->ds;
}

static int ppe_flow_bridge_path(struct qca_ppe_priv *priv,
				struct net_device *odev, int ingress_ifindex)
{
	struct net_device *idev, *master;
	int ifindex = 0;

	ASSERT_RTNL();
	guard(rcu)();
	idev = dev_get_by_index_rcu(&init_net, ingress_ifindex);
	if (!idev || idev == odev ||
	    !ppe_flow_bridge_endpoint(priv, idev) ||
	    !ppe_flow_bridge_endpoint(priv, odev) ||
	    (ppe_dev_is_wifi(idev) && ppe_dev_is_wifi(odev)))
		return 0;
	master = netdev_master_upper_dev_get_rcu(idev);
	if (master && netif_is_bridge_master(master) &&
	    netdev_master_upper_dev_get_rcu(odev) == master &&
	    !br_vlan_enabled(master) &&
	    br_port_get_stp_state(idev) == BR_STATE_FORWARDING &&
	    br_port_get_stp_state(odev) == BR_STATE_FORWARDING &&
	    !br_port_flag_is_set(idev, BR_PORT_LOCKED) &&
	    !br_port_flag_is_set(odev, BR_PORT_LOCKED) &&
	    !(br_port_flag_is_set(idev, BR_ISOLATED) &&
	      br_port_flag_is_set(odev, BR_ISOLATED)))
		ifindex = master->ifindex;
	return ifindex;
}

static void ppe_bridge_rtnl_unlock(bool locked)
{
	if (locked)
		rtnl_unlock();
}

DEFINE_FREE(ppe_bridge_rtnl, bool, ppe_bridge_rtnl_unlock(_T));

/* The hardware key has no AP identity or Ethernet addresses. Multiple APs
 * share CPU ingress, so distinct cookies with the same bridge tuple must
 * not acquire the same hardware slot and later tear each other down. Keep
 * the first owner accelerated; only the conflicting request is declined.
 */
static bool ppe_flow_bridge_key_conflict(struct qca_ppe_priv *priv,
					 const struct ppe_flow_entry *new)
{
	struct ppe_flow_entry *entry;
	bool v6 = new->words[0] & PPE_FLOW_E_TYPE_IPV6;
	u32 address = v6 ? PPE_FLOW_E_IPV6_OFF : PPE_FLOW_E_IPV4_OFF;
	int i, words = v6 ? 4 : 1;

	lockdep_assert_held(&priv->flow_lock);
	list_for_each_entry(entry, &priv->flow_list, list) {
		if ((!entry->bridge && !new->bridge) ||
		    entry->nhwords != new->nhwords ||
		    memcmp(entry->hwords, new->hwords, new->nhwords * sizeof(u32)) ||
		    ppe_entry_get(entry->words, PPE_FLOW_E_PROTO_OFF,
				  PPE_FLOW_E_PROTO_LEN) !=
		    ppe_entry_get(new->words, PPE_FLOW_E_PROTO_OFF, PPE_FLOW_E_PROTO_LEN) ||
		    ppe_entry_get(entry->words, PPE_FLOW_E_SPORT_OFF,
				  PPE_FLOW_E_SPORT_LEN) !=
		    ppe_entry_get(new->words, PPE_FLOW_E_SPORT_OFF, PPE_FLOW_E_SPORT_LEN) ||
		    ppe_entry_get(entry->words, PPE_FLOW_E_DPORT_OFF,
				  PPE_FLOW_E_DPORT_LEN) !=
		    ppe_entry_get(new->words, PPE_FLOW_E_DPORT_OFF, PPE_FLOW_E_DPORT_LEN))
			continue;
		for (i = 0; i < words; i++)
			if (ppe_entry_get(entry->words, address + i * 32, 32) !=
			    ppe_entry_get(new->words, address + i * 32, 32))
				break;
		if (i == words)
			return true;
	}
	return false;
}

static int ppe_flow_offload_replace(struct ppe_flow_block *fb,
				    struct flow_cls_offload *f)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct qca_ppe_priv *priv = fb->priv;
	u32 fw[PPE_FLOW_ENTRY_WORDS_V6] = {};
	u32 hw[PPE_HOST_ENTRY_WORDS_V6] = {};
	struct ppe_flow_entry *entry;
	struct ppe_flow_data data = {};
	struct flow_action_entry *act;
	bool snat, dnat, v6, steer, bridge_edit = false;
	bool bridge_rtnl __free(ppe_bridge_rtnl) = false;
	int i, ret, nfw, nhw, iport, ingress_ifindex, bridge_ifindex = 0;
	int redirects = 0;
	int qca_icasc = 0;
	u16 flow_vid = 0;

	/* The netdev-down path drains flow work while holding RTNL. Try it
	 * before flow_lock, rather than waiting in the opposite lock order.
	 */
	flow_action_for_each(i, act, &rule->action) {
		if (act->id == FLOW_ACTION_MANGLE &&
		    act->mangle.htype == FLOW_ACT_MANGLE_HDR_TYPE_ETH) {
			ret = ppe_flow_mangle_eth(act, &data.eth);
			if (ret)
				return ret;
		}
	}
	data.bridge = is_valid_ether_addr(data.eth.h_source) &&
		      !ppe_flow_local_smac(data.eth.h_source);
	if (data.bridge) {
		if (!rtnl_trylock())
			return -EAGAIN;
		bridge_rtnl = true;
	}

	guard(mutex)(&priv->flow_lock);
	guard(mutex)(&priv->vlan_lock);
	priv->flow_request_cookie = f->cookie;
	priv->flow_request_generation++;

	/* A replace for a cookie already held is the kernel refreshing a flow
	 * whose packets it saw in the software path. If the hardware entry is
	 * intact the refresh is a no-op; if the hardware has lost it - however
	 * that happened - this is the moment to reinstall, or the flow would
	 * stay in software for the rest of its life.
	 */
	entry = rhashtable_lookup_fast(&priv->flow_table, &f->cookie,
				       ppe_flow_ht_params);
	if (entry) {
		ret = ppe_flow_entry_age(priv, entry);
		if (ret >= 0)
			return 0;
		if (ret != -ENOENT)
			return ret;

		priv->flow_reinstalled++;
		ppe_flow_unpublish(priv, entry);
	}

	if (priv->flow_stopping || priv->flow_op_busy ||
	    (!list_empty(&priv->flow_retire_list) || priv->host_retire_pending))
		return -EBUSY;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC))
		return ppe_flow_reject(priv, rule, PPE_REJECT_KEY);

	{
		struct flow_match_meta match;
		struct flow_match_vlan vlan_match;

		flow_rule_match_meta(rule, &match);
		ingress_ifindex = match.key->ingress_ifindex;
		if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
			flow_rule_match_vlan(rule, &vlan_match);
			if (vlan_match.key->vlan_tpid != htons(ETH_P_8021Q))
				return ppe_flow_reject(priv, rule,
						PPE_REJECT_INGRESS_VLAN);
			flow_vid = vlan_match.key->vlan_id;
		}

		iport = ppe_flow_qca_port_by_ifindex(priv, ingress_ifindex,
						 flow_vid, &data.ivid);
		if (iport >= 0) {
			qca_icasc = data.ivid ? ingress_ifindex : 0;
		} else {
			iport = ppe_flow_port_by_ifindex(priv, ingress_ifindex,
						 &data.ingress_svid, data.ingress_mac,
						 false);
		}
		if (iport < 0)
			return ppe_flow_reject(priv, rule, PPE_REJECT_INGRESS_PORT);
		data.ingress_svid_valid = !!data.ingress_svid;
		data.ingress_mac_valid = data.ingress_svid_valid &&
					 is_valid_ether_addr(data.ingress_mac);
	}

	/* An ingress tag is matched by the VSI it is classified into - the
	 * uplink's tag on a PPPoE port, a bridge VLAN the port is a member
	 * of - never by the tag itself, which the hardware key cannot hold.
	 * A tag this driver has no classification for, and a second tag,
	 * stay in software.
	 */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN) ||
	    (data.ingress_svid_valid &&
	     flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)))
		return ppe_flow_reject(priv, rule, PPE_REJECT_INGRESS_VLAN);
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		if (match.key->vlan_tpid != htons(ETH_P_8021Q))
			return ppe_flow_reject(priv, rule, PPE_REJECT_INGRESS_VLAN);
		data.ivid = match.key->vlan_id;
	} else if (data.ingress_svid_valid) {
		data.ivid = data.ingress_svid;
	}

	{
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		if (flow_rule_has_control_flags(match.mask->flags,
						f->common.extack))
			return ppe_flow_reject(priv, rule, PPE_REJECT_KEY);
		data.addr_type = match.key->addr_type;
	}

	{
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		data.l4proto = match.key->ip_proto;
	}

	if (ppe_flow_proto(data.l4proto) < 0)
		return ppe_flow_reject(priv, rule, PPE_REJECT_PROTO);

	/* The entry matches any flags; a flag the rule excludes is only
	 * honoured if its exception takes it to the CPU.
	 */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_TCP)) {
		struct flow_match_tcp match;

		flow_rule_match_tcp(rule, &match);
		if (match.key->flags & match.mask->flags ||
		    match.mask->flags & ~htons(PPE_FLOW_TCP_TRAP))
			return ppe_flow_reject(priv, rule, PPE_REJECT_KEY);
	}

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			if (act->mangle.htype == FLOW_ACT_MANGLE_HDR_TYPE_ETH) {
				ret = ppe_flow_mangle_eth(act, &data.eth);
			} else {
				bridge_edit = true;
				ret = 0;
			}
			if (ret)
				return ppe_flow_reject(priv, rule, PPE_REJECT_ACTION);
			break;
		case FLOW_ACTION_REDIRECT:
			data.odev = act->dev;
			redirects++;
			break;
		case FLOW_ACTION_CSUM:
			bridge_edit = true;
			break;
		case FLOW_ACTION_VLAN_PUSH:
			if (data.vlan_valid ||
			    act->vlan.proto != htons(ETH_P_8021Q))
				return ppe_flow_reject(priv, rule, PPE_REJECT_ACTION);
			data.vlan_id = act->vlan.vid;
			data.vlan_valid = true;
			break;
		case FLOW_ACTION_VLAN_POP:
			bridge_edit = true;
			/* Routed egress rebuilds the L2 header from the
			 * nexthop, which sheds the ingress encapsulation on
			 * its own.
			 */
			break;
		case FLOW_ACTION_PRIORITY:
			/* A priority the profiles cannot hold, such as the
			 * 802.1d tags of 256 and up that Wi-Fi stamps take,
			 * leaves the flow to DSCP: refusing it would send the
			 * whole flow to the CPU for the sake of its queue.
			 */
			if (act->priority <= PPE_QOS_MAX_PRI)
				data.priority = act->priority;
			break;
		case FLOW_ACTION_PPPOE_PUSH:
			if (data.pppoe_valid)
				return ppe_flow_reject(priv, rule, PPE_REJECT_ACTION);
			data.pppoe_sid = act->pppoe.sid;
			data.pppoe_valid = true;
			break;
		default:
			return ppe_flow_reject(priv, rule, PPE_REJECT_ACTION);
		}
	}

	if (!data.odev || !is_valid_ether_addr(data.eth.h_source) ||
	    !is_valid_ether_addr(data.eth.h_dest))
		return ppe_flow_reject(priv, rule, PPE_REJECT_L2);
	steer = ppe_flow_fw_carries(priv, data.odev);
	if (steer && data.bridge)
		return ppe_flow_reject(priv, rule, PPE_REJECT_L2);
	if (data.bridge) {
		if (!priv->data->bridge_flow || bridge_edit ||
		    redirects != 1 || data.ivid || data.vlan_valid || data.pppoe_valid ||
		    data.ingress_svid_valid || qca_icasc ||
		    flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN) ||
		    (data.l4proto != IPPROTO_TCP && data.l4proto != IPPROTO_UDP))
			return ppe_flow_reject(priv, rule, PPE_REJECT_L2);
		bridge_ifindex = ppe_flow_bridge_path(priv, data.odev, ingress_ifindex);
		if (!bridge_ifindex)
			return ppe_flow_reject(priv, rule, PPE_REJECT_L2);
	}

	switch (data.addr_type) {
	case FLOW_DISSECTOR_KEY_IPV4_ADDRS: {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);
		data.v4_src = match.key->src;
		data.v4_dst = match.key->dst;
		v6 = false;
		break;
	}
	case FLOW_DISSECTOR_KEY_IPV6_ADDRS: {
		struct flow_match_ipv6_addrs match;

		flow_rule_match_ipv6_addrs(rule, &match);
		data.v6_src = match.key->src;
		data.v6_dst = match.key->dst;
		v6 = true;
		break;
	}
	default:
		return ppe_flow_reject(priv, rule, PPE_REJECT_KEY);
	}

	if (ppe_flow_proto(data.l4proto) != PPE_FLOW_PROTO_OTHER) {
		struct flow_match_ports match;

		if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS))
			return ppe_flow_reject(priv, rule, PPE_REJECT_KEY);

		flow_rule_match_ports(rule, &match);
		data.sport = match.key->src;
		data.dport = match.key->dst;
	}

	data.v4_src_new = data.v4_src;
	data.v4_dst_new = data.v4_dst;
	data.sport_new = data.sport;
	data.dport_new = data.dport;

	/* The firmware translates a steered connection; the switch only
	 * hands it over.
	 */
	flow_action_for_each(i, act, &rule->action) {
		if (steer || act->id != FLOW_ACTION_MANGLE)
			continue;

		switch (act->mangle.htype) {
		case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
			ret = ppe_flow_mangle_ipv4(act, &data);
			break;
		case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
		case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
			ret = ppe_flow_mangle_ports(act, &data);
			break;
		case FLOW_ACT_MANGLE_HDR_TYPE_IP6:
			/* The IPv6 flow entry has no L4 port rewrite fields and
			 * the nexthop's NAT address is 32 bits wide, so IPv6
			 * address translation cannot be expressed at all.
			 */
			return ppe_flow_reject(priv, rule, PPE_REJECT_NAT_IPV6);
		default:
			ret = 0;
			break;
		}
		if (ret)
			return ppe_flow_reject(priv, rule, PPE_REJECT_ACTION);
	}

	snat = data.v4_src_new != data.v4_src || data.sport_new != data.sport;
	dnat = data.v4_dst_new != data.v4_dst || data.dport_new != data.dport;

	/* SNAT and DNAT share the same bits of the flow entry, so a rule that
	 * needs both - a hairpinned connection - has no hardware expression.
	 */
	if (snat && dnat)
		return ppe_flow_reject(priv, rule, PPE_REJECT_NAT_BOTH);

	if (v6 && (snat || dnat))
		return ppe_flow_reject(priv, rule, PPE_REJECT_NAT_IPV6);

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->index = U32_MAX;
	entry->host_index = U32_MAX;
	entry->previous_host = U32_MAX;
	entry->cookie = f->cookie;
	entry->block = fb->block;
	entry->nexthop = -1;
	entry->eg_l3_if = -1;
	entry->pub_ip = -1;
	entry->my_mac = -1;
	entry->l3_if = -1;
	entry->uplink = -1;
	entry->in_uplink = -1;
	entry->session = -1;
	entry->dsa_service = -1;
	entry->dsa_egress_port = -1;
	entry->iport = iport;
	entry->icasc = qca_icasc;
	entry->bridge = data.bridge;
	entry->bridge_ifindex = bridge_ifindex;
	entry->bridge_iif = data.bridge ? ingress_ifindex : 0;
	entry->bridge_oif = data.bridge ? data.odev->ifindex : 0;

	if (data.bridge) {
		/* Native L2 mode checks the physical ingress port, not a VSI. */
		entry->src_if = iport;
		ret = 0;
	} else {
		ret = ppe_flow_alloc_ingress(priv, iport, data.ivid,
					     data.ingress_svid_valid,
					     data.ingress_mac_valid ? data.ingress_mac : NULL,
					     entry);
		if (!ret) {
			entry->ingress_vsi = entry->src_if;
			entry->vsi_ref = true;
		}
	}
	if (ret) {
		ppe_flow_reject(priv, rule, ret == -EOPNOTSUPP ?
				PPE_REJECT_INGRESS_VLAN : PPE_REJECT_RESOURCE);
		goto err_free;
	}

	if (entry->vsi_ref)
		priv->vsi_flow_refs[entry->ingress_vsi]++;

	entry->steer = steer;
	entry->fw_ifindex = steer ? data.odev->ifindex : 0;
	if (steer)
		entry->oport = QCA_PPE_CPU_PORT;
	else
		ret = ppe_flow_alloc_egress(priv, &data, snat, dnat, iport, entry);
	if (ret) {
		ppe_flow_reject(priv, rule, ret == -ENOSPC ? PPE_REJECT_RESOURCE :
				ret == -EBUSY ? PPE_REJECT_HAIRPIN :
				PPE_REJECT_EGRESS_PORT);
		goto err_ingress;
	}

	/* The NSS firmware owns CPU-port receive while it carries flows. It
	 * does not return the EDMA flow index that ppe_flow_xmit() needs to
	 * select an external device or a software shaper. Leave these flows
	 * in software; only firmware companions can use that CPU-port path.
	 */
	if (entry->xdev && rcu_access_pointer(priv->fw_carries)) {
		ret = ppe_flow_reject(priv, rule, PPE_REJECT_CPU_PATH);
		goto err_egress;
	}

	ppe_flow_encode(&data, v6, snat, dnat, steer,
			data.bridge ? entry->oport : entry->nexthop, entry->src_if,
			fw, hw);
	nfw = v6 ? PPE_FLOW_ENTRY_WORDS_V6 : PPE_FLOW_ENTRY_WORDS_V4;
	nhw = v6 ? PPE_HOST_ENTRY_WORDS_V6 : PPE_HOST_ENTRY_WORDS_V4;

	memcpy(entry->words, fw, nfw * sizeof(*fw));
	entry->nwords = nfw;
	memcpy(entry->hwords, hw, nhw * sizeof(*hw));
	entry->nhwords = nhw;
	entry->profile = data.priority;
	entry->words[0] &= ~PPE_FLOW_E_AGE_MASK;
	if (ppe_flow_bridge_key_conflict(priv, entry)) {
		ret = -EEXIST;
		ppe_flow_reject(priv, rule, PPE_REJECT_KEY);
		goto err_egress;
	}
	ret = ppe_flow_op(priv, PPE_TBL_OP_ADD, fw, nfw, hw, nhw, &entry->index,
			  &entry->host_index);
	if (ret) {
		ppe_flow_reject_hw(priv, rule, ret);
		if (priv->flow_op_busy) {
			/* A returned index is not yet an acquired host lease. */
			entry->host_index = U32_MAX;
			entry->state = PPE_FLOW_PENDING_ADD;
			ppe_flow_retire(priv, entry);
			return ret;
		}
		goto err_egress;
	}
	ppe_host_ref_get(priv, entry->host_index);
	entry->words[0] |= entry->host_index << PPE_FLOW_E_HOST_IDX_OFF;
	ret = ppe_flow_counter_clear(priv, entry->index);
	if (ret)
		goto err_hw;
	entry->last_used = jiffies;

	ret = rhashtable_insert_fast(&priv->flow_table, &entry->node,
				     ppe_flow_ht_params);
	if (ret)
		goto err_hw;

	if (entry->xdev & PPE_XDEV_UL)
		entry->ct = ppe_flow_ct_get(f->cookie);
	if (entry->xdev && priv->ct_dscp)
		entry->dscp = ppe_flow_ct_dscp(f->cookie);
	if (entry->xdev)
		ppe_flow_slot_set(priv, entry, entry->index);
	list_add_tail(&entry->list, &priv->flow_list);
	priv->flow_offloaded++;


	return 0;

err_hw:
	entry->state = PPE_FLOW_RETIRING;
	ppe_flow_retire(priv, entry);
	return ret;
err_egress:
	ppe_flow_free_egress(priv, entry);
err_ingress:
	ppe_flow_free_ingress(priv, entry);
err_free:
	kfree(entry);

	return ret;
}

static int ppe_flow_offload_destroy(struct qca_ppe_priv *priv,
				    struct flow_cls_offload *f)
{
	struct ppe_flow_entry *entry;

	guard(mutex)(&priv->flow_lock);
	guard(mutex)(&priv->vlan_lock);

	entry = rhashtable_lookup_fast(&priv->flow_table, &f->cookie,
				       ppe_flow_ht_params);
	if (!entry) {
		priv->flow_destroy_miss++;
		priv->flow_destroy_miss_cookie = f->cookie;
		return -ENOENT;
	}

	ppe_flow_unpublish(priv, entry);

	return 0;
}

static int ppe_flow_offload_stats(struct qca_ppe_priv *priv,
				  struct flow_cls_offload *f)
{
	struct ppe_flow_entry *entry;
	int age;

	guard(mutex)(&priv->flow_lock);

	entry = rhashtable_lookup_fast(&priv->flow_table, &f->cookie,
				       ppe_flow_ht_params);
	if (!entry)
		return -ENOENT;
	if (entry->steer)
		return 0;

	/* The hit counter does not advance for every forwarding type, but any
	 * hit rewinds the entry's age to its maximum while the hardware
	 * decrements it once per aging period - so an entry still at maximum
	 * age was used within the last period, which is all the flowtable's
	 * idle detection needs. The comparison excludes an entry whose slot
	 * the hardware has aged out and handed to another flow.
	 */
	age = ppe_flow_entry_age(priv, entry);
	if (age >= 0 && (entry->unread_packets || age == PPE_FLOW_AGE_MAX))
		entry->last_used = jiffies;

	flow_stats_update(&f->stats, entry->unread_bytes, entry->unread_packets,
			  0, entry->last_used, FLOW_ACTION_HW_STATS_DELAYED);
	entry->unread_packets = 0;
	entry->unread_bytes = 0;

	return 0;
}

static int ppe_flow_block_cb(enum tc_setup_type type, void *type_data,
			     void *cb_priv)
{
	struct flow_cls_offload *cls = type_data;
	struct ppe_flow_block *fb = cb_priv;
	struct qca_ppe_priv *priv = fb->priv;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		return ppe_flow_offload_replace(fb, cls);
	case FLOW_CLS_DESTROY:
		return ppe_flow_offload_destroy(priv, cls);
	case FLOW_CLS_STATS:
		return ppe_flow_offload_stats(priv, cls);
	default:
		return -EOPNOTSUPP;
	}
}

static LIST_HEAD(ppe_block_cb_list);

static void ppe_flow_block_release(void *cb_priv)
{
	struct ppe_flow_block *fb = cb_priv;
	struct qca_ppe_priv *priv = fb->priv;
	struct ppe_flow_entry *entry, *tmp;

	mutex_lock(&priv->flow_lock);
	mutex_lock(&priv->vlan_lock);
	list_for_each_entry_safe(entry, tmp, &priv->flow_list, list) {
		if (entry->block != fb->block)
			continue;

		ppe_flow_unpublish(priv, entry);
	}
	mutex_unlock(&priv->vlan_lock);
	mutex_unlock(&priv->flow_lock);

	kfree(fb);
}

/* Every user port of the switch binds the same flowtable block, so it is shared
 * and reference counted rather than refused as busy.
 */
int ppe_setup_ft_block(struct qca_ppe_priv *priv,
		       struct flow_block_offload *f)
{
	struct flow_block_cb *block_cb;
	struct ppe_flow_block *fb;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &ppe_block_cb_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, ppe_flow_block_cb,
						priv);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}

		fb = kzalloc(sizeof(*fb), GFP_KERNEL);
		if (!fb)
			return -ENOMEM;
		fb->priv = priv;
		fb->block = f->block;

		block_cb = flow_block_cb_alloc(ppe_flow_block_cb, priv, fb,
					       ppe_flow_block_release);
		if (IS_ERR(block_cb)) {
			kfree(fb);
			return PTR_ERR(block_cb);
		}

		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &ppe_block_cb_list);
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, ppe_flow_block_cb,
						priv);
		if (!block_cb)
			return -ENOENT;

		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* A shared queue gives a sparse flow the loss rate the bulk imposes on it,
 * and a flow that sends a few packets a second recovers a lost one by a
 * retransmission timer, not by the next packet: a speed test's latency
 * probe stalls for hundreds of milliseconds while the download it measures
 * is unharmed. The hardware has no queue per flow, but a flow entry names a
 * priority profile the classifier ranks above every other source, so a flow
 * the counters show to be sparse is moved to the list the scheduler serves
 * ahead of the band, inside the band's shaper, and back the moment it is
 * not. A flow is sparse after ten periods under the packet count, one
 * second, which a TCP flow that is ramping never spends; a promoted flow
 * that bursts is demoted at the end of the period it burst in.
 */
#define PPE_SPARSE_PERIOD_MS	100
#define PPE_SPARSE_PKTS		8
#define PPE_SPARSE_QUIET	10

static bool ppe_sparse_flows = true;
module_param_named(sparse_flows, ppe_sparse_flows, bool, 0644);
MODULE_PARM_DESC(sparse_flows,
		 "Serve flows of under 80 packets a second ahead of a shaped port's bulk queue");

/* IPv4 has a writable five-word flat image in HPPE. Keep the valid key,
 * slot, host reference and counters throughout a priority change, including
 * packets already submitted through EDMA. IPv6's op image spans two slots
 * and retains the op-engine replacement below.
 */
static int ppe_flow_profile_ipv4(struct qca_ppe_priv *priv,
				 struct ppe_flow_entry *entry, u8 profile)
{
	u32 old[PPE_FLOW_ENTRY_WORDS_V4], w[PPE_FLOW_ENTRY_WORDS_V4];
	u32 read[PPE_FLOW_ENTRY_WORDS_V4];
	int ret, rollback;

	memcpy(old, entry->words, sizeof(old));
	old[0] |= FIELD_PREP(PPE_FLOW_E_AGE_MASK, PPE_FLOW_AGE_MAX);
	memcpy(w, old, sizeof(w));
	w[1] &= ~BIT(PPE_FLOW_E_PRI_PROFILE_OFF % 32);
	w[2] &= ~GENMASK(PPE_FLOW_E_PRI_PROFILE_LEN - 2, 0);
	ppe_entry_set(w, PPE_FLOW_E_PRI_PROFILE_OFF, PPE_FLOW_E_PRI_PROFILE_LEN,
		      profile);

	/* Write the complete SDK flat image, including its final commit word.
	 * The valid bit is never cleared, and no forwarding/key field changes.
	 */
	ret = regmap_bulk_write(priv->regmap, PPE_IN_FLOW_TBL(entry->index),
				w, ARRAY_SIZE(w));
	if (!ret) {
		ret = ppe_flow_entry_read(priv, entry->index, read, ARRAY_SIZE(read));
		if (!ret) {
			read[0] &= ~PPE_FLOW_E_AGE_MASK;
			w[0] &= ~PPE_FLOW_E_AGE_MASK;
			if (memcmp(read, w, sizeof(w)))
				ret = -EIO;
		}
	}
	if (!ret) {
		memcpy(entry->words, w, sizeof(w));
		priv->flow_profile_inplace++;
		return 0;
	}

	rollback = regmap_bulk_write(priv->regmap, PPE_IN_FLOW_TBL(entry->index),
				     old, ARRAY_SIZE(old));
	if (!rollback) {
		rollback = ppe_flow_entry_read(priv, entry->index, read, ARRAY_SIZE(read));
		if (!rollback) {
			read[0] &= ~PPE_FLOW_E_AGE_MASK;
			if (memcmp(read, entry->words, sizeof(read)))
				rollback = -EIO;
		}
	}
	if (rollback)
		priv->flow_profile_rollback_failed++;
	return ret;
}

static int ppe_flow_profile_set(struct qca_ppe_priv *priv,
				struct ppe_flow_entry *entry, u8 profile)
{
	u32 w[PPE_FLOW_ENTRY_WORDS_V6];
	u32 index, host_index, pkts;
	u64 bytes;
	int ret, add_ret;

	lockdep_assert_held(&priv->flow_lock);


	/* Only an identity-checked slot can be modified. All software allocation
	 * is serialized by flow_lock; autonomous aging cannot install a sibling.
	 */
	ret = ppe_flow_entry_age(priv, entry);
	if (ret < 0)
		goto out;

	if (!(entry->words[0] & PPE_FLOW_E_TYPE_IPV6)) {
		ret = ppe_flow_profile_ipv4(priv, entry, profile);
		goto out;
	}

	if (priv->flow_op_busy) {
		ret = -EBUSY;
		goto out;
	}
	ret = ppe_flow_counter_delta(priv, entry, &pkts, &bytes);
	if (ret)
		goto out;
	ppe_flow_account(priv, entry, pkts, bytes);
	memcpy(w, entry->words, entry->nwords * sizeof(*w));
	w[0] = (w[0] & ~PPE_FLOW_E_HOST_IDX_MASK) |
	       FIELD_PREP(PPE_FLOW_E_AGE_MASK, PPE_FLOW_AGE_MAX);
	ppe_entry_clear(w, PPE_FLOW_E_PRI_PROFILE_OFF,
			PPE_FLOW_E_PRI_PROFILE_LEN);
	ppe_entry_set(w, PPE_FLOW_E_PRI_PROFILE_OFF, PPE_FLOW_E_PRI_PROFILE_LEN,
		      profile);

	if (entry->xdev_bound) {
		ppe_flow_slot_clear(priv, entry->index);
		entry->xdev_bound = false;
	}
	ret = ppe_flow_entry_delete(priv, entry->index);
	if (ret) {
		if (priv->flow_op_busy) {
			entry->state = PPE_FLOW_PENDING_DELETE;
			ppe_flow_unpublish(priv, entry);
		} else if (entry->xdev) {
			ppe_flow_slot_set(priv, entry, entry->index);
		}
		goto out;
	}

	add_ret = ppe_flow_op(priv, PPE_TBL_OP_ADD, w, entry->nwords,
			      entry->hwords, entry->nhwords, &index, &host_index);
	if (add_ret) {
		if (priv->flow_op_busy) {
			ret = add_ret;
			goto pending_add;
		}
		/* Restore the old priority rather than leaving a deleted entry
		 * published with its previous index and counter baseline.
		 */
		memcpy(w, entry->words, entry->nwords * sizeof(*w));
		w[0] = (w[0] & ~PPE_FLOW_E_HOST_IDX_MASK) |
		       FIELD_PREP(PPE_FLOW_E_AGE_MASK, PPE_FLOW_AGE_MAX);
		ret = ppe_flow_op(priv, PPE_TBL_OP_ADD, w, entry->nwords,
				  entry->hwords, entry->nhwords, &index, &host_index);
		if (ret) {
			priv->flow_profile_rollback_failed++;
			if (priv->flow_op_busy)
				goto pending_add;
			/* Both ADDs are confirmed failed; the original DEL completed. */
			entry->state = PPE_FLOW_RETIRING;
			ppe_flow_unpublish(priv, entry);
			goto out;
		}
	}

	if (host_index != entry->host_index) {
		ppe_host_ref_get(priv, host_index);
		ppe_host_ref_put(priv, entry->host_index);
		entry->host_index = host_index;
	}
	memcpy(entry->words, w, entry->nwords * sizeof(*w));
	entry->words[0] = (w[0] | host_index << PPE_FLOW_E_HOST_IDX_OFF) &
			  ~PPE_FLOW_E_AGE_MASK;
	entry->index = index;
	ret = ppe_flow_counter_clear(priv, index);
	if (ret) {
		entry->state = PPE_FLOW_RETIRING;
		ppe_flow_unpublish(priv, entry);
		goto out;
	}
	if (entry->xdev)
		ppe_flow_slot_set(priv, entry, index);
	entry->packets = 0;
	entry->bytes = 0;
	ret = add_ret;
	goto out;
pending_add:
	/* Preserve the pending image and old host lease until the result says
	 * whether the replacement acquired the same host. */
	entry->previous_host = entry->host_index;
	entry->host_index = U32_MAX;
	entry->index = U32_MAX;
	memcpy(entry->words, w, entry->nwords * sizeof(*w));
	entry->words[0] &= ~PPE_FLOW_E_AGE_MASK;
	entry->state = PPE_FLOW_PENDING_ADD;
	ppe_flow_unpublish(priv, entry);
out:
	if (ret)
		priv->flow_profile_failed++;
	return ret;
}

static void ppe_sparse_work(struct work_struct *work)
{
	struct qca_ppe_priv *priv = container_of(work, struct qca_ppe_priv,
						 sparse_work.work);
	struct ppe_flow_entry *entry, *tmp;
	u32 pkts, delta;

	mutex_lock(&priv->flow_lock);
	mutex_lock(&priv->vlan_lock);
	/* Host deletion uses a different result queue but the same mutation
	 * admission barrier. Never reuse a host while its command is unknown. */
	if (priv->flow_op_busy && priv->flow_op_rslt_reg == PPE_HOST_TBL_OP_RSLT) {
		int ret = ppe_flow_op_finish(priv, NULL, NULL);

		if (!ret || ret == -ENOENT) {
			clear_bit(priv->host_retire_index, priv->host_retired);
			priv->host_retire_pending--;
		}
	}
	if (!priv->flow_op_busy) {
		unsigned long host;

		for_each_set_bit(host, priv->host_retired, priv->data->num_host_entries) {
			int ret = ppe_host_del(priv, host);

			if (!ret || ret == -ENOENT) {
				clear_bit(host, priv->host_retired);
				priv->host_retire_pending--;
			}
			if (priv->flow_op_busy)
				break;
		}
	}
	list_for_each_entry_safe(entry, tmp, &priv->flow_retire_list, list) {
		int ret = ppe_flow_entry_destroy(priv, entry);

		if (ret) {
			priv->flow_retire_cookie = entry->cookie;
			priv->flow_retire_result = ret;
			continue;
		}
		list_del(&entry->list);
		priv->flow_retire_pending--;
		kfree(entry);
	}
	list_for_each_entry_safe(entry, tmp, &priv->flow_list, list) {
		u64 bytes;


		if (regmap_read(priv->regmap, PPE_IN_FLOW_CNT_TBL(entry->index), &pkts)) {
			priv->flow_counter_read_failed++;
			continue;
		}
		delta = pkts - (u32)entry->packets;

		/* The age read costs the entry's words and only a flow that
		 * moved has anything to attribute, so it is taken on the
		 * delta rather than every period.
		 */
		if (delta) {
			u32 acct;

			if (ppe_flow_entry_age(priv, entry) < 0 ||
			    ppe_flow_counter_delta(priv, entry, &acct, &bytes))
				continue;
			ppe_flow_account(priv, entry, acct, bytes);
		}

		/* CPU egress has three bands, without a fourth sparse list. */
		if (entry->oport == QCA_PPE_CPU_PORT)
			continue;

		/* A sparse profile serves a shaped port and would otherwise
		 * override the DSCP written by its ACL classifier.
		 */
		if (delta > PPE_SPARSE_PKTS || !ppe_sparse_flows ||
		    !ppe_port_shaped(priv, entry->oport)) {
			entry->quiet = 0;
			if (entry->sparse &&
			    !ppe_flow_profile_set(priv, entry, entry->profile)) {
				entry->sparse = false;
				priv->flow_sparse_demoted++;
			}
			continue;
		}

		if (entry->sparse)
			continue;
		if (entry->quiet < PPE_SPARSE_QUIET) {
			entry->quiet++;
			continue;
		}
		if (!ppe_flow_profile_set(priv, entry, PPE_QOS_SPARSE_PRI)) {
			entry->sparse = true;
			priv->flow_sparse_promoted++;
		}
	}
	mutex_unlock(&priv->vlan_lock);
	mutex_unlock(&priv->flow_lock);

	schedule_delayed_work(&priv->sparse_work,
			      msecs_to_jiffies(PPE_SPARSE_PERIOD_MS));
}

int ppe_flow_offload_init(struct qca_ppe_priv *priv)
{
	struct device *dev = priv->ds.dev;
	int ret;

	priv->eg_l3_if = devm_kcalloc(dev, PPE_EG_L3_IF_ENTRIES,
				      sizeof(*priv->eg_l3_if), GFP_KERNEL);
	priv->pub_ip = devm_kcalloc(dev, PPE_PUB_IP_ENTRIES,
				    sizeof(*priv->pub_ip), GFP_KERNEL);
	priv->nexthop = devm_kcalloc(dev, priv->data->num_nexthop_entries,
				     sizeof(*priv->nexthop), GFP_KERNEL);
	priv->host_ref = devm_kcalloc(dev, priv->data->num_host_entries,
				      sizeof(*priv->host_ref), GFP_KERNEL);
	priv->my_mac = devm_kcalloc(dev, PPE_MY_MAC_ENTRIES,
				    sizeof(*priv->my_mac), GFP_KERNEL);
	priv->host_retired = devm_bitmap_zalloc(dev, priv->data->num_host_entries,
					      GFP_KERNEL);
	priv->flow_xdev = devm_kcalloc(dev, priv->data->num_flow_entries,
				       sizeof(*priv->flow_xdev), GFP_KERNEL);
	priv->ul_port = -1;
	priv->flow_ct = devm_kcalloc(dev, priv->data->num_flow_entries,
				     sizeof(*priv->flow_ct), GFP_KERNEL);
	priv->flow_dscp = devm_kcalloc(dev, priv->data->num_flow_entries,
				       sizeof(*priv->flow_dscp), GFP_KERNEL);
	priv->flow_vid = devm_kcalloc(dev, priv->data->num_flow_entries,
				      sizeof(*priv->flow_vid), GFP_KERNEL);
	priv->flow_port = devm_kcalloc(dev, priv->data->num_flow_entries,
				       sizeof(*priv->flow_port), GFP_KERNEL);
	if (!priv->host_retired || !priv->eg_l3_if || !priv->pub_ip || !priv->nexthop ||
	    !priv->host_ref || !priv->my_mac || !priv->flow_xdev ||
	    !priv->flow_ct || !priv->flow_dscp || !priv->flow_vid ||
	    !priv->flow_port)
		return -ENOMEM;

	INIT_LIST_HEAD(&priv->flow_list);
	INIT_LIST_HEAD(&priv->flow_retire_list);

	ret = rhashtable_init(&priv->flow_table, &ppe_flow_ht_params);
	if (ret)
		return ret;

	priv->netdev_nb.notifier_call = ppe_flow_netdev_event;
	ret = register_netdevice_notifier(&priv->netdev_nb);
	if (ret) {
		rhashtable_destroy(&priv->flow_table);
		return ret;
	}
	priv->flow_switchdev_nb.notifier_call = ppe_flow_switchdev_event;
	ret = register_switchdev_blocking_notifier(&priv->flow_switchdev_nb);
	if (ret) {
		unregister_netdevice_notifier(&priv->netdev_nb);
		rhashtable_destroy(&priv->flow_table);
		return ret;
	}

	INIT_DELAYED_WORK(&priv->sparse_work, ppe_sparse_work);
	schedule_delayed_work(&priv->sparse_work,
			      msecs_to_jiffies(PPE_SPARSE_PERIOD_MS));

	return 0;
}

void ppe_flow_offload_exit(struct qca_ppe_priv *priv)
{
	struct ppe_flow_entry *entry, *tmp;
	int i;

	unregister_switchdev_blocking_notifier(&priv->flow_switchdev_nb);
	unregister_netdevice_notifier(&priv->netdev_nb);
	cancel_delayed_work_sync(&priv->sparse_work);

	mutex_lock(&priv->flow_lock);
	mutex_lock(&priv->vlan_lock);
	list_for_each_entry_safe(entry, tmp, &priv->flow_list, list) {
		rhashtable_remove_fast(&priv->flow_table, &entry->node,
				       ppe_flow_ht_params);
		list_del(&entry->list);
		ppe_flow_entry_destroy(priv, entry);
		kfree(entry);
	}
	list_for_each_entry_safe(entry, tmp, &priv->flow_retire_list, list) {
		list_del(&entry->list);
		kfree(entry);
	}
	mutex_unlock(&priv->vlan_lock);
	mutex_unlock(&priv->flow_lock);

	rhashtable_destroy(&priv->flow_table);

	for (i = 0; i < ARRAY_SIZE(priv->shaper_dev); i++)
		if (priv->shaper_dev[i])
			unregister_netdev(priv->shaper_dev[i]);
}
