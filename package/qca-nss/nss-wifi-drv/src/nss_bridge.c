// SPDX-License-Identifier: GPL-2.0-only
/* Giving the firmware a connection the bridge forwards between a radio and a
 * port.
 *
 * Where no kernel bridge flowtable owns a connection, learn it here. The
 * firmware has no MAC forwarder: it carries a connection as a 5-tuple rule that
 * changes neither addresses nor TTL. So the connection is learned here, from
 * bridge conntrack and the frames the bridge forwards: each direction gives
 * the port it arrives on and the address it is sent to, and once both
 * directions are known the rule is pushed from process context.
 *
 * A pushed connection stops reaching conntrack, so the firmware's traffic
 * reports keep the entry alive. The rule goes when the entry dies, when the
 * firmware reports it idle, when a port leaves the firmware, and when the
 * bridge learns an end station's address on a different port.
 */

#include <linux/cleanup.h>
#include <linux/etherdevice.h>
#include <linux/hashtable.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/netfilter.h>
#include <linux/moduleparam.h>
#include <linux/netfilter_bridge.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_helper.h>
#include <net/switchdev.h>

#include "nss_drv.h"

#define NSS_BR_MAX_PRIORITY	15
#define NSS_BR_IDLE		(30 * HZ)
/* ponytail: a memory bound, not the firmware's table size */
#define NSS_BR_MAX		1024

enum nss_br_state {
	NSS_BR_LEARN,
	NSS_BR_READY,		/* both directions known, not yet pushed */
	NSS_BR_PUSHED,
	NSS_BR_SKIP,		/* not offloaded for the life of the entry */
};

enum nss_br_why {
	NSS_BR_PORT,		/* a port the firmware does not own */
	NSS_BR_VLAN,
	NSS_BR_CT,		/* translated or helped */
	NSS_BR_FULL,
	NSS_BR_FIRMWARE,
	NSS_BR_DYING,
	NSS_BR_IDLE_DROP,
	NSS_BR_MOVED,
	NSS_BR_GONE,		/* a port left the firmware */
	NSS_BR_POLICY,		/* excluded by the firewall bridge scope */
	NSS_BR_WHY_MAX,
};

static const char *const nss_br_why_name[] = {
	"port", "vlan", "ct", "full", "firmware",
	"dying", "idle", "moved", "gone", "policy",
};

struct nss_br_conn {
	struct hlist_node hnode;
	struct rcu_head rcu;
	struct list_head todo;
	struct nf_conn *ct;
	struct nss_flow_br_dir dir[IP_CT_DIR_MAX];
	int ifindex[IP_CT_DIR_MAX];
	u8 smac[IP_CT_DIR_MAX][ETH_ALEN];
	u8 seen;
	u8 state;
	bool moved;
	u32 priority;
	u32 timeout;
	unsigned long used;
};

static DEFINE_HASHTABLE(nss_br_tbl, 8);
static DEFINE_SPINLOCK(nss_br_lock);
static struct nss_core *nss_br_core;
static u32 nss_br_n, nss_br_pushed, nss_br_why[NSS_BR_WHY_MAX];

static void nss_br_gc(struct work_struct *work);
static DECLARE_DELAYED_WORK(nss_br_work, nss_br_gc);

/* With a firewall bridge selection, let its flowtable own the allowed
 * bridges. The fallback learner must not bypass an explicitly excluded one.
 */
static bool nss_br_enabled = true;

static int nss_br_set_enabled(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_bool(val, kp);

	if (!ret && READ_ONCE(nss_br_core))
		mod_delayed_work(system_percpu_wq, &nss_br_work, 0);
	return ret;
}

static const struct kernel_param_ops nss_br_enabled_ops = {
	.set = nss_br_set_enabled,
	.get = param_get_bool,
};
module_param_cb(bridge_offload, &nss_br_enabled_ops, &nss_br_enabled, 0600);
MODULE_PARM_DESC(bridge_offload, "Fallback bridge learning; firewall selections use nf_flowtable instead");


static struct nss_br_conn *nss_br_find(const struct nf_conn *ct)
{
	struct nss_br_conn *e;

	hash_for_each_possible_rcu(nss_br_tbl, e, hnode, (unsigned long)ct)
		if (e->ct == ct)
			return e;

	return NULL;
}

/* The device a bridge port stands on, and the tag a VLAN port adds to it.
 * NULL for a tag on a tag: the rule carries one per side.
 */
static struct net_device *nss_br_real(struct net_device *dev, u16 *vid,
				      u16 *vtpid)
{
	*vid = 0;
	*vtpid = 0;
	if (!is_vlan_dev(dev))
		return dev;

	*vid = vlan_dev_vlan_id(dev);
	*vtpid = ntohs(vlan_dev_vlan_proto(dev));
	dev = vlan_dev_real_dev(dev);

	return is_vlan_dev(dev) ? NULL : dev;
}

/* Why this frame's connection cannot be offloaded, or NSS_BR_WHY_MAX. */
static int nss_br_check(struct sk_buff *skb, const struct nf_conn *ct,
			const struct nf_hook_state *state,
			struct nss_flow_br_dir *in)
{
	struct net_device *rin, *rout;
	u16 vid, vtpid;
	u32 mtu;

	if (ct->status & IPS_NAT_MASK || nfct_help(ct))
		return NSS_BR_CT;
	/* The bridge has applied the egress port's tagging by now, and the
	 * rule carries no bridge tag.
	 */
	if (skb_vlan_tag_present(skb))
		return NSS_BR_VLAN;
	/* A VLAN port is its device with the tag the rule then carries. */
	rin = nss_br_real(state->in, &in->vid, &in->vtpid);
	rout = nss_br_real(state->out, &vid, &vtpid);
	if (!rin || !rout)
		return NSS_BR_VLAN;
	in->ifnum = nss_flow_ifnum(nss_br_core, rin->ifindex, &in->mtu);
	if (in->ifnum < 0 || nss_flow_ifnum(nss_br_core, rout->ifindex, &mtu) < 0)
		return NSS_BR_PORT;

	return NSS_BR_WHY_MAX;
}

static unsigned int nss_br_hook(void *priv, struct sk_buff *skb,
				const struct nf_hook_state *state)
{
	struct nss_flow_br_dir in = {};
	struct net_device *rin, *rout;
	enum ip_conntrack_info ctinfo;
	u16 vid, vtpid;
	struct nss_br_conn *e;
	struct nf_conn *ct;
	bool kick = false;
	int dir, why;
	u8 proto;

	if (!READ_ONCE(nss_br_enabled))
		return NF_ACCEPT;

	ct = nf_ct_get(skb, &ctinfo);
	if (!ct || test_bit(IPS_OFFLOAD_BIT, &ct->status) ||
	    !is_unicast_ether_addr(eth_hdr(skb)->h_dest))
		return NF_ACCEPT;

	e = nss_br_find(ct);
	if (e && READ_ONCE(e->state) != NSS_BR_LEARN)
		return NF_ACCEPT;

	/* Wired to wired is the switch's; a VLAN port is as wireless as the
	 * device under it.
	 */
	rin = nss_br_real(state->in, &vid, &vtpid);
	rout = nss_br_real(state->out, &vid, &vtpid);
	if ((!rin || !nss_dev_is_wifi(rin)) && (!rout || !nss_dev_is_wifi(rout)))
		return NF_ACCEPT;

	/* A UDP sender may fall silent once the reply starts, so its
	 * direction is learned before there is one.
	 */
	proto = nf_ct_protonum(ct);
	if (proto != IPPROTO_UDP &&
	    (proto != IPPROTO_TCP ||
	     READ_ONCE(ct->proto.tcp.state) != TCP_CONNTRACK_ESTABLISHED))
		return NF_ACCEPT;

	why = nss_br_check(skb, ct, state, &in);
	dir = CTINFO2DIR(ctinfo);

	spin_lock_bh(&nss_br_lock);

	e = nss_br_find(ct);
	if (!e) {
		if (nss_br_n >= NSS_BR_MAX) {
			nss_br_why[NSS_BR_FULL]++;
			goto out;
		}
		e = kzalloc(sizeof(*e), GFP_ATOMIC);
		if (!e)
			goto out;
		nf_conntrack_get(&ct->ct_general);
		e->ct = ct;
		hash_add_rcu(nss_br_tbl, &e->hnode, (unsigned long)ct);
		nss_br_n++;
	}

	if (e->state != NSS_BR_LEARN)
		goto out;

	if (why != NSS_BR_WHY_MAX) {
		nss_br_why[why]++;
		WRITE_ONCE(e->state, NSS_BR_SKIP);
		goto out;
	}

	ether_addr_copy(in.dmac, eth_hdr(skb)->h_dest);
	e->dir[dir] = in;
	e->ifindex[dir] = state->in->ifindex;
	ether_addr_copy(e->smac[dir], eth_hdr(skb)->h_source);
	if (dir == IP_CT_DIR_ORIGINAL)
		e->priority = min_t(u32, skb->priority, NSS_BR_MAX_PRIORITY);
	e->seen |= BIT(dir);
	if (e->seen == (BIT(IP_CT_DIR_ORIGINAL) | BIT(IP_CT_DIR_REPLY))) {
		smp_store_release(&e->state, NSS_BR_READY);
		kick = true;
	}

out:
	spin_unlock_bh(&nss_br_lock);

	if (kick)
		mod_delayed_work(system_percpu_wq, &nss_br_work, 0);

	return NF_ACCEPT;
}

static const struct nf_hook_ops nss_br_ops = {
	.hook		= nss_br_hook,
	.pf		= NFPROTO_BRIDGE,
	.hooknum	= NF_BR_FORWARD,
	.priority	= NF_BR_PRI_LAST,
};

/* An end station seen on another port: whatever the firmware still sends to
 * the old one is lost.
 */
static int nss_br_fdb_event(struct notifier_block *nb, unsigned long event,
			    void *ptr)
{
	struct switchdev_notifier_fdb_info *fi = ptr;
	struct net_device *dev = switchdev_notifier_info_to_dev(ptr);
	struct nss_br_conn *e;
	bool kick = false;
	int bkt, d;

	if (event != SWITCHDEV_FDB_ADD_TO_DEVICE || fi->is_local)
		return NOTIFY_DONE;

	spin_lock_bh(&nss_br_lock);
	hash_for_each(nss_br_tbl, bkt, e, hnode) {
		for (d = 0; d < IP_CT_DIR_MAX; d++) {
			if (!(e->seen & BIT(d)) ||
			    e->ifindex[d] == dev->ifindex ||
			    !ether_addr_equal(e->smac[d], fi->addr))
				continue;
			WRITE_ONCE(e->moved, true);
			kick = true;
		}
	}
	spin_unlock_bh(&nss_br_lock);

	if (kick)
		mod_delayed_work(system_percpu_wq, &nss_br_work, 0);

	return NOTIFY_DONE;
}

static struct notifier_block nss_br_nb = {
	.notifier_call = nss_br_fdb_event,
};

static void nss_br_free(struct nss_br_conn *e)
{
	spin_lock_bh(&nss_br_lock);
	hash_del_rcu(&e->hnode);
	nss_br_n--;
	spin_unlock_bh(&nss_br_lock);

	nf_ct_put(e->ct);
	kfree_rcu(e, rcu);
}

static void nss_br_push(struct nss_br_conn *e)
{
	int ret;

	e->timeout = nf_ct_expires(e->ct);
	ret = nss_flow_br_add(e->ct, e->dir, e->priority);
	/* Shared pressure backoff bounds the firmware probes. Keep this
	 * connection in software while GC retries it once per second; an
	 * expired conntrack entry is still removed by the normal lifetime.
	 */
	if (ret == -EAGAIN || ret == -ENOSPC)
		return;
	if (ret) {
		nss_br_why[NSS_BR_FIRMWARE]++;
		WRITE_ONCE(e->state, NSS_BR_SKIP);
		return;
	}

	e->used = jiffies;
	nss_br_pushed++;
	WRITE_ONCE(e->state, NSS_BR_PUSHED);
}

/* A deferred create must still name the same live firmware ports. */
static int nss_br_ports(struct nss_br_conn *e)
{
	u32 mtu;
	int d;

	for (d = 0; d < IP_CT_DIR_MAX; d++) {
		struct net_device *dev, *real;
		u16 vid, vtpid;
		guard(rcu)();

		/* Keep the bridge-port identity for FDB moves, but resolve
		 * its VLAN exactly as at creation before checking the NSS node.
		 */
		dev = dev_get_by_index_rcu(&init_net, e->ifindex[d]);
		real = dev ? nss_br_real(dev, &vid, &vtpid) : NULL;
		if (!real || vid != e->dir[d].vid ||
		    vtpid != e->dir[d].vtpid ||
		    nss_flow_ifnum(nss_br_core, real->ifindex, &mtu) !=
		    e->dir[d].ifnum)
			return NSS_BR_GONE;
	}

	return NSS_BR_WHY_MAX;
}

/* Refresh conntrack while the firmware reports traffic. */
static int nss_br_keep(struct nss_br_conn *e)
{
	unsigned long used;
	int why;

	why = nss_br_ports(e);
	if (why != NSS_BR_WHY_MAX)
		return why;

	used = nss_flow_br_used(e->ct);
	if (!used)
		return NSS_BR_GONE;
	if (used != e->used) {
		e->used = used;
		nf_ct_refresh(e->ct, e->timeout);
	}
	if (time_after(jiffies, e->used + NSS_BR_IDLE))
		return NSS_BR_IDLE_DROP;

	return NSS_BR_WHY_MAX;
}

static void nss_br_tend(struct nss_br_conn *e)
{
	u8 state = smp_load_acquire(&e->state);
	int why = NSS_BR_WHY_MAX;

	if (!READ_ONCE(nss_br_enabled))
		why = NSS_BR_POLICY;
	else if (nf_ct_is_dying(e->ct))
		why = NSS_BR_DYING;
	else if (test_bit(IPS_OFFLOAD_BIT, &e->ct->status))
		why = NSS_BR_CT;
	else if (READ_ONCE(e->moved))
		why = NSS_BR_MOVED;
	else if (state == NSS_BR_READY) {
		why = nss_br_ports(e);
		if (why == NSS_BR_WHY_MAX)
			nss_br_push(e);
	}
	else if (state == NSS_BR_PUSHED)
		why = nss_br_keep(e);

	if (why == NSS_BR_WHY_MAX)
		return;

	if (state == NSS_BR_PUSHED) {
		nss_flow_br_del(e->ct);
		nss_br_why[why]++;
	}
	nss_br_free(e);
}

/* Only this work and the stop path free entries, so the list it collects
 * stays valid after the lock is dropped.
 */
static void nss_br_gc(struct work_struct *work)
{
	struct nss_br_conn *e, *n;
	LIST_HEAD(todo);
	int bkt;

	spin_lock_bh(&nss_br_lock);
	hash_for_each(nss_br_tbl, bkt, e, hnode)
		list_add_tail(&e->todo, &todo);
	spin_unlock_bh(&nss_br_lock);

	list_for_each_entry_safe(e, n, &todo, todo)
		nss_br_tend(e);

	schedule_delayed_work(&nss_br_work, HZ);
}

/* Bridged offload is optional: without bridge conntrack the core runs
 * without it.
 */
void nss_bridge_start(struct nss_core *core)
{
	int ret;

	if (nss_br_core)
		return;

	ret = nf_ct_netns_get(&init_net, NFPROTO_BRIDGE);
	if (ret) {
		dev_info(core->dev, "no bridge conntrack, bridged offload off: %d\n",
			 ret);
		return;
	}

	nss_br_core = core;
	ret = nf_register_net_hook(&init_net, &nss_br_ops);
	if (ret)
		goto put;
	ret = register_switchdev_notifier(&nss_br_nb);
	if (ret)
		goto unhook;

	schedule_delayed_work(&nss_br_work, HZ);

	return;

unhook:
	nf_unregister_net_hook(&init_net, &nss_br_ops);
put:
	nss_br_core = NULL;
	nf_ct_netns_put(&init_net, NFPROTO_BRIDGE);
	dev_warn(core->dev, "bridged offload off: %d\n", ret);
}

/* The core is down, so its rules are already gone with it. */
void nss_bridge_stop(void)
{
	struct nss_br_conn *e;
	struct hlist_node *tmp;
	int bkt;

	if (!nss_br_core)
		return;

	nf_unregister_net_hook(&init_net, &nss_br_ops);
	unregister_switchdev_notifier(&nss_br_nb);
	cancel_delayed_work_sync(&nss_br_work);

	hash_for_each_safe(nss_br_tbl, bkt, tmp, e, hnode) {
		hash_del(&e->hnode);
		nf_ct_put(e->ct);
		kfree(e);
	}
	nss_br_n = 0;

	nss_br_core = NULL;
	nf_ct_netns_put(&init_net, NFPROTO_BRIDGE);
}

void nss_bridge_print(struct seq_file *s)
{
	u32 cnt[NSS_BR_SKIP + 1] = {};
	struct nss_br_conn *e;
	int bkt, i;

	spin_lock_bh(&nss_br_lock);
	hash_for_each(nss_br_tbl, bkt, e, hnode)
		cnt[e->state]++;
	spin_unlock_bh(&nss_br_lock);

	seq_printf(s, "bridged on %d pushed %u entries %u learn %u ready %u active %u skip %u\n",
		   !!nss_br_core, nss_br_pushed, nss_br_n, cnt[NSS_BR_LEARN],
		   cnt[NSS_BR_READY], cnt[NSS_BR_PUSHED], cnt[NSS_BR_SKIP]);
	seq_puts(s, "bridged rejected");
	for (i = 0; i < NSS_BR_WHY_MAX; i++) {
		if (i == NSS_BR_DYING)
			seq_puts(s, "\nbridged removed");
		seq_printf(s, " %s %u", nss_br_why_name[i], nss_br_why[i]);
	}
	seq_putc(s, '\n');
}
