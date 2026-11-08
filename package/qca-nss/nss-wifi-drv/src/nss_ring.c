// SPDX-License-Identifier: GPL-2.0-only
/* The message and data rings between the host and a running core.
 *
 * The rings live in coherent memory, so the indices each side publishes are
 * visible to the other without maintenance of our own. Each of the ten
 * interrupt causes is its own line - this SoC has no mask or status register
 * for them - so a cause is fixed when its line is claimed and never has to be
 * decoded.
 *
 * A running core's first act is to ask for buffers, because it has none. That
 * request is also how the host learns it is up: there is no ready message in
 * this protocol.
 */

#include <linux/cleanup.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/etherdevice.h>
#include <net/dsa.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>

#include "nss_drv.h"

#define NSS_NAPI_WEIGHT		64
#define NSS_REFILL_BATCH	64

void nss_doorbell(struct nss_core *core, u32 which)
{
	writel(BIT(NSS_H2N_INTR_BASE(core->id) + which),
	       core->qgic + NSS_QGIC_IPC_REG);
}

/* Give the firmware's interface numbers something to deliver to.
 *
 * A frame the firmware exceptions carries the number of the interface it came
 * in on, and for a switch port that number is the port's own index - measured
 * on this SoC, and derived here rather than written down, because the port set
 * is a property of the board. Everything above a switch port gets its number
 * when it is created, and registers itself.
 */
void nss_iface_bind(struct nss_core *core)
{
	struct net_device *dev;

	/* RCU, not rtnl: this runs under core->lock, which rtnl holders take. */
	rcu_read_lock();
	for_each_netdev_rcu(&init_net, dev) {
		struct dsa_port *dp;

		if (!dsa_user_dev_check(dev))
			continue;

		dp = dsa_port_from_netdev(dev);
		if (IS_ERR(dp) || dp->index >= NSS_INTERFACE_MAX)
			continue;

		if (dsa_port_to_conduit(dp) != core->conduit)
			continue;

		dev_hold(dev);
		rcu_assign_pointer(core->iface[dp->index], dev);
	}
	rcu_read_unlock();
}

void nss_iface_unbind(struct nss_core *core)
{
	int i;

	for (i = 0; i < NSS_INTERFACE_MAX; i++) {
		struct net_device *dev;

		dev = rcu_replace_pointer(core->iface[i], NULL, true);
		if (dev)
			dev_put(dev);
	}

	synchronize_rcu();
}

/* Keep the first few buffers the firmware hands up that are neither an
 * ordinary frame nor a message. One of them is how a receive descriptor the
 * host must give back arrives, and its shape on the running firmware is not
 * derivable from anything the host has.
 */
static void nss_ext_take(struct nss_core *core, const struct n2h_descriptor *desc,
			 struct sk_buff *skb)
{
	struct nss_ext_seen *e;
	u32 i;

	if (core->ext_seen >= ARRAY_SIZE(core->ext))
		return;

	if (desc->buffer_type == NSS_N2H_BUFFER_STATUS ||
	    (u32)desc->payload_offs + desc->payload_len > NSS_EMPTY_BUFFER_ALLOC)
		return;

	e = &core->ext[core->ext_seen++];
	e->iface = desc->interface_num;
	e->type = desc->buffer_type;
	e->len = desc->payload_len;
	e->offs = desc->payload_offs;
	for (i = 0; i < ARRAY_SIZE(e->word) && i * sizeof(u32) < e->len; i++)
		e->word[i] = ((const u32 *)(skb->head + desc->payload_offs))[i];
}

/* Hand up a frame the firmware exceptioned.
 *
 * The offset and length come from the other side, so they are checked against
 * the allocation the host made rather than believed: the buffer is the one
 * this driver donated, and nothing the firmware writes into the descriptor can
 * make it larger.
 */
static bool nss_data_recv(struct nss_core *core, struct napi_struct *napi,
			  const struct n2h_descriptor *desc,
			  struct sk_buff *skb)
{
	u32 end = (u32)desc->payload_offs + desc->payload_len;
	struct net_device *dev;

	if (desc->buffer_type != NSS_N2H_BUFFER_PACKET ||
	    desc->interface_num >= NSS_INTERFACE_MAX)
		return false;

	dev = rcu_dereference_bh(core->iface[desc->interface_num]);
	if (!dev || end > NSS_EMPTY_BUFFER_ALLOC || desc->payload_len < ETH_HLEN)
		return false;

	/* The receive poll has claimed this buffer and checked its DMA address. */
	dma_unmap_single(core->dev, NSS_SKB_CB(skb)->dma,
			 NSS_EMPTY_BUFFER_ALLOC, DMA_FROM_DEVICE);

	skb_reserve(skb, desc->payload_offs);
	skb_put(skb, desc->payload_len);
	if (nss_wifili_rx_80211(desc->interface_num, dev, skb, napi))
		return true;

	skb->protocol = eth_type_trans(skb, dev);
	if (skb->pkt_type == PACKET_MULTICAST)
		nss_wifili_me_snoop(desc->interface_num, skb);

	dev_sw_netstats_rx_add(dev, desc->payload_len);
	napi_gro_receive(napi, skb);

	return true;
}

/* Hand on a message the firmware sent unasked.
 *
 * Nothing the host sent is outstanding, so this is never a completion and the
 * sixteen header bytes reserved for the host hold whatever the buffer's last
 * user left there. The descriptor's interface number is zero on every message
 * buffer, so the dispatch is on the interface inside the payload, whose top
 * byte is a core id. The buffer is already unmapped, so the payload is the
 * host's to read.
 */
static void nss_notify_recv(struct nss_core *core,
			    const struct n2h_descriptor *desc,
			    struct sk_buff *skb)
{
	u32 end = (u32)desc->payload_offs + desc->payload_len;
	const struct nss_cmn_msg *ncm;

	if (desc->buffer_type != NSS_N2H_BUFFER_STATUS ||
	    end > NSS_EMPTY_BUFFER_ALLOC || desc->payload_len < sizeof(*ncm))
		return;

	core->notify++;

	ncm = (const void *)(skb->head + desc->payload_offs);
	nss_msg_seen(core, ncm, desc->payload_len);
	nss_msg_census(ncm);
	nss_flow_notify(ncm, desc->payload_len);

	if (nss_wifili_is_soc(ncm->interface))
		nss_wifili_notify(core, ncm, desc->payload_len);
	else if (!nss_mesh_notify(core, ncm, desc->payload_len))
		nss_wifili_vdev_notify(core, ncm, desc->payload_len);
}

/* Whether the firmware may keep the buffer a frame arrives in. A kept buffer
 * counts against the firmware's high-water mark, which this driver holds
 * equal to what it lends, and it comes back later under the same cookie
 * carrying a received frame, which the completion path below frees unread.
 */
static bool nss_tx_reusable;
module_param_named(tx_reusable, nss_tx_reusable, bool, 0644);
MODULE_PARM_DESC(tx_reusable, "offer the firmware the buffer a frame arrives in");

static int nss_lent_take(struct nss_core *core, struct sk_buff *skb);
static void nss_lent_put(struct nss_core *core, u32 slot);

static enum dma_data_direction nss_tx_dir(const struct sk_buff *skb)
{
	return NSS_SKB_CB(skb)->bidir ? DMA_BIDIRECTIONAL : DMA_TO_DEVICE;
}

/* Hand a frame down to the firmware to place.
 *
 * The same descriptor the message layer fills, on the data queue rather than
 * the command one, with the interface the firmware is to send it out of. The
 * buffer is the host's until the firmware gives it back on a receive ring
 * carrying the address handed out here, so it is marked to say which of the
 * two kinds of buffer it is: a donated one is replaced when it returns, a
 * transmitted one is freed, unless it carries a packet the firmware gave
 * back to the host.
 *
 * One descriptor, so one segment: a frame arriving in pieces is linearised
 * rather than described piecewise, because nothing here yet has a reason to
 * chain them and a chain the firmware reads wrongly is a fault rather than a
 * dropped frame.
 *
 * A frame handed to a host-side node with the virtual flag is routed by the
 * firmware as if it had arrived on that node's device, and one it does not
 * route comes back to the host in the same buffer. So the buffer is mapped
 * both ways: what the host reads back is what the firmware left there.
 */
int nss_data_send(struct nss_core *core, struct sk_buff *skb, u32 if_num,
		  u16 flags)
{
	struct nss_if_mem_map *map = core->if_map;
	struct nss_h2n_ring *ring;
	bool bidir = flags & NSS_H2N_FLAG_VIRTUAL_BUFFER;
	enum dma_data_direction dir = bidir ? DMA_BIDIRECTIONAL : DMA_TO_DEVICE;
	struct h2n_descriptor *desc;
	dma_addr_t dma;
	u32 next, shift;
	int slot, qid;

	if (!core->running)
		return -ENODEV;

	/* The firmware serves every data ring alike. One per flow keeps a
	 * flow in order and spreads flows over the rings' locks.
	 */
	qid = NSS_H2N_RING_DATA +
	      skb_get_hash(skb) % (NSS_H2N_RINGS - NSS_H2N_RING_DATA);
	ring = &core->h2n[qid];

	if (skb_is_nonlinear(skb) && skb_linearize(skb))
		return -ENOMEM;

	/* The firmware checks that a frame begins on a word boundary within
	 * the buffer it is given, and drops one that does not before it
	 * reaches a ring. A frame this driver builds is aligned by
	 * construction; one converted from 802.11 is not, because the header
	 * it replaced was not a multiple of four bytes long. Moving it down
	 * to the next boundary costs a copy of the frame on the paths that
	 * are not the fast one - the encapsulated frames, which are the bulk,
	 * arrive aligned and are left alone.
	 */
	shift = (skb->data - skb->head) & 3;
	if (shift) {
		if (skb_headroom(skb) < shift || skb_cow_head(skb, 0))
			return -ENOBUFS;

		memmove(skb->data - shift, skb->data, skb->len);
		skb->data -= shift;
		skb_set_tail_pointer(skb, skb->len);
	}

	dma = dma_map_single(core->dev, skb->head,
			     skb_end_pointer(skb) - skb->head, dir);
	if (dma_mapping_error(core->dev, dma))
		return -ENOMEM;

	NSS_SKB_CB(skb)->dma = dma;
	NSS_SKB_CB(skb)->tx = true;
	NSS_SKB_CB(skb)->bidir = bidir;
	/* A transmitted buffer comes back once, under this slot's generation. */
	slot = nss_lent_take(core, skb);
	if (slot < 0) {
		dma_unmap_single(core->dev, dma, skb_end_pointer(skb) - skb->head,
				 dir);
		return -ENOBUFS;
	}
	scoped_guard(spinlock_bh, &ring->lock) {
		next = (ring->hlos_index + 1) & (NSS_RING_ENTRIES - 1);
		if (next == READ_ONCE(map->h2n_nss_index[qid])) {
			nss_lent_put(core, slot);
			dma_unmap_single(core->dev, dma,
					 skb_end_pointer(skb) - skb->head, dir);
			return -EBUSY;
		}

		desc = &ring->desc[ring->hlos_index];
		desc->interface_num = if_num;
		desc->buffer = dma;
		desc->buffer_len = skb_end_pointer(skb) - skb->head;
		desc->payload_offs = skb->data - skb->head;
		desc->payload_len = skb->len;
		desc->mss = 0;
		/* Not the frame's priority: the tag is a classid whose upper
		 * half selects a shaper node, so a bare priority written here
		 * would name node zero. It is filled by whatever assigns the
		 * shaping, and nothing does yet.
		 */
		desc->qos_tag = 0;
		desc->buffer_type = NSS_H2N_BUFFER_PACKET;
		desc->bit_flags = NSS_H2N_FLAG_FIRST_SEGMENT |
				  NSS_H2N_FLAG_LAST_SEGMENT | flags |
				  (nss_tx_reusable && !bidir ?
				   NSS_H2N_FLAG_BUFFER_REUSABLE : 0);
		if (skb->ip_summed == CHECKSUM_PARTIAL)
			desc->bit_flags |= NSS_H2N_FLAG_GEN_IP_CHECKSUM |
					   NSS_H2N_FLAG_GEN_L4_CHECKSUM;
		desc->opaque = NSS_SKB_CB(skb)->cookie;

		core->tx_posted++;

		dma_wmb();
		ring->hlos_index = next;
		WRITE_ONCE(map->h2n_hlos_index[qid], next);
	}

	nss_doorbell(core, NSS_H2N_INTR_DATA_CMD);

	return 0;
}

/* Keep every line the firmware writes, not just the last ringful.
 *
 * The block the firmware asks for at boot fits thirty-two entries and a fault
 * fills far more than that, so by the time the host is told a fault happened
 * the beginning of it - the part that says what went wrong - has been
 * overwritten by the lines that follow it.
 *
 * So the host copies entries out as they appear, appending anything new to a
 * buffer big enough for a whole fault. A line is lost if it arrives faster
 * than the sampling or after the buffer is full, and the readout says how
 * many rather than hiding it.
 */
#define NSS_LOG_SHADOW_ENTRIES	1024
/* How often the shadow samples the firmware's write index. A sample that
	 * finds nothing new costs one read, but the timer stays armed until the
	 * shadow is full, so the rate is a standing cost on a box whose whole
 * point is not to have one. It is a parameter so the cost can be measured
 * against the risk of missing a line rather than argued about.
 */
static unsigned int nss_log_poll_us = 200;
static int nss_log_poll_set(const char *value, const struct kernel_param *kp)
{
	unsigned int interval;
	int ret = kstrtouint(value, 0, &interval);

	if (ret)
		return ret;
	if (interval < 100 || interval > 1000000)
		return -ERANGE;
	WRITE_ONCE(*(unsigned int *)kp->arg, interval);
	return 0;
}

static const struct kernel_param_ops nss_log_poll_ops = {
	.set = nss_log_poll_set,
	.get = param_get_uint,
};
module_param_cb(log_poll_us, &nss_log_poll_ops, &nss_log_poll_us, 0644);
MODULE_PARM_DESC(log_poll_us, "how often to sample the firmware log ring");

/* Runs from a timer and from whoever is reading, so it is serialised: two
 * appenders racing on the same index would write past the buffer.
 */
static void nss_log_shadow_take(struct nss_core *core)
{
	u32 n = core->log_entries;
	u32 written, first, i;

	if (!core->log || !core->shadow || !n)
		return;

	guard(spinlock_irqsave)(&core->shadow_lock);

	written = READ_ONCE(core->log->current_entry);
	if (written == core->shadow_seen)
		return;

	/* Anything more than a ring behind is already gone. */
	first = max(core->shadow_seen, written > n ? written - n : 0);

	for (i = first; i < written; i++) {
		struct nss_log_entry *src = &core->log->log_ring_buffer[i % n];

		if (core->shadow_held >= NSS_LOG_SHADOW_ENTRIES)
			break;

		core->shadow[core->shadow_held++] = *src;
	}

	core->shadow_seen = written;
}

static enum hrtimer_restart nss_log_shadow_tick(struct hrtimer *t)
{
	struct nss_core *core = container_of(t, struct nss_core, shadow_timer);

	nss_log_shadow_take(core);
	if (READ_ONCE(core->shadow_held) >= NSS_LOG_SHADOW_ENTRIES)
		return HRTIMER_NORESTART;
	hrtimer_forward_now(t, us_to_ktime(READ_ONCE(nss_log_poll_us)));

	return HRTIMER_RESTART;
}

int nss_log_shadow_init(struct nss_core *core)
{
	core->shadow = devm_kcalloc(core->dev, NSS_LOG_SHADOW_ENTRIES,
				    sizeof(*core->shadow), GFP_KERNEL);
	if (!core->shadow)
		return -ENOMEM;

	spin_lock_init(&core->shadow_lock);
	hrtimer_setup(&core->shadow_timer, nss_log_shadow_tick, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);

	return 0;
}

void nss_log_shadow_start(struct nss_core *core)
{
	scoped_guard(spinlock_irqsave, &core->shadow_lock) {
		core->shadow_seen = 0;
		core->shadow_held = 0;
	}

	hrtimer_start(&core->shadow_timer, us_to_ktime(READ_ONCE(nss_log_poll_us)),
		      HRTIMER_MODE_REL);
}

void nss_log_shadow_stop(struct nss_core *core)
{
	hrtimer_cancel(&core->shadow_timer);
	nss_log_shadow_take(core);
}

/* Walk what the firmware has been saying, oldest first.
 *
 * The write position is a free-running count of lines ever written, not an
 * index, so how many are valid follows from it rather than from the ring
 * being full. The host zeroed the block, so a slot whose cookie does not read
 * back as NSS_LOG_COOKIE has not been written and is skipped.
 */
static void nss_log_walk(struct nss_core *core, struct seq_file *s)
{
	u32 n = core->log_entries;
	u32 written, count, i;

	if (!core->log || !n)
		return;

	nss_log_shadow_take(core);

	written = READ_ONCE(core->log->current_entry);
	scoped_guard(spinlock_irqsave, &core->shadow_lock)
		count = core->shadow_held;
	if (written > count && s)
		seq_printf(s, "[%u lines arrived faster than they could be kept]\n",
			   written - count);

	for (i = 0; i < count; i++) {
		struct nss_log_entry *e = &core->shadow[i];

		if (e->cookie != NSS_LOG_COOKIE)
			continue;

		if (s)
			seq_printf(s, "[%llu] %.*s\n", e->sequence_num,
				   NSS_LOG_LINE_WIDTH, e->message);
		else
			dev_err(core->dev, "  fw[%llu] %.*s\n", e->sequence_num,
				NSS_LOG_LINE_WIDTH, e->message);
	}
}

int nss_log_show_ring(struct nss_core *core, struct seq_file *s)
{
	if (!core->log || !core->log_entries)
		return -ENODEV;

	nss_log_walk(core, s);

	return 0;
}

/* The only account of a core that has stopped, so it is dumped whenever one
 * does. The ring has been empty on every start observed here and has carried
 * entries only after a trap, so an empty ring is a result and not a failed
 * read: the core stopped without trapping.
 */
void nss_log_dump(struct nss_core *core, const char *why)
{
	dev_err(core->dev, "%s: firmware log follows\n", why);
	nss_log_walk(core, NULL);
}

/* Hand the firmware empty buffers. It picks its own offset inside each one
 * and reports where the payload landed, so what it is given is the head of
 * the allocation rather than the data pointer.
 */
/* A cookie contains a host table slot and its generation, not a pointer.
 * Claim the live table entry under its lock before reading any skb memory.
 * A duplicate, delayed or mismatched return leaves the current loan intact.
 */
static int nss_lent_take(struct nss_core *core, struct sk_buff *skb)
{
	struct nss_lent_buffer *lent;
	u32 slot;

	guard(spinlock_bh)(&core->lent_lock);

	if (!core->lent_free_n)
		return -ENOSPC;

	slot = core->lent_free[--core->lent_free_n];
	lent = &core->lent[slot];
	/* Never wrap a generation back to a cookie previously lent. */
	if (lent->cookie > U64_MAX - BIT_ULL(core->lent_shift))
		return -ENOSPC;
	lent->cookie += BIT_ULL(core->lent_shift);
	lent->skb = skb;
	NSS_SKB_CB(skb)->cookie = lent->cookie;

	return slot;
}

static struct sk_buff *nss_lent_claim(struct nss_core *core, u64 cookie,
				     u32 dma)
{
	u32 slot = cookie & (BIT_ULL(core->lent_shift) - 1);
	struct nss_lent_buffer *lent;
	struct sk_buff *skb;

	guard(spinlock_bh)(&core->lent_lock);

	if (slot >= core->lent_size)
		return NULL;
	lent = &core->lent[slot];
	if (!lent->skb || lent->cookie != cookie)
		return NULL;
	skb = lent->skb;
	if (NSS_SKB_CB(skb)->dma != dma)
		return NULL;

	lent->skb = NULL;
	core->lent_free[core->lent_free_n++] = slot;

	return skb;
}

static void nss_lent_put(struct nss_core *core, u32 slot)
{
	guard(spinlock_bh)(&core->lent_lock);

	if (slot >= core->lent_size || !core->lent[slot].skb)
		return;

	core->lent[slot].skb = NULL;
	core->lent_free[core->lent_free_n++] = slot;
}

static void nss_lent_reclaim(struct nss_core *core)
{
	u32 i;

	for (i = 0; i < core->lent_size; i++) {
		struct sk_buff *skb = core->lent[i].skb;

		if (!skb)
			continue;

		if (NSS_SKB_CB(skb)->tx)
			dma_unmap_single(core->dev, NSS_SKB_CB(skb)->dma,
					 skb_end_pointer(skb) - skb->head,
					 nss_tx_dir(skb));
		else
			dma_unmap_single(core->dev, NSS_SKB_CB(skb)->dma,
					 NSS_EMPTY_BUFFER_ALLOC, DMA_FROM_DEVICE);
		dev_kfree_skb_any(skb);
	}

	kvfree(core->lent);
	kvfree(core->lent_free);
	core->lent = NULL;
	core->lent_free = NULL;
	core->lent_free_n = 0;
	core->lent_size = 0;
}

static int nss_lent_init(struct nss_core *core, u32 n)
{
	u32 i;

	spin_lock_init(&core->lent_lock);
	if (!n || n > INT_MAX)
		return -EINVAL;

	core->lent = kvcalloc(n, sizeof(*core->lent), GFP_KERNEL);
	core->lent_free = kvmalloc_array(n, sizeof(*core->lent_free),
					 GFP_KERNEL);
	if (!core->lent || !core->lent_free) {
		kvfree(core->lent);
		kvfree(core->lent_free);
		core->lent = NULL;
		core->lent_free = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < n; i++) {
		core->lent_free[i] = i;
		core->lent[i].cookie = i;
	}
	core->lent_free_n = n;
	core->lent_size = n;
	core->lent_shift = fls(n - 1);

	return 0;
}

static int nss_refill(struct nss_core *core, int budget)
{
	struct nss_h2n_ring *ring = &core->h2n[NSS_H2N_RING_EMPTY_BUF];
	struct nss_if_mem_map *map = core->if_map;
	u32 size = NSS_EMPTY_BUFFER_ALLOC;
	int filled = 0;
	int ret;

	guard(spinlock_bh)(&ring->lock);

	while (filled < budget) {
		u32 next = (ring->hlos_index + 1) & (NSS_RING_ENTRIES - 1);
		struct h2n_descriptor *desc;
		struct sk_buff *skb;
		dma_addr_t dma;

		if (atomic_read(&core->buffers_queued) + filled >= core->pool_size)
			break;

		if (next == READ_ONCE(map->h2n_nss_index[NSS_H2N_RING_EMPTY_BUF]))
			break;

		/* Custody of these buffers passes to the firmware for as long
		 * as it likes, so they are allocated outright rather than
		 * carved out of the per-CPU fragment cache, where each one in
		 * flight would pin the whole page it came from.
		 */
		skb = alloc_skb(size, GFP_ATOMIC);
		if (!skb)
			break;

		dma = dma_map_single(core->dev, skb->head, size,
				     DMA_FROM_DEVICE);
		if (dma_mapping_error(core->dev, dma)) {
			kfree_skb(skb);
			break;
		}

		NSS_SKB_CB(skb)->dma = dma;
		NSS_SKB_CB(skb)->tx = false;
		NSS_SKB_CB(skb)->bidir = false;

		ret = nss_lent_take(core, skb);
		if (ret < 0) {
			dma_unmap_single(core->dev, dma, size, DMA_FROM_DEVICE);
			kfree_skb(skb);
			break;
		}
		desc = &ring->desc[ring->hlos_index];
		desc->buffer = dma;
		desc->buffer_len = size;
		desc->payload_len = 0;
		desc->payload_offs = 0;
		desc->buffer_type = NSS_H2N_BUFFER_EMPTY;
		desc->bit_flags = 0;
		desc->interface_num = 0;
		desc->opaque = NSS_SKB_CB(skb)->cookie;

		ring->hlos_index = next;
		filled++;
	}

	if (filled) {
		dma_wmb();
		WRITE_ONCE(map->h2n_hlos_index[NSS_H2N_RING_EMPTY_BUF],
			   ring->hlos_index);
		atomic_add(filled, &core->buffers_queued);
		nss_doorbell(core, NSS_H2N_INTR_EMPTY_BUF);
	}

	return filled;
}

/* The firmware is up the first time it asks for buffers. */
static void nss_check_booted(struct nss_core *core)
{
	scoped_guard(spinlock_bh, &core->state_lock) {
		/* A queued SOS from a faulted image is not a new boot. */
		if (!core->booting)
			return;

		core->booting = false;
		core->running = true;
	}
	complete(&core->booted);
	dev_info(core->dev, "core %u booted\n", core->id);
}

static int nss_poll_sos(struct napi_struct *napi, int budget)
{
	struct nss_irq_ctx *ctx = container_of(napi, struct nss_irq_ctx, napi);
	struct nss_core *core = ctx->core;

	nss_check_booted(core);
	/* Every free slot rather than a poll-sized batch. The firmware asks
	 * when its own pool is low and takes what it needs, so a partial
	 * answer leaves it short and it has to ask again.
	 */
	nss_refill(core, NSS_RING_ENTRIES);

	napi_complete(napi);
	enable_irq(ctx->irq);

	return 0;
}

/* Take back whatever the firmware has returned. A buffer comes back either as
 * a packet on a data queue or as an unused one on the return queue; both are
 * released here, because nothing above this driver consumes them yet.
 */
static int nss_poll_n2h(struct napi_struct *napi, int budget)
{
	struct nss_irq_ctx *ctx = container_of(napi, struct nss_irq_ctx, napi);
	struct nss_core *core = ctx->core;
	struct nss_n2h_ring *ring;
	struct nss_if_mem_map *map;
	u32 nss_index, count;
	int done = 0;
	int qid;

	/* Ring 0 carries buffers the firmware is giving back unused; rings 1
	 * to 4 are the four data queues.
	 */
	if (ctx->cause == NSS_CAUSE_EMPTY_BUFFER_QUEUE)
		qid = NSS_N2H_RING_EMPTY_BUF;
	else
		qid = ctx->cause - NSS_CAUSE_DATA_QUEUE_0 + 1;

	ring = &core->n2h[qid];
	map = core->if_map;

	nss_index = READ_ONCE(map->n2h_nss_index[qid]);
	count = (nss_index - ring->hlos_index) & (NSS_RING_ENTRIES - 1);
	count = min_t(u32, count, budget);
	dma_rmb();

	while (done < count) {
		struct n2h_descriptor *desc = &ring->desc[ring->hlos_index];
		struct sk_buff *skb;

		/* The message buffer comes back on this ring like everything
		 * else, and is the host's own: it is answered rather than
		 * freed, and it was never one of the donated buffers.
		 */
		core->rx_type[desc->buffer_type & 7]++;
		if (desc->interface_num < NSS_INTERFACE_MAX)
			core->rx_iface[desc->interface_num]++;

		/* Resolve the opaque slot and generation in the host loan table.
		 * Only its live owner may supply skb metadata or a DMA mapping.
		 * A failed claim leaves the loan for a valid return or core halt;
		 * message replies use their separate coherent buffer.
		 */
		if (!nss_msg_complete(core, desc)) {
			skb = nss_lent_claim(core, desc->opaque, desc->buffer);

			if (!skb) {
				atomic64_inc(&core->rx_desync);
				dev_warn_ratelimited(core->dev,
						     "n2h: unclaimed cookie %llx buf %x type %u q %d\n",
						     desc->opaque, desc->buffer,
						     desc->buffer_type, qid);
			/* A buffer this driver transmitted comes back on the
			 * same ring, and its descriptor still carries the
			 * offset and length of the frame that was sent. Read
			 * as a receive it is reserved and put a second time
			 * over data already there, which walks the tail off
			 * the end of the allocation. What the host sent is a
			 * completion and nothing else, so it is claimed here
			 * before any of it is believed.
			 */
			} else if (NSS_SKB_CB(skb)->tx) {
				atomic64_inc(&core->tx_done);
				/* A frame sent to a host-side node that the
				 * firmware did not route comes back in its own
				 * buffer, as a packet rather than a completion.
				 */
				if (nss_redir_recv(core, desc, skb))
					goto next;
				dma_unmap_single(core->dev, NSS_SKB_CB(skb)->dma,
						 skb_end_pointer(skb) - skb->head,
						 nss_tx_dir(skb));
				dev_kfree_skb_any(skb);
			} else {
				atomic_dec(&core->buffers_queued);
				if (nss_redir_recv(core, desc, skb) ||
				    nss_mesh_recv(core, napi, desc, skb) ||
				    nss_wifili_special_recv(core, napi, desc, skb) ||
				    nss_data_recv(core, napi, desc, skb))
					goto next;
				dma_unmap_single(core->dev, NSS_SKB_CB(skb)->dma,
						 NSS_EMPTY_BUFFER_ALLOC,
						 DMA_FROM_DEVICE);
				nss_ext_take(core, desc, skb);
				nss_notify_recv(core, desc, skb);
				dev_kfree_skb_any(skb);
			}
		}
next:
		ring->hlos_index = (ring->hlos_index + 1) &
				   (NSS_RING_ENTRIES - 1);
		done++;
	}

	if (done)
		WRITE_ONCE(map->n2h_hlos_index[qid], ring->hlos_index);

	/* Top the ring up rather than replacing only what came back. The
	 * firmware holds one buffer per connection it accelerates, for the
	 * sync message that connection will send, so a pool kept level with
	 * returns settles at a ring's worth and every create rule after that
	 * is refused for want of a buffer. The ceiling is the host's, not the
	 * firmware's, which takes whatever it is offered.
	 */
	nss_refill(core, NSS_RING_ENTRIES);

	if (done < budget) {
		napi_complete(napi);
		enable_irq(ctx->irq);
	}

	return done;
}

/* A core that has dumped is finished. Stock brings the whole box down here,
 * on the argument that the data plane is gone either way; this driver keeps
 * the box routing on the host, reads the firmware log, and restarts the core.
 */
static int nss_poll_coredump(struct napi_struct *napi, int budget)
{
	struct nss_irq_ctx *ctx = container_of(napi, struct nss_irq_ctx, napi);
	struct nss_core *core = ctx->core;

	dev_err(core->dev, "core %u has crashed\n", core->id);
	nss_log_dump(core, "coredump");
	nss_core_fault(core);

	napi_complete(napi);

	/* The line is deliberately left masked: the core is not coming back
	 * without a reset, and re-arming would only invite the same interrupt
	 * again.
	 */
	return 0;
}

static irqreturn_t nss_isr(int irq, void *data)
{
	struct nss_irq_ctx *ctx = data;

	disable_irq_nosync(irq);
	napi_schedule(&ctx->napi);

	return IRQ_HANDLED;
}

typedef int (*nss_poll_t)(struct napi_struct *napi, int budget);

static nss_poll_t nss_poll_fn(enum nss_cause cause)
{
	switch (cause) {
	case NSS_CAUSE_EMPTY_BUFFER_SOS:
		return nss_poll_sos;
	case NSS_CAUSE_EMPTY_BUFFER_QUEUE:
	case NSS_CAUSE_DATA_QUEUE_0:
	case NSS_CAUSE_DATA_QUEUE_1:
	case NSS_CAUSE_DATA_QUEUE_2:
	case NSS_CAUSE_DATA_QUEUE_3:
		return nss_poll_n2h;
	case NSS_CAUSE_COREDUMP_COMPLETE:
		return nss_poll_coredump;
	default:
		/* Paged buffers, transmit unblocking and the profiler are not
		 * used by this driver, so their lines are left unclaimed:
		 * they are edge triggered, and nothing here would act on
		 * them.
		 */
		return NULL;
	}
}

int nss_rings_start(struct nss_core *core)
{
	struct platform_device *pdev = to_platform_device(core->dev);
	unsigned int cpu;
	int i, ret;

	core->ndev = alloc_netdev_dummy(0);
	if (!core->ndev)
		return -ENOMEM;

	/* Empty buffers fill the table up to the pool ceiling, so transmitted
	 * frames need room of their own beside them.
	 */
	ret = nss_lent_init(core, 2 * nss_pool_ceiling());
	if (ret)
		goto err;

	for (i = 0; i < NSS_CAUSE_MAX; i++) {
		struct nss_irq_ctx *ctx = &core->irq[i];
		nss_poll_t poll = nss_poll_fn(i);

		ctx->core = core;
		ctx->cause = i;

		if (!poll)
			continue;

		ctx->irq = platform_get_irq(pdev, i);
		if (ctx->irq < 0) {
			ret = ctx->irq;
			goto err;
		}

		ret = request_irq(ctx->irq, nss_isr, IRQF_NO_AUTOEN,
				  dev_name(core->dev), ctx);
		if (ret)
			goto err;

		/* One CPU per data queue, which the firmware spreads
		 * host-bound frames over by flow.
		 */
		if (i >= NSS_CAUSE_DATA_QUEUE_0 &&
		    i <= NSS_CAUSE_DATA_QUEUE_3) {
			cpu = cpumask_local_spread(i - NSS_CAUSE_DATA_QUEUE_0,
						   NUMA_NO_NODE);
			irq_set_affinity_and_hint(ctx->irq, cpumask_of(cpu));
		}

		netif_napi_add_weight(core->ndev, &ctx->napi, poll,
				      NSS_NAPI_WEIGHT);
		napi_enable(&ctx->napi);
		ctx->napi_added = true;
		enable_irq(ctx->irq);
	}

	return 0;

err:
	nss_rings_stop(core);

	return ret;
}

/* Wait out any receive poll already running.
 *
 * A poll that has read a callback the WLAN driver is withdrawing is holding a
 * pointer into memory that driver is about to free, and clearing the pointer
 * does not reach a poll that read it a moment earlier.
 */
void nss_rings_quiesce(struct nss_core *core)
{
	int i;

	for (i = 0; i < NSS_CAUSE_MAX; i++)
		if (core->irq[i].napi_added)
			napi_synchronize(&core->irq[i].napi);
}

void nss_rings_stop(struct nss_core *core)
{
	int i;

	for (i = 0; i < NSS_CAUSE_MAX; i++) {
		struct nss_irq_ctx *ctx = &core->irq[i];

		if (!ctx->napi_added)
			continue;

		/* The poll first: it ends by re-enabling the line, which must
		 * still be claimed. A disabled NAPI ignores the interrupt.
		 */
		napi_disable(&ctx->napi);
		irq_update_affinity_hint(ctx->irq, NULL);
		free_irq(ctx->irq, ctx);
		netif_napi_del(&ctx->napi);
		ctx->napi_added = false;
	}

	if (core->ndev) {
		free_netdev(core->ndev);
		core->ndev = NULL;
	}

	/* After the poll is gone, because it is the other side of the table. */
	nss_lent_reclaim(core);
}
