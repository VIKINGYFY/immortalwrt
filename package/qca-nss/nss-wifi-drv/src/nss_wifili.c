// SPDX-License-Identifier: GPL-2.0-only
/* Bringing wifili up against rings the host owns.
 *
 * The initialisation message is the largest structure in this ABI and the one
 * with the most room to be silently misread: a field whose offset moved
 * between firmware lines is accepted, stored, and produces a data plane that
 * does not work for reasons nothing reports. It can be checked without a
 * radio, and it is checked here before there is one.
 *
 * The method is refusal: each one-field corruption is refused with its own
 * error code, so each arm below confirms one offset, and the valid arm that
 * follows them confirms that the whole structure is acceptable.
 *
 * The rings are the host's own coherent memory and nothing consumes them.
 * That is enough for every check here, because what is under test is the
 * message rather than the data plane.
 */

#include <linux/container_of.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/if_ether.h>
#include <linux/igmp.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/addrconf.h>
#include <net/if_inet6.h>
#include <net/ip.h>
#include <net/mld.h>

#include "nss_drv.h"
#include <linux/soc/qcom/nss_wifi.h>

/* Small on purpose. Nothing drains these, and the receive ring the firmware
 * fills from its own pool is the one that would cost something if it were not.
 */
#define NSS_WIFILI_RING_ENTRIES		32
#define NSS_WIFILI_ENTRY_WORDS		8
/* Transmit descriptors per pool. The firmware holds one for every frame in
 * flight and refuses the frame when it cannot get one, silently as far as the
 * host is concerned - only the firmware's own desc_alloc_fail counter shows
 * it, and at 512 it still dropped frames on a busy radio. Use 4096 on small
 * memory boards for moderate bursts and 8192 on larger boards. The probe
 * below hands over a single page and keeps a pool that fits it.
 */
#define NSS_WIFILI_TX_DESC		8192
#define NSS_WIFILI_PROBE_TX_DESC	512
#define NSS_WIFILI_TX_PROCESSQ		1024

/* The firmware carves both kinds out of the memory given here, at a stride
 * of its own for each, and a block smaller than the declared count leads to
 * a range trap. The size follows the count: every pool's descriptors plus one
 * spare per pool.
 */
#define NSS_WIFILI_TX_DESC_STRIDE	80
#define NSS_WIFILI_TX_DESC_EXT_STRIDE	160

#define NSS_WIFILI_TX_DESC_MEM(n, pools, stride) \
	(((n) + 1) * (pools) * (stride))

/* The firmware finds a descriptor by page and offset, and a page is at most
 * this long: one larger block starts but transmits nothing. It is a whole
 * number of descriptors of either stride.
 */
#define NSS_WIFILI_TX_DESC_PAGE		(240 * SZ_1K)

/* The receive buffer the hardware would be programmed with, and the tag block
 * in front of it. Both are bounds-checked by the firmware, which is why they
 * are here rather than left zero.
 */
#define NSS_WIFILI_TLV_SIZE		256
#define NSS_WIFILI_RX_BUF_LEN		2048

/* This SoC, as the firmware enumerates targets. */
#define NSS_WIFILI_TARGET_QCA8074V2	24

/* Where the per-radio rings begin, which is what tells the firmware to read
 * their write pointer out of the second shadow array rather than a register.
 */
#define NSS_WIFILI_LMAC_RING_START	128

enum nss_wifili_ring_id {
	NSS_WIFILI_RING_TCL,
	NSS_WIFILI_RING_TX_COMP,
	NSS_WIFILI_RING_REO_DEST,
	NSS_WIFILI_RING_REO_REINJECT,
	NSS_WIFILI_RING_RX_REL,
	NSS_WIFILI_RING_REO_EXCEPTION,
	NSS_WIFILI_RING_MAX,
};

/* Which of the deliberate faults to introduce, or none. */
enum nss_wifili_fault {
	NSS_WIFILI_FAULT_NONE,
	NSS_WIFILI_FAULT_RING_ALIGN,
	NSS_WIFILI_FAULT_RING_DIR,
	NSS_WIFILI_FAULT_RING_ENTRIES,
	NSS_WIFILI_FAULT_TARGET,
	NSS_WIFILI_FAULT_TLV_SIZE,
	NSS_WIFILI_FAULT_RX_BUF_LEN,
};

struct nss_wifili_mem {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

struct nss_wifili_ctx {
	struct nss_wifili_mem ring[NSS_WIFILI_RING_MAX];
	struct nss_wifili_mem shadow;
	struct nss_wifili_mem doorbell;
	struct nss_wifili_mem txdesc;
	struct nss_wifili_mem txdescext;
};

/* The one core that runs wifili. The WLAN driver is loaded and unloaded
 * independently of this one and knows nothing about which core is which, so
 * the binding lives here.
 */
static struct nss_core *nss_wifili_core;
static DEFINE_MUTEX(nss_wifili_lock);

/* Fixed before either bus driver probes. Zero keeps the non-WLAN data plane
 * independent of ath11k; the service selects only installed, usable buses.
 */
#define NSS_WLAN_AHB	BIT(0)
#define NSS_WLAN_PCI	BIT(1)
static unsigned int nss_wlan = NSS_WLAN_AHB | NSS_WLAN_PCI;
module_param_named(wlan, nss_wlan, uint, 0444);
MODULE_PARM_DESC(wlan, "WLAN buses: bit 0 AHB, bit 1 PCI; 0 for non-WLAN offload only");

/* One wifili instance: what the WLAN driver said about its rings, and what
 * the firmware made of them. Instance 0 carries the radios inside the SoC,
 * the others radios on their own chip, each under an interface and a node
 * type of its own.
 */
struct nss_wifili_soc {
	struct nss_wifi_soc desc;
	struct nss_wifi_pdev pdev[NSS_WIFILI_MAX_PDEV_NUM_MSG];
	int radio_if[NSS_WIFILI_MAX_PDEV_NUM_MSG];
	u8 pdevs;
	bool used;
	bool started;
	int last_error;
	int vdev_error;
	/* The device of the radios whose rings the firmware reaps. Every
	 * interface on it is the firmware's, with or without a virtual device
	 * of its own. The bus device outlives the WLAN driver, so its address
	 * stays its own.
	 */
	struct device *radio;
	/* The descriptor pages, normal ones first, then from txext on the
	 * extension ones.
	 */
	struct nss_wifili_mem txdesc[NSS_WIFILI_TX_DESC_PAGES];
	u8 txpages;
	u8 txext;
	struct nss_wifili_device_stats stats;
	u32 stats_seen;
};

static const struct {
	u32 if_num;
	u32 node;
} nss_wifili_soc_if[] = {
	{ NSS_INTERFACE_WIFILI, NSS_DYNAMIC_INTERFACE_TYPE_WIFILI_INTERNAL },
	{ NSS_INTERFACE_WIFILI_EXT0, NSS_DYNAMIC_INTERFACE_TYPE_WIFILI_EXTERNAL0 },
	{ NSS_INTERFACE_WIFILI_EXT1, NSS_DYNAMIC_INTERFACE_TYPE_WIFILI_EXTERNAL1 },
};

static struct nss_wifili_soc nss_wifili_socs[ARRAY_SIZE(nss_wifili_soc_if)];

static u32 nss_wifili_soc_ifnum(const struct nss_wifili_soc *soc)
{
	return nss_wifili_soc_if[soc - nss_wifili_socs].if_num;
}

/* A registered instance, under nss_wifili_lock. */
static struct nss_wifili_soc *nss_wifili_soc_get(int id)
{
	if (id < 0 || id >= ARRAY_SIZE(nss_wifili_socs) ||
	    !nss_wifili_socs[id].used)
		return NULL;

	return &nss_wifili_socs[id];
}

/* The instance a firmware message is about, from the softirq that delivers
 * it. The callbacks it carries are withdrawn before the instance is reused.
 */
static struct nss_wifili_soc *nss_wifili_soc_of(u32 if_num)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(nss_wifili_soc_if); i++)
		if (nss_wifili_soc_if[i].if_num == NSS_INTERFACE_NUM_GET(if_num))
			return &nss_wifili_socs[i];

	return NULL;
}

bool nss_wifili_is_soc(u32 if_num)
{
	return nss_wifili_soc_of(if_num);
}

bool nss_wifili_owns(const struct net_device *dev)
{
	int i;

	if (!dev->dev.parent)
		return false;

	for (i = 0; i < ARRAY_SIZE(nss_wifili_socs); i++)
		if (dev->dev.parent == READ_ONCE(nss_wifili_socs[i].radio))
			return true;

	return false;
}

void nss_wifili_bind(struct nss_core *core)
{
	guard(mutex)(&nss_wifili_lock);
	nss_wifili_core = core;
}

/* Keep what the WLAN driver says; starting the firmware is a separate act,
 * because a running core takes the hardware with it. Returns the instance.
 */
int nss_wifi_soc_register_v2(const struct nss_wifi_soc *desc)
{
	struct nss_wifili_soc *soc;
	int i;

	guard(mutex)(&nss_wifili_lock);

	if (!(nss_wlan & (desc->external ? NSS_WLAN_PCI : NSS_WLAN_AHB)))
		return -EOPNOTSUPP;

	if (desc->num_tcl > NSS_WIFI_MAX_TCL_RINGS ||
	    desc->num_reo > NSS_WIFI_MAX_REO_RINGS ||
	    desc->num_pdevs > NSS_WIFILI_MAX_PDEV_NUM_MSG)
		return -EINVAL;

	for (i = desc->external; i < ARRAY_SIZE(nss_wifili_socs); i++)
		if (!nss_wifili_socs[i].used)
			break;
	if (i == ARRAY_SIZE(nss_wifili_socs) || (!desc->external && i))
		return -EBUSY;

	soc = &nss_wifili_socs[i];
	soc->desc = *desc;
	soc->pdevs = 0;
	soc->vdev_error = 0;
	memset(soc->radio_if, 0xff, sizeof(soc->radio_if));
	soc->used = true;

	return i;
}
EXPORT_SYMBOL_GPL(nss_wifi_soc_register_v2);

static void nss_wifili_soc_stop(struct nss_core *core,
				struct nss_wifili_soc *soc);
static void nss_wifili_forget(struct nss_core *core);

static bool nss_wifili_any_started(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(nss_wifili_socs); i++)
		if (nss_wifili_socs[i].started)
			return true;

	return false;
}

void nss_wifi_soc_unregister(int id)
{
	struct nss_core *core;
	struct nss_wifili_soc *soc;

	guard(mutex)(&nss_wifili_lock);

	soc = nss_wifili_soc_get(id);
	if (!soc)
		return;

	/* The WLAN driver is about to free the rings this reaches into, and a
	 * receive poll that read the callback a moment ago is still holding
	 * it, so the withdrawal is not complete until that poll has ended.
	 */
	WRITE_ONCE(soc->desc.link_desc_return, NULL);
	WRITE_ONCE(soc->desc.rx_80211, NULL);
	WRITE_ONCE(soc->desc.peer_stats, NULL);
	WRITE_ONCE(soc->desc.wds_event, NULL);
	soc->used = false;
	soc->pdevs = 0;

	core = nss_wifili_core;
	if (!core)
		return;

	/* A firmware started on those rings keeps reaping them after they are
	 * freed and after they are allocated again for the next WLAN
	 * instance, whose entries it then resolves against descriptors it
	 * never issued. A running one is told to let go of them. A dead one
	 * is halted, but only once every radio it lent buffers to has
	 * stopped, which is when the last of their resets gets here.
	 */
	if (soc->started && core->running) {
		scoped_guard(mutex, &core->lock)
			nss_wifili_soc_stop(core, soc);
	} else if (soc->started) {
		soc->started = false;
		if (!nss_wifili_any_started()) {
			nss_core_stop(core);
			nss_wifili_forget(core);
		}
	} else {
		nss_rings_quiesce(core);
	}
}
EXPORT_SYMBOL_GPL(nss_wifi_soc_unregister);

static bool nss_wifili_described(const struct nss_wifili_soc *soc)
{
	return soc->used && soc->pdevs == soc->desc.num_pdevs;
}

int nss_wifi_pdev_register(int id, const struct nss_wifi_pdev *pdev)
{
	struct nss_core *core = NULL;
	struct nss_wifili_soc *soc;

	scoped_guard(mutex, &nss_wifili_lock) {
		soc = nss_wifili_soc_get(id);
		if (!soc || soc->pdevs >= soc->desc.num_pdevs)
			return -EINVAL;

		soc->pdev[soc->pdevs++] = *pdev;

		pr_info("nss-wifi-drv: radio %u rxdma id %u base %#x x%u hw %#x/%#x swdesc %u\n",
			pdev->radio_id, pdev->rxdma.id, pdev->rxdma.base,
			pdev->rxdma.num_entries, pdev->rxdma.hwreg[0],
			pdev->rxdma.hwreg[1], pdev->num_rx_swdesc);

		if (nss_wifili_described(soc))
			core = nss_wifili_core;
	}

	/* Complete the ring handover before mac80211 restores interfaces.
	 * The restart worker takes the WiFiLi mutex, so wait outside it.
	 */
	if (core)
		nss_core_resume(core);

	return 0;
}
EXPORT_SYMBOL_GPL(nss_wifi_pdev_register);

/* Reset the WLAN driver because the core runs Wi-Fi (@started): the radios
 * hold buffers it lent, freed once they have stopped, and the reset halts the
 * core on its way down. Or because a running core ran Wi-Fi before a halt:
 * the reset is what registers the radios' interfaces with it.
 */
bool nss_wifili_reset(struct nss_core *core, bool started)
{
	bool reset = false;
	int i;

	guard(mutex)(&nss_wifili_lock);

	if (!core->wifi_on || core->wifili_started != started ||
	    (!started && !core->running))
		return false;

	for (i = 0; i < ARRAY_SIZE(nss_wifili_socs); i++) {
		struct nss_wifili_soc *soc = &nss_wifili_socs[i];

		if (!soc->used || !soc->desc.reset ||
		    (started && !soc->started))
			continue;

		soc->desc.reset(soc->desc.priv);
		reset = true;
	}

	return reset;
}

/* The one message the firmware sends unasked that must be acted on.
 *
 * It reaps rings holding MSDU-link descriptors it cannot put back, because
 * the ring that returns one to the idle list stays with the WLAN driver, and
 * hands each one back here instead. An unreturned descriptor is gone from the
 * idle list for good, and an empty idle list stops receive on every radio
 * rather than only on the path that emptied it - so late is the same as never
 * and this may not be deferred.
 */
static DEFINE_SPINLOCK(nss_wifili_stats_lock);

/* What the firmware says about its own receive path. Empty rings and full
 * rings that drop look identical from the host, and these are what tell them
 * apart.
 */
static void nss_wifili_soc_stats_print(struct seq_file *s,
				       const struct nss_wifili_soc *soc)
{
	struct nss_wifili_device_stats st;
	u32 seen;
	int i;

	spin_lock_bh(&nss_wifili_stats_lock);
	st = soc->stats;
	seen = soc->stats_seen;
	spin_unlock_bh(&nss_wifili_stats_lock);

	seq_printf(s, "wifili %u reports %u\n", nss_wifili_soc_ifnum(soc),
		   seen);
	if (!seen)
		return;

	for (i = 0; i < NSS_WIFILI_MAX_PDEV_NUM_MSG; i++)
		seq_printf(s, "radio%d replenished %u hw_desc_unavailable %u  pool no_pb %u alloc %u alloc_fail %u\n",
			   i, st.rxdma_stats[i].rx_buf_replenished,
			   st.rxdma_stats[i].rx_hw_desc_unavailable,
			   st.rx_sw_pool_stats[i].rx_no_pb,
			   st.rx_sw_pool_stats[i].desc_alloc,
			   st.rx_sw_pool_stats[i].desc_alloc_fail);

	for (i = 0; i < NSS_WIFILI_MAX_REO_DATA_RINGS_MSG; i++)
		seq_printf(s, "reo%d reaped %u error %u invalid_cookie %u defrag %u\n",
			   i, st.rxreo_stats[i].ring_reaped,
			   st.rxreo_stats[i].ring_error,
			   st.rxreo_stats[i].invalid_cookie,
			   st.rxreo_stats[i].defrag_reaped);

	seq_printf(s, "wbm invalid_buf_mgr %u src_rxdma %u src_reo %u src_invalid %u\n",
		   st.rxwbm_stats.invalid_buf_mgr,
		   st.rxwbm_stats.err_src_rxdma,
		   st.rxwbm_stats.err_src_reo,
		   st.rxwbm_stats.err_src_invalid);
}

void nss_wifili_stats_print(struct seq_file *s)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(nss_wifili_socs); i++)
		nss_wifili_soc_stats_print(s, &nss_wifili_socs[i]);
}

static void nss_wifili_peer_stats(const struct nss_wifili_soc *soc,
				  const struct nss_cmn_msg *ncm, u32 len)
{
	void (*report)(void *priv, u16 peer_id,
		       const struct nss_wifi_peer_stats *st);
	const struct nss_wifili_peer_stats_msg *m;
	u32 body, n, stride, i;
	const u8 *p;

	report = READ_ONCE(soc->desc.peer_stats);
	body = len - sizeof(*ncm);
	if (!report || body < sizeof(*m) || ncm->len < sizeof(*m))
		return;

	/* The length names a message of one peer, whatever the count. */
	stride = ncm->len - sizeof(*m);
	if (stride < sizeof(m->wpcs[0]))
		return;

	m = (const void *)(ncm + 1);
	n = min_t(u32, m->npeers, (body - sizeof(*m)) / stride);

	for (i = 0, p = (const u8 *)m->wpcs; i < n; i++, p += stride) {
		const struct nss_wifili_peer_ctrl_stats *c = (const void *)p;
		struct nss_wifi_peer_stats st = {
			.tx_packets = c->tx.tx_ucast_cnt + c->tx.tx_mcast_cnt +
				      c->tx.tx_bcast_cnt,
			.tx_bytes = c->tx.tx_ucast_bytes + c->tx.tx_mcast_bytes +
				    c->tx.tx_bcast_bytes,
			.tx_acked = c->tx.tx_success_cnt,
			.tx_retries = c->tx.retries,
			.rx_packets = c->rx.rx_recvd,
			.rx_bytes = c->rx.rx_recvd_bytes,
		};
		int j;

		for (j = 0; j < ARRAY_SIZE(c->tx.drop_stats); j++)
			st.tx_failed += c->tx.drop_stats[j];

		report(soc->desc.priv, c->peer_id, &st);
	}
}

static bool nss_wifili_wds_notify(struct nss_wifili_soc *soc,
				  const struct nss_cmn_msg *cm, size_t len);

void nss_wifili_notify(struct nss_core *core, const struct nss_cmn_msg *ncm,
		       u32 len)
{
	struct nss_wifili_soc *soc = nss_wifili_soc_of(ncm->interface);
	void (*home)(void *priv, const u32 *buf_addr_info);
	const struct nss_wifili_msg *m;

	if (ncm->type == NSS_WIFILI_PEER_STATS_MSG) {
		nss_wifili_peer_stats(soc, ncm, len);
		return;
	}

	if (nss_wifili_wds_notify(soc, ncm, len))
		return;

	/* The firmware counts its own receive path, and those counters are the
	 * only place that separates a radio whose buffers were never filled
	 * from one that fills and drops. They arrive unasked once statistics
	 * are on, so the newest is kept and read out on demand.
	 */
	if (ncm->type == NSS_WIFILI_STATS_MSG &&
	    len >= offsetof(struct nss_wifili_msg, msg) +
		   sizeof(struct nss_wifili_stats_sync_msg) -
		   core->fw->soc_stats_skip) {
		const size_t head = offsetof(struct nss_wifili_device_stats,
					     rxreo_stats);

		m = container_of_const(ncm, struct nss_wifili_msg, cm);
		spin_lock_bh(&nss_wifili_stats_lock);
		memcpy((u8 *)&soc->stats + head,
		       (const u8 *)&m->msg.wlsoc_stats.stats + head -
		       core->fw->soc_stats_skip,
		       sizeof(soc->stats) - head);
		soc->stats_seen++;
		spin_unlock_bh(&nss_wifili_stats_lock);
		return;
	}

	if (ncm->type != NSS_WIFILI_LINK_DESC_INFO_MSG ||
	    len < offsetof(struct nss_wifili_msg, msg) +
		  sizeof(struct nss_wifili_soc_linkdesc_buf_info_msg))
		return;

	/* Counted before the way home is looked up, because whether the
	 * firmware sends these at all, and how often, is what this rung is
	 * measuring: there is no receive to lose yet, and no route back until
	 * the shape of what arrives has been read off the device.
	 */
	atomic64_inc(&core->link_desc_seen);

	home = READ_ONCE(soc->desc.link_desc_return);
	if (!home)
		return;

	m = container_of_const(ncm, struct nss_wifili_msg, cm);
	home(soc->desc.priv, (const u32 *)&m->msg.linkdescinfomsg);
	core->link_desc_returned++;
}

static void nss_wifili_ring_fill(struct nss_wifili_hal_srng_info *r,
				 const struct nss_wifi_ring *src)
{
	r->ring_id = src->id;
	r->ring_dir = src->dir;
	r->num_entries = src->num_entries;
	r->entry_size = src->entry_size;
	r->flags = src->flags;
	r->ring_base_paddr = src->base;
	r->hwreg_base[0] = src->hwreg[0];
	r->hwreg_base[1] = src->hwreg[1];
}

/* Freed with the rest of the core's memory when it halts. */
static int nss_wifili_mem_get(struct nss_core *core, struct nss_wifili_mem *m,
			      size_t size)
{
	u32 dma;

	m->size = size;
	m->cpu = nss_mem_alloc(core, size, SMP_CACHE_BYTES, &dma);
	m->dma = dma;

	return m->cpu ? 0 : -ENOMEM;
}

static void nss_wifili_srng(struct nss_wifili_hal_srng_info *r,
			    const struct nss_wifili_ctx *w,
			    enum nss_wifili_ring_id id, u32 dir)
{
	r->ring_id = id;
	r->ring_base_paddr = w->ring[id].dma;
	r->num_entries = NSS_WIFILI_RING_ENTRIES;
	r->entry_size = NSS_WIFILI_ENTRY_WORDS;
	r->ring_dir = dir;
	r->flags = 0;

	/* The firmware writes the ring's head or tail through this address as
	 * if it were a register. Nothing here has a register to give it, so it
	 * gets a page of the host's own that no one reads.
	 */
	r->hwreg_base[0] = w->doorbell.dma;
	r->hwreg_base[1] = w->doorbell.dma + id * 16;
}

static void nss_wifili_init_fill(struct nss_wifili_init_msg *init,
				 const struct nss_wifili_ctx *w,
				 enum nss_wifili_fault fault)
{
	init->hssm.shadow_rdptr_mem_addr = w->shadow.dma;
	init->hssm.shadow_wrptr_mem_addr = w->shadow.dma + SZ_1K;
	init->hssm.lmac_rings_start_id = NSS_WIFILI_LMAC_RING_START;

	init->num_tcl_data_rings = 1;
	init->num_reo_dest_rings = 1;

	nss_wifili_srng(&init->tcl_ring_info[0], w, NSS_WIFILI_RING_TCL, 0);
	nss_wifili_srng(&init->tx_comp_ring[0], w, NSS_WIFILI_RING_TX_COMP, 1);
	nss_wifili_srng(&init->reo_dest_ring[0], w, NSS_WIFILI_RING_REO_DEST, 1);
	nss_wifili_srng(&init->reo_reinject_ring, w,
			NSS_WIFILI_RING_REO_REINJECT, 0);
	nss_wifili_srng(&init->rx_rel_ring, w, NSS_WIFILI_RING_RX_REL, 1);
	nss_wifili_srng(&init->reo_exception_ring, w,
			NSS_WIFILI_RING_REO_EXCEPTION, 1);

	init->wtdim.num_tx_desc = NSS_WIFILI_PROBE_TX_DESC;
	init->wtdim.num_tx_desc_ext = NSS_WIFILI_PROBE_TX_DESC;
	init->wtdim.num_pool = 1;
	init->wtdim.num_memaddr = 1;
	init->wtdim.memory_addr[0] = w->txdesc.dma;
	init->wtdim.memory_size[0] = w->txdesc.size;
	init->wtdim.num_tx_device_limit = NSS_WIFILI_PROBE_TX_DESC;

	init->target_type = NSS_WIFILI_TARGET_QCA8074V2;
	init->wrip.tlv_size = NSS_WIFILI_TLV_SIZE;
	init->wrip.rx_buf_len = NSS_WIFILI_RX_BUF_LEN;

	switch (fault) {
	case NSS_WIFILI_FAULT_RING_ALIGN:
		init->tcl_ring_info[0].ring_base_paddr |= 1;
		break;
	case NSS_WIFILI_FAULT_RING_DIR:
		init->tcl_ring_info[0].ring_dir = 2;
		break;
	case NSS_WIFILI_FAULT_RING_ENTRIES:
		init->tcl_ring_info[0].num_entries = 64 * 1024;
		break;
	case NSS_WIFILI_FAULT_TARGET:
		init->target_type = 0;
		break;
	case NSS_WIFILI_FAULT_TLV_SIZE:
		init->wrip.tlv_size = 8;
		break;
	case NSS_WIFILI_FAULT_RX_BUF_LEN:
		init->wrip.rx_buf_len = 4096;
		break;
	case NSS_WIFILI_FAULT_NONE:
		break;
	}
}

static int nss_wifili_alloc(struct nss_core *core, struct nss_wifili_ctx *w)
{
	size_t ring = NSS_WIFILI_RING_ENTRIES * NSS_WIFILI_ENTRY_WORDS * 4;
	int i, ret;

	for (i = 0; i < NSS_WIFILI_RING_MAX; i++) {
		ret = nss_wifili_mem_get(core, &w->ring[i], ring);
		if (ret)
			return ret;
	}

	ret = nss_wifili_mem_get(core, &w->shadow, SZ_4K);
	if (ret)
		return ret;

	ret = nss_wifili_mem_get(core, &w->doorbell, SZ_4K);
	if (ret)
		return ret;

	return nss_wifili_mem_get(core, &w->txdesc,
				  PAGE_ALIGN(NSS_WIFILI_TX_DESC_MEM(NSS_WIFILI_PROBE_TX_DESC,
								    1, NSS_WIFILI_TX_DESC_STRIDE)));
}

static void nss_wifili_arm(struct nss_core *core, struct seq_file *s,
			   struct nss_wifili_msg *m,
			   const struct nss_wifili_ctx *w,
			   enum nss_wifili_fault fault, const char *what)
{
	int ret;

	memset(m, 0, sizeof(*m));
	m->cm.interface = NSS_INTERFACE_WIFILI;
	m->cm.type = NSS_WIFILI_INIT_MSG;
	nss_wifili_init_fill(&m->msg.init, w, fault);
	ret = nss_msg_send(core, m, offsetof(struct nss_wifili_msg, msg) +
				    sizeof(m->msg.init));

	seq_printf(s, "%-16s rc %d response %u error %u\n", what, ret,
		   m->cm.response, m->cm.error);
}

/* Each refusal confirms the offset of the one field it corrupted; the
 * acceptance that follows confirms the rest of a structure with a hundred and
 * fifty fields in it.
 */
int nss_wifili_probe(struct nss_core *core, struct seq_file *s)
{
	static const struct {
		enum nss_wifili_fault fault;
		const char *what;
	} arms[] = {
		{ NSS_WIFILI_FAULT_RING_ALIGN,	"ring alignment:" },
		{ NSS_WIFILI_FAULT_RING_DIR,	"ring direction:" },
		{ NSS_WIFILI_FAULT_RING_ENTRIES, "ring entries:" },
		/* Not refused on this firmware, which is a finding rather than
		 * a check: either the target is not validated here or the
		 * field is not where this header puts it.
		 */
		{ NSS_WIFILI_FAULT_TARGET,	"target type:" },
		{ NSS_WIFILI_FAULT_TLV_SIZE,	"tlv size:" },
		{ NSS_WIFILI_FAULT_RX_BUF_LEN,	"rx buffer len:" },
	};
	struct nss_wifili_ctx *w;
	struct nss_wifili_msg *m;
	int i, ret;

	/* Only the first run says anything. An initialisation the firmware
	 * accepted leaves the radio in a state the next message is judged
	 * against, so a second pass answers about the state machine rather
	 * than about the field it corrupted.
	 */
	if (core->wifili_probed) {
		seq_puts(s, "wifili:          already initialised, reload for a clean core\n");
		return 0;
	}

	/* Two kilobytes of union, which is more than a kernel stack frame is
	 * allowed to carry.
	 */
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!m || !w) {
		ret = -ENOMEM;
		goto out;
	}

	ret = nss_wifili_alloc(core, w);
	if (ret)
		goto out;

	for (i = 0; i < ARRAY_SIZE(arms); i++)
		nss_wifili_arm(core, s, m, w, arms[i].fault, arms[i].what);

	/* Last, because it is a one-way door: an initialisation the firmware
	 * accepts leaves the radio in a state only a full teardown returns
	 * from, and the teardown needs a radio that a host-invented ring set
	 * cannot bring up - the physical-device initialisation that follows
	 * this one programs hardware, and with rings the host made up it
	 * traps every thread. So the probe stops here, and a second run wants
	 * a fresh core.
	 */
	nss_wifili_arm(core, s, m, w, NSS_WIFILI_FAULT_NONE, "init:");
	core->wifili_probed = true;

	ret = 0;
out:
	kfree(w);
	kfree(m);

	return ret;
}

/* Give the firmware a node for one radio and return the number it chose. */
static int nss_wifili_radio_alloc(struct nss_core *core,
				  const struct nss_wifili_soc *soc)
{
	struct nss_dynamic_interface_msg d;
	int ret;

	memset(&d, 0, sizeof(d));
	d.cm.interface = NSS_INTERFACE_DYNAMIC;
	d.cm.type = NSS_DYNAMIC_INTERFACE_ALLOC_NODE;
	d.msg.alloc_node.type = nss_wifili_soc_if[soc - nss_wifili_socs].node;

	ret = nss_msg_send(core, &d, sizeof(d));
	if (ret)
		return ret;

	return d.msg.alloc_node.if_num;
}

/* The operating modes the firmware distinguishes. Only the two a router
 * builds are named; an access point forwards between its own stations and a
 * station does not, so the two are not interchangeable.
 */
#define NSS_WIFI_VDEV_OPMODE_AP		1
#define NSS_WIFI_VDEV_OPMODE_STA	3

/* The mode an access point's virtual device is configured with. An invalid
 * value is accepted without error, so acceptance does not show the value is
 * right. It is a parameter so the question is settled by sweeping it on the
 * hardware.
 */
static unsigned int nss_wifili_ap_opmode = NSS_WIFI_VDEV_OPMODE_AP;
module_param_named(ap_opmode, nss_wifili_ap_opmode, uint, 0644);
MODULE_PARM_DESC(ap_opmode, "operating mode an access point's virtual device is given");

/* One per WLAN interface, and one per station plus one per access point vdev:
 * what ath11k allows on IPQ8074 and IPQ6018, 16 vdevs and 512 stations on
 * each of two radios. A WLAN interface or station beyond these gets no data.
 */
#define NSS_WIFILI_VDEV_MAX	(2 * 16)
#define NSS_WIFILI_PEER_MAX	(2 * 512 + NSS_WIFILI_VDEV_MAX)

/* Give a dynamic interface number back to the firmware. */
static void nss_wifili_vdev_free(struct nss_core *core, int if_num)
{
	struct nss_dynamic_interface_msg d;

	memset(&d, 0, sizeof(d));
	d.cm.interface = NSS_INTERFACE_DYNAMIC;
	d.cm.type = NSS_DYNAMIC_INTERFACE_DEALLOC_NODE;
	d.msg.dealloc_node.type = NSS_DYNAMIC_INTERFACE_TYPE_VAP;
	d.msg.dealloc_node.if_num = if_num;
	nss_msg_send(core, &d, sizeof(d));
}

/* One virtual device: the interface number the firmware knows it by, and the
 * network device a frame arriving under that number belongs to.
 */
struct nss_wifili_vdev {
	u8 radio;
	bool wds;
	int parent;
	u16 peer_id;
	struct net_device *dev;
	int if_num;
	u32 vdev_id;
	struct nss_wifili_soc *soc;
};

static struct nss_wifili_vdev nss_wifili_vdev_tbl[NSS_WIFILI_VDEV_MAX];
/* Interface numbers whose receive the WLAN driver gets to look at first, and
 * the instance whose driver that is.
 */
static DECLARE_BITMAP(nss_wifili_vdev_on, NSS_INTERFACE_MAX);
static struct nss_wifili_soc *nss_wifili_vdev_soc[NSS_INTERFACE_MAX];
/* Every virtual device, mesh links included: whose statistics are taken. */
static DECLARE_BITMAP(nss_wifili_vdev_live, NSS_INTERFACE_MAX);

static struct {
	int if_num;
	u32 seen;
	struct nss_wifi_vdev_stats_sync_msg st;
} nss_wifili_vdev_stats[NSS_WIFILI_VDEV_MAX];

/* The firmware keeps a virtual device's own counters and pushes them without
 * being asked. They are the only place that says whether its receive path
 * produced anything for this device, as opposed to producing nothing at all.
 *
 * Each message is a delta: the firmware reads the counters and zeroes them in
 * the same breath, and it does so several times a second, so the newest
 * message on its own says only what happened since the last one and is almost
 * always zero. They are summed here to get a total. Every field is a 32-bit
 * count, so the sum is elementwise over the message, skipping the word a
 * line lacks.
 */
void nss_wifili_vdev_notify(struct nss_core *core, const struct nss_cmn_msg *ncm,
			    u32 len)
{
	u32 if_num = NSS_INTERFACE_NUM_GET(ncm->interface);
	const struct nss_fw_line *fw = core->fw;
	const struct nss_wifi_vdev_msg *m;
	int i;

	if (ncm->type != NSS_WIFI_VDEV_STATS_MSG ||
	    len < offsetof(struct nss_wifi_vdev_msg, msg) + fw->vdev_stats_len ||
	    if_num >= NSS_INTERFACE_MAX ||
	    !test_bit(if_num, nss_wifili_vdev_live))
		return;

	m = container_of_const(ncm, struct nss_wifi_vdev_msg, cm);

	guard(spinlock_bh)(&nss_wifili_stats_lock);

	for (i = 0; i < ARRAY_SIZE(nss_wifili_vdev_stats); i++) {
		if (nss_wifili_vdev_stats[i].seen &&
		    nss_wifili_vdev_stats[i].if_num != if_num)
			continue;
		const u32 *in = (const u32 *)&m->msg.vdev_stats;
		u32 *acc = (u32 *)&nss_wifili_vdev_stats[i].st;
		int w;

		nss_wifili_vdev_stats[i].if_num = if_num;
		for (w = 0; w < fw->vdev_stats_len / sizeof(u32); w++)
			acc[w + (w >= fw->vdev_stats_hole)] += in[w];
		nss_wifili_vdev_stats[i].seen++;
		return;
	}
}

/* A released interface number is handed out again, @if_num < 0 is all. */
static void nss_wifili_vdev_stats_clear(int if_num)
{
	int i;

	guard(spinlock_bh)(&nss_wifili_stats_lock);

	for (i = 0; i < ARRAY_SIZE(nss_wifili_vdev_stats); i++)
		if (if_num < 0 || nss_wifili_vdev_stats[i].if_num == if_num)
			memset(&nss_wifili_vdev_stats[i], 0,
			       sizeof(nss_wifili_vdev_stats[i]));
}

void nss_wifili_vdev_stats_print(struct seq_file *s)
{
	int i;

	guard(spinlock_bh)(&nss_wifili_stats_lock);

	for (i = 0; i < ARRAY_SIZE(nss_wifili_vdev_stats); i++) {
		const struct nss_wifi_vdev_stats_sync_msg *v;

		if (!nss_wifili_vdev_stats[i].seen)
			continue;
		v = &nss_wifili_vdev_stats[i].st;
		seq_printf(s, "vdev if %d reports %u\n",
			   nss_wifili_vdev_stats[i].if_num,
			   nss_wifili_vdev_stats[i].seen);
		seq_printf(s, "  rx enqueue %u fail %u except %u except_fail %u bytes %u mcast %u\n",
			   v->rx_enqueue_cnt, v->rx_enqueue_fail_cnt,
			   v->rx_except_enqueue_cnt,
			   v->rx_except_enqueue_fail_cnt,
			   v->rx_enqueue_bytes, v->rx_mcast_cnt);
		seq_printf(s, "  rx decrypt_err %u mic_err %u\n",
			   v->rx_decrypt_err, v->rx_mic_err);
		seq_printf(s, "  tx rcvd %u enqueue %u fail %u hw_ring_full %u desc_fail %u dropped %u\n",
			   v->tx_rcvd, v->tx_enqueue_cnt,
			   v->tx_enqueue_fail_cnt, v->tx_hw_ring_full,
			   v->tx_desc_alloc_fail, v->dropped);
		seq_printf(s, "  me rcvd %u converted %u no_member_drop %u alloc_fail %u enq_fail %u\n",
			   v->wvmes.mcast_rcvd, v->wvmes.mcast_ucast_converted,
			   v->wvmes.mcast_no_enhance_drop_cnt,
			   v->wvmes.mcast_alloc_fail,
			   v->wvmes.mcast_pbuf_enq_fail);
	}
}

/* Each message carries its own body and its own length.
 *
 * Sending one message's length for another is not caught: the firmware reads
 * the body it expects where it expects it and takes what is there. The enable
 * message is a MAC address, so an enable sent zeroed at some other message's
 * length sets the virtual device's own address to zero - after the
 * configuration that set it correctly - and every message still answers
 * success. A device with no address of its own matches nothing in either
 * direction.
 */
static DECLARE_BITMAP(nss_wifili_vdev_err_seen, 64);

static int __nss_wifili_vdev_msg(struct nss_core *core, int if_num, u32 type,
				 const void *body, size_t len, u32 *error)
{
	u32 seen = type - NSS_WIFI_VDEV_INTERFACE_CONFIGURE_MSG;
	struct nss_wifi_vdev_msg *v;
	int ret;

	v = kzalloc(sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;

	v->cm.interface = if_num;
	v->cm.type = type;
	if (body)
		memcpy(&v->msg, body, len);

	ret = nss_msg_send(core, v,
			   offsetof(struct nss_wifi_vdev_msg, msg) + len);
	if ((!ret || ret == -EPROTO) && v->cm.error) {
		ret = -EIO;
		if (error)
			*error = v->cm.error;
		if (seen < 64 && !test_and_set_bit(seen, nss_wifili_vdev_err_seen))
			dev_warn(core->dev, "interface %d type %u: error %u\n",
				 if_num, type, v->cm.error);
	}

	kfree(v);

	return ret;
}

int nss_wifili_vdev_msg(struct nss_core *core, int if_num, u32 type,
			const void *body, size_t len)
{
	return __nss_wifili_vdev_msg(core, if_num, type, body, len, NULL);
}

int nss_wifili_vdev_cmd(struct nss_core *core, int if_num, u32 cmd, u32 value)
{
	struct nss_wifi_vdev_cmd_msg c = { .cmd = cmd, .value = value };

	return nss_wifili_vdev_msg(core, if_num,
				   NSS_WIFI_VDEV_INTERFACE_CMD_MSG,
				   &c, sizeof(c));
}

static unsigned int nss_wifili_rx_next_hop = NSS_INTERFACE_ETH_RX;
module_param_named(rx_next_hop, nss_wifili_rx_next_hop, uint, 0644);
MODULE_PARM_DESC(rx_next_hop, "interface a virtual device forwards receive to, 156 for the host alone");

/* Where a virtual device sends what it receives.
 *
 * The firmware's own ethernet node is what feeds its connection engines, so
 * it is the only next hop a pushed rule can match on; a frame no rule claims
 * leaves it for the host as an exception. The node that hands everything to
 * the host is the exception path alone, kept selectable because it separates
 * a receive path that does not start from a rule that does not match.
 *
 * Addressed to an allocated device, the message is answered without an error
 * and the core traps seconds later. It is therefore sent once to the base
 * virtual-device node, after the data plane starts, rather than per device.
 */
static int nss_wifili_vdev_next_hop(struct nss_core *core)
{
	struct nss_wifi_vdev_msg *v;
	int ret;

	if (!nss_wifili_rx_next_hop)
		return 0;

	v = kzalloc(sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;

	v->cm.interface = NSS_INTERFACE_VAP_BASE;
	v->cm.type = NSS_WIFI_VDEV_SET_NEXT_HOP;
	v->msg.next_hop.ifnumber = nss_wifili_rx_next_hop;

	ret = nss_msg_send(core, v, offsetof(struct nss_wifi_vdev_msg, msg) +
				    sizeof(v->msg.next_hop));
	if (!ret && v->cm.error)
		ret = -EIO;

	kfree(v);

	return ret;
}

static void nss_me_enable(struct nss_core *core, int if_num);
static void nss_me_drop(int if_num);
static void nss_me_peer_gone(const u8 *mac);

/* Put one virtual device on a radio and take it up.
 *
 * A radio carries no traffic on its own: a frame belongs to a virtual device,
 * and it is the vdev's interface number that a received frame arrives under
 * and that a transmitted one is addressed to. The network device is bound to
 * that number here, which is the whole of the receive path - the firmware
 * exceptions a frame naming the interface, and the table the wired ports
 * already use carries it the rest of the way.
 *
 * The encapsulation and decapsulation come from the caller because the same
 * two values have already been given to the WLAN firmware for this virtual
 * device: encapsulation is what a transmit descriptor declares a frame to be,
 * and a frame that is not what the WLAN firmware expects is released instead
 * of transmitted.
 */
static int nss_wifili_vdev_register(struct nss_wifili_soc *soc,
				    struct net_device *dev, u8 radio,
				    u32 vdev_id, const u8 *mac, bool ap,
				    u8 encap, u8 decap)
{
	struct nss_wifi_vdev_config_msg cfg = {};
	struct nss_dynamic_interface_msg d;
	struct nss_core *core = nss_wifili_core;
	int if_num, ret, i;

	lockdep_assert_held(&nss_wifili_lock);
	WRITE_ONCE(soc->radio, dev->dev.parent);
	if (!core || !soc->started)
		return -EAGAIN;

	if (radio >= soc->pdevs || soc->radio_if[radio] < 0)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(nss_wifili_vdev_tbl); i++)
		if (!nss_wifili_vdev_tbl[i].dev)
			break;
	if (i == ARRAY_SIZE(nss_wifili_vdev_tbl))
		return -ENOSPC;

	memset(&d, 0, sizeof(d));
	d.cm.interface = NSS_INTERFACE_DYNAMIC;
	d.cm.type = NSS_DYNAMIC_INTERFACE_ALLOC_NODE;
	d.msg.alloc_node.type = NSS_DYNAMIC_INTERFACE_TYPE_VAP;
	ret = nss_msg_send(core, &d, sizeof(d));
	if (ret)
		return ret;

	if_num = d.msg.alloc_node.if_num;
	if (if_num < 0 || if_num >= NSS_INTERFACE_MAX)
		return -ERANGE;

	ether_addr_copy(cfg.mac_addr, mac);
	cfg.radio_ifnum = soc->radio_if[radio];
	cfg.vdev_id = vdev_id;
	cfg.opmode = ap ? nss_wifili_ap_opmode : NSS_WIFI_VDEV_OPMODE_STA;

	ret = nss_wifili_vdev_msg(core, if_num,
				  NSS_WIFI_VDEV_INTERFACE_CONFIGURE_MSG,
				  &cfg, sizeof(cfg));
	if (!ret)
		ret = nss_wifili_vdev_cmd(core, if_num,
					  NSS_WIFI_VDEV_ENCAP_TYPE_CMD,
					  encap);
	if (!ret)
		ret = nss_wifili_vdev_cmd(core, if_num,
					  NSS_WIFI_VDEV_DECAP_TYPE_CMD,
					  decap);
	if (!ret)
		ret = nss_wifili_vdev_cmd(core, if_num,
					  ap ? NSS_WIFI_VDEV_CFG_WDS_BACKHAUL_CMD :
			     NSS_WIFI_VDEV_CFG_MCBC_EXC_TO_HOST_CMD, 1);
	if (!ret) {
		struct nss_wifi_vdev_enable_msg up = {};

		ether_addr_copy(up.mac_addr, mac);
		ret = nss_wifili_vdev_msg(core, if_num,
					  NSS_WIFI_VDEV_INTERFACE_UP_MSG,
					  &up, sizeof(up));
	}
	if (ret) {
		nss_wifili_vdev_free(core, if_num);
		return ret;
	}

	nss_wifili_vdev_tbl[i].dev = dev;
	nss_wifili_vdev_tbl[i].if_num = if_num;
	nss_wifili_vdev_tbl[i].vdev_id = vdev_id;
	nss_wifili_vdev_tbl[i].soc = soc;
	nss_wifili_vdev_tbl[i].radio = radio;
	nss_wifili_vdev_tbl[i].wds = false;
	nss_wifili_vdev_tbl[i].parent = -1;

	dev_hold(dev);
	/* Before the device: the receive poll finds the instance by it. */
	WRITE_ONCE(nss_wifili_vdev_soc[if_num], soc);
	rcu_assign_pointer(core->iface[if_num], dev);
	set_bit(if_num, nss_wifili_vdev_on);
	set_bit(if_num, nss_wifili_vdev_live);

	dev_info(core->dev, "vdev %u on radio %u is interface %d, encap %u decap %u\n",
		 vdev_id, radio, if_num, encap, decap);

	if (ap)
		nss_me_enable(core, if_num);

	return if_num;
}

int nss_wifi_vdev_register(int id, struct net_device *dev, u8 radio,
			   u32 vdev_id, const u8 *mac, bool ap, u8 encap,
			   u8 decap)
{
	struct nss_wifili_soc *soc;
	int ret;

	guard(mutex)(&nss_wifili_lock);
	soc = nss_wifili_soc_get(id);
	if (!soc)
		return -ENODEV;

	ret = nss_wifili_vdev_register(soc, dev, radio, vdev_id, mac, ap,
				       encap, decap);
	soc->vdev_error = ret < 0 ? ret : 0;
	return ret;
}
EXPORT_SYMBOL_GPL(nss_wifi_vdev_register);

/* A mesh interface's virtual device. It carries 802.11 both ways, and what
 * it receives goes on to the mesh decapsulation node rather than to the host,
 * so the network device is bound to the mesh nodes, not to it.
 */
int nss_wifi_mesh_register(int id, struct net_device *dev, u8 radio,
			   u32 vdev_id, const u8 *mac, u8 mode,
			   const struct nss_wifi_mesh_ops *ops, void *priv)
{
	struct nss_wifi_vdev_config_msg cfg = {};
	struct nss_wifi_vdev_enable_msg up = {};
	struct nss_dynamic_interface_msg d = {};
	struct nss_wifili_soc *soc;
	struct nss_core *core;
	int if_num, ret, i;

	guard(mutex)(&nss_wifili_lock);

	soc = nss_wifili_soc_get(id);
	if (!soc)
		return -ENODEV;
	WRITE_ONCE(soc->radio, dev->dev.parent);

	core = nss_wifili_core;
	if (!core || !soc->started)
		return -EAGAIN;
	/* The mesh nodes belong to the core, so every instance it drives,
	 * a QCN9074 beside the SoC's radios included, can carry a mesh.
	 */
	if (!core->mesh_capable)
		return -EOPNOTSUPP;
	if (radio >= soc->pdevs || soc->radio_if[radio] < 0)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(nss_wifili_vdev_tbl); i++)
		if (!nss_wifili_vdev_tbl[i].dev)
			break;
	if (i == ARRAY_SIZE(nss_wifili_vdev_tbl))
		return -ENOSPC;

	d.cm.interface = NSS_INTERFACE_DYNAMIC;
	d.cm.type = NSS_DYNAMIC_INTERFACE_ALLOC_NODE;
	d.msg.alloc_node.type = NSS_DYNAMIC_INTERFACE_TYPE_VAP;
	ret = nss_msg_send(core, &d, sizeof(d));
	if (ret)
		return ret;

	if_num = d.msg.alloc_node.if_num;
	if (if_num < 0 || if_num >= NSS_INTERFACE_MAX)
		return -ERANGE;

	/* The order is the vendor stack's on this firmware: the mesh nodes
	 * exist before the device that feeds them is configured.
	 */
	ret = nss_mesh_add(core, dev, if_num, mac, ops, priv);
	if (ret) {
		nss_wifili_vdev_free(core, if_num);
		return ret;
	}

	ether_addr_copy(cfg.mac_addr, mac);
	cfg.radio_ifnum = soc->radio_if[radio];
	cfg.vdev_id = vdev_id;
	cfg.opmode = NSS_WIFI_VDEV_OPMODE_AP;
	cfg.vap_ext_mode = WIFI_VDEV_EXT_MODE_MESH_LINK;

	ret = nss_wifili_vdev_msg(core, if_num,
				  NSS_WIFI_VDEV_INTERFACE_CONFIGURE_MSG,
				  &cfg, sizeof(cfg));
	if (!ret)
		ret = nss_mesh_attach(core, if_num);
	if (!ret)
		ret = nss_wifili_vdev_cmd(core, if_num,
					  NSS_WIFI_VDEV_ENCAP_TYPE_CMD, mode);
	if (!ret)
		ret = nss_wifili_vdev_cmd(core, if_num,
					  NSS_WIFI_VDEV_DECAP_TYPE_CMD, mode);
	if (!ret) {
		ether_addr_copy(up.mac_addr, mac);
		ret = nss_wifili_vdev_msg(core, if_num,
					  NSS_WIFI_VDEV_INTERFACE_UP_MSG,
					  &up, sizeof(up));
	}
	if (ret) {
		nss_mesh_stop(core, if_num);
		nss_wifili_vdev_free(core, if_num);
		nss_mesh_del(core, if_num);
		return ret;
	}

	nss_wifili_vdev_tbl[i].dev = dev;
	nss_wifili_vdev_tbl[i].if_num = if_num;
	nss_wifili_vdev_tbl[i].vdev_id = vdev_id;
	nss_wifili_vdev_tbl[i].soc = soc;
	nss_wifili_vdev_tbl[i].wds = false;
	nss_wifili_vdev_tbl[i].parent = -1;
	nss_wifili_vdev_tbl[i].radio = radio;
	set_bit(if_num, nss_wifili_vdev_live);

	dev_info(core->dev, "mesh vdev %u on radio %u is interface %d\n",
		 vdev_id, radio, if_num);

	return if_num;
}
EXPORT_SYMBOL_GPL(nss_wifi_mesh_register);

static void nss_wifili_peer_flush(struct nss_core *core,
				  const struct nss_wifili_soc *soc,
				  u32 vdev_id);

static int nss_wifili_wds_allow(struct nss_core *core,
				const struct nss_wifili_soc *soc,
				u16 peer_id, int if_num, bool enable);

static void nss_wifili_vdev_release(struct nss_core *core, int i)
{
	struct net_device *dev = nss_wifili_vdev_tbl[i].dev;
	int if_num = nss_wifili_vdev_tbl[i].if_num;
	bool wds = nss_wifili_vdev_tbl[i].wds;
	int j;

	for (j = 0; j < ARRAY_SIZE(nss_wifili_vdev_tbl); j++)
		if (nss_wifili_vdev_tbl[j].dev &&
		    nss_wifili_vdev_tbl[j].wds &&
		    nss_wifili_vdev_tbl[j].parent == if_num)
			nss_wifili_vdev_release(core, j);

	/* The frame table first, and a grace period, so no receive poll is
	 * still holding the device when it goes. Halting a core clears the
	 * whole table on its own, so the reference is dropped against what was
	 * there rather than against this having been the one to remove it.
	 */
	clear_bit(if_num, nss_wifili_vdev_on);
	clear_bit(if_num, nss_wifili_vdev_live);
	nss_wifili_vdev_stats_clear(if_num);
	if (rcu_replace_pointer(core->iface[if_num], NULL, true)) {
		synchronize_rcu();
		dev_put(dev);
	}
	nss_me_drop(if_num);

	nss_wifili_vdev_tbl[i].dev = NULL;
	nss_wifili_vdev_tbl[i].if_num = -1;

	if (wds) {
		struct nss_dynamic_interface_msg d = {};

		nss_wifili_vdev_msg(core, if_num, NSS_IF_CLOSE, NULL,
				    sizeof(struct nss_if_close));
		d.cm.interface = NSS_INTERFACE_DYNAMIC;
		d.cm.type = NSS_DYNAMIC_INTERFACE_DEALLOC_NODE;
		d.msg.dealloc_node.type = NSS_DYNAMIC_INTERFACE_TYPE_WIFI_EXT_VDEV_WDS;
		d.msg.dealloc_node.if_num = if_num;
		nss_msg_send(core, &d, sizeof(d));
		return;
	}

	nss_wifili_peer_flush(core, nss_wifili_vdev_tbl[i].soc,
			      nss_wifili_vdev_tbl[i].vdev_id);
	nss_mesh_stop(core, if_num);

	/* Down before the node goes: a virtual device released while it is
	 * still up faults the firmware.
	 */
	nss_wifili_vdev_msg(core, if_num, NSS_WIFI_VDEV_INTERFACE_DOWN_MSG,
			    NULL, sizeof(struct nss_wifi_vdev_disable_msg));
	nss_wifili_vdev_free(core, if_num);
	nss_mesh_del(core, if_num);
}

void nss_wifi_vdev_unregister(struct net_device *dev)
{
	struct nss_core *core;
	int i;

	guard(mutex)(&nss_wifili_lock);

	core = nss_wifili_core;
	if (!core)
		return;

	for (i = 0; i < ARRAY_SIZE(nss_wifili_vdev_tbl); i++)
		if (nss_wifili_vdev_tbl[i].dev == dev)
			nss_wifili_vdev_release(core, i);
}
EXPORT_SYMBOL_GPL(nss_wifi_vdev_unregister);

/* Tell the firmware how a virtual device's frames are protected.
 *
 * The transmit descriptor the firmware builds carries the encryption the
 * hardware is to apply, and it takes it from the virtual device rather than
 * from the frame. Left unset it is "none", and every frame after the key is
 * installed goes out in the clear for a station that will discard it - which
 * looks like an association that gets as far as authentication and stops.
 *
 * The values are the firmware's own, not the subsystem's cipher suites, so
 * the mapping is here rather than in the WLAN driver.
 */
static u32 nss_wifili_sec_type(u32 cipher)
{
	switch (cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		return 3;
	case WLAN_CIPHER_SUITE_WEP104:
		return 2;
	case WLAN_CIPHER_SUITE_TKIP:
		return 4;
	case WLAN_CIPHER_SUITE_CCMP:
		return 6;
	case WLAN_CIPHER_SUITE_CCMP_256:
		return 8;
	case WLAN_CIPHER_SUITE_GCMP:
		return 9;
	case WLAN_CIPHER_SUITE_GCMP_256:
		return 10;
	default:
		return 0;
	}
}

/* The firmware's transmit path reads this from the virtual device, not from
 * the frame, so one value covers every frame the device sends - including the
 * authentication exchange of a station that has no key yet. Setting it the
 * moment the first station installs a pairwise key therefore encrypts the
 * first frame of the next station's exchange with a key that station does not
 * have. Switchable, so which of those two failures is real can be measured.
 */
static bool nss_wifili_set_security = true;
module_param_named(set_security, nss_wifili_set_security, bool, 0644);
MODULE_PARM_DESC(set_security, "tell a virtual device which cipher to apply");

int nss_wifi_vdev_security(int if_num, u32 cipher)
{
	struct nss_wifi_vdev_msg *v;
	struct nss_core *core;
	int ret;

	guard(mutex)(&nss_wifili_lock);

	core = nss_wifili_core;
	if (!core || if_num < 0)
		return -ENODEV;

	if (!nss_wifili_set_security)
		return 0;

	v = kzalloc(sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;

	v->cm.interface = if_num;
	v->cm.type = NSS_WIFI_VDEV_INTERFACE_CMD_MSG;
	v->msg.vdev_cmd.cmd = NSS_WIFI_VDEV_SECURITY_TYPE_CMD;
	v->msg.vdev_cmd.value = nss_wifili_sec_type(cipher);

	ret = nss_msg_send(core, v, offsetof(struct nss_wifi_vdev_msg, msg) +
				    sizeof(v->msg.vdev_cmd));
	if (!ret && v->cm.error)
		ret = -EIO;

	dev_info(core->dev, "interface %d protects frames with %u\n", if_num,
		 v->msg.vdev_cmd.value);

	kfree(v);

	return ret;
}
EXPORT_SYMBOL_GPL(nss_wifi_vdev_security);

/* An access point vdev forwards between its own stations unless told not to,
 * so AP isolation has to reach it.
 */
int nss_wifi_vdev_ap_isolate(int if_num, bool isolate)
{
	struct nss_core *core;

	guard(mutex)(&nss_wifili_lock);

	core = nss_wifili_core;
	if (!core || !core->wifili_started || if_num < 0)
		return -ENODEV;

	return nss_wifili_vdev_cmd(core, if_num,
				   NSS_WIFI_VDEV_CFG_AP_BRIDGE_CMD, !isolate);
}
EXPORT_SYMBOL_GPL(nss_wifi_vdev_ap_isolate);

/* Tell the firmware about a station.
 *
 * The firmware keeps its own record of a peer, in memory the host owns and
 * hands over by address, so a create is an allocation as much as a message.
 * A peer block smaller than the record traps the core, and how large the
 * record is differs between firmware lines. The block is therefore a
 * parameter, and it is allocated generously: what is declared to the
 * firmware is nothing.
 *
 * The address-search index and hash come from the WLAN firmware by way of the
 * peer map, so this runs after the driver has waited for that - which is what
 * makes every one of these sends an ordinary sleeping one and needs no
 * machinery for posting from a context that cannot sleep.
 */
/* Whether a station is registered with the firmware at all. Off, the data
 * plane comes up with virtual devices and no peers, which is what separates a
 * fault in the peer path from one in everything around it.
 */
static bool nss_wifili_peers = true;
module_param_named(peers, nss_wifili_peers, bool, 0644);
MODULE_PARM_DESC(peers, "register stations with the firmware");

static bool nss_wifili_stats_push = true;
module_param_named(stats_push, nss_wifili_stats_push, bool, 0644);
MODULE_PARM_DESC(stats_push, "ask the firmware for its periodic statistics push");

static unsigned int nss_wifili_peer_mem = 2 * PAGE_SIZE;
module_param_named(peer_mem, nss_wifili_peer_mem, uint, 0644);
MODULE_PARM_DESC(peer_mem, "bytes given to the firmware for each peer record");

#define NSS_WIFILI_PEER_MEM	nss_wifili_peer_mem

struct nss_wifili_peer {
	const struct nss_wifili_soc *soc;
	u16 peer_id;
	u32 vdev_id;
	u8 mac[ETH_ALEN];
	void *mem;
	dma_addr_t dma;
	size_t size;		/* what was allocated, not what is asked for now */
};

static struct nss_wifili_peer nss_wifili_peer_tbl[NSS_WIFILI_PEER_MAX];

/* Each message carries its own body and its own length, for the same reason
 * the virtual device's do: the firmware reads the body it expects where it
 * expects it, and a length borrowed from another message is not refused.
 */
static int nss_wifili_soc_msg(struct nss_core *core,
			      const struct nss_wifili_soc *soc, u32 type,
			      const void *body, size_t len)
{
	struct nss_wifili_msg *m;
	int ret;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	m->cm.interface = nss_wifili_soc_ifnum(soc);
	m->cm.type = type;
	if (len)
		memcpy(&m->msg, body, len);

	ret = nss_msg_send(core, m,
			   offsetof(struct nss_wifili_msg, msg) + len);
	if (!ret && m->cm.error)
		ret = -EIO;

	kfree(m);

	return ret;
}

static int nss_wifili_peer_msg(struct nss_core *core,
			       const struct nss_wifili_soc *soc, u32 type,
			       const struct nss_wifili_peer_msg *pm)
{
	return nss_wifili_soc_msg(core, soc, type, pm, core->fw->peer_msg_len);
}

static void nss_wifili_peer_release(struct nss_core *core, int i);

/* The firmware forwards a frame between two stations of a BSS without the
 * host. A peer it holds isolated is kept off that forward, so its frames
 * reach the host and pass mac80211's checks: a station that has associated
 * but not finished its key exchange then reaches no other station.
 */
static int nss_wifili_peer_isolate(struct nss_core *core,
				   struct nss_wifili_soc *soc, u16 peer_id,
				   bool isolate)
{
	struct nss_wifili_peer_isolation_msg im = {
		.peer_id = peer_id,
		.isolation = isolate,
	};

	return nss_wifili_soc_msg(core, soc, NSS_WIFILI_ISOLATION_MSG, &im,
				  sizeof(im));
}

int nss_wifi_peer_create(int id, u32 vdev_id, const u8 *mac, u16 peer_id,
			 u16 hw_ast_idx, u32 tx_ast_hash)
{
	struct nss_wifili_peer_msg pm = {};
	struct nss_wifili_soc *soc;
	struct nss_core *core;
	dma_addr_t dma;
	size_t size;
	void *mem;
	int ret, i;

	guard(mutex)(&nss_wifili_lock);

	if (!nss_wifili_peers)
		return 0;

	soc = nss_wifili_soc_get(id);
	core = nss_wifili_core;
	if (!core || !soc || !soc->started)
		return -EAGAIN;

	/* The WLAN driver drops a station's record on paths that never reach a
	 * per-peer deletion - a device going down, a recovery sweep - and the
	 * station then associates again. Releasing any record this address
	 * already holds on this vdev is what bounds the table; the same
	 * station may be live on another radio's vdev.
	 */
	for (i = 0; i < ARRAY_SIZE(nss_wifili_peer_tbl); i++)
		if (nss_wifili_peer_tbl[i].mem &&
		    nss_wifili_peer_tbl[i].soc == soc &&
		    nss_wifili_peer_tbl[i].vdev_id == vdev_id &&
		    ether_addr_equal(nss_wifili_peer_tbl[i].mac, mac))
			nss_wifili_peer_release(core, i);

	for (i = 0; i < ARRAY_SIZE(nss_wifili_peer_tbl); i++)
		if (!nss_wifili_peer_tbl[i].mem)
			break;
	if (i == ARRAY_SIZE(nss_wifili_peer_tbl))
		return -ENOSPC;

	size = NSS_WIFILI_PEER_MEM;
	mem = dma_alloc_coherent(core->dev, size, &dma, GFP_KERNEL);
	if (!mem)
		return -ENOMEM;

	ether_addr_copy(pm.peer_mac_addr, mac);
	pm.vdev_id = vdev_id;
	pm.peer_id = peer_id;
	pm.hw_ast_idx = hw_ast_idx;
	pm.tx_ast_hash = tx_ast_hash;
	pm.nss_peer_mem = dma;

	/* The working stack sends a size of 0. With a size declared, most
	 * frames queued to the peer are dropped.
	 */
	pm.peer_memory_size = 0;

	ret = nss_wifili_peer_msg(core, soc, NSS_WIFILI_PEER_CREATE_MSG, &pm);
	if (ret) {
		dma_free_coherent(core->dev, size, mem, dma);
		return ret;
	}

	/* What the hardware resolves a unicast frame against. A search index
	 * or hash of zero is what an unmapped peer looks like, and a frame
	 * built on one is dropped after the ring rather than at it.
	 */
	dev_info(core->dev, "peer %pM: id %u ast_idx %u ast_hash %#x vdev %u\n",
		 mac, peer_id, hw_ast_idx, tx_ast_hash, vdev_id);

	nss_wifili_peer_tbl[i].soc = soc;
	nss_wifili_peer_tbl[i].peer_id = peer_id;
	nss_wifili_peer_tbl[i].vdev_id = vdev_id;
	ether_addr_copy(nss_wifili_peer_tbl[i].mac, mac);
	nss_wifili_peer_tbl[i].mem = mem;
	nss_wifili_peer_tbl[i].dma = dma;
	nss_wifili_peer_tbl[i].size = size;

	/* Isolated until authorised, so a station that has only associated
	 * cannot reach the others through the firmware's forward.
	 */
	ret = nss_wifili_peer_isolate(core, soc, peer_id, true);
	if (ret)
		nss_wifili_peer_release(core, i);

	return ret;
}
EXPORT_SYMBOL_GPL(nss_wifi_peer_create);

/* The memory is freed only after the delete is answered: the firmware writes
 * into it until it has let the peer go, and a synchronous send is what makes
 * "after" mean anything here.
 */
static void nss_wifili_peer_release(struct nss_core *core, int i)
{
	struct nss_wifili_peer_msg pm = {};
	int ret, j;

	for (j = 0; j < ARRAY_SIZE(nss_wifili_vdev_tbl); j++)
		if (nss_wifili_vdev_tbl[j].dev && nss_wifili_vdev_tbl[j].wds &&
		    nss_wifili_vdev_tbl[j].soc == nss_wifili_peer_tbl[i].soc &&
		    nss_wifili_vdev_tbl[j].peer_id == nss_wifili_peer_tbl[i].peer_id)
			nss_wifili_vdev_release(core, j);

	ether_addr_copy(pm.peer_mac_addr, nss_wifili_peer_tbl[i].mac);
	pm.vdev_id = nss_wifili_peer_tbl[i].vdev_id;
	pm.peer_id = nss_wifili_peer_tbl[i].peer_id;
	ret = nss_wifili_peer_msg(core, nss_wifili_peer_tbl[i].soc,
				  NSS_WIFILI_PEER_DELETE_MSG, &pm);
	/* -ENODEV is a core that is no longer running the record. */
	if (ret && ret != -ENODEV) {
		dev_warn(core->dev, "peer %pM: delete failed: %d, record kept\n",
			 nss_wifili_peer_tbl[i].mac, ret);
		return;
	}

	dev_info(core->dev, "peer %pM: id %u released from vdev %u\n",
		 nss_wifili_peer_tbl[i].mac, nss_wifili_peer_tbl[i].peer_id,
		 nss_wifili_peer_tbl[i].vdev_id);

	dma_free_coherent(core->dev, nss_wifili_peer_tbl[i].size,
			  nss_wifili_peer_tbl[i].mem,
			  nss_wifili_peer_tbl[i].dma);
	nss_wifili_peer_tbl[i].mem = NULL;
	nss_me_peer_gone(nss_wifili_peer_tbl[i].mac);
}

/* A virtual device takes its peers with it. The WLAN driver removes them from
 * its own records without a per-peer deletion when a device goes down, so a
 * peer released that way is never named here, and both this table and the
 * firmware's would keep it for as long as the driver stayed loaded.
 */
static void nss_wifili_peer_flush(struct nss_core *core,
				  const struct nss_wifili_soc *soc,
				  u32 vdev_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(nss_wifili_peer_tbl); i++)
		if (nss_wifili_peer_tbl[i].mem &&
		    nss_wifili_peer_tbl[i].soc == soc &&
		    nss_wifili_peer_tbl[i].vdev_id == vdev_id)
			nss_wifili_peer_release(core, i);
}

/* What protects a station's frames, told per peer.
 *
 * The virtual device's own cipher decides what a transmit descriptor claims;
 * this is the one the receive path consults when it has to undo the protection
 * itself, which is every fragment and every message integrity check. Without
 * it those frames are handled as though they were in the clear.
 */
int nss_wifi_peer_security(int id, u16 peer_id, bool group, u32 cipher,
			   const u8 *mic_key)
{
	struct nss_wifili_peer_security_type_msg sm = {};
	struct nss_wifili_soc *soc;
	struct nss_core *core;

	guard(mutex)(&nss_wifili_lock);

	soc = nss_wifili_soc_get(id);
	core = nss_wifili_core;
	if (!core || !soc || !soc->started)
		return -EAGAIN;

	sm.peer_id = peer_id;
	sm.pkt_type = group ? 1 : 0;
	sm.security_type = nss_wifili_sec_type(cipher);
	if (mic_key)
		memcpy(sm.mic_key, mic_key, sizeof(sm.mic_key));

	return nss_wifili_soc_msg(core, soc, NSS_WIFILI_PEER_SECURITY_TYPE_MSG,
				  &sm, sizeof(sm));
}
EXPORT_SYMBOL_GPL(nss_wifi_peer_security);

/* A peer the firmware holds unauthorised has its received frames counted and
 * dropped rather than delivered, so the flag follows the station's port both
 * ways: a station mac80211 takes back to associated is unauthorised here too.
 * Only an authorised peer is let onto the forward between stations.
 */
int nss_wifi_peer_authorize(int id, u16 peer_id, bool authorized)
{
	struct nss_wifili_peer_update_auth_flag au = {};
	struct nss_wifili_soc *soc;
	struct nss_core *core;
	int ret;

	guard(mutex)(&nss_wifili_lock);

	soc = nss_wifili_soc_get(id);
	core = nss_wifili_core;
	if (!core || !soc || !soc->started)
		return -EAGAIN;

	if (!authorized) {
		ret = nss_wifili_peer_isolate(core, soc, peer_id, true);
		if (ret)
			goto block;
	}

	au.peer_id = peer_id;
	au.auth_flag = authorized;

	ret = nss_wifili_soc_msg(core, soc, NSS_WIFILI_PEER_UPDATE_AUTH_FLAG,
				 &au, sizeof(au));
	if (ret)
		goto block;
	if (!authorized)
		return 0;

	ret = nss_wifili_peer_isolate(core, soc, peer_id, false);
	if (!ret)
		return 0;
block:
	soc->last_error = ret;
	nss_core_block(core);
	return ret;
}
EXPORT_SYMBOL_GPL(nss_wifi_peer_authorize);

void nss_wifi_peer_delete(int id, u32 vdev_id, const u8 *mac, u16 peer_id)
{
	struct nss_wifili_soc *soc;
	struct nss_core *core;
	int i;

	guard(mutex)(&nss_wifili_lock);

	soc = nss_wifili_soc_get(id);
	core = nss_wifili_core;
	if (!core || !soc)
		return;

	for (i = 0; i < ARRAY_SIZE(nss_wifili_peer_tbl); i++) {
		if (!nss_wifili_peer_tbl[i].mem ||
		    nss_wifili_peer_tbl[i].soc != soc ||
		    nss_wifili_peer_tbl[i].peer_id != peer_id ||
		    nss_wifili_peer_tbl[i].vdev_id != vdev_id ||
		    !ether_addr_equal(nss_wifili_peer_tbl[i].mac, mac))
			continue;

		nss_wifili_peer_release(core, i);
		return;
	}
}
EXPORT_SYMBOL_GPL(nss_wifi_peer_delete);

int nss_wifi_vdev_tx(int if_num, struct sk_buff *skb)
{
	struct nss_core *core = READ_ONCE(nss_wifili_core);

	if (!core || if_num < 0 || if_num >= NSS_INTERFACE_MAX ||
	    !test_bit(if_num, nss_wifili_vdev_live))
		return -ENODEV;

	return nss_data_send(core, skb, if_num, 0);
}
EXPORT_SYMBOL_GPL(nss_wifi_vdev_tx);

/* WDS nodes share the parent's WLAN vdev; they never own a WLAN peer. */
static int nss_wifili_wds_allow(struct nss_core *core,
				const struct nss_wifili_soc *soc,
				u16 peer_id, int if_num, bool enable)
{
	struct nss_wifili_peer_wds_4addr_allow_msg m = {
		.peer_id = peer_id, .if_num = if_num, .enable = enable,
	};

	return nss_wifili_soc_msg(core, soc, NSS_WIFILI_PEER_4ADDR_EVENT_MSG,
				  &m, sizeof(m));
}

static bool nss_wifili_has_peer(const struct nss_wifili_soc *soc, u32 vdev_id,
				u16 peer_id, const u8 *addr)
{
	int i;

	lockdep_assert_held(&nss_wifili_lock);
	for (i = 0; i < ARRAY_SIZE(nss_wifili_peer_tbl); i++)
		if (nss_wifili_peer_tbl[i].mem && nss_wifili_peer_tbl[i].soc == soc &&
		    nss_wifili_peer_tbl[i].peer_id == peer_id &&
		    nss_wifili_peer_tbl[i].vdev_id == vdev_id &&
		    ether_addr_equal(nss_wifili_peer_tbl[i].mac, addr))
			return true;
	return false;
}

int nss_wifi_wds_vdev_register(struct net_device *dev, struct net_device *parent,
			       u16 peer_id, const u8 *peer)
{
	struct nss_wifi_ext_vdev_configure_if_msg cfg = {};
	struct nss_wifi_ext_vdev_wds_msg wds = { .wds_peer_id = peer_id };
	struct nss_if_open up = {};
	struct nss_dynamic_interface_msg d = {};
	struct nss_wifili_vdev *p = NULL;
	struct nss_core *core;
	int i, slot = -1, if_num, ret;

	guard(mutex)(&nss_wifili_lock);
	core = nss_wifili_core;
	if (!core || !core->running)
		return -EAGAIN;
	for (i = 0; i < ARRAY_SIZE(nss_wifili_vdev_tbl); i++) {
		if (nss_wifili_vdev_tbl[i].dev == dev) {
			if (nss_wifili_vdev_tbl[i].wds &&
			    nss_wifili_vdev_tbl[i].peer_id == peer_id)
				return nss_wifili_vdev_tbl[i].if_num;
			return -EBUSY;
		}
		if (nss_wifili_vdev_tbl[i].dev == parent && !nss_wifili_vdev_tbl[i].wds)
			p = &nss_wifili_vdev_tbl[i];
		if (!nss_wifili_vdev_tbl[i].dev && slot < 0)
			slot = i;
	}
	if (!p || !p->soc->started)
		return -ENODEV;
	if (slot < 0)
		return -ENOSPC;
	if (!nss_wifili_has_peer(p->soc, p->vdev_id, peer_id, peer))
		return -ENOENT;

	d.cm.interface = NSS_INTERFACE_DYNAMIC;
	d.cm.type = NSS_DYNAMIC_INTERFACE_ALLOC_NODE;
	d.msg.alloc_node.type = NSS_DYNAMIC_INTERFACE_TYPE_WIFI_EXT_VDEV_WDS;
	ret = nss_msg_send(core, &d, sizeof(d));
	if (ret)
		return ret;
	if_num = d.msg.alloc_node.if_num;
	if (if_num < 0 || if_num >= NSS_INTERFACE_MAX)
		return -ERANGE;

	ether_addr_copy(cfg.mac_addr, dev->dev_addr);
	cfg.radio_ifnum = p->soc->radio_if[p->radio];
	cfg.pvap_ifnum = p->if_num;
	ether_addr_copy(wds.mac_addr, peer);
	ret = nss_wifili_vdev_msg(core, if_num, NSS_WIFI_EXT_VDEV_CONFIGURE_IF,
				  &cfg, sizeof(cfg));
	if (!ret)
		ret = nss_wifili_vdev_msg(core, if_num, NSS_WIFI_EXT_VDEV_CONFIGURE_WDS,
					  &wds, sizeof(wds));
	if (!ret)
		ret = nss_wifili_wds_allow(core, p->soc, peer_id, if_num, true);
	if (!ret)
		ret = nss_wifili_vdev_msg(core, if_num, NSS_IF_OPEN, &up, sizeof(up));
	if (ret) {
		nss_wifili_wds_allow(core, p->soc, peer_id, if_num, false);
		nss_wifili_vdev_msg(core, if_num, NSS_IF_CLOSE, NULL,
				    sizeof(struct nss_if_close));
		memset(&d, 0, sizeof(d));
		d.cm.interface = NSS_INTERFACE_DYNAMIC;
		d.cm.type = NSS_DYNAMIC_INTERFACE_DEALLOC_NODE;
		d.msg.dealloc_node.type = NSS_DYNAMIC_INTERFACE_TYPE_WIFI_EXT_VDEV_WDS;
		d.msg.dealloc_node.if_num = if_num;
		nss_msg_send(core, &d, sizeof(d));
		return ret;
	}

	nss_wifili_vdev_tbl[slot] = (struct nss_wifili_vdev) {
		.dev = dev, .if_num = if_num, .vdev_id = p->vdev_id, .soc = p->soc,
		.wds = true, .parent = p->if_num, .peer_id = peer_id, .radio = p->radio,
	};
	dev_hold(dev);
	WRITE_ONCE(nss_wifili_vdev_soc[if_num], p->soc);
	rcu_assign_pointer(core->iface[if_num], dev);
	set_bit(if_num, nss_wifili_vdev_on);
	set_bit(if_num, nss_wifili_vdev_live);
	dev_info(core->dev, "WDS %s: interface %d parent %d peer %u\n",
		 dev->name, if_num, p->if_num, peer_id);
	return if_num;
}
EXPORT_SYMBOL_GPL(nss_wifi_wds_vdev_register);

int nss_wifi_wds_peer(int id, enum nss_wifi_wds_cmd cmd,
		      const struct nss_wifi_wds_peer *p)
{
	struct nss_wifili_wds_peer_msg m = {};
	struct nss_wifili_wds_peer_map_msg map = {};
	struct nss_wifili_soc *soc;
	struct nss_core *core;
	u32 type;
	int i;

	guard(mutex)(&nss_wifili_lock);
	soc = nss_wifili_soc_get(id);
	core = nss_wifili_core;
	if (!core || !soc || !soc->started)
		return -ENODEV;
	if (!nss_wifili_has_peer(soc, p->vdev_id, p->peer_id, p->peer)) {
		/* The firmware deletes the peer before HTT unmaps its WDS
		 * sources. Complete that source cleanup after the peer record
		 * is gone, but never address a reused identifier's new owner.
		 */
		if (cmd != NSS_WIFI_WDS_DEL)
			return -ENOENT;
		for (i = 0; i < ARRAY_SIZE(nss_wifili_peer_tbl); i++)
			if (nss_wifili_peer_tbl[i].mem &&
			    nss_wifili_peer_tbl[i].soc == soc &&
			    nss_wifili_peer_tbl[i].peer_id == p->peer_id)
				return -ESTALE;
	}
	if (!is_valid_ether_addr(p->addr) || !is_valid_ether_addr(p->peer) ||
	    p->radio >= soc->pdevs || p->type != 3)
		return -EINVAL;
	ether_addr_copy(m.dest_mac, p->addr);
	ether_addr_copy(m.peer_mac, p->peer);
	m.ast_type = cmd == NSS_WIFI_WDS_DEL ? 0 : p->type;
	/* AST operations use the NSS radio namespace, not WMI target IDs. */
	m.pdev_id = soc->pdev[p->radio].radio_id;
	m.peer_id = p->peer_id;
	if (cmd == NSS_WIFI_WDS_MAP) {
		ether_addr_copy(map.dest_mac, p->addr);
		map.peer_id = p->peer_id;
		map.ast_idx = p->ast_idx;
		map.vdev_id = p->vdev_id;
		return nss_wifili_soc_msg(core, soc, NSS_WIFILI_WDS_PEER_MAP_MSG,
					  &map, sizeof(map));
	}
	switch (cmd) {
	case NSS_WIFI_WDS_ADD:
		type = NSS_WIFILI_WDS_PEER_ADD_MSG;
		break;
	case NSS_WIFI_WDS_DEL:
		type = NSS_WIFILI_WDS_PEER_DEL_MSG;
		break;
	case NSS_WIFI_WDS_UPDATE:
		type = NSS_WIFILI_WDS_PEER_UPDATE_MSG;
		break;
	default:
		return -EINVAL;
	}
	return nss_wifili_soc_msg(core, soc, type, &m, sizeof(m));
}
EXPORT_SYMBOL_GPL(nss_wifi_wds_peer);

static void nss_wifili_wds_event(struct nss_wifili_soc *soc,
				 struct net_device *dev, enum nss_wifi_wds_event ev,
				 const struct nss_wifi_wds_info *info)
{
	void (*event)(void *priv, struct net_device *dev, enum nss_wifi_wds_event ev,
		      const struct nss_wifi_wds_info *info);

	event = READ_ONCE(soc->desc.wds_event);
	if (event)
		event(soc->desc.priv, dev, ev, info);
}

static bool nss_wifili_wds_notify(struct nss_wifili_soc *soc,
				  const struct nss_cmn_msg *cm, size_t len)
{
	const u8 *body = (const u8 *)(cm + 1);
	struct nss_wifi_wds_info info = {};
	u16 count;
	size_t stride, i;

	if (cm->type != NSS_WIFILI_WDS_ACTIVE_INFO_MSG &&
	    cm->type != NSS_WIFILI_PEER_MEC_AGEOUT_MSG)
		return false;
	if (!soc || len < sizeof(*cm) + sizeof(count))
		return true;
	memcpy(&count, body, sizeof(count));
	body += sizeof(count);
	stride = cm->type == NSS_WIFILI_WDS_ACTIVE_INFO_MSG ? 2 : 8;
	if (count > (len - sizeof(*cm) - sizeof(count)) / stride)
		return true;
	for (i = 0; i < count; i++, body += stride) {
		if (cm->type == NSS_WIFILI_WDS_ACTIVE_INFO_MSG) {
			memcpy(&info.ast_idx, body, sizeof(info.ast_idx));
			nss_wifili_wds_event(soc, NULL, NSS_WIFI_WDS_ACTIVE, &info);
		} else {
			ether_addr_copy(info.addr, body);
			info.radio = body[6];
			nss_wifili_wds_event(soc, NULL, NSS_WIFI_WDS_MEC_EXPIRE, &info);
		}
	}
	return true;
}

/* An extended packet is metadata, never an ordinary Ethernet receive. */
bool nss_wifili_special_recv(struct nss_core *core, struct napi_struct *napi,
			     const struct n2h_descriptor *desc, struct sk_buff *skb)
{
	struct nss_wifi_vdev_wds_per_packet_metadata m;
	struct nss_wifi_vdev_addr4_data_metadata a;
	struct nss_wifi_wds_info info = {};
	struct nss_wifili_soc *soc;
	struct net_device *dev;
	const u8 *meta;
	u32 type, off = NSS_WIFI_VDEV_PER_PACKET_METADATA_OFFSET;
	u32 end = (u32)desc->payload_offs + desc->payload_len;
	bool (*rx)(void *priv, struct net_device *dev, struct sk_buff *skb,
		   struct napi_struct *napi, bool mcbc);

	if (desc->buffer_type != NSS_N2H_BUFFER_PACKET_EXT ||
	    desc->interface_num >= NSS_INTERFACE_MAX ||
	    !test_bit(desc->interface_num, nss_wifili_vdev_on))
		return false;
	soc = READ_ONCE(nss_wifili_vdev_soc[desc->interface_num]);
	dev = rcu_dereference_bh(core->iface[desc->interface_num]);
	if (!soc || !dev)
		return false;
	dma_unmap_single(core->dev, NSS_SKB_CB(skb)->dma,
			 NSS_EMPTY_BUFFER_ALLOC, DMA_FROM_DEVICE);
	if (end > NSS_EMPTY_BUFFER_ALLOC || desc->payload_offs < off + sizeof(type))
		goto drop;
	meta = skb->head + off;
	memcpy(&type, meta, sizeof(type));
	meta += sizeof(type);
	skb_reserve(skb, desc->payload_offs);
	skb_put(skb, desc->payload_len);
	skb->dev = dev;
	switch (type) {
	case NSS_WIFI_VDEV_EXT_DATA_PKT_TYPE_WDS_LEARN:
		if (desc->payload_offs < off + sizeof(type) + sizeof(m))
			break;
		memcpy(&m, meta, sizeof(m));
		info.peer_id = m.peer_id;
		info.addr4 = m.addr4_valid;
		if (m.wds_type == 1 && skb->len >= 2 * ETH_ALEN) {
			ether_addr_copy(info.addr, ((struct ethhdr *)skb->data)->h_source);
			nss_wifili_wds_event(soc, dev, NSS_WIFI_WDS_LEARN, &info);
		} else if (m.wds_type == 2 && skb->len >= 10) {
			/* Firmware encodes the MAC as swapped high16 then low32. */
			info.addr[0] = skb->data[9];
			info.addr[1] = skb->data[8];
			info.addr[2] = skb->data[7];
			info.addr[3] = skb->data[6];
			info.addr[4] = skb->data[5];
			info.addr[5] = skb->data[4];
			nss_wifili_wds_event(soc, dev, NSS_WIFI_WDS_MEC, &info);
		}
		break;
	case NSS_WIFI_VDEV_EXT_DATA_PKT_TYPE_4ADDR:
		if (desc->payload_offs < off + sizeof(type) + sizeof(a))
			break;
		memcpy(&a, meta, sizeof(a));
		info.peer_id = a.peer_id;
		info.addr4 = a.addr4_valid;
		nss_wifili_wds_event(soc, dev, NSS_WIFI_WDS_4ADDR, &info);
		break;
	case NSS_WIFI_VDEV_EXT_DATA_PKT_TYPE_MCBC_RX:
		if (desc->payload_offs < soc->desc.tlv_size || skb->len < ETH_HLEN)
			break;
		rx = READ_ONCE(soc->desc.rx_80211);
		if (rx && rx(soc->desc.priv, dev, skb, napi, true))
			return true;
		break;
	}
drop:
	dev_kfree_skb_any(skb);
	return true;
}

/* Multicast to unicast on access point vdevs.
 *
 * With multicast enhancement on, the vdev sends a group frame as one unicast
 * copy per member in its snoop table and drops a group that has no entry;
 * the well-known link-local groups, IGMP and MLD stay multicast. Membership
 * is learnt from the reports stations send, which reach the host as
 * exceptions, and expires after the IGMP/MLD membership interval like the
 * bridge's own entries.
 */
#define NSS_ME_MAX		64
#define NSS_ME_INTERVAL		(260 * HZ)
#define NSS_ME_SCAN		(10 * HZ)
/* Any value from 1 to 3 converts to the members' own addresses. */
#define NSS_ME_MODE		2
#define NSS_ME_ERRS		48

static bool nss_me_param = true;
module_param_named(me, nss_me_param, bool, 0644);
MODULE_PARM_DESC(me, "convert multicast to unicast on access point vdevs");

union nss_me_grp {
	__be32 ip4;
	struct in6_addr ip6;
};

struct nss_me_mbr {
	union nss_me_grp grp;
	u16 proto;		/* ETH_P_IP or ETH_P_IPV6, 0 when free */
	u8 mac[ETH_ALEN];
	int if_num;
	unsigned long expires;
	bool want;
	bool in_fw;
};

static struct nss_me_mbr nss_me_tbl[NSS_ME_MAX];
static DECLARE_BITMAP(nss_me_on, NSS_INTERFACE_MAX);
static DEFINE_SPINLOCK(nss_me_lock);
static struct {
	u32 joins, leaves, expired, full, added, removed;
	u32 no_core, no_peer, host_err, fw_err[NSS_ME_ERRS];
} nss_me_cnt;

static void nss_me_sync(struct work_struct *work);
static DECLARE_DELAYED_WORK(nss_me_work, nss_me_sync);

static void nss_me_enable(struct nss_core *core, int if_num)
{
	if (!nss_me_param)
		return;
	if (nss_wifili_vdev_cmd(core, if_num, NSS_WIFI_VDEV_ENABLE_ME_CMD,
				NSS_ME_MODE))
		dev_warn(core->dev, "interface %d: no multicast to unicast\n",
			 if_num);
	else
		set_bit(if_num, nss_me_on);
}

static void nss_me_update(int if_num, const u8 *mac, u16 proto,
			  const union nss_me_grp *grp, bool join)
{
	struct nss_me_mbr *e, *free = NULL;
	int i;

	guard(spinlock_bh)(&nss_me_lock);

	for (i = 0; i < NSS_ME_MAX; i++) {
		e = &nss_me_tbl[i];
		if (!e->proto) {
			free = free ?: e;
			continue;
		}
		if (e->if_num == if_num && e->proto == proto &&
		    ether_addr_equal(e->mac, mac) &&
		    !memcmp(&e->grp, grp, sizeof(*grp)))
			break;
	}

	if (i == NSS_ME_MAX) {
		if (!join)
			return;
		if (!free) {
			nss_me_cnt.full++;
			return;
		}
		e = free;
		e->grp = *grp;
		e->proto = proto;
		ether_addr_copy(e->mac, mac);
		e->if_num = if_num;
		e->in_fw = false;
	}

	if (join) {
		nss_me_cnt.joins++;
		e->expires = jiffies + NSS_ME_INTERVAL;
	} else {
		nss_me_cnt.leaves++;
	}
	e->want = join;
	if (e->want != e->in_fw)
		mod_delayed_work(system_wq, &nss_me_work, 0);
}

static void nss_me_ip4(int if_num, const u8 *mac, __be32 group, bool join)
{
	union nss_me_grp g = {};

	g.ip4 = group;

	if (ipv4_is_multicast(group) && !ipv4_is_local_multicast(group))
		nss_me_update(if_num, mac, ETH_P_IP, &g, join);
}

static void nss_me_ip6(int if_num, const u8 *mac, const struct in6_addr *group,
		       bool join)
{
	union nss_me_grp g = {};

	g.ip6 = *group;

	if (ipv6_addr_is_multicast(group) &&
	    IPV6_ADDR_MC_SCOPE(group) > IPV6_ADDR_SCOPE_LINKLOCAL)
		nss_me_update(if_num, mac, ETH_P_IPV6, &g, join);
}

static void nss_me_igmp(int if_num, struct sk_buff *skb)
{
	const u8 *mac = eth_hdr(skb)->h_source;
	struct igmpv3_report *r;
	struct igmpv3_grec *g;
	unsigned int len;
	int i, n;

	if (ip_mc_check_igmp(skb))
		return;

	switch (igmp_hdr(skb)->type) {
	case IGMP_HOST_MEMBERSHIP_REPORT:
	case IGMPV2_HOST_MEMBERSHIP_REPORT:
		nss_me_ip4(if_num, mac, igmp_hdr(skb)->group, true);
		return;
	case IGMP_HOST_LEAVE_MESSAGE:
		nss_me_ip4(if_num, mac, igmp_hdr(skb)->group, false);
		return;
	case IGMPV3_HOST_MEMBERSHIP_REPORT:
		break;
	default:
		return;
	}

	/* Sources are not tracked: only an empty include list leaves. */
	r = igmpv3_report_hdr(skb);
	n = ntohs(r->ngrec);
	len = skb_transport_offset(skb) + sizeof(*r);
	for (i = 0; i < n; i++) {
		len += sizeof(*g);
		if (!ip_mc_may_pull(skb, len))
			return;
		g = (void *)(skb->data + len - sizeof(*g));
		len += ntohs(g->grec_nsrcs) * 4;
		if (!ip_mc_may_pull(skb, len))
			return;
		nss_me_ip4(if_num, mac, g->grec_mca,
			   !((g->grec_type == IGMPV3_MODE_IS_INCLUDE ||
			      g->grec_type == IGMPV3_CHANGE_TO_INCLUDE) &&
			     !g->grec_nsrcs));
	}
}

static void nss_me_mld(int if_num, struct sk_buff *skb)
{
	const u8 *mac = eth_hdr(skb)->h_source;
	struct mld_msg *m;
	struct mld2_grec *g;
	unsigned int len;
	int i, n;

	if (ipv6_mc_check_mld(skb))
		return;

	m = (struct mld_msg *)skb_transport_header(skb);
	switch (m->mld_type) {
	case ICMPV6_MGM_REPORT:
		nss_me_ip6(if_num, mac, &m->mld_mca, true);
		return;
	case ICMPV6_MGM_REDUCTION:
		nss_me_ip6(if_num, mac, &m->mld_mca, false);
		return;
	case ICMPV6_MLD2_REPORT:
		break;
	default:
		return;
	}

	n = ntohs(((struct mld2_report *)m)->mld2r_ngrec);
	len = skb_transport_offset(skb) + sizeof(struct icmp6hdr);
	for (i = 0; i < n; i++) {
		len += sizeof(*g);
		if (!ipv6_mc_may_pull(skb, len))
			return;
		g = (void *)(skb->data + len - sizeof(*g));
		len += ntohs(g->grec_nsrcs) * sizeof(struct in6_addr);
		if (!ipv6_mc_may_pull(skb, len))
			return;
		nss_me_ip6(if_num, mac, &g->grec_mca,
			   !((g->grec_type == MLD2_MODE_IS_INCLUDE ||
			      g->grec_type == MLD2_CHANGE_TO_INCLUDE) &&
			     !g->grec_nsrcs));
	}
}

/* From the receive poll, for a frame a vdev handed up. The firmware hands up
 * frames the hardware did not decapsulate, such as a block ack request, the
 * same way as ethernet; only the receive descriptor ahead of the frame tells
 * them apart, so the WLAN driver is asked first.
 */
bool nss_wifili_rx_80211(u32 if_num, struct net_device *dev,
			 struct sk_buff *skb, struct napi_struct *napi)
{
	bool (*rx)(void *priv, struct net_device *dev, struct sk_buff *skb,
		   struct napi_struct *napi, bool mcbc);
	struct nss_wifili_soc *soc;

	if (!test_bit(if_num, nss_wifili_vdev_on))
		return false;

	soc = READ_ONCE(nss_wifili_vdev_soc[if_num]);
	rx = READ_ONCE(soc->desc.rx_80211);

	return rx && rx(soc->desc.priv, dev, skb, napi, false);
}

/* From the receive poll, for a multicast frame a vdev handed up. */
void nss_wifili_me_snoop(u32 if_num, struct sk_buff *skb)
{
	if (!test_bit(if_num, nss_me_on))
		return;

	skb_reset_network_header(skb);
	if (skb->protocol == htons(ETH_P_IP))
		nss_me_igmp(if_num, skb);
	else if (skb->protocol == htons(ETH_P_IPV6))
		nss_me_mld(if_num, skb);
}

/* The snoop-table messages share one layout up to the member address. */
static void nss_me_msg_fill(struct nss_wifi_vdev_me_snptbl_grp_mbr_add_msg *a,
			    const struct nss_me_mbr *m)
{
	a->ether_type = m->proto;
	if (m->proto == ETH_P_IP) {
		a->u.grpaddr_ip4 = m->grp.ip4;
		ip_eth_mc_map(m->grp.ip4, a->grp_addr);
	} else {
		memcpy(a->u.grpaddr_ip6, &m->grp.ip6, sizeof(m->grp.ip6));
		ipv6_eth_mc_map(&m->grp.ip6, a->grp_addr);
	}
	ether_addr_copy(a->grp_member_addr, m->mac);
}

static int nss_me_peer_id(int if_num, const u8 *mac)
{
	int i, v;

	for (v = 0; v < ARRAY_SIZE(nss_wifili_vdev_tbl); v++)
		if (nss_wifili_vdev_tbl[v].dev &&
		    nss_wifili_vdev_tbl[v].if_num == if_num)
			break;
	if (v == ARRAY_SIZE(nss_wifili_vdev_tbl))
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(nss_wifili_peer_tbl); i++)
		if (nss_wifili_peer_tbl[i].mem &&
		    nss_wifili_peer_tbl[i].soc == nss_wifili_vdev_tbl[v].soc &&
		    nss_wifili_peer_tbl[i].vdev_id == nss_wifili_vdev_tbl[v].vdev_id &&
		    ether_addr_equal(nss_wifili_peer_tbl[i].mac, mac))
			return nss_wifili_peer_tbl[i].peer_id;

	return -ENOENT;
}

/* Every refusal is counted by its reason: host side, or the firmware's code. */
static int nss_me_msg(struct nss_core *core, int if_num, u32 type,
		      const void *body, size_t len, u32 ok_error)
{
	u32 error = 0;
	int ret;

	ret = __nss_wifili_vdev_msg(core, if_num, type, body, len, &error);
	if (!ret || (error && error == ok_error))
		return 0;

	/* Error 0 here is a firmware refusal that carried no code. */
	if (!error && ret != -EPROTO)
		nss_me_cnt.host_err++;
	else
		nss_me_cnt.fw_err[min(error, NSS_ME_ERRS - 1)]++;
	return ret;
}

/* A group goes in before its first member and out with its last. A list or
 * member the firmware already holds is what was asked for.
 */
static bool nss_me_fw_add(struct nss_core *core, const struct nss_me_mbr *m,
			  bool first)
{
	struct nss_wifi_vdev_me_snptbl_grp_mbr_add_msg a = {};
	struct nss_wifi_vdev_me_snptbl_grp_create_msg c = {};
	int peer_id = nss_me_peer_id(m->if_num, m->mac);

	if (peer_id < 0) {
		nss_me_cnt.no_peer++;
		return false;
	}

	nss_me_msg_fill(&a, m);
	a.peer_id = peer_id;
	c.ether_type = a.ether_type;
	memcpy(&c.u, &a.u, sizeof(c.u));
	ether_addr_copy(c.grp_addr, a.grp_addr);

	if (first &&
	    nss_me_msg(core, m->if_num,
		       NSS_WIFI_VDEV_SNOOPLIST_GRP_LIST_CREATE_MSG, &c,
		       sizeof(c), NSS_WIFI_VDEV_SNOOPTABLE_GRP_LIST_EXIST))
		return false;

	if (!nss_me_msg(core, m->if_num,
			NSS_WIFI_VDEV_SNOOPLIST_GRP_MEMBER_ADD_MSG, &a,
			sizeof(a), NSS_WIFI_VDEV_SNOOPTABLE_GRP_MEMBER_EXIST))
		return true;

	if (first)
		nss_me_msg(core, m->if_num,
			   NSS_WIFI_VDEV_SNOOPLIST_GRP_LIST_DELETE_MSG, &c,
			   sizeof(c), 0);
	return false;
}

static void nss_me_fw_del(struct nss_core *core, const struct nss_me_mbr *m,
			  bool last)
{
	struct nss_wifi_vdev_me_snptbl_grp_mbr_add_msg a = {};
	struct nss_wifi_vdev_me_snptbl_grp_mbr_delete_msg d = {};
	struct nss_wifi_vdev_me_snptbl_grp_delete_msg g = {};

	nss_me_msg_fill(&a, m);
	if (last) {
		g.ether_type = a.ether_type;
		memcpy(&g.u, &a.u, sizeof(g.u));
		ether_addr_copy(g.grp_addr, a.grp_addr);
		nss_me_msg(core, m->if_num,
			   NSS_WIFI_VDEV_SNOOPLIST_GRP_LIST_DELETE_MSG, &g,
			   sizeof(g), 0);
		return;
	}

	d.ether_type = a.ether_type;
	memcpy(&d.u, &a.u, sizeof(d.u));
	ether_addr_copy(d.grp_addr, a.grp_addr);
	ether_addr_copy(d.grp_member_addr, a.grp_member_addr);
	nss_me_msg(core, m->if_num,
		   NSS_WIFI_VDEV_SNOOPLIST_GRP_MEMBER_REMOVE_MSG, &d,
		   sizeof(d), 0);
}

/* Whether another member of the same group on the same vdev is installed. */
static bool nss_me_shared(const struct nss_me_mbr *m)
{
	const struct nss_me_mbr *e;

	for (e = nss_me_tbl; e < nss_me_tbl + NSS_ME_MAX; e++)
		if (e != m && e->proto == m->proto && e->in_fw &&
		    e->if_num == m->if_num &&
		    !memcmp(&e->grp, &m->grp, sizeof(m->grp)))
			return true;

	return false;
}

/* Whether an entry needs a message, with a copy of it to send from. */
static bool nss_me_pending(struct nss_me_mbr *e, struct nss_me_mbr *m,
			   bool *shared, bool *busy)
{
	guard(spinlock_bh)(&nss_me_lock);

	if (e->proto && e->want && time_after(jiffies, e->expires)) {
		e->want = false;
		nss_me_cnt.expired++;
	}
	if (e->proto && !e->want && !e->in_fw)
		e->proto = 0;
	*busy |= !!e->proto;
	if (!e->proto || e->want == e->in_fw)
		return false;

	*m = *e;
	*shared = nss_me_shared(e);
	return true;
}

/* Bring the firmware's tables to what the host table wants. Only this work
 * sets in_fw or frees an entry, and it holds the wifili lock throughout, so
 * no vdev goes away under a message.
 */
static void nss_me_sync(struct work_struct *work)
{
	struct nss_me_mbr *e, m;
	struct nss_core *core;
	bool shared, ok, busy = false;

	guard(mutex)(&nss_wifili_lock);
	core = nss_wifili_core;

	for (e = nss_me_tbl; e < nss_me_tbl + NSS_ME_MAX; e++) {
		if (!nss_me_pending(e, &m, &shared, &busy))
			continue;

		ok = false;
		if (!core) {
			nss_me_cnt.no_core++;
		} else if (m.want) {
			ok = nss_me_fw_add(core, &m, !shared);
			nss_me_cnt.added += ok;
		} else {
			nss_me_fw_del(core, &m, !shared);
			nss_me_cnt.removed++;
		}

		/* A refused member is forgotten; the next report retries. */
		scoped_guard(spinlock_bh, &nss_me_lock) {
			e->in_fw = ok;
			if (!ok && m.want)
				e->proto = 0;
		}
	}

	if (busy)
		mod_delayed_work(system_wq, &nss_me_work, NSS_ME_SCAN);
}

/* Without a firmware message: the vdev, or the whole core, is going. */
static void nss_me_drop(int if_num)
{
	struct nss_me_mbr *e;

	guard(spinlock_bh)(&nss_me_lock);

	for (e = nss_me_tbl; e < nss_me_tbl + NSS_ME_MAX; e++)
		if (if_num < 0 || e->if_num == if_num)
			e->proto = 0;

	if (if_num < 0)
		bitmap_zero(nss_me_on, NSS_INTERFACE_MAX);
	else
		clear_bit(if_num, nss_me_on);
}

static void nss_me_peer_gone(const u8 *mac)
{
	struct nss_me_mbr *e;

	guard(spinlock_bh)(&nss_me_lock);

	for (e = nss_me_tbl; e < nss_me_tbl + NSS_ME_MAX; e++) {
		/* The station may still be associated on another vdev. */
		if (!e->proto || !ether_addr_equal(e->mac, mac) ||
		    nss_me_peer_id(e->if_num, mac) >= 0)
			continue;
		e->want = false;
		mod_delayed_work(system_wq, &nss_me_work, 0);
	}
}

void nss_wifili_me_exit(void)
{
	cancel_delayed_work_sync(&nss_me_work);
}

void nss_wifili_me_print(struct seq_file *s)
{
	const struct nss_me_mbr *e;
	int i;

	guard(spinlock_bh)(&nss_me_lock);

	seq_printf(s, "joins %u leaves %u expired %u full %u added %u removed %u\n",
		   nss_me_cnt.joins, nss_me_cnt.leaves, nss_me_cnt.expired,
		   nss_me_cnt.full, nss_me_cnt.added, nss_me_cnt.removed);
	seq_printf(s, "refused: no_core %u no_peer %u host %u\n",
		   nss_me_cnt.no_core, nss_me_cnt.no_peer,
		   nss_me_cnt.host_err);
	for (i = 0; i < NSS_ME_ERRS; i++)
		if (nss_me_cnt.fw_err[i])
			seq_printf(s, "refused: firmware error %d: %u\n", i,
				   nss_me_cnt.fw_err[i]);
	seq_printf(s, "vdevs %*pbl\n", NSS_INTERFACE_MAX, nss_me_on);

	for (e = nss_me_tbl; e < nss_me_tbl + NSS_ME_MAX; e++) {
		if (!e->proto)
			continue;
		if (e->proto == ETH_P_IP)
			seq_printf(s, "if %d %pI4", e->if_num, &e->grp.ip4);
		else
			seq_printf(s, "if %d %pI6c", e->if_num, &e->grp.ip6);
		seq_printf(s, " %pM want %d fw %d expires %ds\n", e->mac,
			   e->want, e->in_fw,
			   (int)((long)(e->expires - jiffies) / HZ));
	}
}

/* Hand one frame down, to see whether the firmware takes it.
 *
 * Nothing above this yet posts a frame: the WLAN driver refuses transmit while
 * the offload is on, and the divert that replaces that refusal needs a virtual
 * device per interface, which needs a peer to be worth having. This is the
 * step below all of it - a frame the host builds, addressed to the virtual
 * device the probe already makes, so the data queue and the completion that
 * comes back can be exercised before any of that exists.
 *
 * The frame goes nowhere: with no peer, the firmware has nobody to send it to.
 * What is being asked is whether the descriptor is consumed and the buffer
 * handed back, which is the whole of the transmit contract minus its purpose.
 */
/* Which virtual device the probe posts to. A frame this driver builds reaches
 * the transmit ring on the first one; whether it does on the second is what
 * separates a frame the firmware will not take from a device it will not send
 * on.
 */
static unsigned int nss_wifili_probe_vdev;
module_param_named(probe_vdev, nss_wifili_probe_vdev, uint, 0644);
MODULE_PARM_DESC(probe_vdev, "which virtual device the transmit probe posts to");

static u8 nss_wifili_probe_dst[ETH_ALEN];
module_param_array_named(probe_dst, nss_wifili_probe_dst, byte, NULL, 0644);
MODULE_PARM_DESC(probe_dst, "destination for the transmit probe, unset for broadcast");

int nss_wifili_tx(struct seq_file *s)
{
	struct nss_core *core = nss_wifili_core;
	u64 before, after;
	struct sk_buff *skb;
	int i, ret, if_num, sent = 0;

	if (!core || !core->running)
		return -ENODEV;

	if (nss_wifili_probe_vdev >= ARRAY_SIZE(nss_wifili_vdev_tbl))
		return -EINVAL;

	if_num = nss_wifili_vdev_tbl[nss_wifili_probe_vdev].dev ?
		 nss_wifili_vdev_tbl[nss_wifili_probe_vdev].if_num : -1;
	if (if_num < 0) {
		seq_puts(s, "no virtual device: bring a WLAN interface up first\n");
		return 0;
	}

	before = atomic64_read(&core->tx_done);

	for (i = 0; i < 8; i++) {
		skb = alloc_skb(128 + NET_SKB_PAD, GFP_KERNEL);
		if (!skb)
			break;
		skb_reserve(skb, NET_SKB_PAD);
		/* A broadcast frame of its own making: destination, source and
		 * an ethertype nothing claims, so nothing downstream of a
		 * firmware that did forward it would act on one.
		 */
		memset(skb_put(skb, 60), 0, 60);
		memset(skb->data, 0xff, ETH_ALEN);
		/* A destination the firmware has to resolve to a peer, when one
		 * is named. Broadcast needs no lookup, so the two together say
		 * whether a lookup is what a frame dies on.
		 */
		if (!is_zero_ether_addr(nss_wifili_probe_dst))
			ether_addr_copy(skb->data, nss_wifili_probe_dst);
		skb->data[12] = 0x88;
		skb->data[13] = 0xb5;

		ret = nss_data_send(core, skb, if_num, 0);
		if (ret) {
			dev_kfree_skb_any(skb);
			seq_printf(s, "post %d: rc %d\n", i, ret);
			break;
		}
		sent++;
	}

	msleep(200);
	after = atomic64_read(&core->tx_done);

	seq_printf(s, "posted %d to interface %d, completed %llu\n",
		   sent, if_num, after - before);

	return 0;
}

/* Start the firmware's Wi-Fi data plane on the rings the WLAN driver owns.
 *
 * This is where the ring set stops being invented. Everything the probe above
 * could not reach - the physical-device initialisation, which programs
 * hardware, and the start that puts the firmware's workers on the rings -
 * needs rings that exist, and these are them.
 */
/* The Wi-Fi share of the payload pool and each radio's transmit budget.
 * Zero selects the physical-memory profile before the first WLAN starts.
 */
static unsigned int nss_wifili_payloads;
module_param_named(payloads, nss_wifili_payloads, uint, 0644);

static unsigned int nss_wifili_radio_bufs;
module_param_named(radio_bufs, nss_wifili_radio_bufs, uint, 0644);

static unsigned int nss_wifili_mem_profile;
module_param_named(mem_profile, nss_wifili_mem_profile, uint, 0644);
MODULE_PARM_DESC(mem_profile, "memory profile the firmware sizes its pools from");

/* How many payloads a radio may carve for its own receive.
 *
 * Without this the firmware leaves the radio on the budget it starts with and
 * fills no receive descriptor, so the ring the WLAN firmware needs full is
 * filled by nobody - and a ring the host fills instead carries cookies the
 * firmware cannot resolve. The count is asked for across all four peer-count
 * ranges, because which one applies is the firmware's choice and a range left
 * unset is a station count that silently loses the budget again.
 */
#define NSS_WIFILI_BUF_RANGES	4

static int nss_wifili_radio_buf_cfg(struct nss_core *core,
				    const struct nss_wifili_soc *soc, int if_num,
				    u32 bufs)
{
	struct nss_wifili_msg *m __free(kfree) = kzalloc(sizeof(*m), GFP_KERNEL);
	u32 range;
	int ret;

	if (!m)
		return -ENOMEM;

	for (range = 0; range < NSS_WIFILI_BUF_RANGES; range++) {
		memset(m, 0, sizeof(*m));
		m->cm.interface = nss_wifili_soc_ifnum(soc);
		m->cm.type = NSS_WIFILI_RADIO_BUF_CFG;
		m->msg.radiocfgmsg.radio_if_num = if_num;
		m->msg.radiocfgmsg.radiomsg.radiobufcfgmsg.buf_cnt = bufs;
		m->msg.radiocfgmsg.radiomsg.radiobufcfgmsg.range = range;

		ret = nss_msg_send(core, m,
				   offsetof(struct nss_wifili_msg, msg) +
				   sizeof(m->msg.radiocfgmsg));
		if (ret)
			return ret;
	}

	return 0;
}

static void nss_wifili_radio_free(struct nss_core *core,
				  const struct nss_wifili_soc *soc, int if_num)
{
	struct nss_dynamic_interface_msg d = {};

	d.cm.interface = NSS_INTERFACE_DYNAMIC;
	d.cm.type = NSS_DYNAMIC_INTERFACE_DEALLOC_NODE;
	d.msg.dealloc_node.type = nss_wifili_soc_if[soc - nss_wifili_socs].node;
	d.msg.dealloc_node.if_num = if_num;
	nss_msg_send(core, &d, sizeof(d));
}

/* The transmit descriptors go back to the core's memory once the firmware
 * has let the instance go.
 */
static void nss_wifili_txdesc_free(struct nss_core *core,
				   struct nss_wifili_soc *soc)
{
	int i;

	for (i = 0; i < soc->txpages; i++)
		nss_mem_free(core, soc->txdesc[i].cpu);
	memset(soc->txdesc, 0, sizeof(soc->txdesc));
	soc->txpages = 0;
	soc->txext = 0;
}

/* One kind of descriptor for every radio, cut into pages after those already
 * taken.
 */
static int nss_wifili_txdesc_get(struct nss_core *core,
				 struct nss_wifili_soc *soc, size_t len)
{
	while (len) {
		size_t size = min_t(size_t, len, NSS_WIFILI_TX_DESC_PAGE);
		int ret;

		if (soc->txpages == NSS_WIFILI_TX_DESC_PAGES)
			return -ENOSPC;
		ret = nss_wifili_mem_get(core, &soc->txdesc[soc->txpages],
					 PAGE_ALIGN(size));
		if (ret)
			return ret;
		soc->txpages++;
		len -= size;
	}

	return 0;
}

/* Take one instance off its rings while the core runs on for the others: its
 * virtual devices and peers first, then the instance in the order the
 * firmware tears one down, stop, each radio, reset. The WLAN driver's next
 * description starts it again.
 */
static void nss_wifili_soc_stop(struct nss_core *core,
				struct nss_wifili_soc *soc)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(nss_wifili_vdev_tbl); i++)
		if (nss_wifili_vdev_tbl[i].dev &&
		    nss_wifili_vdev_tbl[i].soc == soc)
			nss_wifili_vdev_release(core, i);
	for (i = 0; i < ARRAY_SIZE(nss_wifili_peer_tbl); i++)
		if (nss_wifili_peer_tbl[i].mem &&
		    nss_wifili_peer_tbl[i].soc == soc)
			nss_wifili_peer_release(core, i);

	if (soc->started) {
		ret = nss_wifili_soc_msg(core, soc, NSS_WIFILI_STOP_MSG, NULL, 0);
		if (ret)
			dev_warn(core->dev, "wifili %u: stop failed: %d\n",
				 nss_wifili_soc_ifnum(soc), ret);
	}

	for (i = 0; i < ARRAY_SIZE(soc->radio_if); i++) {
		struct nss_wifili_pdev_deinit_msg d = {
			.ifnum = soc->pdev[i].radio_id,
		};

		if (soc->radio_if[i] < 0)
			continue;

		ret = nss_wifili_soc_msg(core, soc, NSS_WIFILI_PDEV_DEINIT_MSG,
					 &d, sizeof(d));
		if (ret)
			dev_warn(core->dev, "wifili %u: radio %u deinit failed: %d\n",
				 nss_wifili_soc_ifnum(soc), d.ifnum, ret);
		nss_wifili_radio_free(core, soc, soc->radio_if[i]);
		soc->radio_if[i] = -1;
	}

	ret = nss_wifili_soc_msg(core, soc, NSS_WIFILI_SOC_RESET_MSG, NULL, 0);
	dev_info(core->dev, "wifili %u: stopped, reset rc %d\n",
		 nss_wifili_soc_ifnum(soc), ret);

	soc->started = false;
	core->wifili_started = nss_wifili_any_started();
	nss_wifili_txdesc_free(core, soc);
}

static int nss_wifili_soc_start(struct nss_core *core,
				struct nss_wifili_soc *soc, struct seq_file *s)
{
	const struct nss_wifi_soc *desc = &soc->desc;
	u32 ifnum = nss_wifili_soc_ifnum(soc);
	u32 tx_desc = nss_low_mem() ? 4096 : NSS_WIFILI_TX_DESC;
	struct nss_wifili_msg *m;
	bool first = !core->wifili_probed;
	bool inited = false;
	int i, ret;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	if (!nss_wifili_payloads)
		nss_wifili_payloads = nss_low_mem() ? 4096 : 8192;
	if (!nss_wifili_radio_bufs)
		nss_wifili_radio_bufs = nss_low_mem() ? 4096 : 8192;

	/* The transmit descriptors are the firmware's to carve up and the
	 * host's to provide, so they are not part of what the WLAN driver
	 * describes.
	 */
	ret = nss_wifili_txdesc_get(core, soc,
				    NSS_WIFILI_TX_DESC_MEM(tx_desc,
							   soc->pdevs,
							   NSS_WIFILI_TX_DESC_STRIDE));
	if (ret)
		goto out;

	soc->txext = soc->txpages;
	ret = nss_wifili_txdesc_get(core, soc,
				    NSS_WIFILI_TX_DESC_MEM(tx_desc,
							   soc->pdevs,
							   NSS_WIFILI_TX_DESC_EXT_STRIDE));
	if (ret)
		goto out;

	m->cm.interface = ifnum;
	m->cm.type = NSS_WIFILI_INIT_MSG;
	m->msg.init.hssm.dev_base_addr = desc->dev_base;
	m->msg.init.hssm.shadow_rdptr_mem_addr = desc->shadow_rd;
	m->msg.init.hssm.shadow_wrptr_mem_addr = desc->shadow_wr;
	m->msg.init.hssm.lmac_rings_start_id = desc->lmac_ring_start;
	m->msg.init.num_tcl_data_rings = desc->num_tcl;
	m->msg.init.num_reo_dest_rings = desc->num_reo;

	/* How much memory the firmware sizes its own pools from. Nothing here
	 * has ever set it, so it has always said zero, and a receive path with
	 * no buffers of its own to fill a ring with is a receive path that
	 * reaps an empty ring for ever. The value is a profile rather than a
	 * size and the header names no constants for it, so it is a parameter
	 * to be swept rather than a number to be guessed.
	 */
	m->msg.init.soc_mem_profile = nss_wifili_mem_profile;

	for (i = 0; i < desc->num_tcl; i++) {
		nss_wifili_ring_fill(&m->msg.init.tcl_ring_info[i],
				     &desc->tcl[i]);
		nss_wifili_ring_fill(&m->msg.init.tx_comp_ring[i],
				     &desc->txcomp[i]);
	}

	for (i = 0; i < desc->num_reo; i++)
		nss_wifili_ring_fill(&m->msg.init.reo_dest_ring[i],
				     &desc->reo_dest[i]);

	nss_wifili_ring_fill(&m->msg.init.reo_reinject_ring,
			     &desc->reo_reinject);
	nss_wifili_ring_fill(&m->msg.init.rx_rel_ring, &desc->rx_rel);
	nss_wifili_ring_fill(&m->msg.init.reo_exception_ring,
			     &desc->reo_exception);

	/* One descriptor pool per radio. The message carries a separate count
	 * for each, and the firmware reaps a pool per radio whatever the count
	 * says, so a pool left at zero is one it reaps against and never
	 * sized.
	 */
	m->msg.init.wtdim.num_pool = soc->pdevs;
	m->msg.init.wtdim.num_tx_desc = tx_desc;
	m->msg.init.wtdim.num_tx_desc_ext = tx_desc;
	if (soc->pdevs > 1) {
		m->msg.init.wtdim.num_tx_desc_2 = tx_desc;
		m->msg.init.wtdim.num_tx_desc_ext_2 = tx_desc;
	}
	if (soc->pdevs > 2) {
		m->msg.init.wtdim.num_tx_desc_3 = tx_desc;
		m->msg.init.wtdim.num_tx_desc_ext_3 = tx_desc;
	}

	m->msg.init.wtdim.num_memaddr = soc->txpages;
	for (i = 0; i < soc->txpages; i++) {
		m->msg.init.wtdim.memory_addr[i] = soc->txdesc[i].dma;
		m->msg.init.wtdim.memory_size[i] = soc->txdesc[i].size;
	}
	m->msg.init.wtdim.ext_desc_page_num = soc->txext;
	m->msg.init.wtdim.num_tx_device_limit = tx_desc * soc->pdevs;

	/* Left at zero, the core takes a range trap seconds after it starts. */
	m->msg.init.tx_sw_internode_queue_size = NSS_WIFILI_TX_PROCESSQ;

	m->msg.init.target_type = desc->target_type;
	m->msg.init.wrip.tlv_size = desc->tlv_size;
	m->msg.init.wrip.rx_buf_len = desc->rx_buf_len;

	/* A fault in the firmware's transmit path names a descriptor by a page
	 * and an offset within it, both of which are the host's. Print what was
	 * handed over so a trapped address can be told apart from a stray one.
	 */
	dev_info(core->dev,
		 "wifili: txdesc %pad ext %pad, %u+%u pages, %u pools, %u tcl, %u reo\n",
		 &soc->txdesc[0].dma, &soc->txdesc[soc->txext].dma,
		 soc->txext, soc->txpages - soc->txext, soc->pdevs,
		 desc->num_tcl, desc->num_reo);
	for (i = 0; i < desc->num_tcl; i++)
		dev_info(core->dev,
			 "wifili: tcl%d id %u base %#x x%u  txcomp id %u base %#x x%u hw %#x/%#x  tclhw %#x/%#x tcle %u\n",
			 i, desc->tcl[i].id,
			 desc->tcl[i].base,
			 desc->tcl[i].num_entries,
			 desc->txcomp[i].id,
			 desc->txcomp[i].base,
			 desc->txcomp[i].num_entries,
			 desc->txcomp[i].hwreg[0],
			 desc->txcomp[i].hwreg[1],
			 desc->tcl[i].hwreg[0],
			 desc->tcl[i].hwreg[1],
			 desc->tcl[i].entry_size);
	/* Where the ring pointers really live. A ring's head and tail are not
	 * all in its registers: the pointer the other side publishes is in one
	 * of two shared arrays, indexed by the ring's own number in one and by
	 * its number less the first local-MAC ring in the other. Printing the
	 * bases is what makes those readable as ordinary memory, which is the
	 * only safe way to read them.
	 */
	dev_info(core->dev,
		 "wifili: shadow rd %#x wr %#x lmac from %u\n",
		 desc->shadow_rd, desc->shadow_wr,
		 desc->lmac_ring_start);
	/* The receive rings, for the same reason and one more: whether the
	 * firmware ever puts a buffer in the ring the hardware writes into is
	 * the difference between a receive path that is misrouted and one
	 * that never starts, and the two look identical from the host.
	 */
	for (i = 0; i < desc->num_reo; i++)
		dev_info(core->dev,
			 "wifili: reo%d id %u base %#x x%u hw %#x/%#x\n",
			 i, desc->reo_dest[i].id,
			 desc->reo_dest[i].base,
			 desc->reo_dest[i].num_entries,
			 desc->reo_dest[i].hwreg[0],
			 desc->reo_dest[i].hwreg[1]);
	for (i = 0; i < soc->pdevs; i++)
		dev_info(core->dev,
			 "wifili: radio %u rxdma id %u base %#x x%u hw %#x/%#x swdesc %u\n",
			 soc->pdev[i].radio_id,
			 soc->pdev[i].rxdma.id,
			 soc->pdev[i].rxdma.base,
			 soc->pdev[i].rxdma.num_entries,
			 soc->pdev[i].rxdma.hwreg[0],
			 soc->pdev[i].rxdma.hwreg[1],
			 soc->pdev[i].num_rx_swdesc);

	ret = nss_msg_send(core, m, offsetof(struct nss_wifili_msg, msg) +
				    sizeof(m->msg.init));
	seq_printf(s, "%-14s rc %d response %u error %u\n", "soc init:", ret,
		   m->cm.response, m->cm.error);
	if (ret)
		goto out;
	inited = true;

	/* The receive rings are filled from the firmware's Wi-Fi share of the
	 * payload pool, one payload per entry for as long as the radio is up.
	 * Declared before the radios are initialised, because the first fill
	 * runs when they are. The share is the core's, set once per boot.
	 */
	if (first) {
		ret = nss_wifi_pool_set(core, nss_wifili_payloads);
		seq_printf(s, "%-14s rc %d (%u payloads)\n", "wifi pool:", ret,
			   nss_wifili_payloads);
		if (ret)
			goto out;
	}
	core->wifili_probed = true;

	for (i = 0; i < soc->pdevs; i++) {
		int if_num;

		/* A radio is a node before it is a radio. The initialisation
		 * that follows is addressed to the SoC but names the radio,
		 * and the firmware has nothing to name until one exists.
		 */
		if_num = nss_wifili_radio_alloc(core, soc);
		soc->radio_if[i] = if_num;
		seq_printf(s, "radio %u node:  if_num %d\n",
			   soc->pdev[i].radio_id, if_num);
		if (if_num < 0) {
			ret = if_num;
			goto out;
		}

		memset(m, 0, sizeof(*m));
		m->cm.interface = ifnum;
		m->cm.type = NSS_WIFILI_PDEV_INIT_MSG;
		nss_wifili_ring_fill(&m->msg.pdevmsg.rxdma_ring,
				     &soc->pdev[i].rxdma);
		m->msg.pdevmsg.radio_id = soc->pdev[i].radio_id;
		m->msg.pdevmsg.lmac_id = soc->pdev[i].lmac_id;
		m->msg.pdevmsg.target_pdev_id = soc->pdev[i].target_pdev_id;
		m->msg.pdevmsg.num_rx_swdesc = soc->pdev[i].num_rx_swdesc;
		ret = nss_msg_send(core, m,
				   offsetof(struct nss_wifili_msg, msg) +
				   sizeof(m->msg.pdevmsg));
		seq_printf(s, "radio %u init:  rc %d response %u error %u\n",
			   soc->pdev[i].radio_id, ret, m->cm.response,
			   m->cm.error);
		if (ret)
			goto out;

		/* Per-packet statistics, which the firmware raises as one
		 * status message per frame and the host reads nowhere. Left
		 * on they are the whole of the receive path's budget: fifteen
		 * thousand of them a second arrive on the message ring beside
		 * two thousand frames. The working stack turns them off here,
		 * between initialising a radio and starting it.
		 */
		memset(m, 0, sizeof(*m));
		m->cm.interface = ifnum;
		m->cm.type = NSS_WIFILI_STATS_V2_CFG_MSG;
		m->msg.scm.cfg = 0;
		ret = nss_msg_send(core, m,
				   offsetof(struct nss_wifili_msg, msg) +
				   sizeof(m->msg.scm));
		seq_printf(s, "radio %u v2stats: rc %d\n",
			   soc->pdev[i].radio_id, ret);
		if (ret)
			goto out;

		ret = nss_wifili_radio_buf_cfg(core, soc, if_num,
					       nss_wifili_radio_bufs);
		seq_printf(s, "radio %u bufs:  rc %d (%u per range)\n",
			   soc->pdev[i].radio_id, ret,
			   nss_wifili_radio_bufs);
		if (ret)
			goto out;
	}

	memset(m, 0, sizeof(*m));
	m->cm.interface = ifnum;
	m->cm.type = NSS_WIFILI_START_MSG;
	ret = nss_msg_send(core, m, offsetof(struct nss_wifili_msg, msg));
	seq_printf(s, "%-14s rc %d response %u error %u\n", "start:", ret,
		   m->cm.response, m->cm.error);
	if (!ret) {
		soc->started = true;
		core->wifili_started = true;
		if (ifnum == NSS_INTERFACE_WIFILI)
			core->mesh_capable = core->fw->mesh &&
					     nss_mesh_probe(core);
		core->wifi_on = true;

		if (first) {
			ret = nss_wifili_vdev_next_hop(core);
			seq_printf(s, "%-14s rc %d\n", "next hop:", ret);
			if (ret)
				goto out;
		}

		/* Ask for the statistics push. The firmware raises no wifili
		 * message at all until it is asked, so a receive path that
		 * never starts and one that starts and drops everything are
		 * indistinguishable from the host; the counters in that push
		 * are what tells them apart. It is a diagnostic and nothing
		 * on the data path depends on it, so it can be left off while
		 * what the firmware does when asked is still in question.
		 */
		if (!nss_wifili_stats_push)
			goto out;

		memset(m, 0, sizeof(*m));
		m->cm.interface = ifnum;
		m->cm.type = NSS_WIFILI_STATS_CFG_MSG;
		m->msg.scm.cfg = 1;
		ret = nss_msg_send(core, m,
				   offsetof(struct nss_wifili_msg, msg) +
				   sizeof(m->msg.scm));
		seq_printf(s, "%-14s rc %d response %u error %u\n", "stats:",
			   ret, m->cm.response, m->cm.error);
		ret = 0;
	}
out:
	/* Nothing the firmware has been given rings for is left half
	 * started: it is told to let them go, as on a reset.
	 */
	if (ret && inited)
		nss_wifili_soc_stop(core, soc);
	else if (!soc->started)
		nss_wifili_txdesc_free(core, soc);
	kfree(m);

	return ret;
}

/* What a halted core held went with it, its memory freed by the halt and its
 * interface numbers free for the next boot to hand out again. The records are
 * freed rather than deleted, because the next firmware never knew them, and
 * the device references went with the halt's interface table.
 */
static void nss_wifili_forget(struct nss_core *core)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(nss_wifili_peer_tbl); i++) {
		struct nss_wifili_peer *p = &nss_wifili_peer_tbl[i];

		if (p->mem)
			dma_free_coherent(core->dev, p->size, p->mem, p->dma);
		p->mem = NULL;
	}
	for (i = 0; i < ARRAY_SIZE(nss_wifili_vdev_tbl); i++) {
		nss_wifili_vdev_tbl[i].dev = NULL;
		nss_wifili_vdev_tbl[i].if_num = -1;
	}
	bitmap_zero(nss_wifili_vdev_on, NSS_INTERFACE_MAX);
	bitmap_zero(nss_wifili_vdev_live, NSS_INTERFACE_MAX);
	nss_wifili_vdev_stats_clear(-1);
	nss_me_drop(-1);
	nss_mesh_reset();

	for (i = 0; i < ARRAY_SIZE(nss_wifili_socs); i++) {
		struct nss_wifili_soc *soc = &nss_wifili_socs[i];

		soc->started = false;
		memset(soc->txdesc, 0, sizeof(soc->txdesc));
		soc->txpages = 0;
		soc->txext = 0;
		memset(soc->radio_if, 0xff, sizeof(soc->radio_if));
	}
}

/* After every halt, before the core can boot again. */
void nss_wifili_halted(struct nss_core *core)
{
	guard(mutex)(&nss_wifili_lock);
	nss_wifili_forget(core);
}

/* Called under the WiFiLi mutex; the core mutex follows it everywhere. */
static int nss_wifili_start_locked(struct nss_core *core, struct seq_file *s)
{
	struct nss_wifili_soc *soc;
	int i, ret, error = 0, instances = 0;

	guard(mutex)(&core->lock);
	if (!core->running)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(nss_wifili_socs); i++) {
		soc = &nss_wifili_socs[i];
		if (!soc->used)
			continue;
		instances++;
		if (soc->started)
			continue;
		ret = nss_wifili_described(soc) ?
			nss_wifili_soc_start(core, soc, s) : -EAGAIN;
		soc->last_error = ret;
		if (ret && !error)
			error = ret;
		seq_printf(s, "instance: %u rc %d\n",
			   nss_wifili_soc_ifnum(soc), ret);
	}
	return error ?: (instances == nss_wifili_expected() ? 0 : -EAGAIN);
}

int nss_wifili_start(struct seq_file *s)
{
	guard(mutex)(&nss_wifili_lock);
	if (!nss_wifili_core)
		return -ENODEV;
	return nss_wifili_start_locked(nss_wifili_core, s);
}

void nss_wifili_resume(struct nss_core *core)
{
	struct seq_file s = {};
	int ret;

	guard(mutex)(&nss_wifili_lock);
	if (!core->wifi_on)
		return;
	ret = nss_wifili_start_locked(core, &s);
	dev_info(core->dev, "Wi-Fi data plane resume: %d\n", ret);
}

/* Count enabled physical targets on the selected buses, including a PCI
 * target that has not probed yet. No WLAN is a valid standalone NSS mode.
 */
unsigned int nss_wifili_expected(void)
{
	static const struct of_device_id ahb[] = {
		{ .compatible = "qcom,ipq8074-wifi" },
		{ .compatible = "qcom,ipq6018-wifi" },
		{ }
	};
	struct pci_dev *pdev = NULL;
	struct device_node *np;
	unsigned int expected = 0;

	if (nss_wlan & NSS_WLAN_AHB)
		for_each_matching_node(np, ahb)
			if (of_device_is_available(np))
				expected++;
	if (nss_wlan & NSS_WLAN_PCI)
		while ((pdev = pci_get_device(0x17cb, 0x1104, pdev)))
			if (pci_resource_start(pdev, 0) <= U32_MAX)
				expected++;
	return expected;
}

ssize_t nss_wifili_state(struct nss_core *core, char *buf)
{
	unsigned int expected, registered = 0, described = 0, started = 0;
	unsigned int vdevs = 0, peers = 0, vdev_errors = 0;
	struct nss_wifili_soc *soc;
	bool rings_ready, ready;
	ssize_t len;
	int i;

	guard(mutex)(&nss_wifili_lock);
	guard(mutex)(&core->lock);
	expected = nss_wifili_expected();
	for (i = 0; i < ARRAY_SIZE(nss_wifili_socs); i++) {
		soc = &nss_wifili_socs[i];
		registered += soc->used;
		described += nss_wifili_described(soc);
		started += soc->used && soc->started;
		vdev_errors += soc->used && soc->vdev_error;
	}
	for (i = 0; i < ARRAY_SIZE(nss_wifili_vdev_tbl); i++)
		vdevs += !!nss_wifili_vdev_tbl[i].dev;
	for (i = 0; i < ARRAY_SIZE(nss_wifili_peer_tbl); i++)
		peers += !!nss_wifili_peer_tbl[i].mem;
	rings_ready = registered == expected && described == expected;
	ready = core->running && rings_ready && started == expected;
	len = sysfs_emit(buf,
		"running=%u expected=%u registered=%u described=%u started=%u rings_ready=%u ready=%u vdevs=%u peers=%u wlan=%u vdev_errors=%u\n",
		core->running, expected, registered, described, started,
		rings_ready, ready, vdevs, peers, nss_wlan, vdev_errors);
	for (i = 0; i < ARRAY_SIZE(nss_wifili_socs); i++) {
		soc = &nss_wifili_socs[i];
		len += sysfs_emit_at(buf, len,
			"instance=%u registered=%u radios=%u expected_radios=%u started=%u last_error=%d vdev_error=%d\n",
			nss_wifili_soc_ifnum(soc), soc->used, soc->pdevs,
			soc->used ? soc->desc.num_pdevs : 0,
			soc->used && soc->started, soc->last_error,
			soc->vdev_error);
	}
	return len;
}
