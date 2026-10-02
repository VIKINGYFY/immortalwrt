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
 *
 * Three side tables carry what does not fit in the flow entry: the egress L3
 * interface holds the source MAC and the PPPoE session, the nexthop holds the
 * destination MAC, the egress port, the VLAN tag and the DNAT address, and the
 * public-address table holds the SNAT address. All three are shared between
 * flows that need the same contents and are reference counted.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/if_pppox.h>
#include <linux/if_vlan.h>
#include <linux/module.h>
#include <linux/hashtable.h>
#include <linux/jhash.h>
#include <net/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/netfilter.h>
#include <linux/netfilter_netdev.h>
#include <linux/soc/qcom/qca_ppe.h>
#include <linux/netdevice.h>
#include <linux/rhashtable.h>
#include <linux/seq_file.h>
#include <net/dsa.h>
#include <net/cfg80211.h>
#include <net/flow_offload.h>
#include <net/netfilter/nf_flow_table.h>
#include <net/pkt_cls.h>

#include "qca_ppe.h"

struct ppe_wifi_ingress {
	struct net_device *dev;
	struct qca_ppe_priv *priv;
	struct nf_hook_ops hook;
	u8 mac[ETH_ALEN];
	int vsi, xlt, my_mac, master_ifindex;
	bool ingress_core, cpu_egress_core;
	u16 vid;
	atomic_t flows;
	atomic64_t injected, injected_bytes;
	bool disabled;
};
static int ppe_dsa_core_port_get(struct qca_ppe_priv *priv, int port, bool ingress);
static void ppe_dsa_core_port_put(struct qca_ppe_priv *priv, int port, bool ingress);

static struct ppe_wifi_ingress ppe_wifi_ingress[QCA_PPE_WIFI_INGRESS_SLOTS];
static const struct qca_ppe_wifi_inject_ops __rcu *ppe_wifi_inject_ops;

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
	struct ppe_wifi_ingress *wifi_ingress;
	/* The profile the entry is given; filled only where the flowtable
	 * reports a priority, and zero means DSCP still decides.
	 */
	u8 priority;
};

/* Direction-specific identity: downlink post-edit, uplink pre-edit.
 * Padding is zeroed before hashing. */
struct ppe_wifi_key {
	__be32 src, dst;
	__be16 sport, dport;
	u8 proto, iport;
	u8 dmac[ETH_ALEN], smac[ETH_ALEN];
	u16 reserved;
};

struct ppe_flow_entry {
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
	u32 host_index;
	int nexthop;
	int my_mac;
	int l3_if;
	int eg_l3_if;
	int pub_ip;
	int wan_port;
	int wan_iport;
	u8 iport;
	u8 oport;
	bool wifi_egress;
	bool wifi_bound;
	struct net_device *wifi_dev;
	struct ppe_wifi_key wifi_key;
	struct hlist_node wifi_node;
	struct ppe_wifi_ingress *wifi_ingress;
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

/* This lock/hash outlive every PPE instance.  Teardown removes a binding
 * under the same lock used by RX and RX takes its own netdev reference. */
static DEFINE_SPINLOCK(ppe_wifi_lock);
static DEFINE_HASHTABLE(ppe_wifi_flows, 8);
static DEFINE_HASHTABLE(ppe_wifi_ingress_flows, 8);
static atomic64_t ppe_wifi_tx = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_drop = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_candidates = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_installed = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_invalid = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_binding_miss = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_link_down = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_xmit_drop = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_accepted = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_accepted_bytes = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_injected = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_injected_bytes = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_inject_fallback = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_ingress_miss = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_ingress_lookup_miss = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_ingress_gso = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_ingress_submit_busy = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_ingress_submit_unavailable = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_ingress_prepare_fail = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_ingress_vlan = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_ingress_checksum_partial = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_ingress_checksum_help = ATOMIC64_INIT(0);
static atomic64_t ppe_wifi_ingress_checksum_fail = ATOMIC64_INIT(0);

static u32 ppe_wifi_hash(const struct ppe_wifi_key *key)
{
	return jhash(key, sizeof(*key), 0);
}

void qca_ppe_wifi_inject_register(const struct qca_ppe_wifi_inject_ops *ops)
{
	rcu_assign_pointer(ppe_wifi_inject_ops, ops);
}
EXPORT_SYMBOL_GPL(qca_ppe_wifi_inject_register);

void qca_ppe_wifi_inject_unregister(const struct qca_ppe_wifi_inject_ops *ops)
{
	if (rcu_access_pointer(ppe_wifi_inject_ops) == ops)
		RCU_INIT_POINTER(ppe_wifi_inject_ops, NULL);
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(qca_ppe_wifi_inject_unregister);

static unsigned int ppe_wifi_ingress_hook(void *data, struct sk_buff *skb,
					const struct nf_hook_state *state)
{
	struct ppe_wifi_ingress *slot = data;
	const struct qca_ppe_wifi_inject_ops *ops;
	struct ppe_wifi_key key = {};
	struct ppe_flow_entry *entry;
	struct sk_buff *copy = NULL;
	const struct iphdr *iph;
	__be16 ports[2];
	bool found = false;
	int ret = -ENOMEM;
	u32 length;

	if (READ_ONCE(slot->disabled) ||
	    !atomic_read(&slot->flows) || skb->protocol != htons(ETH_P_IP) ||
	    skb_mac_header(skb) + ETH_HLEN != skb->data ||
	    !ether_addr_equal(eth_hdr(skb)->h_dest, slot->mac) ||
	    !pskb_may_pull(skb, sizeof(*iph) + sizeof(struct udphdr)))
		return NF_ACCEPT;
	if (skb_is_gso(skb)) {
		atomic64_inc(&ppe_wifi_ingress_gso);
		return NF_ACCEPT;
	}
	if (skb_vlan_tag_present(skb)) {
		atomic64_inc(&ppe_wifi_ingress_vlan);
		return NF_ACCEPT;
	}
	iph = (const void *)skb->data;
	if (iph->version != 4 || iph->ihl != 5 || ip_is_fragment(iph) ||
	    iph->ttl <= 1 || ntohs(iph->tot_len) > skb->len ||
	    ntohs(iph->tot_len) < sizeof(*iph) + sizeof(struct udphdr))
		return NF_ACCEPT;
	if (iph->protocol == IPPROTO_TCP) {
		const struct tcphdr *th;

		if (!pskb_may_pull(skb, sizeof(*iph) + sizeof(*th)))
			return NF_ACCEPT;
		iph = (const void *)skb->data;
		th = (const void *)(iph + 1);
		if (th->fin || th->rst || th->doff < 5 ||
		    sizeof(*iph) + th->doff * 4 > ntohs(iph->tot_len))
			return NF_ACCEPT;
	} else if (iph->protocol != IPPROTO_UDP) {
		return NF_ACCEPT;
	}
	key.src = iph->saddr;
	key.dst = iph->daddr;
	memcpy(ports, iph + 1, sizeof(ports));
	key.sport = ports[0];
	key.dport = ports[1];
	key.proto = iph->protocol;
	key.iport = slot - ppe_wifi_ingress;

	rcu_read_lock();
	ops = rcu_dereference(ppe_wifi_inject_ops);
	if (!ops)
		goto accept;
	/* Publication and removal share this lock. Keep it until submission
	 * so deleting a flow cannot race a newly selected injection. */
	spin_lock_bh(&ppe_wifi_lock);
	hash_for_each_possible(ppe_wifi_ingress_flows, entry, wifi_node,
			       ppe_wifi_hash(&key)) {
		if (!memcmp(&key, &entry->wifi_key, sizeof(key))) {
			found = true;
			break;
		}
	}
	if (!found || READ_ONCE(slot->disabled)) {
		if (!found)
			atomic64_inc(&ppe_wifi_ingress_lookup_miss);
		goto unlock;
	}
	length = skb->len + ETH_HLEN;
	copy = skb_copy_expand(skb, 64 + ETH_HLEN + VLAN_HLEN, 0, GFP_ATOMIC);
	if (!copy)
		goto fallback;
	/* The internal EDMA ingress producer deliberately does not request TX
	 * checksum generation. Complete a Wi-Fi RX packet marked PARTIAL on the
	 * private copy before adding the Ethernet/S-tag headers, then advertise a
	 * plain packet to the producer. */
	if (copy->ip_summed == CHECKSUM_PARTIAL) {
		atomic64_inc(&ppe_wifi_ingress_checksum_partial);
		ret = skb_checksum_help(copy);
		if (ret) {
			atomic64_inc(&ppe_wifi_ingress_checksum_fail);
			goto fallback;
		}
		atomic64_inc(&ppe_wifi_ingress_checksum_help);
	}
	copy->ip_summed = CHECKSUM_NONE;
	skb_push(copy, ETH_HLEN);
	memcpy(copy->data, eth_hdr(skb), ETH_HLEN);
	skb_reset_mac_header(copy);
	ret = __vlan_insert_tag(copy, htons(ETH_P_8021AD), slot->vid);
	if (ret)
		goto fallback;
	copy->protocol = htons(ETH_P_8021AD);
	copy->dev = ops->dev;
	ret = ops->xmit(ops->dev, copy);
	if (!ret) {
		atomic64_inc(&ppe_wifi_injected);
		atomic64_add(length, &ppe_wifi_injected_bytes);
		atomic64_inc(&slot->injected);
		atomic64_add(length, &slot->injected_bytes);
		spin_unlock_bh(&ppe_wifi_lock);
		rcu_read_unlock();
		consume_skb(skb);
		return NF_STOLEN;
	}
fallback:
	if (ret == -EBUSY)
		atomic64_inc(&ppe_wifi_ingress_submit_busy);
	else if (ret == -ENETDOWN)
		atomic64_inc(&ppe_wifi_ingress_submit_unavailable);
	else
		atomic64_inc(&ppe_wifi_ingress_prepare_fail);
	if (copy)
		dev_kfree_skb_any(copy);
	atomic64_inc(&ppe_wifi_inject_fallback);
unlock:
	spin_unlock_bh(&ppe_wifi_lock);
accept:
	rcu_read_unlock();
	return NF_ACCEPT;
}

bool qca_ppe_wifi_ingress_return(struct sk_buff *skb, u8 source_port)
{
	struct ppe_wifi_ingress *slot;
	struct vlan_ethhdr *eth;
	struct net_device *dev;
	u16 vid;

	if (skb_headlen(skb) < sizeof(*eth))
		return false;
	eth = (void *)skb->data;
	if (eth->h_vlan_proto != htons(ETH_P_8021AD))
		return false;
	vid = ntohs(eth->h_vlan_TCI) & VLAN_VID_MASK;
	if (vid < QCA_PPE_WIFI_INGRESS_VID_BASE ||
	    vid >= QCA_PPE_WIFI_INGRESS_VID_BASE + QCA_PPE_WIFI_INGRESS_SLOTS)
		return false;
	slot = &ppe_wifi_ingress[vid - QCA_PPE_WIFI_INGRESS_VID_BASE];
	spin_lock_bh(&ppe_wifi_lock);
	if (!slot->dev || source_port != QCA_PPE_CPU_PORT) {
		spin_unlock_bh(&ppe_wifi_lock);
		return false;
	}
	WRITE_ONCE(slot->disabled, true);
	dev = slot->dev;
	if (dev)
		dev_hold(dev);
	spin_unlock_bh(&ppe_wifi_lock);
	atomic64_inc(&ppe_wifi_ingress_miss);
	if (!dev || !netif_running(dev)) {
		if (dev)
			dev_put(dev);
		dev_kfree_skb_any(skb);
		return true;
	}
	/* The XLT restores the internal S-tag on CPU misses. Remove it before
	 * re-entering the same AP's normal receive path, with injection disabled. */
	memmove(skb->data + VLAN_HLEN, skb->data, 2 * ETH_ALEN);
	skb_pull(skb, VLAN_HLEN);
	skb->dev = dev;
	skb->protocol = eth_type_trans(skb, dev);
	skb_reset_network_header(skb);
	skb->ip_summed = CHECKSUM_NONE;
	netif_receive_skb(skb);
	dev_put(dev);
	return true;
}
EXPORT_SYMBOL_GPL(qca_ppe_wifi_ingress_return);

bool qca_ppe_wifi_xmit(struct sk_buff *skb, u8 iport)
{
	struct ppe_wifi_key key = {};
	struct ppe_flow_entry *entry;
	struct net_device *dev = NULL;
	const struct ethhdr *eth;
	const struct iphdr *iph;
	unsigned int iplen;
	__be16 ports[2];
	int ret;
	atomic64_t *reason = &ppe_wifi_invalid;
	u32 length = skb->len;

	/* Called only for the PPE service marker.  Already edited packets must
	 * never enter IP forwarding again if the binding disappeared. */
	if (skb_headlen(skb) < ETH_HLEN + sizeof(*iph) + sizeof(ports))
		goto drop;
	eth = (const struct ethhdr *)skb->data;
	iph = (const struct iphdr *)(skb->data + ETH_HLEN);
	if (eth->h_proto != htons(ETH_P_IP) || iph->version != 4 ||
	    iph->ihl != 5 || ip_is_fragment(iph) || !iph->ttl)
		goto drop;
	iplen = ntohs(iph->tot_len);
	if (iplen + ETH_HLEN > skb->len || iplen < sizeof(*iph) + sizeof(ports))
		goto drop;
	if (iph->protocol == IPPROTO_TCP) {
		const struct tcphdr *th = (const void *)(iph + 1);

		if (iplen < sizeof(*iph) + sizeof(*th) ||
		    skb_headlen(skb) < ETH_HLEN + sizeof(*iph) + sizeof(*th) ||
		    th->doff < 5 || sizeof(*iph) + th->doff * 4 > iplen)
			goto drop;
	} else if (iph->protocol != IPPROTO_UDP ||
		   iplen < sizeof(*iph) + sizeof(struct udphdr)) {
		goto drop;
	}
	memcpy(ports, iph + 1, sizeof(ports));
	key.src = iph->saddr;
	key.dst = iph->daddr;
	key.sport = ports[0];
	key.dport = ports[1];
	key.proto = iph->protocol;
	key.iport = iport;
	ether_addr_copy(key.dmac, eth->h_dest);
	ether_addr_copy(key.smac, eth->h_source);
	spin_lock_bh(&ppe_wifi_lock);
	hash_for_each_possible(ppe_wifi_flows, entry, wifi_node, ppe_wifi_hash(&key)) {
		if (memcmp(&key, &entry->wifi_key, sizeof(key)))
			continue;
		dev = entry->wifi_dev;
		dev_hold(dev);
		break;
	}
	spin_unlock_bh(&ppe_wifi_lock);
	if (!dev) {
		reason = &ppe_wifi_binding_miss;
		goto drop;
	}
	if (!netif_running(dev) || !netif_carrier_ok(dev)) {
		dev_put(dev);
		reason = &ppe_wifi_link_down;
		goto drop;
	}
	/* TX owns the complete Ethernet frame; native WLAN TX supplies peer,
	 * key, authorization, queueing and radio encapsulation checks. */
	skb->dev = dev;
	skb_reset_mac_header(skb);
	skb_set_network_header(skb, ETH_HLEN);
	skb_set_transport_header(skb, ETH_HLEN + sizeof(*iph));
	skb->protocol = htons(ETH_P_IP);
	skb->ip_summed = CHECKSUM_NONE;
	ret = dev_queue_xmit(skb);
	dev_put(dev);
	if (net_xmit_eval(ret)) {
		atomic64_inc(&ppe_wifi_xmit_drop);
	} else {
		atomic64_inc(&ppe_wifi_accepted);
		atomic64_add(length, &ppe_wifi_accepted_bytes);
	}
	atomic64_inc(&ppe_wifi_tx);
	return true;
drop:
	atomic64_inc(reason);
	atomic64_inc(&ppe_wifi_drop);
	dev_kfree_skb_any(skb);
	return true;
}
EXPORT_SYMBOL_GPL(qca_ppe_wifi_xmit);

static int ppe_wifi_stats_show(struct seq_file *s, void *data)
{
	struct qca_ppe_priv *priv = s->private;
	u32 service_in[2] = {}, service_l2 = 0, service_eg[2] = {};
	int service_ret = 0;

	service_ret |= regmap_bulk_read(priv->regmap,
				       PPE_SERVICE_TBL(QCA_PPE_WIFI_SERVICE_CODE),
				       service_in, ARRAY_SIZE(service_in));
	service_ret |= regmap_read(priv->regmap,
				   PPE_IN_L2_SERVICE_TBL(QCA_PPE_WIFI_SERVICE_CODE),
				   &service_l2);
	service_ret |= regmap_bulk_read(priv->regmap,
				       PPE_EG_SERVICE_TBL(QCA_PPE_WIFI_SERVICE_CODE),
				       service_eg, ARRAY_SIZE(service_eg));

	seq_printf(s, "enabled %u\n"
		   "candidates %lld\ninstalled %lld\nhandoff %lld\ndrop %lld\n"
		   "invalid_frame %lld\nbinding_miss %lld\nlink_down %lld\nxmit_drop %lld\n"
		   "accepted %lld\naccepted_bytes %lld\n",
		   1,
		   atomic64_read(&ppe_wifi_candidates),
		   atomic64_read(&ppe_wifi_installed),
		   atomic64_read(&ppe_wifi_tx), atomic64_read(&ppe_wifi_drop),
		   atomic64_read(&ppe_wifi_invalid), atomic64_read(&ppe_wifi_binding_miss),
		   atomic64_read(&ppe_wifi_link_down), atomic64_read(&ppe_wifi_xmit_drop),
		   atomic64_read(&ppe_wifi_accepted), atomic64_read(&ppe_wifi_accepted_bytes));
	if (!service_ret)
		seq_printf(s, "service_in %08x:%08x\nservice_l2 %08x\n"
			   "service_eg %08x:%08x\n", service_in[0], service_in[1],
			   service_l2, service_eg[0], service_eg[1]);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ppe_wifi_stats);

static int ppe_wifi_ingress_show(struct seq_file *s, void *data)
{
	struct qca_ppe_priv *priv = s->private;
	int i;

	seq_printf(s, "enabled %u\ninjected %lld\ninjected_bytes %lld\n"
		   "submit_fallback %lld\nhardware_miss %lld\n"
		   "lookup_miss %lld\ngso_skip %lld\nvlan_skip %lld\n"
		   "submit_busy %lld\nsubmit_unavailable %lld\nprepare_fail %lld\n"
		   "checksum_partial %lld\nchecksum_help %lld\nchecksum_fail %lld\n",
		   1, atomic64_read(&ppe_wifi_injected),
		   atomic64_read(&ppe_wifi_injected_bytes),
		   atomic64_read(&ppe_wifi_inject_fallback),
		   atomic64_read(&ppe_wifi_ingress_miss),
		   atomic64_read(&ppe_wifi_ingress_lookup_miss),
		   atomic64_read(&ppe_wifi_ingress_gso),
		   atomic64_read(&ppe_wifi_ingress_vlan),
		   atomic64_read(&ppe_wifi_ingress_submit_busy),
		   atomic64_read(&ppe_wifi_ingress_submit_unavailable),
		   atomic64_read(&ppe_wifi_ingress_prepare_fail),
		   atomic64_read(&ppe_wifi_ingress_checksum_partial),
		   atomic64_read(&ppe_wifi_ingress_checksum_help),
		   atomic64_read(&ppe_wifi_ingress_checksum_fail));
	seq_printf(s, "transport cpu_ingress port=%u service=0\n", QCA_PPE_CPU_PORT);
	for (i = 0; i < QCA_PPE_WIFI_INGRESS_SLOTS; i++) {
		struct ppe_wifi_ingress *slot = &ppe_wifi_ingress[i];
		int xlt = -1;
		int vsi = -1;
		bool active;
		u32 rule[3] = {}, action[3] = {};
		u32 eg_rule[2] = {}, eg_action[2] = {};
		u32 vp[3] = {};
		u32 parser;
		int ret;

		spin_lock_bh(&ppe_wifi_lock);
		active = !!slot->dev;
		priv = active ? slot->priv : NULL;
		if (active) {
			seq_printf(s, "slot %d dev=%s vid=%u vsi=%d xlt=%d flows=%d "
				   "disabled=%u priority=%d mac=%pM injected=%lld injected_bytes=%lld\n", i,
				   slot->dev->name, slot->vid, slot->vsi, slot->xlt,
				   atomic_read(&slot->flows), slot->disabled,
				   slot->hook.priority, slot->mac,
				   atomic64_read(&slot->injected),
				   atomic64_read(&slot->injected_bytes));
			xlt = slot->xlt;
			vsi = slot->vsi;
		}
		spin_unlock_bh(&ppe_wifi_lock);
		if (!active || !priv || xlt < 0)
			continue;

		/* One read per active slot gives enough evidence to distinguish a
		 * software submission from a rule that was rejected by the PPE. */
		ret = regmap_bulk_read(priv->regmap, PPE_XLT_RULE_TBL(xlt),
				       rule, ARRAY_SIZE(rule));
		ret |= regmap_bulk_read(priv->regmap, PPE_XLT_ACTION_TBL(xlt),
					 action, ARRAY_SIZE(action));
		ret |= regmap_bulk_read(priv->regmap, PPE_EG_XLT_RULE(xlt),
					 eg_rule, ARRAY_SIZE(eg_rule));
		ret |= regmap_bulk_read(priv->regmap, PPE_EG_XLT_ACTION(xlt),
					 eg_action, ARRAY_SIZE(eg_action));
		ret |= regmap_bulk_read(priv->regmap,
					PPE_L3_VP_PORT_TBL(QCA_PPE_CPU_PORT),
					vp, ARRAY_SIZE(vp));
		ret |= regmap_read(priv->regmap,
			PPE_PORT_PARSING(QCA_PPE_CPU_PORT), &parser);
		if (ret)
			seq_printf(s, "slot %d reg_read_error=%d\n", i, ret);
		else
			seq_printf(s, "slot %d reg vsi=%d xlt=%d in_rule=%08x:%08x:%08x "
				   "in_action=%08x:%08x:%08x eg_rule=%08x:%08x "
				   "eg_action=%08x:%08x vp_l3=%08x:%08x:%08x parser=%08x\n", i, vsi, xlt,
				   rule[0], rule[1], rule[2], action[0], action[1],
				   action[2], eg_rule[0], eg_rule[1], eg_action[0],
				   eg_action[1], vp[0], vp[1], vp[2], parser);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ppe_wifi_ingress);

/* Keep the software-side flow identity and the exact nexthop image together
 * in one readout. The normal `flows` file only exposes hardware counters, so
 * it cannot distinguish a LAN-to-WAN entry from its reverse or show whether
 * the RTL9303 TX service tag was actually put in the nexthop.
 */
static int ppe_offload_entries_show(struct seq_file *s, void *data)
{
	struct qca_ppe_priv *priv = s->private;
	struct ppe_flow_entry *entry;

	seq_puts(s, "cookie index src_if iport oport wifi ivid ovid dsa_service dsa_vid "
		 "nexthop nh_type nh_port nh_stag_fmt nh_svid nh_ctag_fmt nh_cvid "
		 "l3_if eg_l3_if wan_port wan_iport packets bytes nh_words "
		 "wifi_ifindex wifi_dev sw_sc hw_read hw_sc fwd port_valid "
		 "key_src key_sport key_dst key_dport key_proto key_smac key_dmac key_stage "
		 "wifi_ingress_dev wifi_ingress_vsi\n");

	guard(mutex)(&priv->flow_lock);
	list_for_each_entry(entry, &priv->flow_list, list) {
		u32 *words = NULL;
		u16 dsa_vid = 0;
		u64 packets, bytes;
		u32 hw[PPE_FLOW_ENTRY_WORDS_V6] = {};
		int hw_ret;
		int i;

		if (entry->nexthop >= 0 &&
		    entry->nexthop < priv->data->num_nexthop_entries) {
			words = priv->nexthop[entry->nexthop].words;
			if (entry->dsa_service >= 0 &&
			    entry->dsa_service < QCA_PPE_DSA_SERVICE_MAX)
				dsa_vid = priv->dsa_service[entry->dsa_service].vid;
		}

		ppe_flow_counter_read(priv, entry->index, &packets, &bytes);
		seq_printf(s, "%lx %u %u %u %u %u %u %u %d %u %d ",
			   entry->cookie, entry->index, entry->src_if,
			   entry->iport, entry->oport, entry->wifi_egress,
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
		seq_printf(s, "%d %d %d %d %llu %llu ", entry->l3_if,
			   entry->eg_l3_if, entry->wan_port, entry->wan_iport,
			   packets, bytes);
		if (!words)
			seq_putc(s, '-');
		else
			for (i = 0; i < PPE_NEXTHOP_WORDS; i++)
				seq_printf(s, "%s%08x", i ? ":" : "", words[i]);
		hw_ret = (entry->wifi_egress || entry->wifi_ingress) ?
			 ppe_flow_entry_read(priv, entry->index, hw, entry->nwords) :
			 -EOPNOTSUPP;
		seq_printf(s, " %d %s %u %d %u %u %u %pI4 %u %pI4 %u %u %pM %pM",
			   entry->wifi_dev ? entry->wifi_dev->ifindex : 0,
			   entry->wifi_dev ? entry->wifi_dev->name : "none",
			   (u32)ppe_entry_get(entry->words, 68, 8), hw_ret,
			   hw_ret ? 0 : (u32)ppe_entry_get(hw, 68, 8),
			   (u32)ppe_entry_get(entry->words, PPE_FLOW_E_FWD_TYPE_OFF,
					      PPE_FLOW_E_FWD_TYPE_LEN),
			   (u32)ppe_entry_get(entry->words, PPE_FLOW_E_PORT_VALID_OFF,
					      PPE_FLOW_E_PORT_VALID_LEN),
			   &entry->wifi_key.src, ntohs(entry->wifi_key.sport),
			   &entry->wifi_key.dst, ntohs(entry->wifi_key.dport),
			   entry->wifi_key.proto, entry->wifi_key.smac,
			   entry->wifi_key.dmac);
		seq_printf(s, " %s %s %d\n", entry->wifi_ingress ? "ingress" :
			   entry->wifi_egress ? "post" : "none", entry->wifi_ingress ?
			   entry->wifi_ingress->dev->name : "none",
			   entry->wifi_ingress ? entry->wifi_ingress->vsi : -1);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ppe_offload_entries);

/* The downlink service retains L2/L3 editing and publishes its marker. */
static int ppe_wifi_service_write(struct qca_ppe_priv *priv, u8 code,
				  u32 bypass, u32 l2, u8 next_code)
{
	u32 in[2] = { bypass, 0 }, eg[2] = { 0, next_code };
	u32 read_in[2], read_l2, read_eg[2];
	int ret;

	ret = regmap_bulk_write(priv->regmap, PPE_SERVICE_TBL(code), in, 2);
	if (ret)
		return ret;
	ret = regmap_write(priv->regmap, PPE_IN_L2_SERVICE_TBL(code), l2);
	if (ret)
		return ret;
	ret = regmap_bulk_write(priv->regmap, PPE_EG_SERVICE_TBL(code), eg, 2);
	if (ret)
		return ret;
	ret = regmap_bulk_read(priv->regmap, PPE_SERVICE_TBL(code), read_in, 2);
	if (ret)
		return ret;
	ret = regmap_read(priv->regmap, PPE_IN_L2_SERVICE_TBL(code), &read_l2);
	if (ret)
		return ret;
	ret = regmap_bulk_read(priv->regmap, PPE_EG_SERVICE_TBL(code), read_eg, 2);
	if (ret)
		return ret;
	return memcmp(in, read_in, sizeof(in)) || l2 != read_l2 ||
	       memcmp(eg, read_eg, sizeof(eg)) ? -EIO : 0;
}

static int ppe_wifi_service_program(struct qca_ppe_priv *priv)
{
	u32 bypass = BIT(0) | BIT(5) | BIT(10) | PPE_IN_L2_SERVICE_FAKE_MAC_DROP_BYP;
	u32 l2 = FIELD_PREP(PPE_IN_L2_SERVICE_BYPASS, bypass);

	return ppe_wifi_service_write(priv, QCA_PPE_WIFI_SERVICE_CODE, 0,
				     l2 | PPE_IN_L2_SERVICE_DST_VALID,
				     QCA_PPE_WIFI_SERVICE_CODE);
}

void ppe_flow_offload_debugfs_init(struct qca_ppe_priv *priv)
{
	debugfs_create_file("wifi_endpoint", 0400, priv->debugfs, priv,
			    &ppe_wifi_stats_fops);
	debugfs_create_file("wifi_ingress", 0400, priv->debugfs, priv,
			    &ppe_wifi_ingress_fops);
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
 * sourced from the same MAC, shares one slot — without that, the 2560 nexthops
 * would run out long before the 4096 flow entries do.
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
	if (!--priv->host_ref[index])
		ppe_host_del(priv, index);
}

/* The age of the entry in @index, or -ENOENT if the slot no longer holds this
 * flow. Word 0 on its own is not an identity - its host index, protocol and
 * forwarding type are shared by every flow of one client - so a slot the
 * hardware aged out and handed to a sibling would read back as still ours.
 */
static int ppe_flow_entry_age(struct qca_ppe_priv *priv,
			      struct ppe_flow_entry *entry)
{
	u32 w[PPE_FLOW_ENTRY_WORDS_V6];
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

	if (memcmp(w, entry->words, entry->nwords * sizeof(*w)))
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

/* Private CPU-ingress tags classify WLAN frames without borrowing a
 * physical port identity. Slots persist across idle flows; AP events free them. */
static void ppe_wifi_ingress_release(struct ppe_wifi_ingress *slot)
{
	struct qca_ppe_priv *priv = slot->priv;
	struct net_device *dev = slot->dev;
	int xlt = slot->xlt;

	nf_unregister_net_hook(dev_net(dev), &slot->hook);
	spin_lock_bh(&ppe_wifi_lock);
	slot->dev = NULL;
	spin_unlock_bh(&ppe_wifi_lock);
	/* Invalidate the egress key before clearing its action.  Otherwise a
	 * packet can observe a half-torn-down translation while the XLT index is
	 * being returned to the shared allocator. */
	regmap_write(priv->regmap, PPE_EG_XLT_RULE(xlt), 0);
	regmap_write(priv->regmap, PPE_EG_XLT_RULE_W1(xlt), 0);
	regmap_write(priv->regmap, PPE_EG_XLT_ACTION(xlt), 0);
	regmap_write(priv->regmap, PPE_EG_XLT_ACTION_W1(xlt), 0);
	if (slot->cpu_egress_core)
		ppe_dsa_core_port_put(priv, QCA_PPE_CPU_PORT, false);
	if (slot->ingress_core)
		ppe_dsa_core_port_put(priv, QCA_PPE_CPU_PORT, true);
	ppe_xlt_idx_free(priv, &slot->xlt);
	regmap_write(priv->regmap, PPE_L3_VSI_TBL(slot->vsi), 0);
	ppe_tbl_clear(priv, PPE_IN_L3_IF_TBL(slot->vsi), PPE_L3_IF_WORDS);
	if (ppe_res_put(priv->my_mac, slot->my_mac))
		ppe_tbl_clear(priv, PPE_MY_MAC_TBL(slot->my_mac), PPE_MY_MAC_WORDS);
	ppe_vsi_free(priv, slot->vsi);
	slot->ingress_core = false;
	slot->cpu_egress_core = false;
	dev_put(dev);
}

static struct ppe_wifi_ingress *
ppe_wifi_ingress_get(struct qca_ppe_priv *priv, int ifindex, int priority)
{
	struct ppe_wifi_ingress *slot = NULL;
	struct net_device *dev, *master;
	u32 mac_words[PPE_MY_MAC_WORDS] = {};
	u32 words[3], check[3];
	int i, ret, vsi = -1, xlt = -1, my_mac = -1;

	if (priority == INT_MIN)
		return ERR_PTR(-EOPNOTSUPP);
	dev = dev_get_by_index(&init_net, ifindex);
	if (!dev || !dev->ieee80211_ptr ||
	    dev->ieee80211_ptr->iftype != NL80211_IFTYPE_AP || !netif_running(dev)) {
		if (dev)
			dev_put(dev);
		return ERR_PTR(-EOPNOTSUPP);
	}
	for (i = 0; i < QCA_PPE_WIFI_INGRESS_SLOTS; i++) {
		struct ppe_wifi_ingress *s = &ppe_wifi_ingress[i];

		if (s->dev == dev) {
			dev_put(dev);
			return s->priv == priv && s->hook.priority == priority - 1 &&
			       !READ_ONCE(s->disabled) ? s : ERR_PTR(-EBUSY);
		}
		if (!s->dev && !slot)
			slot = s;
	}
	if (!slot) {
		ret = -ENOSPC;
		goto err_dev;
	}
	rcu_read_lock();
	master = netdev_master_upper_dev_get_rcu(dev);
	ether_addr_copy(slot->mac, master ? master->dev_addr : dev->dev_addr);
	slot->master_ifindex = master ? master->ifindex : 0;
	rcu_read_unlock();
	vsi = ppe_vsi_alloc(priv);
	if (vsi < 0) {
		ret = vsi;
		goto err_dev;
	}
	/* Select an independent VSI per AP. Keep the shared CPU port free of
	 * a per-AP L3 binding that would override private-tag classification. */
	xlt = ppe_routed_xlt_idx_alloc(priv);
	if (xlt < 0) {
		ret = xlt;
		goto err_vsi;
	}
	ppe_entry_set(mac_words, PPE_MY_MAC_ADDR_OFF, PPE_MY_MAC_ADDR_LEN,
		      ether_addr_to_u64(slot->mac));
	ppe_entry_set(mac_words, PPE_MY_MAC_VALID_OFF, PPE_MY_MAC_VALID_LEN, 1);
	my_mac = ppe_res_get(priv->my_mac, PPE_MY_MAC_ENTRIES, mac_words,
			    PPE_MY_MAC_WORDS);
	if (my_mac < 0) {
		ret = my_mac;
		goto err_xlt;
	}
	if (priv->my_mac[my_mac].refcount == 1)
		ppe_tbl_write(priv, PPE_MY_MAC_TBL(my_mac), mac_words, PPE_MY_MAC_WORDS);
	slot->ingress_core = false;
	slot->cpu_egress_core = false;
	ret = ppe_dsa_core_port_get(priv, QCA_PPE_CPU_PORT, true);
	if (ret)
		goto err_mac;
	slot->ingress_core = true;
	ret = ppe_dsa_core_port_get(priv, QCA_PPE_CPU_PORT, false);
	if (ret)
		goto err_mac;
	slot->cpu_egress_core = true;
	ppe_vsi_member_set(priv, vsi, GENMASK(priv->data->num_ports - 1, 0));
	regmap_write(priv->regmap, PPE_IN_L3_IF_TBL(vsi),
		     PPE_L3_IF_IPV4_ROUTE_EN);
	regmap_write(priv->regmap, PPE_IN_L3_IF_TBL(vsi) + 4,
		     FIELD_PREP(PPE_L3_IF_TTL_EXCEED_CMD, PPE_L3_IF_TTL_EXCEED_TO_CPU) |
		     FIELD_PREP(PPE_L3_IF_MAC_BITMAP, GENMASK(7, 0)));
	ppe_l3_if_mtu_set(priv, vsi, dev->mtu + ETH_HLEN);
	regmap_write(priv->regmap, PPE_L3_VSI_TBL(vsi),
		     PPE_L3_VSI_IF_VALID | FIELD_PREP(PPE_L3_VSI_IF_INDEX, vsi));
	slot->vid = QCA_PPE_WIFI_INGRESS_VID_BASE + (slot - ppe_wifi_ingress);
	/* Only CPU ingress with the reserved S-tag reaches this VSI. */
	regmap_write(priv->regmap, PPE_XLT_ACTION_TBL(xlt),
		     FIELD_PREP(PPE_XLT_SVID_CMD, PPE_XLT_VID_DELETE));
	regmap_write(priv->regmap, PPE_XLT_ACTION_W1(xlt),
		     PPE_XLT_VSI_CMD | FIELD_PREP(PPE_XLT_VSI, vsi));
	/* A miss returns to CPU with the AP's private S-tag restored. Hits
	 * use the nexthop's untagged physical output. */
	regmap_write(priv->regmap, PPE_EG_XLT_ACTION(xlt),
		     FIELD_PREP(PPE_EG_XLT_SVID_CMD, PPE_EG_XLT_SVID_ADD) |
		     FIELD_PREP(PPE_EG_XLT_SVID, slot->vid));
	regmap_write(priv->regmap, PPE_EG_XLT_ACTION_W1(xlt), 0);
	regmap_write(priv->regmap, PPE_EG_XLT_RULE(xlt), PPE_EG_XLT_VALID |
		     FIELD_PREP(PPE_EG_XLT_PORT_BMP, BIT(QCA_PPE_CPU_PORT)) |
		     PPE_EG_XLT_VSI_INCL | PPE_EG_XLT_VSI_VALID |
		     FIELD_PREP(PPE_EG_XLT_VSI, vsi) |
		     FIELD_PREP(PPE_EG_XLT_SKEY_FMT, PPE_XLT_FMT_ANY));
	regmap_write(priv->regmap, PPE_EG_XLT_RULE_W1(xlt),
		     FIELD_PREP(PPE_EG_XLT_CKEY_FMT, PPE_XLT_FMT_ANY));
	words[0] = PPE_XLT_VALID | FIELD_PREP(PPE_XLT_PORT_BMP, BIT(QCA_PPE_CPU_PORT)) |
		   FIELD_PREP(PPE_XLT_SKEY_FMT, PPE_XLT_SKEY_TAGGED) |
		   PPE_XLT_SKEY_VID_INCL | FIELD_PREP(PPE_XLT_SKEY_VID, slot->vid) |
		   PPE_XLT_CKEY_FMT_0;
	words[1] = 0; /* Customer tag must be absent. */
	words[2] = 0;
	ret = regmap_bulk_write(priv->regmap, PPE_XLT_RULE_TBL(xlt), words, 3);
	if (ret)
		goto err_mac;
	ret = regmap_bulk_read(priv->regmap, PPE_XLT_RULE_TBL(xlt), check, 3);
	if (ret || memcmp(words, check, sizeof(words))) {
		ret = ret ?: -EIO;
		goto err_mac;
	}
	slot->priv = priv;
	slot->vsi = vsi;
	slot->xlt = xlt;
	slot->my_mac = my_mac;
	atomic_set(&slot->flows, 0);
	atomic64_set(&slot->injected, 0);
	atomic64_set(&slot->injected_bytes, 0);
	WRITE_ONCE(slot->disabled, false);
	slot->hook = (struct nf_hook_ops) {
		.hook = ppe_wifi_ingress_hook, .pf = NFPROTO_NETDEV,
		.hooknum = NF_NETDEV_INGRESS, .priority = priority - 1,
		.dev = dev, .priv = slot,
	};
	ret = nf_register_net_hook(dev_net(dev), &slot->hook);
	if (ret)
		goto err_mac;
	spin_lock_bh(&ppe_wifi_lock);
	slot->dev = dev;
	spin_unlock_bh(&ppe_wifi_lock);
	return slot;
err_mac:
	if (slot->cpu_egress_core)
		ppe_dsa_core_port_put(priv, QCA_PPE_CPU_PORT, false);
	if (slot->ingress_core)
		ppe_dsa_core_port_put(priv, QCA_PPE_CPU_PORT, true);
	regmap_write(priv->regmap, PPE_EG_XLT_RULE(xlt), 0);
	regmap_write(priv->regmap, PPE_EG_XLT_RULE_W1(xlt), 0);
	regmap_write(priv->regmap, PPE_EG_XLT_ACTION(xlt), 0);
	regmap_write(priv->regmap, PPE_EG_XLT_ACTION_W1(xlt), 0);
	regmap_write(priv->regmap, PPE_L3_VSI_TBL(vsi), 0);
	ppe_tbl_clear(priv, PPE_IN_L3_IF_TBL(vsi), PPE_L3_IF_WORDS);
	if (ppe_res_put(priv->my_mac, my_mac))
		ppe_tbl_clear(priv, PPE_MY_MAC_TBL(my_mac), PPE_MY_MAC_WORDS);
err_xlt:
	ppe_xlt_idx_free(priv, &xlt);
err_vsi:
	ppe_vsi_free(priv, vsi);
err_dev:
	dev_put(dev);
	return ERR_PTR(ret);
}

static void ppe_wifi_ingress_unbind(struct ppe_flow_entry *entry)
{
	if (!entry->wifi_ingress || !entry->wifi_bound)
		return;
	spin_lock_bh(&ppe_wifi_lock);
	hash_del(&entry->wifi_node);
	entry->wifi_bound = false;
	atomic_dec(&entry->wifi_ingress->flows);
	spin_unlock_bh(&ppe_wifi_lock);
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

static void ppe_dsa_service_egress_clear(struct qca_ppe_priv *priv, int xlt)
{
	/* Invalidate the key before clearing its action, as a miss must never see
	 * an incomplete restore operation during service teardown.
	 */
	regmap_write(priv->regmap, PPE_EG_XLT_RULE(xlt), 0);
	regmap_write(priv->regmap, PPE_EG_XLT_RULE_W1(xlt), 0);
	regmap_write(priv->regmap, PPE_EG_XLT_ACTION(xlt), 0);
	regmap_write(priv->regmap, PPE_EG_XLT_ACTION_W1(xlt), 0);
}

static int ppe_dsa_core_port_get(struct qca_ppe_priv *priv, int port,
				  bool ingress)
{
	u16 *refs;
	int ret;

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
				   PPE_PORT_EG_VLAN_CORE, 0);
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
	ppe_dsa_service_egress_clear(priv, xlt);
	ppe_xlt_idx_free(priv, &xlt);
	ppe_vsi_free(priv, service->vsi);
	ppe_dsa_core_port_put(priv, QCA_PPE_CPU_PORT, false);
	ppe_dsa_core_port_put(priv, port, true);
	memset(service, 0, sizeof(*service));
	service->port = -1;
	service->vsi = -1;
	service->xlt = -1;
}

/* Make a tagged PPPoE WAN port route its ingress traffic in hardware, so the
 * download direction of an offloaded connection reaches the flow lookup on its
 * inner tuple.
 *
 * A dedicated VSI keeps the uplink out of any L2 domain: the ingress VLAN is
 * classified into it with the 802.1Q tag stripped, the VSI carries a route-
 * and PPPoE-terminating L3 interface holding the router's MAC, and the PPPoE
 * session id is recognised so the header is parsed through to the inner IP.
 * Everything is shared by every flow on the port and torn down with the last
 * of them. A frame that misses the flow table is still forwarded to the CPU,
 * with the tag the hardware stripped re-added on that path, so the software
 * PPPoE stack sees on-wire frames throughout.
 */
static int ppe_wan_ingress_get(struct qca_ppe_priv *priv, int port, u16 sid,
			       bool vlan_valid, u16 vlan_id, const u8 *mac,
			       u32 mtu)
{
	u32 words[PPE_MY_MAC_WORDS] = {};
	int vsi, xlt = -1, ret;

	if (priv->wan_ref[port]++) {
		/* A re-dialled session has a new id, and an ingress still
		 * keyed to the dead one silently un-offloads every download on
		 * this uplink: the entries stay valid and correct, the frame
		 * is never parsed through to the tuple they key on, and the
		 * flow lookup is never reached. It is cheaper to write the
		 * session on every install than to keep a copy of it that
		 * could be wrong.
		 */
		regmap_write(priv->regmap, PPE_PPPOE_SESSION(port),
			     FIELD_PREP(PPE_PPPOE_SESSION_ID, sid) |
			     FIELD_PREP(PPE_PPPOE_SESSION_PORT_BMP, BIT(port)) |
			     FIELD_PREP(PPE_PPPOE_SESSION_L3_IF,
					priv->wan_vsi[port]));
		return 0;
	}

	/* The translation rule shares one table with the bridge VLANs, so its
	 * index comes from the allocator they share.
	 */
	if (vlan_valid) {
		xlt = ppe_routed_xlt_idx_alloc(priv);
		if (xlt < 0) {
			priv->wan_ref[port]--;
			return xlt;
		}
	}

	vsi = ppe_vsi_alloc(priv);
	if (vsi < 0) {
		ret = vsi;
		goto err_xlt;
	}
	priv->wan_vsi[port] = vsi;
	priv->wan_vid[port] = vlan_valid ? vlan_id : 0;
	ppe_vsi_member_set(priv, vsi, BIT(port) | BIT(QCA_PPE_CPU_PORT));

	ppe_entry_set(words, PPE_MY_MAC_ADDR_OFF, PPE_MY_MAC_ADDR_LEN,
		      ether_addr_to_u64(mac));
	ppe_entry_set(words, PPE_MY_MAC_VALID_OFF, PPE_MY_MAC_VALID_LEN, 1);
	ret = ppe_res_get(priv->my_mac, PPE_MY_MAC_ENTRIES, words,
			  PPE_MY_MAC_WORDS);
	if (ret < 0)
		goto err_vsi;
	priv->wan_mymac[port] = ret;
	if (priv->my_mac[ret].refcount == 1)
		ppe_tbl_write(priv, PPE_MY_MAC_TBL(ret), words, PPE_MY_MAC_WORDS);

	regmap_write(priv->regmap, PPE_IN_L3_IF_TBL(vsi),
		     PPE_L3_IF_IPV4_ROUTE_EN | PPE_L3_IF_IPV6_ROUTE_EN);
	regmap_write(priv->regmap, PPE_IN_L3_IF_TBL(vsi) + 4,
		     FIELD_PREP(PPE_L3_IF_TTL_EXCEED_CMD,
				PPE_L3_IF_TTL_EXCEED_TO_CPU) |
		     PPE_L3_IF_TTL_EXCEED_DEACCEL |
		     FIELD_PREP(PPE_L3_IF_MAC_BITMAP, GENMASK(7, 0)) |
		     PPE_L3_IF_PPPOE_EN);
	ppe_l3_if_mtu_set(priv, vsi, mtu);
	regmap_write(priv->regmap, PPE_L3_VSI_TBL(vsi),
		     PPE_L3_VSI_IF_VALID | FIELD_PREP(PPE_L3_VSI_IF_INDEX, vsi));

	regmap_write(priv->regmap, PPE_PPPOE_SESSION(port),
		     FIELD_PREP(PPE_PPPOE_SESSION_ID, sid) |
		     FIELD_PREP(PPE_PPPOE_SESSION_PORT_BMP, BIT(port)) |
		     FIELD_PREP(PPE_PPPOE_SESSION_L3_IF, vsi));
	regmap_write(priv->regmap, PPE_PPPOE_SESSION_EXT(port),
		     PPE_PPPOE_EXT_L3_IF_VALID | PPE_PPPOE_EXT_UC_VALID);

	if (vlan_valid) {
		priv->wan_xlt[port] = xlt;

		/* The L3 stage does not parse through a residual 802.1Q tag:
		 * the PPPoE session is only recognised, and the inner tuple
		 * only reaches the flow lookup, once the rule also strips the
		 * tag. The hardware re-adds it toward the CPU (below), so a
		 * frame that misses the flow table still reaches the software
		 * PPPoE stack in its on-wire form. Action and re-tag are in
		 * place before the rule goes live.
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
			     FIELD_PREP(PPE_EG_XLT_CVID, vlan_id));
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
			     FIELD_PREP(PPE_XLT_CKEY_VID, vlan_id));
		regmap_write(priv->regmap, PPE_XLT_RULE_TBL(xlt) + 8, 0);
	}

	return 1;

err_vsi:
	ppe_vsi_free(priv, vsi);
	priv->wan_vsi[port] = -1;
err_xlt:
	if (xlt >= 0)
		ppe_xlt_idx_free(priv, &xlt);
	priv->wan_ref[port]--;
	return ret;
}

static void ppe_wan_ingress_put(struct qca_ppe_priv *priv, int port)
{
	int xlt = priv->wan_xlt[port];
	u32 vsi = priv->wan_vsi[port];

	if (--priv->wan_ref[port])
		return;

	if (xlt >= 0) {
		/* Ingress first: the egress rule is what puts the tag back on
		 * a frame the ingress rule stripped, so taking it down first
		 * would surface stripped frames on the port device. Within the
		 * ingress rule the allocator clears the three key words before
		 * the action's, since a key left live over a zeroed action
		 * blackholes every frame it matches.
		 */
		ppe_xlt_idx_free(priv, &priv->wan_xlt[port]);
		regmap_write(priv->regmap, PPE_EG_XLT_RULE(xlt), 0);
		regmap_write(priv->regmap, PPE_EG_XLT_RULE_W1(xlt), 0);
		regmap_write(priv->regmap, PPE_EG_XLT_ACTION(xlt), 0);
		regmap_write(priv->regmap, PPE_EG_XLT_ACTION_W1(xlt), 0);
	}
	regmap_write(priv->regmap, PPE_PPPOE_SESSION(port), 0);
	regmap_write(priv->regmap, PPE_PPPOE_SESSION_EXT(port), 0);
	regmap_write(priv->regmap, PPE_L3_VSI_TBL(vsi), 0);
	ppe_tbl_clear(priv, PPE_IN_L3_IF_TBL(vsi), PPE_L3_IF_WORDS);
	if (ppe_res_put(priv->my_mac, priv->wan_mymac[port]))
		ppe_tbl_clear(priv, PPE_MY_MAC_TBL(priv->wan_mymac[port]),
			      PPE_MY_MAC_WORDS);
	ppe_vsi_free(priv, vsi);
	priv->wan_vsi[port] = -1;
}

/* The VSI the ingress classification puts this rule's packets in - the routing
 * domain the flow belongs to, and the one ingress identifier its hardware entry
 * can carry. A domain that cannot be named is declined rather than encoded as
 * the bare tuple, which would let the flow forward traffic from another VLAN.
 */
/* A port's PVID only classifies while the bridge enforces its VLANs; until
 * then the bridge VLANs are recorded and name no port.
 */
static u16 ppe_port_pvid(struct qca_ppe_priv *priv, int port)
{
	return priv->vlan_filtering & BIT(port) ? priv->port_pvid[port] : 0;
}

static int ppe_flow_ingress_vsi(struct qca_ppe_priv *priv, int iport, u16 vid)
{
	struct qca_ppe_vlan_entry *vlan;

	lockdep_assert_held(&priv->vlan_lock);

	/* A PPPoE uplink is classified into a VSI of its own, and only the
	 * VLAN and session id that classification names reach it.
	 */
	if (priv->wan_ref[iport])
		return vid && vid != priv->wan_vid[iport] ? -EOPNOTSUPP :
							    priv->wan_vsi[iport];

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
	u32 words[PPE_NEXTHOP_WORDS] = {};
	struct net_device *l3dev;
	u32 mtu;
	int vsi, ret;

	lockdep_assert_held(&priv->vlan_lock);

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
		      priv->wan_ref[iport] ? priv->wan_vid[iport] :
		      vid ? vid : ppe_port_pvid(priv, iport);

	/* A PPPoE uplink's ingress interface belongs to the uplink, so take a
	 * reference rather than programming anything: a sibling flow going away
	 * must not pull the classification out from under this one.
	 */
	if (priv->wan_ref[iport]) {
		priv->wan_ref[iport]++;
		entry->wan_iport = iport;
		return 0;
	}

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
		     PPE_L3_IF_TTL_EXCEED_DEACCEL |
		     FIELD_PREP(PPE_L3_IF_MAC_BITMAP, GENMASK(7, 0)));
	ppe_l3_if_mtu_set(priv, vsi, mtu);
	regmap_write(priv->regmap, PPE_L3_VSI_TBL(vsi),
		     PPE_L3_VSI_IF_VALID | FIELD_PREP(PPE_L3_VSI_IF_INDEX, vsi));

	return 0;
}

static void ppe_flow_entry_destroy(struct qca_ppe_priv *priv,
				   struct ppe_flow_entry *entry);

static void ppe_flow_drop(struct qca_ppe_priv *priv,
			  struct ppe_flow_entry *entry)
{
	priv->flow_stale++;
	rhashtable_remove_fast(&priv->flow_table, &entry->node,
			       ppe_flow_ht_params);
	list_del(&entry->list);
	ppe_flow_entry_destroy(priv, entry);
	kfree(entry);
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

	if (priv->wan_ref[port])
		ppe_l3_if_mtu_set(priv, priv->wan_vsi[port],
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
		    (!entry->wifi_egress && entry->oport == port))
			ppe_flow_drop(priv, entry);
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

	if (event != NETDEV_CHANGEMTU && event != NETDEV_CHANGEADDR &&
	    event != NETDEV_UNREGISTER && event != NETDEV_DOWN &&
	    event != NETDEV_CHANGEUPPER)
		return NOTIFY_DONE;

	guard(mutex)(&priv->flow_lock);
	guard(mutex)(&priv->vlan_lock);

	/* WLAN is an egress endpoint rather than a DSA port.  Remove its
	 * bindings before the netdev is released; software flowtable processing
	 * will continue when no PPE entry can be rebuilt. */
	list_for_each_entry_safe(entry, tmp, &priv->flow_list, list)
		if ((entry->wifi_egress && entry->wifi_dev == dev) ||
		    (entry->wifi_ingress && (entry->wifi_ingress->dev == dev ||
		     entry->wifi_ingress->master_ifindex == dev->ifindex)))
			ppe_flow_drop(priv, entry);

	for (i = 0; i < QCA_PPE_WIFI_INGRESS_SLOTS; i++)
		if (ppe_wifi_ingress[i].dev && ppe_wifi_ingress[i].priv == priv &&
		    (ppe_wifi_ingress[i].dev == dev ||
		     ppe_wifi_ingress[i].master_ifindex == dev->ifindex))
			ppe_wifi_ingress_release(&ppe_wifi_ingress[i]);

	if (event == NETDEV_UNREGISTER || event == NETDEV_DOWN ||
	    event == NETDEV_CHANGEUPPER)
		return NOTIFY_DONE;

	/* Reached through the netdev itself rather than by walking the switch:
	 * this notifier is live before the switch is registered, and every
	 * netdev in the system passes through it.
	 */
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

	if (entry->wifi_ingress) {
		ppe_wifi_ingress_unbind(entry);
		return;
	}
	if (entry->wan_iport >= 0) {
		ppe_wan_ingress_put(priv, entry->wan_iport);
		return;
	}

	if (entry->l3_if >= 0 && !--priv->l3_if_ref[entry->l3_if]) {
		regmap_write(priv->regmap, PPE_L3_VSI_TBL(entry->l3_if), 0);
		ppe_tbl_clear(priv, PPE_IN_L3_IF_TBL(entry->l3_if),
			      PPE_L3_IF_WORDS);
	}

	if (ppe_res_put(priv->my_mac, entry->my_mac))
		ppe_tbl_clear(priv, PPE_MY_MAC_TBL(entry->my_mac),
			      PPE_MY_MAC_WORDS);
	if (entry->dsa_service >= 0)
		ppe_dsa_service_put(priv, entry->dsa_service);
}

static void ppe_flow_wifi_unbind(struct qca_ppe_priv *priv,
				 struct ppe_flow_entry *entry)
{
	if (!entry->wifi_dev)
		return;

	if (entry->wifi_bound) {
		spin_lock_bh(&ppe_wifi_lock);
		hash_del(&entry->wifi_node);
		entry->wifi_bound = false;
		spin_unlock_bh(&ppe_wifi_lock);
	}
	dev_put(entry->wifi_dev);
	entry->wifi_dev = NULL;
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
		if (entry->src_if != vsi)
			continue;

		ppe_flow_drop(priv, entry);
	}
}

/* A flow that ingresses on this port and was installed before the uplink had a
 * classification named the port's plain VSI, and can never match now that one
 * exists. Drop those entries: the flowtable reinstalls whatever is still live.
 */
static void ppe_flow_purge_ingress(struct qca_ppe_priv *priv, int iport,
				   u8 wan_vsi)
{
	struct ppe_flow_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &priv->flow_list, list) {
		if (entry->iport != iport || entry->src_if == wan_vsi)
			continue;

		ppe_flow_drop(priv, entry);
	}
}

/* Build the egress L3 interface, nexthop and public-address entries this rule
 * needs, and program any that were not already in use by another flow.
 */
static int ppe_flow_alloc_egress(struct qca_ppe_priv *priv,
				 struct ppe_flow_data *data, bool snat,
				 bool dnat, int iport,
				 struct ppe_flow_entry *entry)
{
	u32 words[PPE_NEXTHOP_WORDS] = {};
	struct dsa_port *odp;
	u32 eg_mtu;
	u64 mac;
	bool wifi = data->odev && data->odev->ieee80211_ptr;
	int port, ret;

	if (wifi) {
		atomic64_inc(&ppe_wifi_candidates);
		if (!net_eq(dev_net(data->odev), &init_net) ||
		    data->addr_type != FLOW_DISSECTOR_KEY_IPV4_ADDRS ||
		    (data->l4proto != IPPROTO_TCP && data->l4proto != IPPROTO_UDP) ||
		    data->vlan_valid || data->pppoe_valid ||
		    !netif_running(data->odev))
			return -EOPNOTSUPP;
		/* The native PPE has no WLAN virtual port.  Keep the route/NAT rule
		 * in hardware and send its result through CPU port 0; EDMA performs
		 * the final handoff by the exact post-edit flow identity. */
		port = QCA_PPE_CPU_PORT;
		entry->wifi_egress = true;
		entry->oport = QCA_PPE_CPU_PORT;
		entry->ovid = 0;
		entry->wifi_dev = data->odev;
		entry->wifi_key.src = data->v4_src_new;
		entry->wifi_key.dst = data->v4_dst_new;
		entry->wifi_key.sport = data->sport_new;
		entry->wifi_key.dport = data->dport_new;
		entry->wifi_key.proto = data->l4proto;
		entry->wifi_key.iport = iport;
		ether_addr_copy(entry->wifi_key.dmac, data->eth.h_dest);
		ether_addr_copy(entry->wifi_key.smac, data->eth.h_source);
		dev_hold(entry->wifi_dev);
	} else {
		port = ppe_flow_port_by_ifindex(priv, data->odev->ifindex,
						&data->egress_svid, NULL, true);
		if (port < 0)
			return port;
		data->egress_svid_valid = !!data->egress_svid;

		/* A frame sent back out the port it arrived on is discarded by source
		 * port filtering, so offloading it would black-hole what the CPU would
		 * otherwise have forwarded.
		 */
		if (port == iport)
			return -EBUSY;
		entry->oport = port;
		/* An untagged egress is credited to the port's PVID VLAN, the one an
		 * untagged frame on that port belongs to.
		 */
		entry->ovid = data->vlan_valid ? data->vlan_id :
			      ppe_port_pvid(priv, port);
	}

	mac = ether_addr_to_u64(data->eth.h_source);
	ppe_entry_set(words, PPE_EG_L3_IF_MAC_OFF, PPE_EG_L3_IF_MAC_LEN, mac);
	if (data->pppoe_valid) {
		ppe_entry_set(words, PPE_EG_L3_IF_SESSION_OFF,
			      PPE_EG_L3_IF_SESSION_LEN, data->pppoe_sid);
		ppe_entry_set(words, PPE_EG_L3_IF_PPPOE_EN_OFF,
			      PPE_EG_L3_IF_PPPOE_EN_LEN, 1);
	}
	/* The size a routed frame leaves at, which the hardware compares before
	 * it egresses and sends the frame to the CPU when it does not fit. The
	 * port's mtu carries one MAC header and every VLAN tag the nexthop pushes;
	 * a PPPoE session header rides inside that mtu rather than on top of it.
	 * It joins the key because two interfaces sharing a source address need
	 * separate entries when they do not share a size.
	 */
	if (wifi) {
		eg_mtu = data->odev->mtu + ETH_HLEN +
			 data->vlan_valid * VLAN_HLEN;
	} else {
		odp = dsa_to_port(&priv->ds, port);
		eg_mtu = odp->user->mtu + ETH_HLEN +
			 data->vlan_valid * VLAN_HLEN +
			 data->egress_svid_valid * VLAN_HLEN;
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
	if (ret < 0) {
		if (wifi)
			ppe_flow_wifi_unbind(priv, entry);
		return ret;
	}
	entry->eg_l3_if = ret + PPE_VSI_MAX;
	if (priv->eg_l3_if[entry->eg_l3_if].refcount == 1) {
		ppe_tbl_write(priv, PPE_EG_L3_IF_TBL(entry->eg_l3_if), words,
			      PPE_EG_L3_IF_WORDS);
		ppe_eg_l3_if_mtu_set(priv, entry->eg_l3_if, eg_mtu);
	}

	if (snat) {
		u32 pub = ntohl(data->v4_src_new);

		ret = ppe_res_get(priv->pub_ip, PPE_PUB_IP_ENTRIES, &pub, 1);
		if (ret < 0)
			goto err_eg_l3_if;
		entry->pub_ip = ret;
		if (priv->pub_ip[ret].refcount == 1)
			regmap_write(priv->regmap, PPE_PUB_IP_TBL(ret), pub);
	}

	memset(words, 0, sizeof(words));
	/* Type selects how the port field is read: as a physical port (1) or,
	 * left at 0, as a VSI whose FDB resolves the port. The egress port is
	 * known exactly here, so name it directly; left at 0 the port number
	 * would be read as a VSI.
	 */
	ppe_entry_set(words, PPE_NEXTHOP_TYPE_OFF, PPE_NEXTHOP_TYPE_LEN,
		      PPE_NEXTHOP_TYPE_PORT);
	ppe_entry_set(words, PPE_NEXTHOP_PORT_OFF, PPE_NEXTHOP_PORT_LEN, port);
	ppe_entry_set(words, PPE_NEXTHOP_POST_L3_IF_OFF,
		      PPE_NEXTHOP_POST_L3_IF_LEN, entry->eg_l3_if);
	if (data->egress_svid_valid) {
		ppe_entry_set(words, PPE_NEXTHOP_STAG_FMT_OFF,
			      PPE_NEXTHOP_STAG_FMT_LEN, 1);
		ppe_entry_set(words, PPE_NEXTHOP_SVID_OFF,
			      PPE_NEXTHOP_SVID_LEN, data->egress_svid);
	}
	if (data->vlan_valid) {
		ppe_entry_set(words, PPE_NEXTHOP_CTAG_FMT_OFF,
			      PPE_NEXTHOP_CTAG_FMT_LEN, 1);
		ppe_entry_set(words, PPE_NEXTHOP_CVID_OFF,
			      PPE_NEXTHOP_CVID_LEN, data->vlan_id);
	}
	if (snat)
		ppe_entry_set(words, PPE_NEXTHOP_PUB_IP_IDX_OFF,
			      PPE_NEXTHOP_PUB_IP_IDX_LEN, entry->pub_ip);
	ppe_entry_set(words, PPE_NEXTHOP_MAC_OFF, PPE_NEXTHOP_MAC_LEN,
		      ether_addr_to_u64(data->eth.h_dest));
	if (dnat)
		ppe_entry_set(words, PPE_NEXTHOP_DNAT_IP_OFF,
			      PPE_NEXTHOP_DNAT_IP_LEN, ntohl(data->v4_dst_new));

	ret = ppe_res_get(priv->nexthop, priv->data->num_nexthop_entries, words,
			  PPE_NEXTHOP_WORDS);
	if (ret < 0)
		goto err_pub_ip;
	entry->nexthop = ret;
	if (priv->nexthop[ret].refcount == 1)
		ppe_tbl_write(priv, PPE_IN_NEXTHOP_TBL(ret), words,
			      PPE_NEXTHOP_WORDS);

	/* The reverse of a flow that egresses PPPoE arrives PPPoE-encapsulated
	 * on this same port; set the port up to route it so that direction
	 * offloads too.
	 */
	if (data->pppoe_valid && !wifi) {
		struct dsa_port *odp = dsa_to_port(&priv->ds, port);
		u8 wan_vsi;

		ret = ppe_wan_ingress_get(priv, port, data->pppoe_sid,
					  data->vlan_valid, data->vlan_id,
					  odp->user->dev_addr,
					  odp->user->mtu + VLAN_ETH_HLEN +
					  PPPOE_SES_HLEN);
		wan_vsi = priv->wan_vsi[port];
		if (ret < 0)
			goto err_nexthop;
		entry->wan_port = port;

		if (ret == 1)
			ppe_flow_purge_ingress(priv, port, wan_vsi);
	}

	if (data->egress_svid_valid && !wifi) {
		ret = ppe_dsa_core_port_get(priv, port, false);
		if (ret) {
			if (entry->wan_port >= 0) {
				ppe_wan_ingress_put(priv, entry->wan_port);
				entry->wan_port = -1;
			}
			goto err_nexthop;
		}
		entry->dsa_egress_port = port;
	}

	if (wifi) {
		struct ppe_flow_entry *other;
		u32 hash = ppe_wifi_hash(&entry->wifi_key);

		spin_lock_bh(&ppe_wifi_lock);
		hash_for_each_possible(ppe_wifi_flows, other, wifi_node, hash) {
			if (!memcmp(&other->wifi_key, &entry->wifi_key,
				    sizeof(entry->wifi_key))) {
				spin_unlock_bh(&ppe_wifi_lock);
				ret = -EEXIST;
				goto err_nexthop;
			}
		}
		hash_add(ppe_wifi_flows, &entry->wifi_node, hash);
		entry->wifi_bound = true;
		spin_unlock_bh(&ppe_wifi_lock);
	}

	return 0;

err_nexthop:
	if (ppe_res_put(priv->nexthop, entry->nexthop))
		ppe_tbl_clear(priv, PPE_IN_NEXTHOP_TBL(entry->nexthop),
			      PPE_NEXTHOP_WORDS);
err_pub_ip:
	if (ppe_res_put(priv->pub_ip, entry->pub_ip))
		regmap_write(priv->regmap, PPE_PUB_IP_TBL(entry->pub_ip), 0);
err_eg_l3_if:
	if (wifi)
		ppe_flow_wifi_unbind(priv, entry);
	if (ppe_res_put(priv->eg_l3_if, entry->eg_l3_if)) {
		ppe_tbl_clear(priv, PPE_EG_L3_IF_TBL(entry->eg_l3_if),
			      PPE_EG_L3_IF_WORDS);
		ppe_tbl_clear(priv, PPE_IN_L3_IF_TBL(entry->eg_l3_if),
			      PPE_L3_IF_WORDS);
	}

	return ret;
}

static void ppe_flow_free_egress(struct qca_ppe_priv *priv,
				 struct ppe_flow_entry *entry)
{
	ppe_flow_wifi_unbind(priv, entry);

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

	if (entry->wan_port >= 0)
		ppe_wan_ingress_put(priv, entry->wan_port);
	if (entry->dsa_egress_port >= 0)
		ppe_dsa_core_port_put(priv, entry->dsa_egress_port, false);
}

/* The counters are cumulative and narrower than u64 - 32-bit packets, 40-bit
 * bytes - so the deltas are computed in the counters' own widths to survive
 * wraparound.
 */
static u32 ppe_flow_counter_delta(struct qca_ppe_priv *priv,
				  struct ppe_flow_entry *entry, u64 *bytes)
{
	u64 packets, total;
	u32 pkts;

	ppe_flow_counter_read(priv, entry->index, &packets, &total);
	pkts = (u32)(packets - entry->packets);
	*bytes = (total - entry->bytes) & PPE_FLOW_CNT_BYTES;
	entry->packets = packets;
	entry->bytes = total;

	return pkts;
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
				  u16 vid, bool rx, u32 pkts, u64 bytes)
{
	struct net_device *dev = priv->port_br_dev[port];

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

	if (!pkts)
		return;

	entry->unread_packets += pkts;
	entry->unread_bytes += bytes;

	rcu_read_lock();
	if (!entry->wifi_ingress)
		ppe_flow_account_side(priv, entry->iport, entry->ivid, true, pkts, bytes);
	/* WLAN TX counts its own dev_queue_xmit traffic. */
	if (!entry->wifi_egress)
		ppe_flow_account_side(priv, entry->oport, entry->ovid, false, pkts,
				      bytes);
	rcu_read_unlock();
}

/* Both locks: the release below reaches the VLAN side, and a routing domain
 * going away destroys flows with that lock already held, so taking it here
 * would be the same task asking for it twice.
 */
static void ppe_flow_entry_destroy(struct qca_ppe_priv *priv,
				   struct ppe_flow_entry *entry)
{
	u64 bytes;
	int age;

	lockdep_assert_held(&priv->flow_lock);
	lockdep_assert_held(&priv->vlan_lock);

	ppe_wifi_ingress_unbind(entry);

	/* Delete by index - the key does not have to be restaged. Only a read
	 * that says the slot belongs to another flow is a reason not to: the
	 * hardware removes an idle entry on its own and may have handed the
	 * slot on, but a read that failed leaves it unknown, and leaving a live
	 * entry pointing at side tables about to be reused is the worse half of
	 * that trade.
	 */
	age = ppe_flow_entry_age(priv, entry);
	if (age >= 0) {
		u32 pkts = ppe_flow_counter_delta(priv, entry, &bytes);

		ppe_flow_account(priv, entry, pkts, bytes);
	}
	if (age != -ENOENT)
		ppe_flow_entry_delete(priv, entry->index);
	ppe_host_ref_put(priv, entry->host_index);
	ppe_flow_free_egress(priv, entry);
	ppe_flow_free_ingress(priv, entry);
}

/* Encode the flow entry and its host half. The key is always the tuple as the
 * packet arrives: the destination address in the flow entry, the source address
 * in the host entry.
 */
static void ppe_flow_encode(struct ppe_flow_data *data, bool v6, bool snat,
				    bool dnat, bool cpu_port, u32 nexthop,
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

	/* A NAT flow must keep its NAT forwarding type to apply the translation.
	 * Its nexthop supplies CPU port 0; only a plain route needs the direct
	 * port override used by QSDK's RFS route rules. */
	fwd = snat ? PPE_FLOW_FWD_SNAT : dnat ? PPE_FLOW_FWD_DNAT :
						PPE_FLOW_FWD_ROUTE;
	ppe_entry_set(fw, PPE_FLOW_E_FWD_TYPE_OFF, PPE_FLOW_E_FWD_TYPE_LEN, fwd);
	ppe_entry_set(fw, PPE_FLOW_E_NEXTHOP_OFF, PPE_FLOW_E_NEXTHOP_LEN,
		      nexthop);
	if (cpu_port && !snat && !dnat) {
		ppe_entry_set(fw, PPE_FLOW_E_PORT_VALID_OFF,
			      PPE_FLOW_E_PORT_VALID_LEN, 1);
		ppe_entry_set(fw, PPE_FLOW_E_PORT_OFF, PPE_FLOW_E_PORT_LEN,
			      QCA_PPE_CPU_PORT);
	} else if (snat)
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
	bool snat, dnat, v6, wifi_ingress = false;
	int i, ret, nfw, nhw, iport, ingress_ifindex;

	guard(mutex)(&priv->flow_lock);
	guard(mutex)(&priv->vlan_lock);

	/* A replace for a cookie already held is the kernel refreshing a flow
	 * whose packets it saw in the software path. If the hardware entry is
	 * intact the refresh is a no-op; if the hardware has lost it - however
	 * that happened - this is the moment to reinstall, or the flow would
	 * stay in software for the rest of its life.
	 */
	entry = rhashtable_lookup_fast(&priv->flow_table, &f->cookie,
				       ppe_flow_ht_params);
	if (entry) {
		if (ppe_flow_entry_age(priv, entry) >= 0)
			return 0;

		priv->flow_reinstalled++;
		rhashtable_remove_fast(&priv->flow_table, &entry->node,
				       ppe_flow_ht_params);
		list_del(&entry->list);
		ppe_flow_entry_destroy(priv, entry);
		kfree(entry);
	}

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC))
		return ppe_flow_reject(priv, rule, PPE_REJECT_KEY);

	{
		struct flow_match_meta match;

		flow_rule_match_meta(rule, &match);
		ingress_ifindex = match.key->ingress_ifindex;
		iport = ppe_flow_port_by_ifindex(priv, ingress_ifindex,
					 &data.ingress_svid, data.ingress_mac, false);
		if (iport < 0) {
			struct net_device *idev = dev_get_by_index(&init_net, ingress_ifindex);

			wifi_ingress = idev &&
				idev->ieee80211_ptr &&
				idev->ieee80211_ptr->iftype == NL80211_IFTYPE_AP;
			if (idev)
				dev_put(idev);
			if (!wifi_ingress)
				return ppe_flow_reject(priv, rule, PPE_REJECT_INGRESS_PORT);
			iport = QCA_PPE_CPU_PORT;
		}
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

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			if (act->mangle.htype == FLOW_ACT_MANGLE_HDR_TYPE_ETH)
				ret = ppe_flow_mangle_eth(act, &data.eth);
			else
				ret = 0;
			if (ret)
				return ppe_flow_reject(priv, rule, PPE_REJECT_ACTION);
			break;
		case FLOW_ACTION_REDIRECT:
			data.odev = act->dev;
			break;
		case FLOW_ACTION_CSUM:
			break;
		case FLOW_ACTION_VLAN_PUSH:
			if (data.vlan_valid ||
			    act->vlan.proto != htons(ETH_P_8021Q))
				return ppe_flow_reject(priv, rule, PPE_REJECT_ACTION);
			data.vlan_id = act->vlan.vid;
			data.vlan_valid = true;
			break;
		case FLOW_ACTION_VLAN_POP:
			/* Routed egress rebuilds the L2 header from the
			 * nexthop, which sheds the ingress encapsulation on
			 * its own.
			 */
			break;
		case FLOW_ACTION_PRIORITY:
			if (act->priority > PPE_QOS_MAX_PRI)
				return ppe_flow_reject(priv, rule, PPE_REJECT_ACTION);
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

	flow_action_for_each(i, act, &rule->action) {
		if (act->id != FLOW_ACTION_MANGLE)
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

	if (wifi_ingress) {
		if (v6 || data.ivid || data.vlan_valid || data.pppoe_valid ||
		    data.odev->ieee80211_ptr ||
		    (data.l4proto != IPPROTO_TCP && data.l4proto != IPPROTO_UDP))
			return ppe_flow_reject(priv, rule, PPE_REJECT_INGRESS_PORT);
		data.wifi_ingress = ppe_wifi_ingress_get(priv, ingress_ifindex, f->common.prio);
		if (IS_ERR(data.wifi_ingress))
			return ppe_flow_reject(priv, rule, PPE_REJECT_INGRESS_PORT);
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->cookie = f->cookie;
	entry->block = fb->block;
	entry->nexthop = -1;
	entry->eg_l3_if = -1;
	entry->pub_ip = -1;
	entry->my_mac = -1;
	entry->l3_if = -1;
	entry->wan_port = -1;
	entry->wan_iport = -1;
	entry->dsa_service = -1;
	entry->dsa_egress_port = -1;
	entry->iport = iport;
	INIT_HLIST_NODE(&entry->wifi_node);
	entry->wifi_ingress = data.wifi_ingress;

	if (entry->wifi_ingress) {
		entry->src_if = entry->wifi_ingress->vsi;
		entry->wifi_key.src = data.v4_src;
		entry->wifi_key.dst = data.v4_dst;
		entry->wifi_key.sport = data.sport;
		entry->wifi_key.dport = data.dport;
		entry->wifi_key.proto = data.l4proto;
		entry->wifi_key.iport = entry->wifi_ingress - ppe_wifi_ingress;
		ret = 0;
	} else {
		ret = ppe_flow_alloc_ingress(priv, iport, data.ivid,
					     data.ingress_svid_valid,
					     data.ingress_mac_valid ? data.ingress_mac : NULL,
					     entry);
	}
	if (ret) {
		ppe_flow_reject(priv, rule, ret == -EOPNOTSUPP ?
				PPE_REJECT_INGRESS_VLAN : PPE_REJECT_RESOURCE);
		goto err_free;
	}

	ret = ppe_flow_alloc_egress(priv, &data, snat, dnat, iport, entry);
	if (ret) {
		ppe_flow_reject(priv, rule, ret == -ENOSPC ? PPE_REJECT_RESOURCE :
				ret == -EBUSY ? PPE_REJECT_HAIRPIN :
				PPE_REJECT_EGRESS_PORT);
		goto err_ingress;
	}

	if (entry->wifi_ingress) {
		struct ppe_flow_entry *other;
		bool duplicate = false;

		spin_lock_bh(&ppe_wifi_lock);
		hash_for_each_possible(ppe_wifi_ingress_flows, other, wifi_node,
				       ppe_wifi_hash(&entry->wifi_key))
			if (!memcmp(&other->wifi_key, &entry->wifi_key,
				    sizeof(entry->wifi_key))) {
				duplicate = true;
				break;
			}
		spin_unlock_bh(&ppe_wifi_lock);
		if (duplicate) {
			ret = -EEXIST;
			goto err_egress;
		}
	}

	ppe_flow_encode(&data, v6, snat, dnat, entry->wifi_egress,
			entry->nexthop, entry->src_if,
			fw, hw);
	if (entry->wifi_egress)
		ppe_entry_set(fw, 68, 8, QCA_PPE_WIFI_SERVICE_CODE);
	nfw = v6 ? PPE_FLOW_ENTRY_WORDS_V6 : PPE_FLOW_ENTRY_WORDS_V4;
	nhw = v6 ? PPE_HOST_ENTRY_WORDS_V6 : PPE_HOST_ENTRY_WORDS_V4;

	ret = ppe_flow_op(priv, PPE_TBL_OP_ADD, fw, nfw, hw, nhw, &entry->index,
			  &entry->host_index);
	if (ret) {
		ppe_flow_reject(priv, rule, PPE_REJECT_HW_OP);
		goto err_egress;
	}
	ppe_host_ref_get(priv, entry->host_index);

	/* The hardware writes the host index it resolved into the entry and
	 * counts the age down from there, so the stored image carries the one
	 * and drops the other for readbacks to compare equal.
	 */
	memcpy(entry->words, fw, nfw * sizeof(*fw));
	entry->nwords = nfw;
	memcpy(entry->hwords, hw, nhw * sizeof(*hw));
	entry->nhwords = nhw;
	entry->profile = data.priority;
	entry->words[0] = (fw[0] | entry->host_index << PPE_FLOW_E_HOST_IDX_OFF) &
			  ~PPE_FLOW_E_AGE_MASK;

	/* The counter survives the entry that filled it, so a recycled slot has
	 * to start from zero rather than from the previous flow's total.
	 */
	ppe_flow_counter_clear(priv, entry->index);
	entry->last_used = jiffies;

	ret = rhashtable_insert_fast(&priv->flow_table, &entry->node,
				     ppe_flow_ht_params);
	if (ret)
		goto err_hw;

	list_add_tail(&entry->list, &priv->flow_list);
	priv->flow_offloaded++;
	if (entry->wifi_egress) {
		atomic64_inc(&ppe_wifi_installed);
	}

	if (entry->wifi_ingress) {
		spin_lock_bh(&ppe_wifi_lock);
		hash_add(ppe_wifi_ingress_flows, &entry->wifi_node,
			 ppe_wifi_hash(&entry->wifi_key));
		entry->wifi_bound = true;
		atomic_inc(&entry->wifi_ingress->flows);
		spin_unlock_bh(&ppe_wifi_lock);
	}

	return 0;

err_hw:
	ppe_flow_entry_delete(priv, entry->index);
	ppe_host_ref_put(priv, entry->host_index);
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
		return -ENOENT;
	}

	rhashtable_remove_fast(&priv->flow_table, &entry->node,
			       ppe_flow_ht_params);
	list_del(&entry->list);
	ppe_flow_entry_destroy(priv, entry);
	kfree(entry);

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

		rhashtable_remove_fast(&priv->flow_table, &entry->node,
				       ppe_flow_ht_params);
		list_del(&entry->list);
		ppe_flow_entry_destroy(priv, entry);
		kfree(entry);
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

/* The entry is replaced rather than rewritten in place: an IPv6 entry's
 * words are spread over two slots in a layout only the op engine holds. The
 * host entry is the same one, found again by its key; the counter starts
 * over with the new slot, and so does everything derived from it.
 */
static int ppe_flow_profile_set(struct qca_ppe_priv *priv,
				struct ppe_flow_entry *entry, u8 profile)
{
	u32 w[PPE_FLOW_ENTRY_WORDS_V6];
	u32 index, host_index, pkts;
	u64 bytes;
	int ret;

	lockdep_assert_held(&priv->flow_lock);

	/* The hardware ages an idle entry out and hands its slot on, and a
	 * flow under the count is the likeliest to have been; a delete by
	 * index would then take the sibling's entry.
	 */
	ret = ppe_flow_entry_age(priv, entry);
	if (ret < 0)
		return ret;

	pkts = ppe_flow_counter_delta(priv, entry, &bytes);
	ppe_flow_account(priv, entry, pkts, bytes);

	/* The stored image carries the host index and no age; an add is
	 * staged the way the encoder stages it, at full age and with no host
	 * index, for the hardware to write the one it resolves.
	 */
	memcpy(w, entry->words, entry->nwords * sizeof(*w));
	w[0] = (w[0] & ~PPE_FLOW_E_HOST_IDX_MASK) |
	       FIELD_PREP(PPE_FLOW_E_AGE_MASK, PPE_FLOW_AGE_MAX);
	w[1] &= ~BIT(PPE_FLOW_E_PRI_PROFILE_OFF % 32);
	w[2] &= ~GENMASK(PPE_FLOW_E_PRI_PROFILE_LEN - 2, 0);
	ppe_entry_set(w, PPE_FLOW_E_PRI_PROFILE_OFF, PPE_FLOW_E_PRI_PROFILE_LEN,
		      profile);

	ret = ppe_flow_entry_delete(priv, entry->index);
	if (ret)
		return ret;

	ret = ppe_flow_op(priv, PPE_TBL_OP_ADD, w, entry->nwords,
			  entry->hwords, entry->nhwords, &index, &host_index);
	if (ret)
		return ret;

	if (host_index != entry->host_index) {
		ppe_host_ref_get(priv, host_index);
		ppe_host_ref_put(priv, entry->host_index);
		entry->host_index = host_index;
	}
	memcpy(entry->words, w, entry->nwords * sizeof(*w));
	entry->words[0] = (w[0] | host_index << PPE_FLOW_E_HOST_IDX_OFF) &
			  ~PPE_FLOW_E_AGE_MASK;
	entry->index = index;
	ppe_flow_counter_clear(priv, index);
	entry->packets = 0;
	entry->bytes = 0;

	return 0;
}

static void ppe_sparse_work(struct work_struct *work)
{
	struct qca_ppe_priv *priv = container_of(work, struct qca_ppe_priv,
						 sparse_work.work);
	struct ppe_flow_entry *entry;
	u32 pkts, delta;

	mutex_lock(&priv->flow_lock);
	list_for_each_entry(entry, &priv->flow_list, list) {
		u64 bytes;

		regmap_read(priv->regmap, PPE_IN_FLOW_CNT_TBL(entry->index),
			    &pkts);
		delta = pkts - (u32)entry->packets;

		/* The age read costs the entry's words and only a flow that
		 * moved has anything to attribute, so it is taken on the
		 * delta rather than every period.
		 */
		if (delta && ppe_flow_entry_age(priv, entry) >= 0) {
			u32 acct = ppe_flow_counter_delta(priv, entry, &bytes);

			ppe_flow_account(priv, entry, acct, bytes);
		}

		if (delta > PPE_SPARSE_PKTS || !ppe_sparse_flows) {
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
	mutex_unlock(&priv->flow_lock);

	schedule_delayed_work(&priv->sparse_work,
			      msecs_to_jiffies(PPE_SPARSE_PERIOD_MS));
}

int ppe_flow_offload_init(struct qca_ppe_priv *priv)
{
	struct device *dev = priv->ds.dev;
	int i, ret;

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
	if (!priv->eg_l3_if || !priv->pub_ip || !priv->nexthop ||
	    !priv->host_ref || !priv->my_mac)
		return -ENOMEM;

	for (i = 0; i < QCA_PPE_MAX_PORTS; i++) {
		priv->wan_vsi[i] = -1;
		priv->wan_mymac[i] = -1;
		priv->wan_xlt[i] = -1;
	}

	INIT_LIST_HEAD(&priv->flow_list);

	/* CPU-return still needs PPE route/NAT and Ethernet editing. Clearing
	 * metadata-preserve bits publishes the service marker to EDMA. */
	ret = ppe_wifi_service_program(priv);
	if (ret)
		return ret;

	ret = rhashtable_init(&priv->flow_table, &ppe_flow_ht_params);
	if (ret)
		return ret;

	priv->netdev_nb.notifier_call = ppe_flow_netdev_event;
	ret = register_netdevice_notifier(&priv->netdev_nb);
	if (ret) {
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
	for (int i = 0; i < QCA_PPE_WIFI_INGRESS_SLOTS; i++)
		if (ppe_wifi_ingress[i].dev && ppe_wifi_ingress[i].priv == priv)
			ppe_wifi_ingress_release(&ppe_wifi_ingress[i]);

	mutex_unlock(&priv->vlan_lock);
	mutex_unlock(&priv->flow_lock);

	rhashtable_destroy(&priv->flow_table);
}
