// SPDX-License-Identifier: GPL-2.0-only
/* Giving the firmware a connection the kernel has finished routing.
 *
 * The kernel offers every flow it accepts into its own software fast path to
 * each hardware back end registered on the table, and counts how many took it;
 * a back end that refuses one leaves it for the others and for software. So
 * this is a second back end beside the switch's, and neither needs to know the
 * other exists: the switch declines a flow that did not arrive on one of its
 * ports, and this declines one with no wireless or redirected end.
 *
 * The kernel offers the two directions of a connection separately and the
 * firmware wants a single rule carrying both. The two offers arrive one after
 * the other for the same connection, so the first is kept and the second
 * completes it. That is not merely convenient: the address a frame leaves
 * with in one direction appears only in that direction's offer, and a rule
 * built with the wrong one is accepted, acknowledged, and quietly delivers
 * every packet to the wrong station.
 *
 * Everything else the rule needs that the offer does not carry comes from the
 * connection the flow belongs to, which holds both tuples before and after
 * translation and, for TCP, the window the firmware has to keep checking
 * against once the kernel stops seeing the packets.
 */

#include <linux/cleanup.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/if_vlan.h>
#include <linux/hashtable.h>
#include <linux/jhash.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/rhashtable.h>
#include <linux/sizes.h>
#include <linux/sysfs.h>
#include <linux/soc/qcom/nss_wifi.h>
#include <net/flow_offload.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_flow_table.h>
#include <net/dsa.h>
#include <net/pkt_cls.h>

#include "nss_drv.h"

/* Clamp the priority to the range the egress queues express; a higher value
 * selects no distinct queue. The shipped PPE flow back end bounds the same
 * tag the same way.
 */
#define NSS_FLOW_MAX_PRIORITY	15

/* All four VLAN slots say "no tag" before anything else is decided, the
 * same value the public host caller fills them with unconditionally.
 */
#define NSS_FLOW_VLAN_NONE	0xfff

/* What one direction of a connection contributes to the rule. The VLAN and
 * PPPoE fields are what this direction pushes, which is the other side's
 * encapsulation.
 */
struct nss_flow_dir {
	u8 mac[ETH_ALEN];	/* address the other direction delivers to */
	u8 smac[ETH_ALEN];
	int ifindex;		/* the device this direction arrives on */
	int ifnum;
	u32 mtu;
	u16 vid;
	u16 vtpid;		/* 0: untagged */
	u16 sid;
	bool pppoe;
	bool seen;
};

/* Message types and bodies of the PPPoE node (public nss_pppoe.h). */
#define NSS_PPPOE_MSG_SESSION_CREATE	0
#define NSS_PPPOE_MSG_SESSION_DESTROY	1
#define NSS_FLOW_PPPOE_HLEN		8

struct nss_pppoe_create_msg {
	s32 base_if_num;
	u32 mtu;
	u8 server_mac[ETH_ALEN];
	u8 local_mac[ETH_ALEN];
	u16 session_id;
};

struct nss_pppoe_destroy_msg {
	u16 session_id;
	u8 server_mac[ETH_ALEN];
	u8 local_mac[ETH_ALEN];
};

/* A firmware PPPoE session, shared by every
 * rule over the same session.
 */
struct nss_flow_pppoe {
	u16 sid;
	u8 server[ETH_ALEN];
	u8 local[ETH_ALEN];
	int port;
	u32 vlan_tag;		/* the rule carries it; 0 for none */
	int if_num;
	u32 ref;
};

/* ponytail: a fixed table, a router has one or two sessions */
static struct nss_flow_pppoe nss_flow_pppoe_tbl[4];

struct nss_flow_entry {
	struct rhash_head node;
	struct hlist_node sync_node;
	struct list_head retire_node;
	struct rcu_head rcu;	/* the debugfs walker holds no lock */
	unsigned long flow;
	struct nss_flow_dir dir[FLOW_OFFLOAD_DIR_MAX];
	u32 priority;
	u8 protocol;
	bool v6;
	bool installed;
	bool pending;	/* CREATE may have reached firmware without an ACK */
	u8 retire_attempts;
	int retire_errno;
	bool bridged;
	bool learned;	/* bridge learner, rather than a flowtable, owns it */
	struct nss_flow_pppoe *pppoe;
	enum flow_offload_tuple_dir pppoe_dir;	/* the side that is PPPoE */
	u64 lastused;
	u64 cnt[4];	/* flow rx, flow tx, return rx, return tx */
	union {
		struct nss_ipv4_5tuple t4;
		struct nss_ipv6_5tuple t6;
	};
};

static const struct rhashtable_params nss_flow_ht_params = {
	.head_offset	= offsetof(struct nss_flow_entry, node),
	.key_offset	= offsetof(struct nss_flow_entry, flow),
	.key_len	= sizeof(unsigned long),
	.automatic_shrinking = true,
};

static DEFINE_MUTEX(nss_flow_lock);
static LIST_HEAD(nss_flow_blocks);
static struct rhashtable nss_flow_table;
static LIST_HEAD(nss_flow_retire_list);
static void nss_flow_retire_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(nss_flow_retire_worker, nss_flow_retire_work);
#define NSS_FLOW_RETIRE_RETRIES	3
#define NSS_FLOW_RETIRE_BATCH	16
/* The cookie table owns lifetime; this index only matches firmware syncs.
 * Receive-side notifications and process-side sweeps share the short lock.
 * A tuple is immutable while its entry is linked here.
 */
static DEFINE_HASHTABLE(nss_flow_sync_table, 10);
static DEFINE_SPINLOCK(nss_flow_sync_lock);
/* Capacity is per address family and only grows after firmware ACKs.
 * Admission stops firmware LRU eviction cycling active connections through
 * the serialized command ring while new flows can use software forwarding.
 */
#define NSS_FLOW_BASE_CONN	1024
#define NSS_FLOW_CONN		4096
static u32 nss_flow_limit[2] = {
	NSS_FLOW_BASE_CONN, NSS_FLOW_BASE_CONN,
};

static u32 nss_flow_table_bytes[2];
static u16 nss_flow_sweep_index[2];
static unsigned long nss_flow_swept;
static u32 nss_flow_installed[2];
static u32 nss_flow_pending[2];
static u32 nss_flow_retiring[2];
static u64 nss_flow_retire_errors, nss_flow_retire_recovered;
static u64 nss_flow_retire_halts;
static u64 nss_flow_sweep_skipped[2];
static u32 nss_flow_full[2];
static bool nss_flow_ready;
static struct nss_core *nss_flow_core;

/* Table sizes come from the running image, never from a guessed entry size.
 * Once CFG is sent, keep both DMA blocks until core halt even on a timeout:
 * a late firmware ACK may already have taken ownership of their addresses.
 */
static int nss_flow_configure_family(struct nss_core *core, bool v6,
				     u32 count, u32 *bytes)
{
	struct nss_conn_table_size_msg size = {
		.cm.interface = v6 ? NSS_INTERFACE_IPV6 : NSS_INTERFACE_IPV4,
		.cm.type = NSS_IPV4_TX_CONN_TABLE_SIZE_MSG,
		.num_conn = cpu_to_be32(count),
	};
	struct nss_conn_cfg_msg cfg = {
		.cm.interface = size.cm.interface,
		.cm.type = NSS_IPV4_TX_CONN_CFG_RULE_MSG,
		.num_conn = cpu_to_be32(count),
	};
	void *ce, *cme;
	u32 ce_size, cme_size;
	int ret;

	ret = nss_msg_send_quiet(core, &size,
				 offsetofend(struct nss_conn_table_size_msg,
					     cme_table_size));
	if (ret || size.cm.response != NSS_CMN_RESPONSE_ACK || size.cm.error) {
		dev_err(core->dev, "IPv%u table-size reply: rc %d response %u error %u\n",
			v6 ? 6 : 4, ret, size.cm.response, size.cm.error);
		return ret ?: -EPROTO;
	}
	if (size.cm.len < 3 * sizeof(u32) ||
	    be32_to_cpu(size.num_conn) != count)
		return -EPROTO;

	ce_size = be32_to_cpu(size.ce_table_size);
	cme_size = be32_to_cpu(size.cme_table_size);
	if (!ce_size || !cme_size || ce_size > SZ_16M || cme_size > SZ_16M)
		return -EOVERFLOW;

	ce = nss_mem_alloc(core, ce_size, 64, &cfg.ce_mem);
	if (!ce)
		return -ENOMEM;
	cme = nss_mem_alloc(core, cme_size, 64, &cfg.cme_mem);
	if (!cme) {
		nss_mem_free(core, ce);
		return -ENOMEM;
	}

	ret = nss_msg_send_quiet(core, &cfg,
				 offsetofend(struct nss_conn_cfg_msg, cme_mem));
	if (ret || cfg.cm.response != NSS_CMN_RESPONSE_ACK || cfg.cm.error) {
		dev_err(core->dev, "IPv%u table-config reply: rc %d response %u error %u\n",
			v6 ? 6 : 4, ret, cfg.cm.response, cfg.cm.error);
		return ret ?: -EPROTO;
	}
	if (cfg.cm.len < 3 * sizeof(u32) ||
	    be32_to_cpu(cfg.num_conn) != count)
		return -EPROTO;

	*bytes = ce_size + cme_size;
	dev_info(core->dev, "IPv%u connection capacity %u acknowledged, CE %u CME %u bytes\n",
		 v6 ? 6 : 4, count, ce_size, cme_size);
	return 0;
}

int nss_flow_configure(struct nss_core *core)
{
	u32 count = NSS_FLOW_CONN, bytes[2] = {};
	int i, ret;

	guard(mutex)(&nss_flow_lock);
	/* A core restart begins a fresh statistics scan. */
	memset(nss_flow_sweep_index, 0, sizeof(nss_flow_sweep_index));
	nss_flow_swept = jiffies - HZ;
	if (strcmp(core->fw->name, "12.5") &&
	    strcmp(core->fw->name, "11.4"))
		return -EOPNOTSUPP;

	scoped_guard(spinlock_bh, &nss_flow_sync_lock)
		memset(nss_flow_limit, 0, sizeof(nss_flow_limit));
	for (i = 0; i < ARRAY_SIZE(nss_flow_limit); i++) {
		ret = nss_flow_configure_family(core, i, count, &bytes[i]);
		if (ret) {
			dev_err(core->dev, "IPv%u connection capacity %u failed: %d; stopping core\n",
				i ? 6 : 4, count, ret);
			return ret;
		}
	}
	scoped_guard(spinlock_bh, &nss_flow_sync_lock) {
		for (i = 0; i < ARRAY_SIZE(nss_flow_limit); i++) {
			nss_flow_limit[i] = count;
			nss_flow_table_bytes[i] = bytes[i];
		}
	}
	return 0;
}
/* Why a flow was not taken. A flow the firmware does not carry and a flow it
 * was never offered look the same from outside, and so do the several reasons
 * for the first, so each is counted where it is decided.
 */
enum nss_flow_reject {
	NSS_FLOW_REJECT_NOT_OURS,
	NSS_FLOW_REJECT_KEY,
	NSS_FLOW_REJECT_INTERFACE,
	NSS_FLOW_REJECT_ACTION,
	NSS_FLOW_REJECT_NO_MAC,
	NSS_FLOW_REJECT_NO_CT,
	NSS_FLOW_REJECT_FIRMWARE,
	NSS_FLOW_REJECT_MAX,
};

static const char *const nss_flow_reject_name[] = {
	"not-ours", "key", "interface", "action", "no-mac", "no-ct",
	"firmware",
};

static u32 nss_flow_pushed, nss_flow_bound, nss_flow_reject[NSS_FLOW_REJECT_MAX];
static int nss_flow_last_ifindex;

/* Resource refusals affect new rules only. Both entry points hold the flow
 * mutex, so a family has one bounded retry window even under many new flows.
 * This is independent of the fixed per-family connection admission limit.
 */
struct nss_flow_pressure {
	unsigned long retry_at;
	u32 refused;
	u32 deferred;
	u8 backoff;
};

static struct nss_flow_pressure nss_flow_pressure[2];

struct nss_flow_buffers {
	u32 payload_free;
	u32 ocm_free;
	u32 ocm_total;
	u32 ddr_free;
	u32 ddr_total;
	u64 payload_failed;
	u64 pbuf_failed;
	u64 queue_dropped;
	u64 syncs;
};

static struct nss_flow_buffers nss_flow_buffers;

/* Called from NAPI. Reject truncated statistics before reading the prefix. */
static bool nss_flow_buffer_sync(u32 iface, const struct nss_cmn_msg *cm,
				 u32 len)
{
	const struct nss_n2h_buffer_stats *st = (const void *)(cm + 1);
	struct nss_flow_buffers *b = &nss_flow_buffers;

	if (iface != NSS_INTERFACE_N2H || cm->type)
		return false;
	if (cm->len < sizeof(*st) || len < sizeof(*cm) + sizeof(*st))
		return true;

	guard(spinlock_bh)(&nss_flow_sync_lock);
	b->payload_free = st->payload_free;
	b->ocm_free = st->ocm.free;
	b->ocm_total = st->ocm.total;
	b->ddr_free = st->ddr.free;
	b->ddr_total = st->ddr.total;
	b->payload_failed += st->payload_alloc_failed;
	b->pbuf_failed += (u64)st->ocm.alloc_with_payload_failed +
			  st->ocm.alloc_without_payload_failed +
			  st->ddr.alloc_with_payload_failed +
			  st->ddr.alloc_without_payload_failed;
	b->queue_dropped += st->queue_dropped;
	b->syncs++;
	return true;
}

/* Where in the firmware a packet stopped and why it went to the host. */
struct nss_flow_node {
	const char *name;
	u32 iface, type, excep_off;
	struct nss_cmn_node_stats stats;
	unsigned int n;
	u32 excep[NSS_NODE_EXCEPTIONS_MAX];
};

static struct nss_flow_node nss_flow_nodes[] = {
	{ "eth_rx", NSS_INTERFACE_ETH_RX, NSS_ETH_RX_NODE_STATS_SYNC_MSG,
	  NSS_ETH_RX_NODE_EXCEPTIONS_OFF },
	{ "ipv4", NSS_INTERFACE_IPV4, NSS_IPV4_RX_NODE_STATS_SYNC_MSG,
	  NSS_IPV4_NODE_EXCEPTIONS_OFF },
	{ "ipv6", NSS_INTERFACE_IPV6, NSS_IPV6_RX_NODE_STATS_SYNC_MSG,
	  NSS_IPV6_NODE_EXCEPTIONS_OFF },
};

static bool nss_flow_node_sync(u32 iface, const struct nss_cmn_msg *ncm,
			       u32 len)
{
	struct nss_flow_node *nd;
	size_t off;

	for (nd = nss_flow_nodes; nd < nss_flow_nodes + ARRAY_SIZE(nss_flow_nodes); nd++) {
		if (nd->iface != iface || nd->type != ncm->type)
			continue;

		/* The buffer can be longer than the message it carries. */
		len = min_t(u32, len, sizeof(*ncm) + ncm->len);
		off = sizeof(*ncm) + nd->excep_off;
		if (len <= off)
			return true;

		memcpy(&nd->stats, ncm + 1, sizeof(nd->stats));
		nd->n = min_t(unsigned int, (len - off) / 4,
			      NSS_NODE_EXCEPTIONS_MAX);
		memcpy(nd->excep, (const u8 *)ncm + off, nd->n * 4);
		return true;
	}

	return false;
}

/* The last few connections that were not taken, with the tuple of each.
 * The counters say how often a reason fires but not for which connection,
 * and a box carrying ordinary traffic fires most of them for flows nobody
 * asked about - so a reason on its own cannot answer why one particular
 * flow stayed in software.
 */
struct nss_flow_reject_rec {
	union nf_inet_addr sip, dip;
	__be16 sport, dport;
	u8 l3proto, proto, dir, why;
	u32 detail;
};

static struct nss_flow_reject_rec nss_flow_rej[8];
static u32 nss_flow_rej_seen;

static int nss_flow_reject_at(enum nss_flow_reject why, int ret,
			      struct flow_cls_offload *f, u32 detail)
{
	struct nss_flow_reject_rec *r;
	struct flow_offload_tuple *t;

	nss_flow_reject[why]++;

	if (!f)
		return ret;

	t = (struct flow_offload_tuple *)f->cookie;
	r = &nss_flow_rej[nss_flow_rej_seen++ % ARRAY_SIZE(nss_flow_rej)];
	r->why = why;
	r->dir = t->dir;
	r->l3proto = t->l3proto;
	r->proto = t->l4proto;
	r->sip.in6 = t->src_v6;
	r->dip.in6 = t->dst_v6;
	r->sport = t->src_port;
	r->dport = t->dst_port;
	r->detail = detail;

	return ret;
}

void nss_flow_bind(struct nss_core *core)
{
	guard(mutex)(&nss_flow_lock);
	nss_flow_core = core;
}

/* The connection an offer belongs to.
 *
 * The kernel names a direction by the address of its tuple, and the tuple says
 * which direction it is, so the connection is the same tuple array walked back
 * to its first element.
 */
static struct flow_offload *nss_flow_of(unsigned long cookie)
{
	const struct flow_offload_tuple *t = (const void *)cookie;
	struct flow_offload_tuple_rhash *th;

	th = container_of(t, struct flow_offload_tuple_rhash, tuple);

	return container_of(th - t->dir, struct flow_offload, tuplehash[0]);
}

static enum flow_offload_tuple_dir nss_flow_dir_of(unsigned long cookie)
{
	const struct flow_offload_tuple *t = (const void *)cookie;

	return t->dir;
}

/* The firmware's number for a network device, and its maximum frame size.
 *
 * The driver already holds one device per interface it has given the
 * firmware - a switch port it armed, a virtual device it created for a
 * radio - so the number is where the device is found rather than anything
 * that has to be derived.
 */
int nss_flow_ifnum(struct nss_core *core, int ifindex, u32 *mtu)
{
	int i;

	guard(rcu)();

	/* A mesh device is bound to two nodes; flows go to the one that
	 * builds the mesh header.
	 */
	i = nss_mesh_ifnum(ifindex);
	if (i >= 0) {
		struct net_device *dev = rcu_dereference(core->iface[i]);

		if (!dev)
			return -ENODEV;
		*mtu = dev->mtu;
		return i;
	}

	for (i = 0; i < NSS_INTERFACE_MAX; i++) {
		struct net_device *dev = rcu_dereference(core->iface[i]);

		if (!dev || dev->ifindex != ifindex)
			continue;
		/* A switch port has an interface number so the firmware can
		 * hand it an exception, which says nothing about whether the
		 * firmware may transmit there. Only an armed port belongs in a
		 * rule: the firmware accepts one naming an unarmed port and
		 * then injects into a fabric path the host still owns.
		 */
		if (dsa_user_dev_check(dev) && !(core->phys_armed & BIT(i)))
			return -EPERM;
		*mtu = dev->mtu;
		return i;
	}

	return -ENODEV;
}

static bool nss_flow_is_wireless(int ifindex)
{
	struct net_device *dev;

	guard(rcu)();

	dev = dev_get_by_index_rcu(&init_net, ifindex);

	return dev && nss_dev_is_wifi(dev);
}

/* A connection this firmware may carry has a station or a redirected device
 * at one end; between two switch ports the switch carries it.
 */
bool nss_flow_is_ours(int ifindex)
{
	return nss_flow_is_wireless(ifindex) || nss_redir_bound(ifindex);
}

/* Everything the firmware is told about one direction, out of that
 * direction's own offer.
 */
static int nss_flow_take_dir(struct nss_core *core, struct nss_flow_entry *e,
			     enum flow_offload_tuple_dir dir,
			     struct flow_cls_offload *f)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct nss_flow_dir *d = &e->dir[dir];
	struct flow_match_meta meta;
	struct flow_action_entry *act;
	struct net_device *out = NULL;
	struct ethhdr eth = {};
	int i;

	/* A refresh offers the same direction again. */
	*d = (struct nss_flow_dir){};

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META))
		return nss_flow_reject_at(NSS_FLOW_REJECT_KEY, -EOPNOTSUPP, f, 0);

	flow_rule_match_meta(rule, &meta);
	d->ifindex = meta.key->ingress_ifindex;
	d->ifnum = nss_flow_ifnum(core, meta.key->ingress_ifindex, &d->mtu);
	if (d->ifnum < 0) {
		nss_flow_last_ifindex = meta.key->ingress_ifindex;
		return nss_flow_reject_at(NSS_FLOW_REJECT_INTERFACE, d->ifnum, f,
					  meta.key->ingress_ifindex);
	}

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			if (act->mangle.htype != FLOW_ACT_MANGLE_HDR_TYPE_ETH)
				break;
			/* The kernel rewrites the header as two words at
			 * fixed offsets, so the address is read back out of
			 * a header rather than out of the action.
			 */
			if (act->mangle.offset > sizeof(eth) - sizeof(u32))
				return nss_flow_reject_at(NSS_FLOW_REJECT_ACTION,
							  -EOPNOTSUPP, f,
							  act->id);
			*(u32 *)((u8 *)&eth + act->mangle.offset) =
				(*(u32 *)((u8 *)&eth + act->mangle.offset) &
				 ~act->mangle.mask) | act->mangle.val;
			break;
		case FLOW_ACTION_PRIORITY:
			e->priority = min_t(u32, act->priority,
					    NSS_FLOW_MAX_PRIORITY);
			break;
		case FLOW_ACTION_VLAN_PUSH:
			/* One tag: the rule has one VLAN slot per side. */
			if (d->vtpid)
				return nss_flow_reject_at(NSS_FLOW_REJECT_ACTION,
							  -EOPNOTSUPP, f,
							  act->id);
			d->vid = act->vlan.vid;
			d->vtpid = ntohs(act->vlan.proto);
			break;
		case FLOW_ACTION_PPPOE_PUSH:
			d->sid = act->pppoe.sid;
			d->pppoe = true;
			break;
		case FLOW_ACTION_REDIRECT:
			out = act->dev;
			break;
		case FLOW_ACTION_VLAN_POP:
		case FLOW_ACTION_CSUM:
			break;
		default:
			/* A tunnel, a VLAN or an encapsulation this rule has
			 * no field for. Leaving the flow in software is the
			 * only honest answer.
			 */
			dev_dbg_ratelimited(core->dev,
					    "flow declined: action %d, ingress %d\n",
					    act->id,
					    meta.key->ingress_ifindex);
			return nss_flow_reject_at(NSS_FLOW_REJECT_ACTION,
						  -EOPNOTSUPP, f, act->id);
		}
	}

	/* A modem's device has no header to build, so its neighbours carry
	 * no address. The frame the firmware builds for it is taken off
	 * before the modem sees the packet; the device's own address is as
	 * good as any.
	 */
	if (!is_valid_ether_addr(eth.h_dest) && out &&
	    out->type == ARPHRD_RAWIP && nss_redir_bound(out->ifindex)) {
		ether_addr_copy(eth.h_dest, out->dev_addr);
		if (!is_valid_ether_addr(eth.h_source))
			ether_addr_copy(eth.h_source, out->dev_addr);
	}

	if (!is_valid_ether_addr(eth.h_dest))
		return nss_flow_reject_at(NSS_FLOW_REJECT_NO_MAC, -EOPNOTSUPP, f, 0);

	/* The address this direction delivers to is the one the firmware
	 * needs for the connection's other half.
	 */
	ether_addr_copy(d->mac, eth.h_dest);
	ether_addr_copy(d->smac, eth.h_source);
	d->seen = true;

	return 0;
}

static void nss_flow_mac(u16 out[3], const u8 *mac)
{
	memcpy(out, mac, ETH_ALEN);
}

int nss_flow_dyn_alloc(struct nss_core *core, u32 type)
{
	struct nss_dynamic_interface_msg d = {};
	int ret;

	d.cm.interface = NSS_INTERFACE_DYNAMIC;
	d.cm.type = NSS_DYNAMIC_INTERFACE_ALLOC_NODE;
	d.msg.alloc_node.type = type;
	ret = nss_msg_send(core, &d, sizeof(d));
	if (ret)
		return ret;
	if (d.msg.alloc_node.if_num < 0 ||
	    d.msg.alloc_node.if_num >= NSS_INTERFACE_MAX)
		return -ERANGE;

	return d.msg.alloc_node.if_num;
}

void nss_flow_dyn_free(struct nss_core *core, u32 type, int if_num)
{
	struct nss_dynamic_interface_msg d = {};

	d.cm.interface = NSS_INTERFACE_DYNAMIC;
	d.cm.type = NSS_DYNAMIC_INTERFACE_DEALLOC_NODE;
	d.msg.dealloc_node.type = type;
	d.msg.dealloc_node.if_num = if_num;
	nss_msg_send(core, &d, sizeof(d));
}

static void nss_flow_pppoe_put(struct nss_core *core, struct nss_flow_pppoe *p)
{
	struct nss_pppoe_destroy_msg dm = {};

	if (--p->ref)
		return;

	dm.session_id = p->sid;
	ether_addr_copy(dm.server_mac, p->server);
	ether_addr_copy(dm.local_mac, p->local);
	nss_phys_msg(core, p->if_num, NSS_PPPOE_MSG_SESSION_DESTROY, &dm,
		     sizeof(dm));
	nss_flow_dyn_free(core, NSS_DYNAMIC_INTERFACE_TYPE_PPPOE, p->if_num);
}

/* A session on @port, created on first use and shared by every rule over it.
 *
 * The firmware parses a VLAN tag inline and the rule carries the tag, so no
 * VLAN node is needed. No base interface is named: given one, the firmware
 * also writes the session and an L3 interface of its own into the PPE tables
 * qca_ppe owns, and wired flows over the session then fail the source
 * interface check.
 */
static struct nss_flow_pppoe *nss_flow_pppoe_ref(struct nss_core *core, u16 sid,
						 const u8 *server,
						 const u8 *local, int port,
						 u32 mtu, u32 tag)
{
	struct nss_pppoe_create_msg cm = {};
	struct nss_flow_pppoe *p, *free = NULL;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(nss_flow_pppoe_tbl); i++) {
		p = &nss_flow_pppoe_tbl[i];
		if (!p->ref) {
			free = free ?: p;
			continue;
		}
		if (p->sid == sid && p->port == port && p->vlan_tag == tag &&
		    ether_addr_equal(p->server, server)) {
			p->ref++;
			return p;
		}
	}
	if (!free)
		return ERR_PTR(-ENOSPC);
	p = free;

	ret = nss_flow_dyn_alloc(core, NSS_DYNAMIC_INTERFACE_TYPE_PPPOE);
	if (ret < 0)
		return ERR_PTR(ret);
	p->if_num = ret;

	cm.base_if_num = -1;
	cm.mtu = mtu - NSS_FLOW_PPPOE_HLEN;
	ether_addr_copy(cm.server_mac, server);
	ether_addr_copy(cm.local_mac, local);
	cm.session_id = sid;
	ret = nss_phys_msg(core, p->if_num, NSS_PPPOE_MSG_SESSION_CREATE, &cm,
			   sizeof(cm));
	if (ret) {
		nss_flow_dyn_free(core, NSS_DYNAMIC_INTERFACE_TYPE_PPPOE,
				  p->if_num);
		return ERR_PTR(ret);
	}

	p->sid = sid;
	ether_addr_copy(p->server, server);
	ether_addr_copy(p->local, local);
	p->port = port;
	p->vlan_tag = tag;
	p->ref = 1;

	return p;
}

int nss_flow_pppoe_hold(struct nss_core *core, u16 sid, const u8 *server,
			const u8 *local, int port, u32 mtu, u32 tag)
{
	struct nss_flow_pppoe *p;

	guard(mutex)(&nss_flow_lock);
	p = nss_flow_pppoe_ref(core, sid, server, local, port, mtu, tag);

	return IS_ERR(p) ? PTR_ERR(p) : p->if_num;
}

void nss_flow_pppoe_drop(struct nss_core *core, int if_num)
{
	int i;

	guard(mutex)(&nss_flow_lock);
	for (i = 0; i < ARRAY_SIZE(nss_flow_pppoe_tbl); i++) {
		if (nss_flow_pppoe_tbl[i].ref &&
		    nss_flow_pppoe_tbl[i].if_num == if_num) {
			nss_flow_pppoe_put(core, &nss_flow_pppoe_tbl[i]);
			return;
		}
	}
}

/* The session the PPPoE side of @e rides on. The direction that pushes the
 * PPPoE header carries the concentrator's address as its destination and
 * ours as its source.
 */
static int nss_flow_pppoe_take(struct nss_core *core, struct nss_flow_entry *e)
{
	struct nss_flow_dir *o = &e->dir[FLOW_OFFLOAD_DIR_ORIGINAL];
	struct nss_flow_dir *r = &e->dir[FLOW_OFFLOAD_DIR_REPLY];
	struct nss_flow_dir *push, *wan;
	struct nss_flow_pppoe *p;
	u32 tag;

	/* A rule pushed again keeps the session it already holds. */
	if (e->pppoe)
		return 0;

	/* VLAN tags are carried inline by the IPv4/IPv6 rule, including
	 * when neither side uses PPPoE. Only PPPoE needs a session node.
	 * Keep two PPPoE endpoints in software until separate sessions can
	 * be represented; a single session cannot describe both ends.
	 */
	if (o->pppoe == r->pppoe)
		return o->pppoe ? -EOPNOTSUPP : 0;

	push = o->pppoe ? o : r;
	wan = o->pppoe ? r : o;
	e->pppoe_dir = o->pppoe ? FLOW_OFFLOAD_DIR_REPLY :
				  FLOW_OFFLOAD_DIR_ORIGINAL;
	tag = push->vtpid ? (u32)push->vtpid << 16 | push->vid : 0;

	p = nss_flow_pppoe_ref(core, push->sid, push->mac, push->smac,
			       wan->ifnum, wan->mtu, tag);
	if (IS_ERR(p))
		return PTR_ERR(p);
	e->pppoe = p;

	return 0;
}

/* What the rule says about each side's encapsulation, the way the public host
 * caller fills it: the tag inline, the session node as the next hop, the
 * port as the interface.
 */
struct nss_flow_encap {
	u32 vlan[FLOW_OFFLOAD_DIR_MAX];
	int pppoe[FLOW_OFFLOAD_DIR_MAX];	/* session interface, or -1 */
};

static void nss_flow_encap_get(const struct nss_flow_entry *e,
			       struct nss_flow_encap *x)
{
	int side;

	for (side = 0; side < FLOW_OFFLOAD_DIR_MAX; side++) {
		/* A side's encapsulation is what the other direction pushes. */
		const struct nss_flow_dir *push = &e->dir[!side];

		if (push->vtpid)
			x->vlan[side] = (u32)push->vtpid << 16 | push->vid;
		else
			x->vlan[side] = NSS_FLOW_VLAN_NONE;
		x->pppoe[side] = e->pppoe && e->pppoe_dir == side ?
				 e->pppoe->if_num : -1;
	}
}

/* Admission counts firmware rules, not partially assembled host offers. */
static int nss_flow_admit(bool v6)
{
	guard(spinlock_bh)(&nss_flow_sync_lock);

	if (nss_flow_installed[v6] + nss_flow_pending[v6] >=
	    nss_flow_limit[v6]) {
		nss_flow_full[v6]++;
		return -ENOSPC;
	}
	return 0;
}

/* A resource NACK is judged here instead of printed twice. Back off probes
 * for 1..8 seconds while packets remain on the host; one ACK ends backoff.
 */
static int nss_flow_create(struct nss_core *core, struct nss_flow_entry *e,
			   void *msg, size_t len, u32 *fw_err)
{
	bool v6 = e->v6, pending;
	struct nss_flow_pressure *p = &nss_flow_pressure[v6];
	struct nss_cmn_msg *cm = msg;
	struct nss_flow_entry *old;
	unsigned long delay;
	int ret;

	ret = nss_flow_admit(v6);
	if (ret)
		return ret;

	/* A cookie can be reused while an older tuple is still retiring.
	 * Never let a delayed delete remove the replacement rule.
	 */
	list_for_each_entry(old, &nss_flow_retire_list, retire_node) {
		if (old->v6 != v6)
			continue;
		if (v6 ? !memcmp(&old->t6, &e->t6, sizeof(e->t6)) :
			 !memcmp(&old->t4, &e->t4, sizeof(e->t4)))
			return -EAGAIN;
	}

	if (p->backoff && time_before(jiffies, p->retry_at)) {
		p->deferred++;
		return -EAGAIN;
	}

	ret = nss_msg_send_tracked(core, msg, len, &pending);
	if (pending || (!ret && cm->error)) {
		guard(spinlock_bh)(&nss_flow_sync_lock);
		e->pending = true;
		nss_flow_pending[v6]++;
	}
	if (cm->response == NSS_CMN_RESPONSE_EMSG &&
	    cm->error == NSS_IPV4_CR_BUFFER_ALLOC_FAIL_ERROR) {
		*fw_err = cm->error;
		p->refused++;
		p->backoff = min_t(u8, p->backoff + 1, 4);
		delay = HZ << (p->backoff - 1);
		p->retry_at = jiffies + delay;
		dev_warn_ratelimited(core->dev,
				     "IPv%u rule buffers exhausted; software forwarding, retry in %u ms\n",
				     v6 ? 6 : 4, jiffies_to_msecs(delay));
		return -EAGAIN;
	}

	if (ret || cm->error) {
		*fw_err = cm->error;
		dev_warn_ratelimited(core->dev,
				     "IPv%u rule create failed: rc %d error %u\n",
				     v6 ? 6 : 4, ret, cm->error);
		return ret ?: -EIO;
	}

	p->backoff = 0;
	return 0;
}

static_assert(NSS_IPV4_CR_BUFFER_ALLOC_FAIL_ERROR ==
	      NSS_IPV6_CR_BUFFER_ALLOC_FAIL_ERROR);

static int nss_flow_send_v4(struct nss_core *core, struct nss_flow_entry *e,
			    struct nf_conn *ct, u32 *fw_err)
{
	const struct nf_conntrack_tuple *o, *r;
	struct nss_ipv4_rule_create_msg *c;
	struct nss_flow_encap x;
	struct nss_ipv4_msg *m __free(kfree) =
		kzalloc(sizeof(*m), GFP_KERNEL);
	int ret;

	if (!m)
		return -ENOMEM;

	o = &ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple;
	r = &ct->tuplehash[IP_CT_DIR_REPLY].tuple;
	c = &m->msg.rule_create;

	/* The firmware matches on host-endian words. */
	c->tuple.protocol = e->protocol;
	c->tuple.flow_ip = ntohl(o->src.u3.ip);
	c->tuple.flow_ident = ntohs(o->src.u.all);
	c->tuple.return_ip = ntohl(o->dst.u3.ip);
	c->tuple.return_ident = ntohs(o->dst.u.all);

	/* Translation is what the connection's two tuples differ by: the
	 * reply's destination is where the flow direction is rewritten to,
	 * and the reply's source is where the return direction comes from.
	 */
	c->conn_rule.flow_ip_xlate = ntohl(r->dst.u3.ip);
	c->conn_rule.flow_ident_xlate = ntohs(r->dst.u.all);
	c->conn_rule.return_ip_xlate = ntohl(r->src.u3.ip);
	c->conn_rule.return_ident_xlate = ntohs(r->src.u.all);

	nss_flow_mac(c->conn_rule.flow_mac,
		     e->dir[FLOW_OFFLOAD_DIR_REPLY].mac);
	nss_flow_mac(c->conn_rule.return_mac,
		     e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].mac);

	c->conn_rule.flow_interface_num =
		e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].ifnum;
	c->conn_rule.return_interface_num =
		e->dir[FLOW_OFFLOAD_DIR_REPLY].ifnum;
	c->conn_rule.flow_mtu = e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].mtu;
	c->conn_rule.return_mtu = e->dir[FLOW_OFFLOAD_DIR_REPLY].mtu;

	c->vlan_primary_rule.ingress_vlan_tag = NSS_FLOW_VLAN_NONE;
	c->vlan_primary_rule.egress_vlan_tag = NSS_FLOW_VLAN_NONE;
	c->vlan_secondary_rule.ingress_vlan_tag = NSS_FLOW_VLAN_NONE;
	c->vlan_secondary_rule.egress_vlan_tag = NSS_FLOW_VLAN_NONE;

	c->qos_rule.flow_qos_tag = e->priority;
	c->qos_rule.return_qos_tag = e->priority;

	/* Left zero the nexthop fields name interface 0 and the frames go
	 * there, so they repeat the connection rule's interfaces. The public
	 * host caller sets them the same way.
	 */
	c->nexthop_rule.flow_nexthop = c->conn_rule.flow_interface_num;
	c->nexthop_rule.return_nexthop = c->conn_rule.return_interface_num;

	c->rule_flags = e->bridged ? NSS_IPV4_RULE_CREATE_FLAG_BRIDGE_FLOW :
				     NSS_IPV4_RULE_CREATE_FLAG_ROUTED;
	c->valid_flags = NSS_IPV4_RULE_CREATE_CONN_VALID |
			 NSS_IPV4_RULE_CREATE_QOS_VALID |
			 NSS_IPV4_RULE_CREATE_NEXTHOP_VALID;

	nss_flow_encap_get(e, &x);
	if (x.vlan[0] != NSS_FLOW_VLAN_NONE || x.vlan[1] != NSS_FLOW_VLAN_NONE) {
		c->vlan_primary_rule.ingress_vlan_tag = x.vlan[0];
		c->vlan_primary_rule.egress_vlan_tag = x.vlan[1];
		c->valid_flags |= NSS_IPV4_RULE_CREATE_VLAN_VALID;
	}
	if (x.pppoe[0] >= 0) {
		c->pppoe_rule.flow_if_exist = 1;
		c->pppoe_rule.flow_if_num = x.pppoe[0];
		c->nexthop_rule.flow_nexthop = x.pppoe[0];
		c->conn_rule.flow_mtu -= NSS_FLOW_PPPOE_HLEN;
		c->valid_flags |= NSS_IPV4_RULE_CREATE_PPPOE_VALID;
	}
	if (x.pppoe[1] >= 0) {
		c->pppoe_rule.return_if_exist = 1;
		c->pppoe_rule.return_if_num = x.pppoe[1];
		c->nexthop_rule.return_nexthop = x.pppoe[1];
		c->conn_rule.return_mtu -= NSS_FLOW_PPPOE_HLEN;
		c->valid_flags |= NSS_IPV4_RULE_CREATE_PPPOE_VALID;
	}

	if (e->protocol == IPPROTO_TCP) {
		spin_lock_bh(&ct->lock);
		c->tcp_rule.flow_window_scale = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_scale;
		c->tcp_rule.flow_max_window = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_maxwin;
		c->tcp_rule.flow_end = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_end;
		c->tcp_rule.flow_max_end = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_maxend;
		c->tcp_rule.return_window_scale = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_scale;
		c->tcp_rule.return_max_window = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_maxwin;
		c->tcp_rule.return_end = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_end;
		c->tcp_rule.return_max_end = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_maxend;
		spin_unlock_bh(&ct->lock);
		c->valid_flags |= NSS_IPV4_RULE_CREATE_TCP_VALID;
		/* Both directions are marked liberal when the flow is handed
		 * over, so a window snapshot from before the handoff must
		 * not be enforced against packets it never saw.
		 */
		c->rule_flags |= NSS_IPV4_RULE_CREATE_FLAG_NO_SEQ_CHECK;
	}

	e->t4 = c->tuple;

	m->cm.interface = NSS_INTERFACE_IPV4;
	m->cm.type = NSS_IPV4_TX_CREATE_RULE_MSG;
	ret = nss_flow_create(core, e, m, offsetof(struct nss_ipv4_msg, msg) +
			      sizeof(*c), fw_err);

	return ret;
}

/* The rule carries each address word in host order, as the interface
 * requires; an address written any other way never matches. The public host
 * caller does the same per-word ntohl.
 */
static void nss_flow_addr6(u32 out[4], const union nf_inet_addr *in)
{
	int i;

	for (i = 0; i < 4; i++)
		out[i] = ntohl(in->ip6[i]);
}

static int nss_flow_send_v6(struct nss_core *core, struct nss_flow_entry *e,
			    struct nf_conn *ct, u32 *fw_err)
{
	const struct nf_conntrack_tuple *o;
	struct nss_ipv6_rule_create_msg *c;
	struct nss_flow_encap x;
	struct nss_ipv6_msg *m __free(kfree) =
		kzalloc(sizeof(*m), GFP_KERNEL);
	int ret;

	if (!m)
		return -ENOMEM;

	o = &ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple;
	c = &m->msg.rule_create;

	c->tuple.protocol = e->protocol;
	nss_flow_addr6(c->tuple.flow_ip, &o->src.u3);
	c->tuple.flow_ident = ntohs(o->src.u.all);
	nss_flow_addr6(c->tuple.return_ip, &o->dst.u3);
	c->tuple.return_ident = ntohs(o->dst.u.all);

	nss_flow_mac(c->conn_rule.flow_mac,
		     e->dir[FLOW_OFFLOAD_DIR_REPLY].mac);
	nss_flow_mac(c->conn_rule.return_mac,
		     e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].mac);

	c->conn_rule.flow_interface_num =
		e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].ifnum;
	c->conn_rule.return_interface_num =
		e->dir[FLOW_OFFLOAD_DIR_REPLY].ifnum;
	c->conn_rule.flow_mtu = e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].mtu;
	c->conn_rule.return_mtu = e->dir[FLOW_OFFLOAD_DIR_REPLY].mtu;

	c->vlan_primary_rule.ingress_vlan_tag = NSS_FLOW_VLAN_NONE;
	c->vlan_primary_rule.egress_vlan_tag = NSS_FLOW_VLAN_NONE;
	c->vlan_secondary_rule.ingress_vlan_tag = NSS_FLOW_VLAN_NONE;
	c->vlan_secondary_rule.egress_vlan_tag = NSS_FLOW_VLAN_NONE;

	c->qos_rule.flow_qos_tag = e->priority;
	c->qos_rule.return_qos_tag = e->priority;

	c->nexthop_rule.flow_nexthop = c->conn_rule.flow_interface_num;
	c->nexthop_rule.return_nexthop = c->conn_rule.return_interface_num;

	c->rule_flags = e->bridged ? NSS_IPV6_RULE_CREATE_FLAG_BRIDGE_FLOW :
				     NSS_IPV6_RULE_CREATE_FLAG_ROUTED;
	c->valid_flags = NSS_IPV6_RULE_CREATE_CONN_VALID |
			 NSS_IPV6_RULE_CREATE_QOS_VALID |
			 NSS_IPV6_RULE_CREATE_NEXTHOP_VALID;

	nss_flow_encap_get(e, &x);
	if (x.vlan[0] != NSS_FLOW_VLAN_NONE || x.vlan[1] != NSS_FLOW_VLAN_NONE) {
		c->vlan_primary_rule.ingress_vlan_tag = x.vlan[0];
		c->vlan_primary_rule.egress_vlan_tag = x.vlan[1];
		c->valid_flags |= NSS_IPV6_RULE_CREATE_VLAN_VALID;
	}
	if (x.pppoe[0] >= 0) {
		c->pppoe_rule.flow_if_exist = 1;
		c->pppoe_rule.flow_if_num = x.pppoe[0];
		c->nexthop_rule.flow_nexthop = x.pppoe[0];
		c->conn_rule.flow_mtu -= NSS_FLOW_PPPOE_HLEN;
		c->valid_flags |= NSS_IPV6_RULE_CREATE_PPPOE_VALID;
	}
	if (x.pppoe[1] >= 0) {
		c->pppoe_rule.return_if_exist = 1;
		c->pppoe_rule.return_if_num = x.pppoe[1];
		c->nexthop_rule.return_nexthop = x.pppoe[1];
		c->conn_rule.return_mtu -= NSS_FLOW_PPPOE_HLEN;
		c->valid_flags |= NSS_IPV6_RULE_CREATE_PPPOE_VALID;
	}

	if (e->protocol == IPPROTO_TCP) {
		spin_lock_bh(&ct->lock);
		c->tcp_rule.flow_window_scale = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_scale;
		c->tcp_rule.flow_max_window = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_maxwin;
		c->tcp_rule.flow_end = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_end;
		c->tcp_rule.flow_max_end = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_maxend;
		c->tcp_rule.return_window_scale = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_scale;
		c->tcp_rule.return_max_window = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_maxwin;
		c->tcp_rule.return_end = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_end;
		c->tcp_rule.return_max_end = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_maxend;
		spin_unlock_bh(&ct->lock);
		c->valid_flags |= NSS_IPV6_RULE_CREATE_TCP_VALID;
		c->rule_flags |= NSS_IPV6_RULE_CREATE_FLAG_NO_SEQ_CHECK;
	}

	e->t6 = c->tuple;

	m->cm.interface = NSS_INTERFACE_IPV6;
	m->cm.type = NSS_IPV6_TX_CREATE_RULE_MSG;
	ret = nss_flow_create(core, e, m, offsetof(struct nss_ipv6_msg, msg) +
			      sizeof(*c), fw_err);

	return ret;
}

static u32 nss_flow_sync_hash_v4(u32 protocol, u32 flow_ip, u32 flow_ident,
			       u32 return_ip, u32 return_ident)
{
	return jhash_3words(flow_ip, return_ip,
			    jhash_2words(flow_ident, return_ident, protocol), 0);
}

static u32 nss_flow_sync_hash_v6(u32 protocol, const u32 *flow_ip, u32 flow_ident,
			       const u32 *return_ip, u32 return_ident)
{
	return jhash_3words(jhash(flow_ip, 4 * sizeof(u32), 0),
			    jhash(return_ip, 4 * sizeof(u32), 0),
			    jhash_2words(flow_ident, return_ident, protocol), 1);
}

static bool nss_flow_sync_drop(struct nss_flow_entry *e)
{
	lockdep_assert_held(&nss_flow_sync_lock);
	hash_del(&e->sync_node);
	if (!e->installed)
		return false;

	WRITE_ONCE(e->installed, false);
	nss_flow_installed[e->v6]--;
	return true;
}

static bool nss_flow_sync_unindex(struct nss_flow_entry *e)
{
	guard(spinlock_bh)(&nss_flow_sync_lock);

	return nss_flow_sync_drop(e);
}

static void nss_flow_sync_install(struct nss_flow_entry *e)
{
	u32 key;

	if (e->v6)
		key = nss_flow_sync_hash_v6(e->t6.protocol, e->t6.flow_ip,
					    e->t6.flow_ident, e->t6.return_ip,
					    e->t6.return_ident);
	else
		key = nss_flow_sync_hash_v4(e->t4.protocol, e->t4.flow_ip,
					    e->t4.flow_ident, e->t4.return_ip,
					    e->t4.return_ident);
	guard(spinlock_bh)(&nss_flow_sync_lock);
	if (!nss_flow_installed[e->v6])
		nss_flow_sweep_index[e->v6] = 0;
	nss_flow_installed[e->v6]++;
	WRITE_ONCE(e->installed, true);
	hash_add(nss_flow_sync_table, &e->sync_node, key);
}

static int nss_flow_withdraw(struct nss_core *core, struct nss_flow_entry *e)
{
	int ret;

	if (!READ_ONCE(e->installed) && !e->pending)
		return 0;
	/* Do not wait out the same lost command for every retiring rule. */
	if (READ_ONCE(core->msg.lost) && !completion_done(&core->msg.done))
		return -EAGAIN;

	if (e->v6) {
		struct {
			struct nss_cmn_msg cm;
			struct nss_ipv6_5tuple tuple;
		} msg = { .tuple = e->t6 }, *m = &msg;
		m->cm.interface = NSS_INTERFACE_IPV6;
		m->cm.type = NSS_IPV6_TX_DESTROY_RULE_MSG;
		ret = nss_msg_send_quiet(core, m, offsetof(struct nss_ipv6_msg, msg) +
					       sizeof(m->tuple));
		if (ret && m->cm.response == NSS_CMN_RESPONSE_EMSG &&
		    m->cm.error == NSS_IPV6_DR_NO_CONNECTION_ENTRY_ERROR)
			ret = 0;
	} else {
		struct {
			struct nss_cmn_msg cm;
			struct nss_ipv4_5tuple tuple;
		} msg = { .tuple = e->t4 }, *m = &msg;
		m->cm.interface = NSS_INTERFACE_IPV4;
		m->cm.type = NSS_IPV4_TX_DESTROY_RULE_MSG;
		ret = nss_msg_send_quiet(core, m, offsetof(struct nss_ipv4_msg, msg) +
					       sizeof(m->tuple));
		if (ret && m->cm.response == NSS_CMN_RESPONSE_EMSG &&
		    m->cm.error == NSS_IPV4_DR_NO_CONNECTION_ENTRY_ERROR)
			ret = 0;
	}

	if (ret)
		return ret;
	nss_flow_sync_unindex(e);
	if (e->pending) {
		guard(spinlock_bh)(&nss_flow_sync_lock);
		e->pending = false;
		nss_flow_pending[e->v6]--;
	}
	return 0;
}

static void nss_flow_release(struct nss_core *core, struct nss_flow_entry *e)
{
	if (e->pppoe)
		nss_flow_pppoe_put(core, e->pppoe);
	kfree_rcu(e, rcu);
}

static void nss_flow_retire(struct nss_flow_entry *e, int ret)
{
	lockdep_assert_held(&nss_flow_lock);
	rhashtable_remove_fast(&nss_flow_table, &e->node, nss_flow_ht_params);
	e->retire_errno = ret;
	list_add_tail(&e->retire_node, &nss_flow_retire_list);
	nss_flow_retiring[e->v6]++;
	nss_flow_retire_errors++;
	schedule_delayed_work(&nss_flow_retire_worker, HZ);
}

static void nss_flow_forget(struct nss_core *core, struct nss_flow_entry *e)
{
	int ret = e->pending ? -EINPROGRESS : nss_flow_withdraw(core, e);

	if (ret) {
		nss_flow_retire(e, ret);
		return;
	}
	rhashtable_remove_fast(&nss_flow_table, &e->node, nss_flow_ht_params);
	nss_flow_release(core, e);
}

/* Keep uncertain rules and their dependencies until firmware confirms the
 * delete. Bound each successful batch, but retry a failed head before later
 * rules so a large backlog cannot postpone containment of a stuck core.
 */
static void nss_flow_retire_work(struct work_struct *work)
{
	struct nss_flow_entry *e;
	struct nss_core *core;
	unsigned long deadline = jiffies + msecs_to_jiffies(20);
	int ret, budget = NSS_FLOW_RETIRE_BATCH;

	guard(mutex)(&nss_flow_lock);
	core = nss_flow_core;
	if (!core || !READ_ONCE(core->running) ||
	    list_empty(&nss_flow_retire_list))
		return;
	do {
		e = list_first_entry(&nss_flow_retire_list, struct nss_flow_entry,
				     retire_node);
		ret = nss_flow_withdraw(core, e);
		if (ret) {
			e->retire_errno = ret;
			if (++e->retire_attempts >= NSS_FLOW_RETIRE_RETRIES) {
				dev_err(core->dev,
					"flow retirement failed: %d; resetting core\n", ret);
				nss_flow_retire_halts++;
				nss_core_fault(core);
				return;
			}
			schedule_delayed_work(&nss_flow_retire_worker, HZ);
			return;
		}
		list_del(&e->retire_node);
		nss_flow_retiring[e->v6]--;
		nss_flow_retire_recovered++;
		nss_flow_release(core, e);
	} while (!list_empty(&nss_flow_retire_list) && --budget &&
		 time_before(jiffies, deadline));
	if (!list_empty(&nss_flow_retire_list))
		schedule_delayed_work(&nss_flow_retire_worker, 1);
}

static int nss_flow_replace(struct nss_core *core, struct flow_cls_offload *f)
{
	enum flow_offload_tuple_dir dir = nss_flow_dir_of(f->cookie);
	struct flow_offload *flow = nss_flow_of(f->cookie);
	unsigned long key = (unsigned long)flow;
	struct nss_flow_entry *e;
	bool bridged = flow->type == NF_FLOW_OFFLOAD_UNSPEC;
	struct flow_match_meta meta;
	struct flow_match_basic basic;
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct nss_flow_dir old;
	u32 fw_err = 0;
	int ret;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC))
		return nss_flow_reject_at(NSS_FLOW_REJECT_KEY, -EOPNOTSUPP, f, 0);

	/* route_init sets ROUTE; bridge_init leaves the zero-initialized
	 * UNSPEC type. Unlike routed rules, these must preserve L3 hop limits.
	 * A kernel flowtable supersedes an earlier bridge-learner rule.
	 */
	if (bridged && flow->ct) {
		unsigned long ct_key = (unsigned long)flow->ct;
		struct nss_flow_entry *learned;

		learned = rhashtable_lookup_fast(&nss_flow_table, &ct_key,
						nss_flow_ht_params);
		if (learned)
			nss_flow_forget(core, learned);
	}

	e = rhashtable_lookup_fast(&nss_flow_table, &key, nss_flow_ht_params);
	if (!e) {
		/* Only a connection a station or a redirected device is one end of
		 * belongs here. The switch carries everything else, and a
		 * flow claimed by both would be forwarded twice. Either
		 * direction may be that end, and the offer in hand names only
		 * its own ingress, so the other direction's is read off the
		 * flow.
		 */
		flow_rule_match_meta(rule, &meta);
		if (!nss_flow_is_ours(meta.key->ingress_ifindex) &&
		    !nss_flow_is_ours(flow->tuplehash[!dir].tuple.iifidx)) {
			nss_flow_last_ifindex = meta.key->ingress_ifindex;
			dev_dbg_ratelimited(core->dev,
					    "flow not ours: ingress %d, other %d\n",
					    meta.key->ingress_ifindex,
					    flow->tuplehash[!dir].tuple.iifidx);
			return nss_flow_reject_at(NSS_FLOW_REJECT_NOT_OURS,
						  -EOPNOTSUPP, f,
						  meta.key->ingress_ifindex);
		}

		/* The IPv6 rule carries no address translation. */
		flow_rule_match_basic(rule, &basic);
		if (basic.key->n_proto == htons(ETH_P_IPV6) &&
		    (test_bit(NF_FLOW_SNAT, &flow->flags) ||
		     test_bit(NF_FLOW_DNAT, &flow->flags)))
			return nss_flow_reject_at(NSS_FLOW_REJECT_ACTION,
						  -EOPNOTSUPP, f, 0);

		ret = nss_flow_admit(basic.key->n_proto == htons(ETH_P_IPV6));
		if (ret)
			return ret;

		e = kzalloc(sizeof(*e), GFP_KERNEL);
		if (!e)
			return -ENOMEM;

		e->flow = key;
		INIT_LIST_HEAD(&e->retire_node);
		e->bridged = bridged;
		e->protocol = basic.key->ip_proto;
		e->v6 = basic.key->n_proto == htons(ETH_P_IPV6);
		WRITE_ONCE(e->lastused, jiffies);

		ret = rhashtable_insert_fast(&nss_flow_table, &e->node,
					     nss_flow_ht_params);
		if (ret) {
			kfree(e);
			return ret;
		}
	}

	old = e->dir[dir];
	ret = nss_flow_take_dir(core, e, dir, f);
	if (ret) {
		e->dir[dir] = old;
		goto drop;
	}

	if (!e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].seen ||
	    !e->dir[FLOW_OFFLOAD_DIR_REPLY].seen)
		return 0;

	/* The connection is only complete now. A refresh of one already
	 * installed asks for nothing new unless the next hop moved, which the
	 * firmware can only be told by a new rule.
	 */
	if (e->installed) {
		if (ether_addr_equal(old.mac, e->dir[dir].mac) &&
		    old.ifnum == e->dir[dir].ifnum)
			return 0;
		ret = nss_flow_withdraw(core, e);
		if (ret) {
			/* Retirement still owns the firmware's old interface. */
			e->dir[dir] = old;
			nss_flow_retire(e, ret);
			return ret;
		}
	}

	if (!flow->ct) {
		ret = nss_flow_reject_at(NSS_FLOW_REJECT_NO_CT, -EOPNOTSUPP, f, 0);
		goto drop;
	}

	/* Both bridge entry points support one VLAN tag per direction.
	 * Translation and PPPoE bridging still have no firmware contract.
	 */
	if (e->bridged &&
	    (test_bit(NF_FLOW_SNAT, &flow->flags) ||
	     test_bit(NF_FLOW_DNAT, &flow->flags) ||
	     e->dir[0].pppoe || e->dir[1].pppoe)) {
		ret = nss_flow_reject_at(NSS_FLOW_REJECT_ACTION,
					 -EOPNOTSUPP, f, 0);
		goto drop;
	}

	ret = nss_flow_pppoe_take(core, e);
	if (ret) {
		nss_flow_reject_at(NSS_FLOW_REJECT_FIRMWARE, ret, f, -ret);
		goto drop;
	}

	ret = e->v6 ? nss_flow_send_v6(core, e, flow->ct, &fw_err) :
		      nss_flow_send_v4(core, e, flow->ct, &fw_err);
	if (ret) {
		/* The firmware's code where it gave one, the transport's
		 * otherwise: a create that never reached it has no other.
		 */
		nss_flow_reject_at(NSS_FLOW_REJECT_FIRMWARE, ret, f,
				   fw_err ? fw_err : -ret);
		goto drop;
	}

	nss_flow_sync_install(e);
	WRITE_ONCE(e->lastused, jiffies);
	nss_flow_pushed++;

	return 0;

drop:
	nss_flow_forget(core, e);

	return ret;
}

static int nss_flow_destroy(struct nss_core *core, struct flow_cls_offload *f)
{
	unsigned long key = (unsigned long)nss_flow_of(f->cookie);
	struct nss_flow_entry *e;

	e = rhashtable_lookup_fast(&nss_flow_table, &key, nss_flow_ht_params);
	if (!e)
		return -ENOENT;

	nss_flow_forget(core, e);

	return 0;
}

static bool nss_flow_is_v4(const struct nss_flow_entry *e,
			   const struct nss_ipv4_conn_sync *r)
{
	return !e->v6 && e->t4.protocol == r->protocol &&
	       e->t4.flow_ip == r->flow_ip &&
	       e->t4.flow_ident == r->flow_ident &&
	       e->t4.return_ip == r->return_ip &&
	       e->t4.return_ident == r->return_ident;
}

static bool nss_flow_is_v6(const struct nss_flow_entry *e,
			   const struct nss_ipv6_conn_sync *r)
{
	return e->v6 && e->t6.protocol == r->protocol &&
	       !memcmp(e->t6.flow_ip, r->flow_ip, sizeof(r->flow_ip)) &&
	       e->t6.flow_ident == r->flow_ident &&
	       !memcmp(e->t6.return_ip, r->return_ip, sizeof(r->return_ip)) &&
	       e->t6.return_ident == r->return_ident;
}

/* A record from the firmware's sweep marks its connection recently used if
 * any of its four counters moved since the previous sweep, and gone if the
 * firmware flushed or evicted it, so that no destroy follows for it.
 */
static void nss_flow_sync_mark_v4(const struct nss_ipv4_conn_sync *r)
{
	struct nss_flow_entry *e;
	u32 key;

	bool gone = r->reason == NSS_IPV4_RULE_SYNC_REASON_FLUSH ||
		    r->reason == NSS_IPV4_RULE_SYNC_REASON_EVICT;

	if (!gone && !(r->flow_rx_packet_count | r->flow_tx_packet_count |
		       r->return_rx_packet_count | r->return_tx_packet_count))
		return;

	key = nss_flow_sync_hash_v4(r->protocol, r->flow_ip, r->flow_ident,
				    r->return_ip, r->return_ident);
	guard(spinlock_bh)(&nss_flow_sync_lock);
	hash_for_each_possible(nss_flow_sync_table, e, sync_node, key) {
		if (!e->installed || !nss_flow_is_v4(e, r))
			continue;
		if (gone) {
			nss_flow_sync_drop(e);
		}
		WRITE_ONCE(e->lastused, jiffies);
		e->cnt[0] += r->flow_rx_packet_count;
		e->cnt[1] += r->flow_tx_packet_count;
		e->cnt[2] += r->return_rx_packet_count;
		e->cnt[3] += r->return_tx_packet_count;
		break;
	}
}

static void nss_flow_sync_mark_v6(const struct nss_ipv6_conn_sync *r)
{
	struct nss_flow_entry *e;
	u32 key;

	bool gone = r->reason == NSS_IPV6_RULE_SYNC_REASON_FLUSH ||
		    r->reason == NSS_IPV6_RULE_SYNC_REASON_EVICT;

	if (!gone && !(r->flow_rx_packet_count | r->flow_tx_packet_count |
		       r->return_rx_packet_count | r->return_tx_packet_count))
		return;

	key = nss_flow_sync_hash_v6(r->protocol, r->flow_ip, r->flow_ident,
				    r->return_ip, r->return_ident);
	guard(spinlock_bh)(&nss_flow_sync_lock);
	hash_for_each_possible(nss_flow_sync_table, e, sync_node, key) {
		if (!e->installed || !nss_flow_is_v6(e, r))
			continue;
		if (gone) {
			nss_flow_sync_drop(e);
		}
		WRITE_ONCE(e->lastused, jiffies);
		e->cnt[0] += r->flow_rx_packet_count;
		e->cnt[1] += r->flow_tx_packet_count;
		e->cnt[2] += r->return_rx_packet_count;
		e->cnt[3] += r->return_tx_packet_count;
		break;
	}
}

/* The firmware drops a connection on its own, flushed or evicted, and says so
 * with a sync carrying the reason. Its rule is gone: a destroy for it is
 * refused, and the flow table's next refresh has to push it again. From the
 * receive poll, sharing the tuple index lock with process-side sweeps.
 */
void nss_flow_notify(const struct nss_cmn_msg *ncm, u32 len)
{
	u32 iface = NSS_INTERFACE_NUM_GET(ncm->interface);
	const struct nss_ipv4_msg *m4;
	const struct nss_ipv6_msg *m6;
	struct nss_flow_entry *e;
	struct hlist_node *tmp;
	u32 key;

	m4 = container_of_const(ncm, struct nss_ipv4_msg, cm);
	m6 = container_of_const(ncm, struct nss_ipv6_msg, cm);
	if (nss_flow_buffer_sync(iface, ncm, len) ||
	    nss_flow_node_sync(iface, ncm, len))
		return;

	if (iface == NSS_INTERFACE_IPV4) {
		if (ncm->type != NSS_IPV4_RX_CONN_STATS_SYNC_MSG ||
		    len < offsetofend(struct nss_ipv4_msg, msg.conn_stats) ||
		    (m4->msg.conn_stats.reason != NSS_IPV4_RULE_SYNC_REASON_FLUSH &&
		     m4->msg.conn_stats.reason != NSS_IPV4_RULE_SYNC_REASON_EVICT))
			return;
		key = nss_flow_sync_hash_v4(m4->msg.conn_stats.protocol,
					    m4->msg.conn_stats.flow_ip,
					    m4->msg.conn_stats.flow_ident,
					    m4->msg.conn_stats.return_ip,
					    m4->msg.conn_stats.return_ident);
	} else if (iface == NSS_INTERFACE_IPV6) {
		if (ncm->type != NSS_IPV6_RX_CONN_STATS_SYNC_MSG ||
		    len < offsetofend(struct nss_ipv6_msg, msg.conn_stats) ||
		    (m6->msg.conn_stats.reason != NSS_IPV6_RULE_SYNC_REASON_FLUSH &&
		     m6->msg.conn_stats.reason != NSS_IPV6_RULE_SYNC_REASON_EVICT))
			return;
		key = nss_flow_sync_hash_v6(m6->msg.conn_stats.protocol,
					    m6->msg.conn_stats.flow_ip,
					    m6->msg.conn_stats.flow_ident,
					    m6->msg.conn_stats.return_ip,
					    m6->msg.conn_stats.return_ident);
	} else {
		return;
	}

	guard(spinlock_bh)(&nss_flow_sync_lock);
	hash_for_each_possible_safe(nss_flow_sync_table, e, tmp, sync_node, key)
		if (iface == NSS_INTERFACE_IPV4 ?
		    nss_flow_is_v4(e, &m4->msg.conn_stats) :
		    nss_flow_is_v6(e, &m6->msg.conn_stats)) {
			nss_flow_sync_drop(e);
		}
}

/* Sweep the firmware's connection table for what moved.
 *
 * The request names a start index and how much room the answer has; the
 * answer carries as many per-connection records as fit and the index to ask
 * from next, zero once the table has wrapped. One answer covers a stretch of
 * the table, not all of it, so each sweep resumes where the last one stopped
 * and the whole table comes round every few sweeps. After the first sweep the
 * per-connection sync messages stop arriving on their own, which otherwise
 * cost one message every couple of milliseconds per connection.
 */
/* Preserve the default table's refresh period when its capacity grows.
 * Eight replies per family per second can take longer than the kernel's
 * 30-second flow timeout at 4096 entries, expiring active hardware rules
 * and queuing unnecessary destroys and creates behind the message mutex.
 */
static int nss_flow_sweep_rounds(bool v6)
{
	return 8 * DIV_ROUND_UP(READ_ONCE(nss_flow_limit[v6]),
			       NSS_FLOW_BASE_CONN);
}

static void nss_flow_sweep_v4(struct nss_core *core)
{
	struct nss_ipv4_msg *m __free(kfree) =
		kzalloc(NSS_EMPTY_BUFFER_SIZE, GFP_KERNEL);
	struct nss_ipv4_conn_sync_many_msg *s;
	u16 *index = &nss_flow_sweep_index[0];
	int rounds = nss_flow_sweep_rounds(false);
	int i;

	if (!m)
		return;
	s = &m->msg.conn_stats_many;

	do {
		memset(m, 0, sizeof(m->cm) + sizeof(*s));
		m->cm.interface = NSS_INTERFACE_IPV4;
		m->cm.type = NSS_IPV4_TX_CONN_STATS_SYNC_MANY_MSG;
		s->index = *index;
		s->size = NSS_EMPTY_BUFFER_SIZE;
		if (nss_msg_transact(core, m,
				     offsetof(struct nss_ipv4_msg, msg) + sizeof(*s),
				     NSS_EMPTY_BUFFER_SIZE))
			return;
		/* The count is the firmware's; the room is not. */
		for (i = 0; i < s->count &&
		     offsetof(struct nss_ipv4_msg, msg) + sizeof(*s) +
		     (i + 1) * sizeof(s->conn_sync[0]) <= NSS_EMPTY_BUFFER_SIZE;
		     i++)
			nss_flow_sync_mark_v4(&s->conn_sync[i]);
		*index = s->next;
	} while (*index && --rounds);
}

static void nss_flow_sweep_v6(struct nss_core *core)
{
	struct nss_ipv6_msg *m __free(kfree) =
		kzalloc(NSS_EMPTY_BUFFER_SIZE, GFP_KERNEL);
	struct nss_ipv6_conn_sync_many_msg *s;
	u16 *index = &nss_flow_sweep_index[1];
	int rounds = nss_flow_sweep_rounds(true);
	int i;

	if (!m)
		return;
	s = &m->msg.conn_stats_many;

	do {
		memset(m, 0, sizeof(m->cm) + sizeof(*s));
		m->cm.interface = NSS_INTERFACE_IPV6;
		m->cm.type = NSS_IPV6_TX_CONN_STATS_SYNC_MANY_MSG;
		s->index = *index;
		s->size = NSS_EMPTY_BUFFER_SIZE;
		if (nss_msg_transact(core, m,
				     offsetof(struct nss_ipv6_msg, msg) + sizeof(*s),
				     NSS_EMPTY_BUFFER_SIZE))
			return;
		for (i = 0; i < s->count &&
		     offsetof(struct nss_ipv6_msg, msg) + sizeof(*s) +
		     (i + 1) * sizeof(s->conn_sync[0]) <= NSS_EMPTY_BUFFER_SIZE;
		     i++)
			nss_flow_sync_mark_v6(&s->conn_sync[i]);
		*index = s->next;
	} while (*index && --rounds);
}

/* What the kernel asks in place of seeing the packets.
 *
 * A connection the firmware carries stops reaching conntrack, so the kernel
 * would age it out on its own schedule unless something says it is still
 * alive. The kernel asks per flow but the firmware answers per table, so one
 * sweep a second serves every flow's answer.
 */
static void nss_flow_sweep(struct nss_core *core)
{
	if (time_after(jiffies, nss_flow_swept + HZ)) {
		nss_flow_swept = jiffies;
		if (READ_ONCE(nss_flow_installed[0])) {
			nss_flow_sweep_v4(core);
		} else {
			nss_flow_sweep_index[0] = 0;
			nss_flow_sweep_skipped[0]++;
		}
		if (READ_ONCE(nss_flow_installed[1])) {
			nss_flow_sweep_v6(core);
		} else {
			nss_flow_sweep_index[1] = 0;
			nss_flow_sweep_skipped[1]++;
		}
	}
}

static bool nss_flow_cached_stats(unsigned long key, unsigned long *lastused)
{
	struct nss_flow_entry *e;
	bool installed;

	rcu_read_lock();
	e = rhashtable_lookup_fast(&nss_flow_table, &key, nss_flow_ht_params);
	installed = e && READ_ONCE(e->installed);
	*lastused = installed ? READ_ONCE(e->lastused) : 0;
	rcu_read_unlock();
	return installed;
}

static int nss_flow_stats(struct flow_cls_offload *f)
{
	unsigned long key = (unsigned long)nss_flow_of(f->cookie), lastused;

	if (!nss_flow_cached_stats(key, &lastused))
		return -ENOENT;

	/* One caller sweeps; parallel per-flow stats work uses the RCU cache.
	 * A larger table must not turn a firmware sweep into thousands of
	 * sleeping statistics workers behind the global flow mutex.
	 */
	{
		ACQUIRE(mutex_try, sweep_guard)(&nss_flow_lock);
		if (!ACQUIRE_ERR(mutex_try, &sweep_guard)) {
			struct nss_core *core = nss_flow_core;

			if (core && READ_ONCE(core->running))
				nss_flow_sweep(core);
			if (!nss_flow_cached_stats(key, &lastused))
				return -ENOENT;
		}
	}

	flow_stats_update(&f->stats, 0, 0, 0, lastused,
			  FLOW_ACTION_HW_STATS_DELAYED);

	return 0;
}

/* New flows rejected at capacity need neither the serialized message path
 * nor its mutex. Existing entries must still reach refresh/withdrawal, so
 * check the cookie under RCU and repeat admission under the mutex on entry.
 * Receive-side retirement shares the counter lock with this snapshot.
 */
static bool nss_flow_capacity_busy(struct flow_cls_offload *f)
{
	unsigned long key = (unsigned long)nss_flow_of(f->cookie);
	const struct flow_offload_tuple *tuple = (const void *)f->cookie;
	struct nss_flow_entry *entry;
	bool full;

	rcu_read_lock();
	entry = rhashtable_lookup_fast(&nss_flow_table, &key,
				       nss_flow_ht_params);
	full = !entry && nss_flow_admit(tuple->l3proto == NFPROTO_IPV6);
	rcu_read_unlock();

	return full;
}

static int nss_flow_block_cb(enum tc_setup_type type, void *type_data,
			     void *cb_priv)
{
	struct flow_cls_offload *f = type_data;
	struct nss_core *core;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	if (f->command == FLOW_CLS_REPLACE && nss_flow_capacity_busy(f))
		return -ENOSPC;
	if (f->command == FLOW_CLS_STATS)
		return nss_flow_stats(f);

	guard(mutex)(&nss_flow_lock);

	core = nss_flow_core;
	if (!core || !core->running)
		return -EOPNOTSUPP;

	switch (f->command) {
	case FLOW_CLS_REPLACE:
		return nss_flow_replace(core, f);
	case FLOW_CLS_DESTROY:
		return nss_flow_destroy(core, f);
	default:
		return -EOPNOTSUPP;
	}
}

/* Withdraw every routed rule through @dev. The flow table lets go of a
 * device's block before it destroys the connections it offloaded, so those
 * destroys reach no block; the live connections are offered again. A halted
 * core holds no rules, and its halt flushes the table.
 */
void nss_flow_withdraw_dev(const struct net_device *dev)
{
	struct rhashtable_iter iter;
	struct nss_flow_entry *e;

	guard(mutex)(&nss_flow_lock);

	if (!nss_flow_core || !nss_flow_core->running)
		return;

	/* The withdrawal sleeps and a walk resumed after a removal may skip
	 * entries, so each one starts the walk again.
	 */
	do {
		rhashtable_walk_enter(&nss_flow_table, &iter);
		rhashtable_walk_start(&iter);
		while ((e = rhashtable_walk_next(&iter)) != NULL)
			if (!IS_ERR(e) && !e->learned &&
			    (e->dir[0].ifindex == dev->ifindex ||
			     e->dir[1].ifindex == dev->ifindex))
				break;
		rhashtable_walk_stop(&iter);
		rhashtable_walk_exit(&iter);
		if (e)
			nss_flow_forget(nss_flow_core, e);
	} while (e);

	/* Interface numbers may be freed after unbind returns. Stop firmware
	 * before any rule still referring to this interface can outlive it.
	 */
	list_for_each_entry(e, &nss_flow_retire_list, retire_node) {
		if (e->dir[0].ifindex != dev->ifindex &&
		    e->dir[1].ifindex != dev->ifindex)
			continue;
		nss_flow_retire_halts++;
		nss_core_fault(nss_flow_core);
		break;
	}
}

/* Take the flow table's block on @dev. A wireless device's driver forwards
 * the offer from its own traffic-control entry point; raw-IP and USB Ethernet
 * devices use the indirect callback with @cleanup.
 */
static int nss_flow_block(struct net_device *dev, struct flow_block_offload *f,
			  struct Qdisc *sch, void *data,
			  void (*cleanup)(struct flow_block_cb *block_cb))
{
	struct flow_block_cb *block_cb;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &nss_flow_blocks;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, nss_flow_block_cb,
						dev);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}

		if (cleanup)
			block_cb = flow_indr_block_cb_alloc(nss_flow_block_cb,
							    dev, dev, NULL, f,
							    dev, sch, data,
							    &nss_flow_blocks,
							    cleanup);
		else
			block_cb = flow_block_cb_alloc(nss_flow_block_cb, dev,
						       dev, NULL);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);

		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &nss_flow_blocks);
		nss_flow_bound++;
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, nss_flow_block_cb,
						dev);
		if (!block_cb)
			return -ENOENT;

		if (!flow_block_cb_decref(block_cb)) {
			nss_flow_withdraw_dev(dev);
			if (cleanup)
				flow_indr_block_cb_remove(block_cb, f);
			else
				flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
			nss_flow_bound--;
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

int nss_flow_setup_block(struct net_device *dev, struct flow_block_offload *f)
{
	return nss_flow_block(dev, f, NULL, NULL, NULL);
}
EXPORT_SYMBOL_GPL(nss_flow_setup_block);

/* Take raw-IP and USB Ethernet devices whether paired or not: a core restart
 * unpairs them for a while. Reachability is decided per connection.
 */
static int nss_flow_indr_cb(struct net_device *dev, struct Qdisc *sch,
			    void *cb_priv, enum tc_setup_type type,
			    void *type_data, void *data,
			    void (*cleanup)(struct flow_block_cb *block_cb))
{
	if (!dev || type != TC_SETUP_FT ||
	    (dev->type != ARPHRD_RAWIP && !nss_redir_is_usb(dev)))
		return -EOPNOTSUPP;

	return nss_flow_block(dev, type_data, sch, data, cleanup);
}

/* A bridged connection, keyed by its conntrack entry: nothing in it is
 * translated and the frames keep their addresses.
 */
int nss_flow_br_add(struct nf_conn *ct, const struct nss_flow_br_dir *dir,
		    u32 priority)
{
	struct nss_flow_entry *e;
	u32 fw_err = 0;
	int d, ret;

	guard(mutex)(&nss_flow_lock);

	if (!nss_flow_core || !nss_flow_core->running)
		return -ENODEV;
	/* The local bridge flowtable owns both rule and lifetime once set. */
	if (test_bit(IPS_OFFLOAD_BIT, &ct->status))
		return -EOPNOTSUPP;

	ret = nss_flow_admit(nf_ct_l3num(ct) == NFPROTO_IPV6);
	if (ret)
		return ret;

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	e->flow = (unsigned long)ct;
	INIT_LIST_HEAD(&e->retire_node);
	e->bridged = true;
	e->learned = true;
	e->protocol = nf_ct_protonum(ct);
	e->v6 = nf_ct_l3num(ct) == NFPROTO_IPV6;
	e->priority = priority;
	for (d = 0; d < FLOW_OFFLOAD_DIR_MAX; d++) {
		e->dir[d].ifnum = dir[d].ifnum;
		e->dir[d].mtu = dir[d].mtu;
		ether_addr_copy(e->dir[d].mac, dir[d].dmac);
		e->dir[d].seen = true;
		/* A side's tag is what the other direction pushes. */
		e->dir[!d].vid = dir[d].vid;
		e->dir[!d].vtpid = dir[d].vtpid;
	}

	ret = rhashtable_insert_fast(&nss_flow_table, &e->node,
				     nss_flow_ht_params);
	if (ret) {
		kfree(e);
		return ret;
	}

	if (e->protocol == IPPROTO_TCP) {
		spin_lock_bh(&ct->lock);
		ct->proto.tcp.seen[0].flags |= IP_CT_TCP_FLAG_BE_LIBERAL;
		ct->proto.tcp.seen[1].flags |= IP_CT_TCP_FLAG_BE_LIBERAL;
		spin_unlock_bh(&ct->lock);
	}

	ret = e->v6 ? nss_flow_send_v6(nss_flow_core, e, ct, &fw_err) :
		      nss_flow_send_v4(nss_flow_core, e, ct, &fw_err);
	if (ret) {
		nss_flow_forget(nss_flow_core, e);
		return ret;
	}

	nss_flow_sync_install(e);
	WRITE_ONCE(e->lastused, jiffies);

	return 0;
}

void nss_flow_br_del(const struct nf_conn *ct)
{
	unsigned long key = (unsigned long)ct;
	struct nss_flow_entry *e;

	guard(mutex)(&nss_flow_lock);

	e = rhashtable_lookup_fast(&nss_flow_table, &key, nss_flow_ht_params);
	if (e && nss_flow_core)
		nss_flow_forget(nss_flow_core, e);
}

/* When the firmware last reported traffic on a bridged connection, or 0
 * when it holds no rule for it.
 */
unsigned long nss_flow_br_used(const struct nf_conn *ct)
{
	unsigned long key = (unsigned long)ct;
	struct nss_flow_entry *e;

	guard(mutex)(&nss_flow_lock);

	if (!nss_flow_core || !nss_flow_core->running)
		return 0;

	nss_flow_sweep(nss_flow_core);
	e = rhashtable_lookup_fast(&nss_flow_table, &key, nss_flow_ht_params);

	return e && e->installed ? e->lastused : 0;
}

/* Match firmware counters to the immutable tuple sent at rule creation. */
static void nss_flow_tuple_print(struct seq_file *s,
                                const struct nss_flow_entry *e)
{
	if (e->v6) {
		struct in6_addr src, dst;
		int i;

		for (i = 0; i < 4; i++) {
			src.s6_addr32[i] = htonl(e->t6.flow_ip[i]);
			dst.s6_addr32[i] = htonl(e->t6.return_ip[i]);
		}
		seq_printf(s, " [%pI6c]:%u -> [%pI6c]:%u",
			   &src, e->t6.flow_ident, &dst, e->t6.return_ident);
	} else {
		__be32 src = htonl(e->t4.flow_ip);
		__be32 dst = htonl(e->t4.return_ip);

		seq_printf(s, " %pI4:%u -> %pI4:%u",
			   &src, e->t4.flow_ident, &dst, e->t4.return_ident);
	}
}

void nss_flow_print(struct seq_file *s)
{
	struct nss_flow_buffers buffers;
	struct rhashtable_iter iter;
	struct nss_flow_entry *e;
	struct nss_flow_node *nd;
	int i;

	guard(mutex)(&nss_flow_lock);
	seq_printf(s, "bound %u pushed %u last-ifindex %d\n",
		   nss_flow_bound, nss_flow_pushed, nss_flow_last_ifindex);
	scoped_guard(spinlock_bh, &nss_flow_sync_lock)
		buffers = nss_flow_buffers;
	seq_printf(s, "buffers syncs %llu payload_free %u payload_failed %llu pbuf_ocm %u/%u pbuf_ddr %u/%u pbuf_failed %llu queue_dropped %llu\n",
		   buffers.syncs, buffers.payload_free, buffers.payload_failed,
		   buffers.ocm_free, buffers.ocm_total,
		   buffers.ddr_free, buffers.ddr_total,
		   buffers.pbuf_failed, buffers.queue_dropped);
	seq_puts(s, "rejected");
	for (i = 0; i < NSS_FLOW_REJECT_MAX; i++)
		seq_printf(s, " %s %u", nss_flow_reject_name[i],
			   nss_flow_reject[i]);
	seq_putc(s, '\n');
	for (i = 0; i < ARRAY_SIZE(nss_flow_pressure); i++) {
		const struct nss_flow_pressure *p = &nss_flow_pressure[i];
		unsigned long remaining = p->backoff &&
			time_before(jiffies, p->retry_at) ? p->retry_at - jiffies : 0;

		seq_printf(s, "resource IPv%u installed %u limit %u full %u refused %u deferred %u retry_ms %u\n",
			   i ? 6 : 4, READ_ONCE(nss_flow_installed[i]),
			   READ_ONCE(nss_flow_limit[i]), READ_ONCE(nss_flow_full[i]),
			   p->refused, p->deferred, jiffies_to_msecs(remaining));
	}
	seq_printf(s, "capacity requested %u table_bytes IPv4 %u IPv6 %u\n",
		   NSS_FLOW_CONN, nss_flow_table_bytes[0],
		   nss_flow_table_bytes[1]);
	seq_printf(s, "retirement pending_create %u/%u retiring %u/%u errors %llu recovered %llu core_resets %llu\n",
		   READ_ONCE(nss_flow_pending[0]), READ_ONCE(nss_flow_pending[1]),
		   nss_flow_retiring[0], nss_flow_retiring[1],
		   nss_flow_retire_errors, nss_flow_retire_recovered,
		   nss_flow_retire_halts);
	seq_printf(s, "sweep empty_skipped %llu/%llu cursor %u/%u\n",
		   nss_flow_sweep_skipped[0], nss_flow_sweep_skipped[1],
		   nss_flow_sweep_index[0], nss_flow_sweep_index[1]);
	list_for_each_entry(e, &nss_flow_retire_list, retire_node) {
		seq_printf(s, "retiring IPv%u installed %u pending %u attempts %u errno %d",
			   e->v6 ? 6 : 4, READ_ONCE(e->installed), e->pending,
			   e->retire_attempts, e->retire_errno);
		nss_flow_tuple_print(s, e);
		seq_putc(s, '\n');
	}

	for (nd = nss_flow_nodes; nd < nss_flow_nodes + ARRAY_SIZE(nss_flow_nodes); nd++) {
		seq_printf(s, "node %s rx %u tx %u drop %u/%u/%u/%u exceptions",
			   nd->name, nd->stats.rx_packets, nd->stats.tx_packets,
			   nd->stats.rx_dropped[0], nd->stats.rx_dropped[1],
			   nd->stats.rx_dropped[2], nd->stats.rx_dropped[3]);
		for (i = 0; i < nd->n; i++)
			if (nd->excep[i])
				seq_printf(s, " %d:%u", i, nd->excep[i]);
		seq_putc(s, '\n');
	}

	for (i = 0; i < ARRAY_SIZE(nss_flow_rej) && i < nss_flow_rej_seen; i++) {
		const struct nss_flow_reject_rec *r = &nss_flow_rej[i];

		seq_printf(s, "reject %s dir %u proto %u ",
			   nss_flow_reject_name[r->why], r->dir, r->proto);
		if (r->l3proto == NFPROTO_IPV6)
			seq_printf(s, "[%pI6c]:%u -> [%pI6c]:%u",
				   &r->sip.in6, ntohs(r->sport),
				   &r->dip.in6, ntohs(r->dport));
		else
			seq_printf(s, "%pI4:%u -> %pI4:%u",
				   &r->sip.in, ntohs(r->sport),
				   &r->dip.in, ntohs(r->dport));
		seq_printf(s, " detail %u\n", r->detail);
	}

	rhashtable_walk_enter(&nss_flow_table, &iter);
	rhashtable_walk_start(&iter);
	while ((e = rhashtable_walk_next(&iter)) != NULL) {
		if (IS_ERR(e))
			continue;
		seq_printf(s, "proto %u %s%s if %d/%d prio %u installed %d age %ums fw flow rx %llu tx %llu return rx %llu tx %llu",
			   e->protocol, e->v6 ? "v6" : "v4",
			   e->bridged ? " bridged" : "",
			   e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].ifnum,
			   e->dir[FLOW_OFFLOAD_DIR_REPLY].ifnum,
			   e->priority, e->installed,
			   jiffies_to_msecs(jiffies - e->lastused),
			   e->cnt[0], e->cnt[1], e->cnt[2], e->cnt[3]);
		if (e->installed)
			nss_flow_tuple_print(s, e);
		seq_putc(s, '\n');
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
}

/* One installed rule is one bidirectional connection. No tuple formatting
 * or firmware transaction is needed by periodic status readers.
 */
ssize_t nss_flow_summary(char *buf)
{
	struct rhashtable_iter iter;
	struct nss_flow_entry *e;
	unsigned int v4 = 0, v6 = 0;

	guard(mutex)(&nss_flow_lock);
	rhashtable_walk_enter(&nss_flow_table, &iter);
	rhashtable_walk_start(&iter);
	while ((e = rhashtable_walk_next(&iter))) {
		if (IS_ERR(e) || !e->installed)
			continue;
		if (e->v6)
			v6++;
		else
			v4++;
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
	return sysfs_emit(buf, "%u %u %u %u\n", v4, v6, v4 + v6,
			  READ_ONCE(nss_flow_limit[0]) + READ_ONCE(nss_flow_limit[1]));
}

/* Halting a core takes every rule it held with it, so the entries only
 * mislead after that; the withdrawals they would send have nowhere to go.
 */
void nss_flow_flush(void)
{
	struct rhashtable_iter iter;
	struct nss_flow_entry *e, *tmp;

	cancel_delayed_work_sync(&nss_flow_retire_worker);
	guard(mutex)(&nss_flow_lock);

	rhashtable_walk_enter(&nss_flow_table, &iter);
	rhashtable_walk_start(&iter);
	while ((e = rhashtable_walk_next(&iter)) != NULL) {
		if (IS_ERR(e))
			continue;
		nss_flow_sync_unindex(e);
		rhashtable_remove_fast(&nss_flow_table, &e->node,
				       nss_flow_ht_params);
		kfree_rcu(e, rcu);
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
	list_for_each_entry_safe(e, tmp, &nss_flow_retire_list, retire_node) {
		nss_flow_sync_unindex(e);
		list_del(&e->retire_node);
		kfree_rcu(e, rcu);
	}
	memset(nss_flow_retiring, 0, sizeof(nss_flow_retiring));

	memset(nss_flow_pppoe_tbl, 0, sizeof(nss_flow_pppoe_tbl));
	memset(nss_flow_pressure, 0, sizeof(nss_flow_pressure));
	memset(nss_flow_full, 0, sizeof(nss_flow_full));
	scoped_guard(spinlock_bh, &nss_flow_sync_lock) {
		memset(nss_flow_pending, 0, sizeof(nss_flow_pending));
		memset(&nss_flow_buffers, 0, sizeof(nss_flow_buffers));
		memset(nss_flow_table_bytes, 0, sizeof(nss_flow_table_bytes));
		nss_flow_limit[0] = NSS_FLOW_BASE_CONN;
		nss_flow_limit[1] = NSS_FLOW_BASE_CONN;
	}
}

int nss_flow_init(void)
{
	int ret;

	ret = rhashtable_init(&nss_flow_table, &nss_flow_ht_params);
	if (ret)
		return ret;

	ret = flow_indr_dev_register(nss_flow_indr_cb, &nss_flow_blocks);
	if (ret) {
		rhashtable_destroy(&nss_flow_table);
		return ret;
	}

	nss_flow_ready = true;

	return 0;
}

static void nss_flow_free(void *ptr, void *arg)
{
	nss_flow_sync_unindex(ptr);
	kfree(ptr);
}

void nss_flow_exit(void)
{
	if (!nss_flow_ready)
		return;

	cancel_delayed_work_sync(&nss_flow_retire_worker);
	WARN_ON(!list_empty(&nss_flow_retire_list));
	flow_indr_dev_unregister(nss_flow_indr_cb, &nss_flow_blocks, NULL);
	rhashtable_free_and_destroy(&nss_flow_table, nss_flow_free, NULL);
	nss_flow_ready = false;
}
