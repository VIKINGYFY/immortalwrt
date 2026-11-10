// SPDX-License-Identifier: GPL-2.0-only
/* 802.11s on the firmware lines that forward a mesh themselves.
 *
 * A mesh interface is three firmware nodes: the virtual device that carries
 * its 802.11 frames, an encapsulation node that takes ethernet in and hands
 * mesh frames to that device, and a decapsulation node the device hands its
 * receive to. The two mesh nodes keep the path tables. The subsystem decides
 * every path, so its table is mirrored into the firmware, and the firmware
 * reports what only it sees - a path in use, a destination it has no path to,
 * a proxy it learned from a frame.
 *
 * The subsystem changes a path with spinlocks held, so a change is queued
 * here and sent from process context, in order.
 */

#include <linux/cleanup.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "nss_drv.h"
#include <linux/soc/qcom/nss_wifi.h>

#define NSS_MESH_MAX		4
#define NSS_MESH_TTL		31
#define NSS_MESH_REFRESH_MS	1000
#define NSS_MESH_QUEUE_MAX	1024

/* How often the proxy table is read back. A proxy whose traffic the firmware
 * carries is never seen by the subsystem, which would otherwise expire it.
 */
#define NSS_MESH_AGE_MS		(60 * MSEC_PER_SEC)

struct nss_mesh {
	struct nss_core *core;
	struct net_device *dev;
	const struct nss_wifi_mesh_ops *ops;	/* NULL: slot free */
	void *priv;
	int link;
	int encap;
	int decap;
	u8 ttl;
	u32 refresh;
	bool forward;
	u64 dropped;
	struct nss_wifi_mesh_stats_sync_msg stats[2];	/* encap, decap */
};

static struct nss_mesh nss_mesh_tbl[NSS_MESH_MAX];
static DEFINE_MUTEX(nss_mesh_lock);

#define NSS_MESH_CMD_DUMP	(NSS_WIFI_MESH_MPP_DEL + 1)

struct nss_mesh_cmd {
	struct list_head node;
	int link;
	u32 cmd;
	struct nss_wifi_mesh_path p;
};

static LIST_HEAD(nss_mesh_queue);
static u32 nss_mesh_queued;
static u64 nss_mesh_overflow;
static DEFINE_SPINLOCK(nss_mesh_queue_lock);

static void nss_mesh_work_fn(struct work_struct *work);
static DECLARE_WORK(nss_mesh_work, nss_mesh_work_fn);
static void nss_mesh_age_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(nss_mesh_age, nss_mesh_age_fn);

static struct nss_mesh *nss_mesh_find(int link)
{
	int i;

	for (i = 0; i < NSS_MESH_MAX; i++)
		if (READ_ONCE(nss_mesh_tbl[i].ops) && nss_mesh_tbl[i].link == link)
			return &nss_mesh_tbl[i];

	return NULL;
}

static int nss_mesh_msg(struct nss_core *core, int if_num, u32 type,
			const void *body, size_t len)
{
	struct nss_wifi_mesh_msg *m __free(kfree) = kzalloc(sizeof(*m),
							    GFP_KERNEL);
	int ret;

	if (!m)
		return -ENOMEM;

	m->cm.interface = if_num;
	m->cm.type = type;
	if (body)
		memcpy(&m->msg, body, len);

	ret = nss_msg_send(core, m, offsetof(struct nss_wifi_mesh_msg, msg) +
				    len);
	if (m->cm.response == NSS_CMN_RESPONSE_EMSG &&
	    ((type == NSS_WIFI_MESH_MSG_MPATH_UPDATE &&
	      m->cm.error == NSS_WIFI_MESH_ERROR_PATH_NOT_FOUND) ||
	     (type == NSS_WIFI_MESH_MSG_PROXY_PATH_UPDATE &&
	      m->cm.error == NSS_WIFI_MESH_ERROR_PROXY_PATH_NOT_FOUND)))
		return -ENOENT;
	if (!ret && m->cm.error)
		ret = -EIO;

	return ret;
}

static int nss_mesh_node(struct nss_core *core, u32 type)
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

static void nss_mesh_node_free(struct nss_core *core, int if_num, u32 type)
{
	struct nss_dynamic_interface_msg d = {};

	if (if_num < 0)
		return;

	d.cm.interface = NSS_INTERFACE_DYNAMIC;
	d.cm.type = NSS_DYNAMIC_INTERFACE_DEALLOC_NODE;
	d.msg.dealloc_node.type = type;
	d.msg.dealloc_node.if_num = if_num;
	nss_msg_send(core, &d, sizeof(d));
}

/* Lines that cannot forward a mesh refuse to say so. */
bool nss_mesh_probe(struct nss_core *core)
{
	struct nss_wifili_msg *m __free(kfree) = kzalloc(sizeof(*m),
							 GFP_KERNEL);
	int ret;

	if (!m)
		return false;

	m->cm.interface = NSS_INTERFACE_WIFILI;
	m->cm.type = NSS_WIFILI_SEND_MESH_CAPABILITY_INFO;
	ret = nss_msg_send(core, m, offsetof(struct nss_wifili_msg, msg) +
				    sizeof(m->msg.cap_info));

	dev_info(core->dev, "mesh forwarding: %s\n", ret ? "no" : "yes");

	return !ret;
}

static int nss_mesh_configure(struct nss_mesh *m, u32 flags)
{
	struct nss_wifi_mesh_config_msg c = {};
	int ret;

	ether_addr_copy(c.local_mac_addr, m->dev->dev_addr);
	c.ttl = m->ttl;
	c.mesh_path_refresh_time = m->refresh;
	c.mpp_learning_mode = NSS_WIFI_MESH_MPP_LEARNING_MODE_INDEPENDENT_NSS;
	c.block_mesh_forwarding = !m->forward;
	c.config_flags = flags;

	c.sibling_ifnum = m->decap;
	ret = nss_mesh_msg(m->core, m->encap,
			   NSS_WIFI_MESH_MSG_INTERFACE_CONFIGURE, &c, sizeof(c));
	if (ret)
		return ret;

	c.sibling_ifnum = m->encap;
	return nss_mesh_msg(m->core, m->decap,
			    NSS_WIFI_MESH_MSG_INTERFACE_CONFIGURE, &c, sizeof(c));
}

static int nss_mesh_open(struct nss_mesh *m, u32 type)
{
	size_t len = type == NSS_IF_OPEN ? sizeof(struct nss_if_open) :
					   sizeof(struct nss_if_close);
	int ret;

	ret = nss_mesh_msg(m->core, m->encap, type, NULL, len);
	if (ret)
		return ret;

	return nss_mesh_msg(m->core, m->decap, type, NULL, len);
}

int nss_mesh_add(struct nss_core *core, struct net_device *dev, int link,
		 const u8 *mac, const struct nss_wifi_mesh_ops *ops, void *priv)
{
	struct nss_mesh *m = NULL;
	int i, ret = 0;

	guard(mutex)(&nss_mesh_lock);

	for (i = 0; i < NSS_MESH_MAX; i++)
		if (!nss_mesh_tbl[i].ops && !nss_mesh_tbl[i].dev)
			m = &nss_mesh_tbl[i];
	if (!m)
		return -ENOSPC;

	memset(m, 0, sizeof(*m));
	m->core = core;
	m->dev = dev;
	m->link = link;
	m->ttl = NSS_MESH_TTL;
	m->refresh = NSS_MESH_REFRESH_MS;
	m->forward = true;
	m->decap = -1;

	m->encap = nss_mesh_node(core, NSS_DYNAMIC_INTERFACE_TYPE_WIFI_MESH_INNER);
	if (m->encap < 0)
		goto fail;

	m->decap = nss_mesh_node(core, NSS_DYNAMIC_INTERFACE_TYPE_WIFI_MESH_OUTER);
	if (m->decap < 0)
		goto fail;

	ret = nss_mesh_configure(m, NSS_WIFI_MESH_CONFIG_FLAG_TTL_VALID |
				 NSS_WIFI_MESH_CONFIG_FLAG_MPATH_REFRESH_VALID |
				 NSS_WIFI_MESH_CONFIG_FLAG_MPP_LEARNING_MODE_VALID |
				 NSS_WIFI_MESH_CONFIG_FLAG_BLOCK_MESH_FWD_VALID |
				 NSS_WIFI_MESH_CONFIG_FLAG_LOCAL_MAC_VALID |
				 NSS_WIFI_MESH_CONFIG_FLAG_SIBLING_IF_NUM_VALID);
	if (ret)
		goto fail;

	/* The decapsulation node delivers ethernet for this host. The
	 * encapsulation node is bound too, so that a flow out of the mesh
	 * resolves to the node that builds the mesh header.
	 */
	dev_hold(dev);
	rcu_assign_pointer(core->iface[m->encap], dev);
	dev_hold(dev);
	rcu_assign_pointer(core->iface[m->decap], dev);

	m->priv = priv;
	smp_store_release(&m->ops, ops);

	mod_delayed_work(system_wq, &nss_mesh_age,
			 msecs_to_jiffies(NSS_MESH_AGE_MS));

	dev_info(core->dev, "mesh %s: link %d encap %d decap %d\n",
		 netdev_name(dev), link, m->encap, m->decap);

	return 0;

fail:
	ret = m->encap < 0 ? m->encap : m->decap < 0 ? m->decap : ret;
	nss_mesh_node_free(core, m->decap,
			   NSS_DYNAMIC_INTERFACE_TYPE_WIFI_MESH_OUTER);
	nss_mesh_node_free(core, m->encap,
			   NSS_DYNAMIC_INTERFACE_TYPE_WIFI_MESH_INNER);
	m->dev = NULL;

	return ret;
}

/* The device forwards its receive to the decapsulation node, and multicast
 * comes to the host as well: the subsystem forwards it and filters its
 * duplicates, and the firmware does neither.
 */
int nss_mesh_attach(struct nss_core *core, int link)
{
	struct nss_wifi_vdev_set_next_hop_msg nh = {};
	struct nss_mesh *m;
	int ret;

	guard(mutex)(&nss_mesh_lock);

	m = nss_mesh_find(link);
	if (!m)
		return -ENODEV;

	nh.ifnumber = m->decap;
	ret = nss_wifili_vdev_msg(core, link, NSS_WIFI_VDEV_SET_NEXT_HOP,
				  &nh, sizeof(nh));
	if (!ret)
		ret = nss_wifili_vdev_cmd(core, link,
					  NSS_WIFI_VDEV_CFG_MCBC_EXC_TO_HOST_CMD,
					  1);
	if (!ret)
		ret = nss_mesh_open(m, NSS_IF_OPEN);

	return ret;
}

static void nss_mesh_unbind(struct nss_core *core, int if_num)
{
	struct net_device *dev;

	dev = rcu_replace_pointer(core->iface[if_num], NULL, true);
	if (dev)
		dev_put(dev);
}

/* Callbacks off and the nodes closed, before the device goes down. */
void nss_mesh_stop(struct nss_core *core, int link)
{
	struct nss_mesh *m;

	guard(mutex)(&nss_mesh_lock);

	m = nss_mesh_find(link);
	if (!m)
		return;

	WRITE_ONCE(m->ops, NULL);
	nss_mesh_unbind(core, m->encap);
	nss_mesh_unbind(core, m->decap);
	synchronize_net();

	nss_mesh_open(m, NSS_IF_CLOSE);
}

/* After the device is released, which is what stops it feeding the nodes. */
void nss_mesh_del(struct nss_core *core, int link)
{
	int i;

	guard(mutex)(&nss_mesh_lock);

	for (i = 0; i < NSS_MESH_MAX; i++) {
		struct nss_mesh *m = &nss_mesh_tbl[i];

		if (!m->dev || m->ops || m->link != link)
			continue;

		nss_mesh_node_free(core, m->decap,
				   NSS_DYNAMIC_INTERFACE_TYPE_WIFI_MESH_OUTER);
		nss_mesh_node_free(core, m->encap,
				   NSS_DYNAMIC_INTERFACE_TYPE_WIFI_MESH_INNER);
		m->dev = NULL;
	}
}

/* A new core never knew the old nodes, and halting it dropped the bindings. */
void nss_mesh_reset(void)
{
	int i;

	guard(mutex)(&nss_mesh_lock);

	for (i = 0; i < NSS_MESH_MAX; i++)
		WRITE_ONCE(nss_mesh_tbl[i].ops, NULL);
	synchronize_net();

	memset(nss_mesh_tbl, 0, sizeof(nss_mesh_tbl));
}

/* A flow out of the mesh goes to the node that builds the mesh header. */
int nss_mesh_ifnum(int ifindex)
{
	int i;

	for (i = 0; i < NSS_MESH_MAX; i++) {
		struct nss_mesh *m = &nss_mesh_tbl[i];

		if (READ_ONCE(m->ops) && m->dev->ifindex == ifindex)
			return m->encap;
	}

	return -ENODEV;
}

static int nss_mesh_queue_cmd(int if_num, u32 cmd,
			      const struct nss_wifi_mesh_path *p)
{
	struct nss_mesh_cmd *c;

	c = kmalloc(sizeof(*c), GFP_ATOMIC);
	if (!c)
		return -ENOMEM;

	c->link = if_num;
	c->cmd = cmd;
	c->p = *p;

	scoped_guard(spinlock_bh, &nss_mesh_queue_lock) {
		if (nss_mesh_queued >= NSS_MESH_QUEUE_MAX) {
			nss_mesh_overflow++;
			kfree(c);
			return -ENOBUFS;
		}
		list_add_tail(&c->node, &nss_mesh_queue);
		nss_mesh_queued++;
	}

	schedule_work(&nss_mesh_work);

	return 0;
}

int nss_wifi_mesh_path(int if_num, enum nss_wifi_mesh_cmd cmd,
		       const struct nss_wifi_mesh_path *p)
{
	if (cmd > NSS_WIFI_MESH_MPP_DEL)
		return -EINVAL;

	return nss_mesh_queue_cmd(if_num, cmd, p);
}
EXPORT_SYMBOL_GPL(nss_wifi_mesh_path);

static int nss_mesh_send(struct nss_mesh *m, const struct nss_mesh_cmd *c)
{
	const struct nss_wifi_mesh_path *p = &c->p;
	union {
		struct nss_wifi_mesh_mpath_add_msg add;
		struct nss_wifi_mesh_mpath_update_msg upd;
		struct nss_wifi_mesh_mpath_del_msg del;
		struct nss_wifi_mesh_proxy_path_add_msg padd;
		struct nss_wifi_mesh_proxy_path_update_msg pupd;
		struct nss_wifi_mesh_proxy_path_del_msg pdel;
	} b = {};
	size_t len;
	u32 type;
	int ret;

	switch (c->cmd) {
	case NSS_WIFI_MESH_MPATH_ADD:
		type = NSS_WIFI_MESH_MSG_MPATH_ADD;
		len = sizeof(b.add);
		ether_addr_copy(b.add.dest_mac_addr, p->mesh_da);
		ether_addr_copy(b.add.next_hop_mac_addr, p->next_hop);
		b.add.metric = p->metric;
		b.add.link_vap_id = m->link;
		b.add.hop_count = p->hop_count;
		b.add.path_flags = p->flags;
		b.add.metadata_type = NSS_WIFI_MESH_PRE_HEADER_NONE;
		break;
	case NSS_WIFI_MESH_MPATH_UPDATE:
		type = NSS_WIFI_MESH_MSG_MPATH_UPDATE;
		len = sizeof(b.upd);
		ether_addr_copy(b.upd.dest_mac_addr, p->mesh_da);
		ether_addr_copy(b.upd.next_hop_mac_addr, p->next_hop);
		ether_addr_copy(b.upd.old_next_hop_mac_addr, p->old_next_hop);
		b.upd.metric = p->metric;
		b.upd.link_vap_id = m->link;
		b.upd.expiry_time = p->exp_ms;
		b.upd.hop_count = p->hop_count;
		b.upd.path_flags = p->flags;
		b.upd.is_mesh_gate = p->gate;
		/* The field is a byte, so the marker is truncated, as the
		 * vendor stack sends it on this line.
		 */
		b.upd.metadata_type = (u8)NSS_WIFI_MESH_PRE_HEADER_NONE;
		b.upd.update_flags = NSS_WIFI_MESH_PATH_UPDATE_FLAG_NEXTHOP |
				     NSS_WIFI_MESH_PATH_UPDATE_FLAG_HOPCOUNT |
				     NSS_WIFI_MESH_PATH_UPDATE_FLAG_METRIC |
				     NSS_WIFI_MESH_PATH_UPDATE_FLAG_MESH_FLAGS |
				     NSS_WIFI_MESH_PATH_UPDATE_FLAG_BLOCK_MESH_FWD |
				     NSS_WIFI_MESH_PATH_UPDATE_FLAG_METADATA_ENABLE_VALID;
		break;
	case NSS_WIFI_MESH_MPATH_DEL:
		type = NSS_WIFI_MESH_MSG_MPATH_DELETE;
		len = sizeof(b.del);
		b.del.link_vap_id = m->link;
		ether_addr_copy(b.del.mesh_dest_mac_addr, p->mesh_da);
		ether_addr_copy(b.del.next_hop_mac_addr, p->next_hop);
		break;
	case NSS_WIFI_MESH_MPP_ADD:
		type = NSS_WIFI_MESH_MSG_PROXY_PATH_ADD;
		len = sizeof(b.padd);
		ether_addr_copy(b.padd.dest_mac_addr, p->da);
		ether_addr_copy(b.padd.mesh_dest_mac, p->mesh_da);
		b.padd.path_flags = p->flags;
		break;
	case NSS_WIFI_MESH_MPP_UPDATE:
		type = NSS_WIFI_MESH_MSG_PROXY_PATH_UPDATE;
		len = sizeof(b.pupd);
		ether_addr_copy(b.pupd.dest_mac_addr, p->da);
		ether_addr_copy(b.pupd.mesh_dest_mac, p->mesh_da);
		b.pupd.path_flags = p->flags;
		b.pupd.bitmap = NSS_WIFI_MESH_PROXY_PATH_UPDATE_FLAG_MDA |
				NSS_WIFI_MESH_PROXY_PATH_UPDATE_FLAG_MESH;
		break;
	case NSS_WIFI_MESH_MPP_DEL:
		type = NSS_WIFI_MESH_MSG_PROXY_PATH_DELETE;
		len = sizeof(b.pdel);
		ether_addr_copy(b.pdel.dest_mac_addr, p->da);
		ether_addr_copy(b.pdel.mesh_dest_mac_addr, p->mesh_da);
		break;
	case NSS_MESH_CMD_DUMP:
		/* The table comes back as notifications. */
		type = NSS_WIFI_MESH_MSG_PROXY_PATH_TABLE_DUMP;
		len = sizeof(struct nss_wifi_mesh_proxy_path_table_dump);
		break;
	default:
		return -EINVAL;
	}

	ret = nss_mesh_msg(m->core, m->encap, type, &b, len);
	if (ret == -ENOENT &&
	    (c->cmd == NSS_WIFI_MESH_MPATH_UPDATE ||
	     c->cmd == NSS_WIFI_MESH_MPP_UPDATE)) {
		struct nss_mesh_cmd add = *c;

		/* Recovery creates empty firmware nodes while mac80211 keeps
		 * its paths. Recreate a missing entry before applying the
		 * update; retry only the explicit missing-path response.
		 */
		add.cmd = c->cmd == NSS_WIFI_MESH_MPATH_UPDATE ?
			  NSS_WIFI_MESH_MPATH_ADD : NSS_WIFI_MESH_MPP_ADD;
		ret = nss_mesh_send(m, &add);
		if (!ret)
			ret = nss_mesh_msg(m->core, m->encap, type, &b, len);
	}

	return ret;
}

static void nss_mesh_work_fn(struct work_struct *work)
{
	LIST_HEAD(list);
	struct nss_mesh_cmd *c, *tmp;

	scoped_guard(spinlock_bh, &nss_mesh_queue_lock) {
		list_splice_init(&nss_mesh_queue, &list);
		nss_mesh_queued = 0;
	}

	list_for_each_entry_safe(c, tmp, &list, node) {
		scoped_guard(mutex, &nss_mesh_lock) {
			struct nss_mesh *m = nss_mesh_find(c->link);

			if (m && nss_mesh_send(m, c))
				m->dropped++;
		}
		kfree(c);
	}
}

static void nss_mesh_age_fn(struct work_struct *work)
{
	struct nss_wifi_mesh_path p = {};
	bool any = false;
	int i;

	for (i = 0; i < NSS_MESH_MAX; i++) {
		int link;

		scoped_guard(mutex, &nss_mesh_lock) {
			link = nss_mesh_tbl[i].ops ? nss_mesh_tbl[i].link : -1;
		}
		if (link < 0)
			continue;

		any = true;
		nss_mesh_queue_cmd(link, NSS_MESH_CMD_DUMP, &p);
	}

	if (any)
		schedule_delayed_work(&nss_mesh_age,
				      msecs_to_jiffies(NSS_MESH_AGE_MS));
}

int nss_wifi_mesh_config(int if_num, u8 ttl, u32 refresh_ms, bool forward)
{
	struct nss_mesh *m;

	guard(mutex)(&nss_mesh_lock);

	m = nss_mesh_find(if_num);
	if (!m)
		return -ENODEV;

	if (m->ttl == ttl && m->refresh == refresh_ms && m->forward == forward)
		return 0;

	m->ttl = ttl;
	m->refresh = refresh_ms;
	m->forward = forward;

	return nss_mesh_configure(m, NSS_WIFI_MESH_CONFIG_FLAG_TTL_VALID |
				  NSS_WIFI_MESH_CONFIG_FLAG_MPATH_REFRESH_VALID |
				  NSS_WIFI_MESH_CONFIG_FLAG_BLOCK_MESH_FWD_VALID);
}
EXPORT_SYMBOL_GPL(nss_wifi_mesh_config);

static struct nss_mesh *nss_mesh_by_node(int if_num, int *which)
{
	int i;

	for (i = 0; i < NSS_MESH_MAX; i++) {
		struct nss_mesh *m = &nss_mesh_tbl[i];

		if (!READ_ONCE(m->ops))
			continue;
		if (m->encap == if_num || m->decap == if_num) {
			*which = m->decap == if_num;
			return m;
		}
		if (m->link == if_num) {
			*which = 2;
			return m;
		}
	}

	return NULL;
}

static void nss_mesh_mpp_dump(struct nss_mesh *m,
			      const struct nss_wifi_mesh_ops *ops,
			      const void *body, u32 blen)
{
	const struct nss_wifi_mesh_proxy_path_table_dump *d = body;
	size_t hdr = offsetof(struct nss_wifi_mesh_proxy_path_table_dump,
			      path_entry);
	u32 i, n;

	if (blen < hdr)
		return;

	n = min_t(u32, d->num_entries,
		  (blen - hdr) / sizeof(d->path_entry[0]));

	for (i = 0; i < n; i++) {
		const struct nss_wifi_mesh_proxy_path_dump_entry *e =
			&d->path_entry[i];
		struct nss_wifi_mesh_path p = {};

		if (e->time_diff > NSS_MESH_AGE_MS)
			continue;

		ether_addr_copy(p.da, e->dest_mac_addr);
		ether_addr_copy(p.mesh_da, e->mesh_dest_mac);
		p.flags = e->flags;
		ops->event(m->priv, NSS_WIFI_MESH_EV_MPP_UPDATE, &p);
	}
}

static size_t nss_mesh_body_len(u32 type)
{
	switch (type) {
	case NSS_WIFI_MESH_MSG_PATH_REFRESH:
		return sizeof(struct nss_wifi_mesh_path_refresh_msg);
	case NSS_WIFI_MESH_MSG_PATH_NOT_FOUND:
		return sizeof(struct nss_wifi_mesh_mpath_not_found_msg);
	case NSS_WIFI_MESH_MSG_MPATH_DELETE:
		return sizeof(struct nss_wifi_mesh_mpath_del_msg);
	case NSS_WIFI_MESH_MSG_PATH_EXPIRY:
		return sizeof(struct nss_wifi_mesh_path_expiry_msg);
	case NSS_WIFI_MESH_MSG_PROXY_PATH_LEARN:
		return sizeof(struct nss_wifi_mesh_proxy_path_learn_msg);
	case NSS_WIFI_MESH_MSG_PROXY_PATH_ADD:
		return sizeof(struct nss_wifi_mesh_proxy_path_add_msg);
	case NSS_WIFI_MESH_MSG_PROXY_PATH_UPDATE:
		return sizeof(struct nss_wifi_mesh_proxy_path_update_msg);
	case NSS_WIFI_MESH_MSG_STATS_SYNC:
		return sizeof(struct nss_wifi_mesh_stats_sync_msg);
	default:
		return 0;
	}
}

/* What the mesh nodes say unasked. Called from softirq. */
bool nss_mesh_notify(struct nss_core *core, const struct nss_cmn_msg *ncm,
		     u32 len)
{
	const struct nss_wifi_mesh_ops *ops;
	struct nss_wifi_mesh_path p = {};
	enum nss_wifi_mesh_event ev;
	const void *body = ncm + 1;
	struct nss_mesh *m;
	u32 blen;
	int which;

	guard(rcu)();

	m = nss_mesh_by_node(NSS_INTERFACE_NUM_GET(ncm->interface), &which);
	if (!m || which == 2)
		return false;

	ops = smp_load_acquire(&m->ops);
	if (!ops)
		return true;

	blen = min_t(u32, len - sizeof(*ncm), ncm->len);

	if (blen < nss_mesh_body_len(ncm->type))
		return true;

	switch (ncm->type) {
	case NSS_WIFI_MESH_MSG_PATH_REFRESH: {
		const struct nss_wifi_mesh_path_refresh_msg *r = body;

		ether_addr_copy(p.mesh_da, r->dest_mac_addr);
		ether_addr_copy(p.next_hop, r->next_hop_mac_addr);
		ev = NSS_WIFI_MESH_EV_REFRESH;
		break;
	}
	case NSS_WIFI_MESH_MSG_PATH_NOT_FOUND: {
		const struct nss_wifi_mesh_mpath_not_found_msg *r = body;

		ether_addr_copy(p.da, r->dest_mac_addr);
		if (r->is_mesh_forward_path)
			ether_addr_copy(p.ta, r->transmitter_mac_addr);
		ev = NSS_WIFI_MESH_EV_NOT_FOUND;
		break;
	}
	case NSS_WIFI_MESH_MSG_MPATH_DELETE: {
		const struct nss_wifi_mesh_mpath_del_msg *r = body;

		ether_addr_copy(p.mesh_da, r->mesh_dest_mac_addr);
		ev = NSS_WIFI_MESH_EV_MPATH_DEL;
		break;
	}
	case NSS_WIFI_MESH_MSG_PATH_EXPIRY: {
		const struct nss_wifi_mesh_path_expiry_msg *r = body;

		ether_addr_copy(p.mesh_da, r->mesh_dest_mac_addr);
		ether_addr_copy(p.next_hop, r->next_hop_mac_addr);
		ev = NSS_WIFI_MESH_EV_EXPIRY;
		break;
	}
	case NSS_WIFI_MESH_MSG_PROXY_PATH_LEARN: {
		const struct nss_wifi_mesh_proxy_path_learn_msg *r = body;

		ether_addr_copy(p.mesh_da, r->mesh_dest_mac);
		ether_addr_copy(p.da, r->dest_mac_addr);
		p.flags = r->path_flags;
		ev = NSS_WIFI_MESH_EV_MPP_LEARN;
		break;
	}
	case NSS_WIFI_MESH_MSG_PROXY_PATH_ADD: {
		const struct nss_wifi_mesh_proxy_path_add_msg *r = body;

		ether_addr_copy(p.mesh_da, r->mesh_dest_mac);
		ether_addr_copy(p.da, r->dest_mac_addr);
		p.flags = r->path_flags;
		ev = NSS_WIFI_MESH_EV_MPP_ADD;
		break;
	}
	case NSS_WIFI_MESH_MSG_PROXY_PATH_UPDATE: {
		const struct nss_wifi_mesh_proxy_path_update_msg *r = body;

		ether_addr_copy(p.mesh_da, r->mesh_dest_mac);
		ether_addr_copy(p.da, r->dest_mac_addr);
		p.flags = r->path_flags;
		ev = NSS_WIFI_MESH_EV_MPP_UPDATE;
		break;
	}
	case NSS_WIFI_MESH_MSG_PROXY_PATH_TABLE_DUMP:
		nss_mesh_mpp_dump(m, ops, body, blen);
		return true;
	case NSS_WIFI_MESH_MSG_STATS_SYNC: {
		/* Each sync carries what was counted since the last one. */
		u32 *sum = (u32 *)&m->stats[which];
		const u32 *d = body;
		size_t i;

		for (i = 0; i < sizeof(m->stats[0]) / sizeof(u32); i++)
			sum[i] += d[i];
		return true;
	}
	default:
		return true;
	}

	ops->event(m->priv, ev, &p);

	return true;
}

/* Frames the mesh nodes and the device hand up with a metadata word in front.
 * Called from softirq; the buffer is the host's once this returns true.
 */
bool nss_mesh_recv(struct nss_core *core, struct napi_struct *napi,
		   const struct n2h_descriptor *desc, struct sk_buff *skb)
{
	u32 end = (u32)desc->payload_offs + desc->payload_len;
	const struct nss_wifi_mesh_ops *ops;
	struct nss_mesh *m;
	u32 type, md;
	int which;

	if (desc->buffer_type != NSS_N2H_BUFFER_PACKET_EXT ||
	    desc->interface_num >= NSS_INTERFACE_MAX)
		return false;

	guard(rcu)();

	m = nss_mesh_by_node(desc->interface_num, &which);
	if (!m || which == 1)
		return false;

	/* The metadata is in the headroom, so the payload starts past it. */
	md = which == 2 ? NSS_WIFI_VDEV_PER_PACKET_METADATA_OFFSET +
			  sizeof(struct nss_wifi_vdev_per_packet_metadata) :
			  NSS_WIFI_MESH_ENCAP_METADATA_OFFSET_TYPE +
			  sizeof(struct nss_wifi_mesh_encap_ext_pkt_metadata);

	ops = smp_load_acquire(&m->ops);
	if (!ops || end > NSS_EMPTY_BUFFER_ALLOC || desc->payload_offs < md ||
	    desc->payload_len < ETH_HLEN)
		return false;

	dma_unmap_single(core->dev, desc->buffer, NSS_EMPTY_BUFFER_ALLOC,
			 DMA_FROM_DEVICE);
	skb_reserve(skb, desc->payload_offs);
	skb_put(skb, desc->payload_len);

	if (which == 2) {
		/* Multicast the device received. The subsystem forwards it
		 * and drops its duplicates, so it enters at 802.11.
		 */
		type = ((struct nss_wifi_vdev_per_packet_metadata *)
			(skb->head + NSS_WIFI_VDEV_PER_PACKET_METADATA_OFFSET))->pkt_type;
		if (type == NSS_WIFI_VDEV_EXT_DATA_PKT_TYPE_MCBC_RX ||
		    type == NSS_WIFI_VDEV_MESH_EXT_DATA_PKT_TYPE_RX_MCAST_EXC) {
			ops->rx(m->priv, skb, napi);
			return true;
		}
	} else {
		/* Ethernet the encapsulation node had no path for: the
		 * subsystem resolves one and sends it as 802.11.
		 */
		type = ((struct nss_wifi_mesh_encap_ext_pkt_metadata *)
			(skb->head + NSS_WIFI_MESH_ENCAP_METADATA_OFFSET_TYPE))->pkt_type;
		if (type == NSS_WIFI_MESH_ENCAP_EXT_DATA_PKT_TYPE_MPATH_NOT_FOUND_EXC) {
			skb->dev = m->dev;
			skb_reset_mac_header(skb);
			skb->protocol = eth_hdr(skb)->h_proto;
			skb_set_network_header(skb, ETH_HLEN);
			dev_queue_xmit(skb);
			return true;
		}
	}

	m->dropped++;
	dev_kfree_skb_any(skb);

	return true;
}

void nss_mesh_print(struct seq_file *s)
{
	int i;

	guard(mutex)(&nss_mesh_lock);

	seq_printf(s, "path queue overflow %llu\n", nss_mesh_overflow);

	for (i = 0; i < NSS_MESH_MAX; i++) {
		const struct nss_mesh *m = &nss_mesh_tbl[i];
		const struct nss_wifi_mesh_encap_stats *e =
			&m->stats[0].mesh_encap_stats;
		const struct nss_wifi_mesh_decap_stats *d =
			&m->stats[1].mesh_decap_stats;

		if (!m->ops)
			continue;

		seq_printf(s, "%s link %d encap %d decap %d ttl %u refresh %u fwd %d dropped %llu\n",
			   netdev_name(m->dev), m->link, m->encap, m->decap,
			   m->ttl, m->refresh, m->forward, m->dropped);
		seq_printf(s, "  encap rx %u tx %u mp_not_found %u mp_active %u mpp_found %u ra_lup_fail %u link_enqueue %u to_host %u\n",
			   m->stats[0].pnode_stats.rx_packets,
			   m->stats[0].pnode_stats.tx_packets,
			   e->mp_not_found, e->mp_active, e->mpp_found,
			   e->ra_lup_fail, e->link_enqueue, e->sent_to_host);
		seq_printf(s, "  decap rx %u tx %u ttl0 %u mpp_lup_fail %u fwd ok %u fail %u mp_fwd_lookup_fail %u\n",
			   m->stats[1].pnode_stats.rx_packets,
			   m->stats[1].pnode_stats.tx_packets,
			   d->ttl_0, d->mpp_lup_fail, d->rx_fwd_success,
			   d->rx_fwd_fail, d->mp_fwd_lookup_fail);
	}
}

void nss_mesh_exit(void)
{
	cancel_delayed_work_sync(&nss_mesh_age);
	cancel_work_sync(&nss_mesh_work);
}
