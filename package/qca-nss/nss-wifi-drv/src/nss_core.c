// SPDX-License-Identifier: GPL-2.0-only
/* Host driver for the NSS firmware behind the Wi-Fi data plane.
 *
 * Bringing a core up is: turn on its clocks and supply, clear the instruction
 * memory it starts executing out of, copy the firmware image into the
 * reserved region it runs from, satisfy the memory the image asks for, and
 * only then release it from reset. The order matters at one point in
 * particular - the image's memory requests carry the addresses the firmware
 * will use, so a core released before they are answered reads pointers that
 * were never written.
 *
 * None of it happens on probe. A running core takes over the switch CPU port,
 * so an image that started one while probing could not also bring up the host
 * ethernet stack; bring-up is asked for through debugfs, and a reboot returns
 * the board to the host data plane.
 */

#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_net.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>
#include <linux/soc/qcom/qca_edma.h>
#include <linux/soc/qcom/qca_ppe.h>

#include "nss_drv.h"

/* Core control registers, in the "nphys" range. */
#define NSS_CORE_RESET_CTRL		0x0004
#define NSS_CORE_BAR			0x0008
#define NSS_CORE_AMC			0x000c
#define NSS_CORE_BOOT_ADDR		0x0010
#define NSS_CORE_INT_STAT2_TYPE		0x0040
#define NSS_CORE_INT_STAT3_TYPE		0x0044
#define NSS_CORE_IFETCH_RANGE		0x0048

/* The core reaches its peripherals through a fixed aperture, and may fetch
 * instructions only from the window the image is linked into. Both are
 * properties of how the firmware was built.
 */
#define NSS_CORE_BAR_ADDR		0x3c000000
#define NSS_CORE_IFETCH_RANGE_ADDR	0xbf004001

/* Copy engine and core-to-core interrupts are level sensitive. */
#define NSS_CORE_INT_STAT2_LEVEL	0xffff
#define NSS_CORE_INT_STAT3_LEVEL	0xff

/* The two reset sets are released in order, and each core's bits sit one byte
 * apart in the register the cores share.
 */
#define NSS_MISC_RESET_FIRST		0x00000020
#define NSS_MISC_RESET_SECOND		0x00000017
#define NSS_MISC_RESET_SHIFT(id)	((id) << 3)

#define NSS_BOOT_TIMEOUT_MS		5000

/* Every clock the core needs, at the rate the firmware is built for. The
 * fabric clocks are shared between the two cores; a Wi-Fi data plane boots
 * only core 0, so they are taken from its node along with its own. The core
 * clock alone carries no rate here: the device tree states the frequencies
 * this SoC supports, and the core runs at the highest of them.
 */
/* The fabric clocks first, then the six that are the core's own. The split is
 * load-bearing rather than cosmetic: a core is given its own clocks back at
 * every start and loses them at every stop, while the fabric ones go on once
 * and stay on, because the switch sits behind them and a wired data plane
 * that has nothing to do with this driver goes down when they do.
 */
static const struct nss_clk_cfg nss_clk_ipq8074[] = {
	{ "nss-noc-clk",		461500000 },
	{ "nss-ptp-ref-clk",		150000000 },
	{ "nss-csr-clk",		200000000 },
	{ "nss-cfg-clk",		100000000 },
	{ "nss-imem-clk",		400000000 },
	{ "nss-nssnoc-qosgen-ref-clk",	 19200000 },
	{ "nss-mem-noc-nss-axi-clk",	461500000 },
	{ "nss-nssnoc-snoc-clk",	266600000 },
	{ "nss-nssnoc-timeout-ref-clk",	  4800000 },
	{ "nss-ce-axi-clk",		200000000 },
	{ "nss-ce-apb-clk",		200000000 },
	{ "nss-nssnoc-ce-axi-clk",	200000000 },
	{ "nss-nssnoc-ce-apb-clk",	200000000 },
	{ "nss-nssnoc-ahb-clk",		200000000 },
	{ "nss-core-clk",			0 },
	{ "nss-ahb-clk",		200000000 },
	{ "nss-axi-clk",		461500000 },
	{ "nss-mpt-clk",		 25000000 },
	{ "nss-nc-axi-clk",		461500000 },
};

/* The rates qca-nss-drv's ipq60xx hal sets. */
static const struct nss_clk_cfg nss_clk_ipq6018[] = {
	{ "nss-noc-clk",		266670000 },
	{ "nss-ptp-ref-clk",		150000000 },
	{ "nss-csr-clk",		200000000 },
	{ "nss-cfg-clk",		100000000 },
	{ "nss-nssnoc-qosgen-ref-clk",	 24000000 },
	{ "nss-nssnoc-snoc-clk",	266600000 },
	{ "nss-snoc-nssnoc-clk",	266670000 },
	{ "nss-nssnoc-timeout-ref-clk",	  6000000 },
	{ "nss-ce-axi-clk",		200000000 },
	{ "nss-ce-apb-clk",		200000000 },
	{ "nss-nssnoc-ce-axi-clk",	200000000 },
	{ "nss-nssnoc-ce-apb-clk",	200000000 },
	{ "nss-mem-noc-ubi32-clk",	533330000 },
	{ "nss-nssnoc-ahb-clk",		200000000 },
	{ "nss-core-clk",			0 },
	{ "nss-ahb-clk",		200000000 },
	{ "nss-axi-clk",		533330000 },
	{ "nss-nc-axi-clk",		266670000 },
	{ "nss-utcm-clk",		266670000 },
};

struct nss_soc {
	const struct nss_clk_cfg *clk;
	/* Program the copy engine and core-to-core interrupts as level
	 * sensitive. qca-nss-drv leaves them at reset on the single-core
	 * IPQ6018.
	 */
	bool level_irqs;
	/* IPQ6018 has a processor reset outside the two clamp sets. */
	u32 core_reset;
};

static const struct nss_soc nss_ipq8074 = {
	.clk = nss_clk_ipq8074,
	.level_irqs = true,
};

static const struct nss_soc nss_ipq6018 = {
	.clk = nss_clk_ipq6018,
	.core_reset = BIT(7), /* GCC_UBI0_CORE_ARES */
};

#define NSS_CLK_COUNT	ARRAY_SIZE(nss_clk_ipq8074)
#define NSS_CLK_FABRIC	13
#define NSS_CLK_OWN	(NSS_CLK_COUNT - NSS_CLK_FABRIC)

static_assert(ARRAY_SIZE(nss_clk_ipq6018) == NSS_CLK_COUNT);

static int nss_clocks_get(struct nss_core *core)
{
	int i, ret;

	for (i = 0; i < NSS_CLK_COUNT; i++)
		core->clks[i].id = core->soc->clk[i].name;

	ret = devm_clk_bulk_get(core->dev, NSS_CLK_COUNT, core->clks);
	if (ret)
		return dev_err_probe(core->dev, ret, "clocks\n");

	for (i = 0; i < NSS_CLK_COUNT; i++) {
		unsigned long rate = core->soc->clk[i].rate ? : core->core_rate;

		ret = clk_set_rate(core->clks[i].clk, rate);
		if (ret)
			return dev_err_probe(core->dev, ret, "%s rate\n",
					     core->clks[i].id);
	}

	return 0;
}

/* Turn the core's clocks on once and leave them on.
 *
 * Thirteen of the nineteen are fabric clocks the switch is behind, not the
 * core's own, so taking them away between one boot and the next disturbs a
 * data plane that has nothing to do with this driver. They go off when the
 * device does.
 */
static int nss_clocks_enable(struct nss_core *core)
{
	int i, ret;

	if (!core->clocks_on) {
		ret = clk_bulk_prepare_enable(NSS_CLK_FABRIC, core->clks);
		if (ret)
			return dev_err_probe(core->dev, ret, "fabric clocks\n");

		for (i = 0; i < core->num_aux_clks; i++) {
			ret = clk_prepare_enable(core->aux_clks[i]);
			if (ret) {
				while (--i >= 0)
					clk_disable_unprepare(core->aux_clks[i]);
				clk_bulk_disable_unprepare(NSS_CLK_FABRIC,
							   core->clks);
				return dev_err_probe(core->dev, ret,
						     "shared clocks\n");
			}
		}

		core->clocks_on = true;
	}

	ret = clk_bulk_prepare_enable(NSS_CLK_OWN, &core->clks[NSS_CLK_FABRIC]);
	if (ret)
		return dev_err_probe(core->dev, ret, "core clocks\n");

	return 0;
}

static void nss_clocks_disable(struct nss_core *core)
{
	int i;

	if (!core->clocks_on)
		return;

	for (i = 0; i < core->num_aux_clks; i++)
		clk_disable_unprepare(core->aux_clks[i]);
	clk_bulk_disable_unprepare(NSS_CLK_FABRIC, core->clks);
	core->clocks_on = false;
}

static const struct nss_fw_line nss_fw_lines[] = {
	{
		.name = "12.5",
		.peer_msg_len = sizeof(struct nss_wifili_peer_msg),
		.vdev_stats_len = sizeof(struct nss_wifi_vdev_stats_sync_msg),
		.vdev_stats_hole = sizeof(struct nss_wifi_vdev_stats_sync_msg) /
				   sizeof(u32),
	},
	{
		.name = "11.4",
		.peer_msg_len = offsetof(struct nss_wifili_peer_msg,
					 peer_memory_size),
		.soc_stats_skip = 8 * sizeof(u32),
		.vdev_stats_len = offsetof(struct nss_wifi_vdev_stats_sync_msg,
					   addr4_exc_fail) - sizeof(u32),
		.vdev_stats_hole = offsetof(struct nss_wifi_vdev_stats_sync_msg,
					    wvmes.mcast_mbr_alloc_failed) /
				   sizeof(u32),
		.mesh = true,
	},
};

static char nss_fw_name[16] = "12.5";
module_param_string(firmware, nss_fw_name, sizeof(nss_fw_name), 0644);
MODULE_PARM_DESC(firmware, "load qca-nss<core>-retail-<firmware>.bin");

/* The line is read from the version string the image carries, such as
 * "Version: NSS.FW.12.5-210-HK.R" or "Version: NSS.HK.11.4.0.5-6-R", so a
 * file named for one line but holding another still gets its own layouts.
 */
static const struct nss_fw_line *nss_fw_line_find(const struct firmware *fw)
{
	static const char tag[] = "Version: NSS.";
	const char *data = (const char *)fw->data;
	const char *v = strnstr(data, tag, fw->size);
	size_t off;
	int i;

	if (!v)
		return NULL;

	off = v - data + sizeof(tag) - 1 + 3;	/* "FW.", "HK." or "CP." */
	for (i = 0; i < ARRAY_SIZE(nss_fw_lines); i++) {
		size_t n = strlen(nss_fw_lines[i].name);

		if (off + n < fw->size &&
		    !memcmp(data + off, nss_fw_lines[i].name, n) &&
		    !isdigit(data[off + n]))
			return &nss_fw_lines[i];
	}

	return NULL;
}

/* The firmware is linked to run from a fixed address, so it is copied to that
 * address rather than anywhere within the region. The region the device tree
 * reserved is what bounds the copy: an image larger than the space between
 * its load address and the end of the region would run into whatever follows.
 */
static int nss_firmware_load(struct nss_core *core)
{
	resource_size_t room = core->region_base + core->region_size -
			       core->load_addr;
	const struct firmware *fw;
	void __iomem *dst;
	char name[48];
	int ret;

	snprintf(name, sizeof(name), "qca-nss%u-retail-%s.bin", core->id,
		 nss_fw_name);

	ret = request_firmware(&fw, name, core->dev);
	if (ret)
		return dev_err_probe(core->dev, ret, "no %s\n", name);

	core->fw = nss_fw_line_find(fw);
	if (!core->fw) {
		dev_err(core->dev, "%s is not a firmware line this driver speaks\n",
			name);
		ret = -ENOEXEC;
		goto out;
	}

	if (fw->size > room) {
		dev_err(core->dev, "%s is %zu bytes, %pa of room\n", name,
			fw->size, &room);
		ret = -EFBIG;
		goto out;
	}

	dst = ioremap(core->load_addr, fw->size);
	if (!dst) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy_toio(dst, fw->data, fw->size);
	iounmap(dst);

	dev_info(core->dev, "loaded %s, line %s, %zu bytes at %pa\n", name,
		 core->fw->name, fw->size, &core->load_addr);
out:
	release_firmware(fw);

	return ret;
}

/* A running core takes CPU-port delivery away from the host, and the EDMA
 * conduit is what takes it back. There is one on this SoC, and the switch is
 * behind it, so it is found rather than described.
 */
static const struct of_device_id nss_edma_of_match[] = {
	{ .compatible = "qualcomm,ipq6018-edma" },
	{ .compatible = "qualcomm,ipq8074-edma" },
	{ }
};

static struct net_device *nss_conduit_get(void)
{
	struct device_node *np __free(device_node) =
		of_find_matching_node(NULL, nss_edma_of_match);
	struct net_device *dev;

	dev = np ? of_find_net_device_by_node(np) : NULL;
	if (dev) {
		dev_hold(dev);
		put_device(&dev->dev);
	}

	return dev;
}

/* The firmware leaves its EDMA rings and the queue table wherever it got to,
 * and a firmware started on them reads descriptors no engine wrote. The
 * conduit's shared DMA reset returns the block to its power-on state; it may
 * only run with the core in reset, and the conduit's driver wants rtnl for it.
 */
static void nss_edma_settle(struct nss_core *core)
{
	struct net_device *conduit = core->conduit;
	u32 flags = ETH_RESET_DMA << ETH_RESET_SHARED_SHIFT;
	int ret;

	/* A WLAN-only reset leaves the core running on the other instance.
	 * It needs no shared DMA reset and must not wait for RTNL there.
	 * Drop the core mutex before taking RTNL, then recheck below.
	 */
	scoped_guard(mutex, &core->lock)
		if (core->loaded || !core->edma_stale)
			return;

	rtnl_lock();
	mutex_lock(&core->lock);
	if (core->edma_stale && !core->loaded) {
		ret = conduit->ethtool_ops->reset ?
		      conduit->ethtool_ops->reset(conduit, &flags) :
		      -EOPNOTSUPP;
		if (ret)
			dev_err(core->dev, "EDMA not reset: %d\n", ret);
		else
			core->edma_stale = false;
	}
	mutex_unlock(&core->lock);
	rtnl_unlock();
}

static void nss_edma_reset_work(struct work_struct *work)
{
	nss_edma_settle(container_of(work, struct nss_core, edma_reset));
}

static void nss_core_release(struct nss_core *core)
{
	u32 val;

	/* Start from the state a cold boot starts from, whatever the last
	 * occupant left: both reset sets asserted and the fetch clamp on. A
	 * release over half-reset state boots a core whose peripheral block
	 * is dead, and the firmware then traps on its first access into it.
	 */
	writel(1, core->nphys + NSS_CORE_RESET_CTRL);
	val = readl(core->misc_reset);
	val |= (NSS_MISC_RESET_FIRST | NSS_MISC_RESET_SECOND |
		core->soc->core_reset) <<
	       NSS_MISC_RESET_SHIFT(core->id);
	writel(val, core->misc_reset);
	usleep_range(10, 20);

	val &= ~(NSS_MISC_RESET_FIRST << NSS_MISC_RESET_SHIFT(core->id));
	writel(val, core->misc_reset);

	/* The core needs 10 to 20 cycles after its reset clamp is released
	 * before the second set may follow.
	 */
	usleep_range(10, 20);

	val &= ~(NSS_MISC_RESET_SECOND << NSS_MISC_RESET_SHIFT(core->id));
	writel(val, core->misc_reset);

	/* Hold the core while its address configuration is written, so it
	 * cannot fetch from a window it has not been given yet.
	 */
	writel(1, core->nphys + NSS_CORE_RESET_CTRL);

	writel(1, core->nphys + NSS_CORE_AMC);
	writel(NSS_CORE_BAR_ADDR, core->nphys + NSS_CORE_BAR);
	writel(core->load_addr, core->nphys + NSS_CORE_BOOT_ADDR);

	if (core->soc->level_irqs) {
		writel(NSS_CORE_INT_STAT2_LEVEL,
		       core->nphys + NSS_CORE_INT_STAT2_TYPE);
		writel(NSS_CORE_INT_STAT3_LEVEL,
		       core->nphys + NSS_CORE_INT_STAT3_TYPE);
	}

	writel(NSS_CORE_IFETCH_RANGE_ADDR, core->nphys + NSS_CORE_IFETCH_RANGE);

	/* Keep the IPQ6018 processor reset until its boot address is ready. */
	val &= ~(core->soc->core_reset << NSS_MISC_RESET_SHIFT(core->id));
	writel(val, core->misc_reset);
	readl(core->misc_reset);
	usleep_range(10, 20);
	writel(0, core->nphys + NSS_CORE_RESET_CTRL);
}

/* Stop the core before anything it is using is taken away. A core left
 * executing keeps driving the switch CPU port and keeps fetching, so a later
 * probe would copy a new image over live firmware and trap it; taking its
 * clocks first would leave a gated register behind, which resets the SoC.
 * Reset is asserted in the reverse order it was released, and only then is
 * the memory it was reading released.
 */
/* Stop a core and put back everything it was using.
 *
 * Asked for by hand as well as at teardown, because a WLAN driver holding a
 * reference to this module means unloading it is no longer a way to stop the
 * firmware - and stopping the firmware is what returns the switch block to
 * the state a cold boot left it in. Idempotent: a core that is not loaded has
 * nothing to put back.
 */
static void nss_core_halt(struct nss_core *core)
{
	u32 val;

	if (!core->loaded)
		return;

	qca_ppe_fw_carrier_set(core->conduit, NULL);
	writel(1, core->nphys + NSS_CORE_RESET_CTRL);

	val = readl(core->misc_reset);
	val |= (NSS_MISC_RESET_SECOND | core->soc->core_reset) <<
	       NSS_MISC_RESET_SHIFT(core->id);
	writel(val, core->misc_reset);

	/* The same settle the release path needs between the two sets. */
	usleep_range(10, 20);

	val |= NSS_MISC_RESET_FIRST << NSS_MISC_RESET_SHIFT(core->id);
	writel(val, core->misc_reset);

	/* Wait out any message or frame already past its running check: the
	 * rings and the lent table go below.
	 */
	scoped_guard(mutex, &core->msg.lock) {
		scoped_guard(spinlock_bh, &core->state_lock) {
			core->booting = false;
			core->running = false;
		}
		core->msg.lost = false;
	}
	synchronize_net();
	/* A core in reset cannot be asked to close its ports, and holds
	 * nothing of theirs in the switch.
	 */
	core->phys_armed = 0;
	nss_bridge_stop();
	nss_flow_flush();
	nss_mc_flush();
	nss_redir_halt(core);

	nss_log_shadow_stop(core);
	nss_rings_stop(core);
	nss_iface_unbind(core);

	/* The core is in reset by now, so the block may be reset under it.
	 * This hands the CPU port back, and a later boot starts from the
	 * same place this one did.
	 */
	schedule_work(&core->edma_reset);

	nss_pbuf_pool_free(core);
	nss_mem_free_all(core);
	clk_bulk_disable_unprepare(NSS_CLK_OWN, &core->clks[NSS_CLK_FABRIC]);

	core->wifili_probed = false;
	core->wifili_started = false;
	WRITE_ONCE(core->loaded, false);
	wake_up_var(&core->loaded);
}

/* How many empty buffers the firmware may hold, and the ceiling the host's own
 * refill stops at; the two are one number because they cannot disagree.
 *
 * The firmware takes one buffer for every connection it accelerates, to carry
 * that connection's statistics back, so a pool sized for the exception path
 * alone stops the flow engines accepting rules once it is spent - the refusal
 * is a buffer allocation failure and says nothing about the rule. An
 * unconfigured pool is unbounded the other way: the firmware accepted 82242
 * buffers, 329 MB of slab on a 411 MB box, within ten seconds of the core
 * booting. The Wi-Fi data plane's own share of it is declared separately,
 * through nss_wifi_pool_set(). The ceiling is that share plus the 16384 the
 * expanded connections and the exception path take; the firmware fills
 * whatever ceiling it is given, and each buffer costs a 4 KB slab object.
 */
static unsigned int nss_pool_size;
module_param_named(pool_size, nss_pool_size, uint, 0644);
MODULE_PARM_DESC(pool_size, "empty buffers lent to firmware; 0 selects 24576 with WLAN, 16384 without");

static unsigned int nss_pool_low;
module_param_named(pool_low, nss_pool_low, uint, 0644);
MODULE_PARM_DESC(pool_low, "refill threshold; 0 selects 4352 with WLAN, 4096 without");
/* The firmware refuses a span narrower than the ring the pool is topped up
 * through, so the asked-for ceiling is not always the one that takes effect.
 * The table of lent buffers is sized from the same answer, and is allocated
 * before the pool is configured.
 */
unsigned int nss_pool_ceiling(void)
{
	bool wlan = nss_wifili_expected();

	if (!nss_pool_size)
		nss_pool_size = wlan ? 24576 : 16384;
	if (!nss_pool_low)
		nss_pool_low = wlan ? 4352 : 4096;
	if (nss_pool_low < NSS_RING_ENTRIES)
		nss_pool_low = NSS_RING_ENTRIES;
	if (nss_pool_size < 2 * nss_pool_low)
		nss_pool_size = 2 * nss_pool_low;

	return nss_pool_size;
}

#define NSS_N2H_RPS_CFG			1
#define NSS_N2H_SET_WATER_MARK		6
#define NSS_N2H_GET_WATER_MARK		7
#define NSS_N2H_WIFI_POOL_BUF_CFG	8

struct nss_n2h_water_mark {
	struct nss_cmn_msg cm;
	__be32 low_water;
	__be32 high_water;
};

struct nss_n2h_payload_info {
	struct nss_cmn_msg cm;
	__be32 pool_size;
	__be32 low_water;
	__be32 high_water;
};

/* Unlike the counts beside it, the flag goes as is: byte-swapped, it leaves
 * the firmware on the first data queue.
 */
struct nss_n2h_rps {
	struct nss_cmn_msg cm;
	u32 enable;
};

struct nss_n2h_wifi_pool {
	struct nss_cmn_msg cm;
	__be32 payloads;
};

/* Say what the pool may reach, and stop the refill at the same number.
 *
 * The firmware drains whatever it holds above its high-water mark straight
 * back, so the mark is what a pool settles at: set below what the host is
 * willing to lend it empties as fast as it is filled and a receive ring never
 * fills, set above it the host stops first and the mark is never reached. The
 * low mark is a ring's worth, the point at which a top-up is worth an
 * interrupt. The counts are byte-swapped, as the other control-plane scalars
 * on this interface are; the addresses beside them are not.
 */
static int nss_pool_set(struct nss_core *core, u32 payloads)
{
	struct nss_n2h_water_mark wm = {};
	int ret;

	wm.cm.interface = NSS_INTERFACE_N2H;
	wm.cm.type = NSS_N2H_SET_WATER_MARK;
	wm.low_water = cpu_to_be32(nss_pool_low);
	wm.high_water = cpu_to_be32(payloads);

	/* Recorded before the message goes out, because this is the first one
	 * after a core boot and the first one is answered late: the firmware
	 * takes it and the host's wait expires first. A ceiling left unset by
	 * that is a host that lends nothing at all.
	 */
	core->pool_size = payloads;

	ret = nss_msg_send(core, &wm, sizeof(wm));
	if (ret)
		return ret;

	return 0;
}

/* Spread host-bound frames over the four data queues by flow, as
 * qca-nss-drv's dev.nss.rps.enable does; the firmware otherwise uses only
 * the first.
 */
static void nss_rps_enable(struct nss_core *core)
{
	struct nss_n2h_rps m = {};
	int ret;

	m.cm.interface = NSS_INTERFACE_N2H;
	m.cm.type = NSS_N2H_RPS_CFG;
	m.enable = 1;
	ret = nss_msg_send(core, &m, sizeof(m));
	if (ret)
		dev_warn(core->dev, "data queue spreading not enabled: %d\n",
			 ret);
}

/* The engines otherwise program the switch themselves for a connection both
 * ends of which they see on a switch port, into tables qca-ppe allocates.
 */
static void nss_accel_in_core(struct nss_core *core)
{
	static const u32 ifaces[][2] = {
		{ NSS_INTERFACE_IPV4, NSS_IPV4_TX_ACCEL_MODE_CFG_MSG },
		{ NSS_INTERFACE_IPV6, NSS_IPV6_TX_ACCEL_MODE_CFG_MSG },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ifaces); i++) {
		struct nss_accel_mode_cfg m = {
			.mode = cpu_to_be32(NSS_ACCEL_MODE_CORE),
		};
		int ret;

		m.cm.interface = ifaces[i][0];
		m.cm.type = ifaces[i][1];
		ret = nss_msg_send(core, &m, sizeof(m));
		if (ret)
			dev_err(core->dev, "interface %u accelerates in the switch: %d\n",
				ifaces[i][0], ret);
	}
}

static void nss_pool_configure(struct nss_core *core)
{
	struct nss_n2h_payload_info p = {};
	int ret;

	ret = nss_pool_set(core, nss_pool_ceiling());
	dev_info(core->dev, "payload pool set to %u: %d\n", nss_pool_size, ret);

	p.cm.interface = NSS_INTERFACE_N2H;
	p.cm.type = NSS_N2H_GET_WATER_MARK;
	ret = nss_msg_send(core, &p, sizeof(p));
	dev_info(core->dev, "payload pool %u low %u high %u: %d\n",
		 be32_to_cpu(p.pool_size), be32_to_cpu(p.low_water),
		 be32_to_cpu(p.high_water), ret);
}

/* Memory the firmware carves its own packet descriptors out of.
 *
 * Without extra pbuf pages a radio's receive ring stops filling short of full,
 * with no error reported. Pages lent with N2H_ADD_BUF_POOL, as qca-nss-drv's
 * extra_pbuf setting does, let it fill.
 *
 * The pages are the firmware's for as long as the core runs. They are mapped
 * for the device and never touched again by the host, and they go back only
 * once the core is in reset.
 */
#define NSS_N2H_ADD_BUF_POOL		5
#define NSS_N2H_PAGES_PER_MSG		32

struct nss_n2h_buf_pool {
	struct nss_cmn_msg cm;
	u32 page_size;
	u32 num_pages;
	u32 addr[NSS_N2H_PAGES_PER_MSG];
	/* 64-bit entries, as in qca-nss-drv's nss_n2h_buf_pool. */
	u64 vaddr[NSS_N2H_PAGES_PER_MSG];
};

static unsigned int nss_pbuf_bytes = 3100000;
module_param_named(pbuf_bytes, nss_pbuf_bytes, uint, 0644);
MODULE_PARM_DESC(pbuf_bytes, "memory lent for the firmware's packet descriptors");

void nss_pbuf_pool_free(struct nss_core *core)
{
	u32 i;

	for (i = 0; i < core->pbuf_pages; i++) {
		dma_unmap_single(core->dev, core->pbuf_dma[i], PAGE_SIZE,
				 DMA_BIDIRECTIONAL);
		kfree(core->pbuf_page[i]);
	}

	kfree(core->pbuf_page);
	kfree(core->pbuf_dma);
	core->pbuf_page = NULL;
	core->pbuf_dma = NULL;
	core->pbuf_pages = 0;
}

static int nss_pbuf_pool_add(struct nss_core *core, size_t bytes)
{
	u32 want = DIV_ROUND_UP(bytes, PAGE_SIZE);
	struct nss_n2h_buf_pool *m;
	int ret = 0;

	if (!want)
		return 0;

	core->pbuf_page = kcalloc(want, sizeof(*core->pbuf_page), GFP_KERNEL);
	core->pbuf_dma = kcalloc(want, sizeof(*core->pbuf_dma), GFP_KERNEL);
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!core->pbuf_page || !core->pbuf_dma || !m) {
		ret = -ENOMEM;
		goto out;
	}

	while (core->pbuf_pages < want) {
		u32 n = 0;

		memset(m, 0, sizeof(*m));
		m->cm.interface = NSS_INTERFACE_N2H;
		m->cm.type = NSS_N2H_ADD_BUF_POOL;
		m->page_size = PAGE_SIZE;

		while (n < NSS_N2H_PAGES_PER_MSG && core->pbuf_pages < want) {
			void *page = kzalloc(PAGE_SIZE, GFP_KERNEL);
			dma_addr_t dma;

			if (!page)
				break;

			dma = dma_map_single(core->dev, page, PAGE_SIZE,
					     DMA_BIDIRECTIONAL);
			if (dma_mapping_error(core->dev, dma)) {
				kfree(page);
				break;
			}

			m->addr[n] = dma;
			core->pbuf_page[core->pbuf_pages] = page;
			core->pbuf_dma[core->pbuf_pages] = dma;
			core->pbuf_pages++;
			n++;
		}

		if (!n) {
			ret = -ENOMEM;
			break;
		}

		m->num_pages = n;
		ret = nss_msg_send(core, m, sizeof(*m));
		if (ret)
			break;
	}

	dev_info(core->dev, "packet descriptors: %u pages of %lu lent: %d\n",
		 core->pbuf_pages, PAGE_SIZE, ret);
out:
	/* Pages already lent stay lent: they go back when the core halts. */
	kfree(m);

	return ret;
}

/* Payloads the firmware keeps for the Wi-Fi data plane.
 *
 * Without NSS_N2H_WIFI_POOL_BUF_CFG the radio receive rings stay empty
 * whatever the pool size.
 */
int nss_wifi_pool_set(struct nss_core *core, u32 payloads)
{
	struct nss_n2h_wifi_pool wp = {};

	wp.cm.interface = NSS_INTERFACE_N2H;
	wp.cm.type = NSS_N2H_WIFI_POOL_BUF_CFG;
	wp.payloads = cpu_to_be32(payloads);

	return nss_msg_send(core, &wp, sizeof(wp));
}

/* The firmware reports its own load for clock scaling about a thousand times
 * a second until told not to, and this driver scales nothing. It is told
 * the rate the core already runs at, so the clock is not moved by this.
 */
#define NSS_INTERFACE_COREFREQ		175
#define NSS_COREFREQ_CHANGE		1
#define NSS_CLK_CORE			14

struct nss_corefreq_msg {
	struct nss_cmn_msg cm;
	u32 frequency;
	u32 start_or_end;
	u32 stats_enable;
	u32 freq_current;
	s32 ack;
};

static void nss_core_stats_off(struct nss_core *core)
{
	struct nss_corefreq_msg m = {};
	int ret;

	/* stats_enable rides on the end-of-change message, as in qca-nss-drv's
	 * nss_freq_change().
	 */
	m.cm.interface = NSS_INTERFACE_COREFREQ;
	m.cm.type = NSS_COREFREQ_CHANGE;
	m.frequency = clk_get_rate(core->clks[NSS_CLK_CORE].clk);
	m.start_or_end = 1;
	ret = nss_msg_send(core, &m, sizeof(m));

	dev_info(core->dev, "load reports off at %u Hz: %d ack %d\n",
		 m.frequency, ret, m.ack);
}

static int nss_core_boot(struct nss_core *core)
{
	int ret;

	lockdep_assert_held(&core->lock);

	if (core->loaded)
		return -EBUSY;

	/* Callers run nss_edma_settle() first. */
	if (core->edma_stale) {
		dev_err(core->dev, "EDMA not reset since a core last ran\n");
		return -EAGAIN;
	}

	ret = nss_clocks_enable(core);
	if (ret)
		return ret;

	/* All of it, as a cold boot leaves it, not only what meminfo hands
	 * out again.
	 */
	memset_io(core->imem, 0, core->imem_size);

	ret = nss_firmware_load(core);
	if (ret)
		goto clk;

	ret = nss_meminfo_init(core);
	if (ret)
		goto free;

	ret = nss_rings_start(core);
	if (ret)
		goto free;

	nss_iface_bind(core);

	core->loaded = true;
	core->edma_stale = true;
	nss_log_shadow_start(core);

	/* A boot that timed out and was then answered leaves the completion
	 * signalled, and the next boot would take that for its own answer.
	 */
	reinit_completion(&core->booted);
	scoped_guard(spinlock_bh, &core->state_lock)
		core->booting = true;
	nss_core_release(core);

	if (!wait_for_completion_timeout(&core->booted,
					 msecs_to_jiffies(NSS_BOOT_TIMEOUT_MS))) {
		dev_err(core->dev, "core %u did not answer in %u ms\n",
			core->id, NSS_BOOT_TIMEOUT_MS);
		dev_err(core->dev,
			"boot registers: reset=%#x misc=%#x bar=%#x addr=%#x fetch=%#x\n",
			readl(core->nphys + NSS_CORE_RESET_CTRL),
			readl(core->misc_reset),
			readl(core->nphys + NSS_CORE_BAR),
			readl(core->nphys + NSS_CORE_BOOT_ADDR),
			readl(core->nphys + NSS_CORE_IFETCH_RANGE));
		nss_log_dump(core, "boot timeout");
		nss_core_halt(core);
		return -ETIMEDOUT;
	}

	/* The firmware maps CPU queues 4-7 to receive ring 3 and never drains
	 * it. Ring 3 takes its buffers from the same fill ring as ring 0, so
	 * 255 frames there stop all receive from the CPU port. Ring 0 hands
	 * the host everything it receives.
	 */
	ret = qca_edma_cpu_queues_to_ring(core->conduit, 0);
	if (ret) {
		dev_err(core->dev, "CPU queues not moved to ring 0: %d\n", ret);
		goto halt;
	}

	ret = qca_ppe_fw_booted(core->conduit);
	if (ret) {
		dev_err(core->dev, "PPE tables not restored: %d\n", ret);
		goto halt;
	}

	nss_pool_configure(core);
	nss_accel_in_core(core);
	nss_rps_enable(core);
	nss_pbuf_pool_add(core, nss_pbuf_bytes);
	nss_core_stats_off(core);
	/* The firmware needs its packet pools before table negotiation. Keep
	 * flow steering disabled until both families acknowledge the handover.
	 * A failed or ambiguous handover halts before its DMA blocks are freed.
	 */
	ret = nss_flow_configure(core);
	if (ret)
		goto halt;

	ret = qca_ppe_fw_carrier_set(core->conduit, nss_flow_is_ours);
	if (ret) {
		dev_err(core->dev, "flows not steered to the firmware: %d\n", ret);
		goto halt;
	}

	nss_bridge_start(core);

	return 0;

halt:
	nss_core_halt(core);
	return ret;

free:
	nss_mem_free_all(core);
clk:
	clk_bulk_disable_unprepare(NSS_CLK_OWN, &core->clks[NSS_CLK_FABRIC]);

	return ret;
}

/* A timed-out boot queues a shared DMA reset. Finish it before rebuilding
 * the firmware rings for one bounded retry. Other failures keep their error.
 */
static int nss_core_boot_retry(struct nss_core *core)
{
	int ret;

	scoped_guard(mutex, &core->lock)
		ret = nss_core_boot(core);
	if (ret != -ETIMEDOUT)
		return ret;

	dev_warn(core->dev,
		 "core %u boot timed out; retrying after EDMA reset\n",
		 core->id);
	flush_work(&core->edma_reset);
	nss_edma_settle(core);

	if (!READ_ONCE(core->up) || READ_ONCE(core->loaded))
		return 0;

	scoped_guard(mutex, &core->lock)
		ret = nss_core_boot(core);

	return ret;
}

/* For the WLAN driver, whose rings the firmware reaps: they are about to go,
 * and the firmware has no way to be told that short of stopping.
 */
void nss_core_stop(struct nss_core *core)
{
	guard(mutex)(&core->lock);
	nss_core_halt(core);
}

/* A core that faults this often is left halted and the host routes. */
#define NSS_FAULTS_MAX		3
#define NSS_FAULT_WINDOW	(10 * 60 * HZ)
#define NSS_HALT_TIMEOUT	(30 * HZ)

/* A dead core is treated as stopped at once and halted from process context,
 * because the halt sleeps.
 */
void nss_core_fault(struct nss_core *core)
{
	/* Stop DMA without releasing shared buffers still owned by WLAN. */
	writel(1, core->nphys + NSS_CORE_RESET_CTRL);
	readl(core->nphys + NSS_CORE_RESET_CTRL);
	scoped_guard(spinlock_bh, &core->state_lock) {
		core->booting = false;
		core->running = false;
	}
	qca_edma_cpu_queues_to_host(core->conduit);
	core->faulted = true;
	schedule_work(&core->restart);
}

/* An authorization message failed. Stop the processor synchronously, but
 * retain shared DMA until WLAN reset has stopped every radio using it.
 */
void nss_core_block(struct nss_core *core)
{
	guard(mutex)(&core->lock);
	WRITE_ONCE(core->up, false);
	if (!core->loaded)
		return;
	dev_err(core->dev, "authorization failed; halting offload until reboot\n");
	writel(1, core->nphys + NSS_CORE_RESET_CTRL);
	readl(core->nphys + NSS_CORE_RESET_CTRL);
	nss_core_fault(core);
}

/* The WLAN driver has described its rings again, before mac80211 takes
 * RTNL to restore its interfaces. The caller must hold neither RTNL nor
 * the WiFiLi mutex: restart may need both to reset shared DMA and start
 * the newly described instance. Do not defer this wait to VDEV creation.
 */
void nss_core_resume(struct nss_core *core)
{
	if (!READ_ONCE(core->up))
		return;

	/* A dead core keeps its buffers until every WLAN instance stops.
	 * Newly described rings must not resume against that old image.
	 * No mac80211 PHY restart may hold RTNL while waiting here.
	 */
	if (!READ_ONCE(core->running) && READ_ONCE(core->loaded) &&
	    !wait_var_event_timeout(&core->loaded,
				    !READ_ONCE(core->loaded), NSS_HALT_TIMEOUT)) {
		dev_err(core->dev, "WLAN instances did not release the halted core\n");
		return;
	}

	schedule_work(&core->restart);
	flush_work(&core->restart);
}

static void nss_core_restart(struct work_struct *work)
{
	struct nss_core *core = container_of(work, struct nss_core, restart);
	bool fault = core->faulted;
	int ret = 0;

	if (fault) {
		core->faulted = false;

		if (time_after(jiffies, core->fault_window)) {
			core->faults = 0;
			core->fault_window = jiffies + NSS_FAULT_WINDOW;
		}
		if (++core->faults > NSS_FAULTS_MAX) {
			dev_err(core->dev, "core %u faulted %u times; leaving it halted\n",
				core->id, core->faults);
			WRITE_ONCE(core->up, false);
		}

		/* The radios still hold buffers the dead core lent them, so
		 * the WLAN driver's reset halts it, and resumes it after.
		 */
		if (nss_wifili_reset(core, true))
			return;

		scoped_guard(mutex, &core->lock)
			nss_core_halt(core);
		nss_wifili_halted(core);
	}

	nss_edma_settle(core);
	if (READ_ONCE(core->up) && !READ_ONCE(core->loaded))
		ret = nss_core_boot_retry(core);
	/* After a fault the radios' interfaces are registered with the dead
	 * instance; after a WLAN reset they are about to register again.
	 */
	if (!ret && fault)
		nss_wifili_reset(core, false);
	else if (!ret)
		nss_wifili_resume(core);

	/* mac80211 may already hold RTNL while waiting for WLAN recovery.
	 * Ring handover must finish before that wait ends; port arming can
	 * follow it from its own worker, without holding up the WLAN driver.
	 */
	schedule_work(&core->arm);
}

static void nss_phys_work(struct work_struct *work)
{
	nss_phys_arm_ports(container_of(work, struct nss_core, arm));
}

static int nss_boot_set(void *data, u64 val)
{
	struct nss_core *core = data;
	int ret = 0;

	/* WLAN recovery automatically restores its rings and interfaces. A
	 * user-requested permanent halt leaves that recovery with no NSS,
	 * so it must instead tear down WLAN modules or reboot. Fault recovery
	 * and driver removal use their own coordinated lifetime paths.
	 */
	if (!val && nss_wifili_expected())
		return -EBUSY;

	WRITE_ONCE(core->up, val);

	if (!val) {
		if (nss_wifili_reset(core, true) &&
		    !wait_var_event_timeout(&core->loaded,
					    !READ_ONCE(core->loaded),
					    NSS_HALT_TIMEOUT))
			return -ETIMEDOUT;

		scoped_guard(mutex, &core->lock)
			nss_core_halt(core);
		nss_wifili_halted(core);
		return 0;
	}

	nss_edma_settle(core);
	ret = nss_core_boot_retry(core);
	if (ret)
		return ret;

	nss_phys_arm_ports(core);
	/* The radios' interfaces registered with the last instance. */
	nss_wifili_reset(core, false);

	return 0;
}

static int nss_boot_get(void *data, u64 *val)
{
	struct nss_core *core = data;

	guard(mutex)(&core->lock);
	*val = core->running;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(nss_boot_fops, nss_boot_get, nss_boot_set, "%llu\n");

/* Bench: take the path a coredump takes, on a core that is still running. */
static int nss_fault_set(void *data, u64 val)
{
	struct nss_core *core = data;

	guard(mutex)(&core->lock);
	if (!core->running)
		return -ENODEV;
	nss_core_fault(core);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(nss_fault_fops, NULL, nss_fault_set, "%llu\n");

static int nss_log_show(struct seq_file *s, void *unused)
{
	struct nss_core *core = s->private;

	guard(mutex)(&core->lock);

	return nss_log_show_ring(core, s);
}
DEFINE_SHOW_ATTRIBUTE(nss_log);

static int nss_probe_show(struct seq_file *s, void *unused)
{
	struct nss_core *core = s->private;

	guard(mutex)(&core->lock);
	if (!core->running)
		return -ENODEV;

	return nss_msg_probe(core, s);
}
DEFINE_SHOW_ATTRIBUTE(nss_probe);

/* What the firmware has actually delivered, by the interface number it named
 * and by the kind of buffer it arrived in. Which interface numbers a running
 * firmware sends on is not something the host can look up, and the exception
 * path has to be written against the ones it really uses.
 */
static int nss_rx_show(struct seq_file *s, void *unused)
{
	struct nss_core *core = s->private;
	int i;

	guard(mutex)(&core->lock);

	for (i = 0; i < ARRAY_SIZE(core->rx_type); i++)
		if (core->rx_type[i])
			seq_printf(s, "type %d: %llu\n", i, core->rx_type[i]);

	for (i = 0; i < NSS_INTERFACE_MAX; i++)
		if (core->rx_iface[i])
			seq_printf(s, "interface %d: %llu\n", i,
				   core->rx_iface[i]);

	seq_printf(s, "notify: %llu link-desc seen: %llu returned: %llu tx: %llu done: %llu\n",
		   core->notify, atomic64_read(&core->link_desc_seen),
		   core->link_desc_returned, core->tx_posted,
		   atomic64_read(&core->tx_done));

	/* Total host buffers lent to the firmware, including WLAN and
	 * protocol/exception work. This is not a free-buffer count; the N2H
	 * snapshots in flows distinguish allocation failures from occupancy.
	 */
	seq_printf(s, "buffers in firmware custody: %d  desync: %llu\n",
		   atomic_read(&core->buffers_queued),
		   atomic64_read(&core->rx_desync));
	seq_printf(s, "tx returned twice: %llu\n", core->tx_dup);

	for (i = 0; i < core->ext_seen; i++) {
		const struct nss_ext_seen *e = &core->ext[i];
		int w;

		seq_printf(s, "ext %d: iface %u type %u len %u offs %u:",
			   i, e->iface, e->type, e->len, e->offs);
		for (w = 0; w < ARRAY_SIZE(e->word); w++)
			seq_printf(s, " %08x", e->word[w]);
		seq_putc(s, '\n');
	}

	for (i = 0; i < ARRAY_SIZE(core->seen); i++) {
		const struct nss_msg_seen *seen = &core->seen[i];
		int w;

		if (!seen->count)
			continue;

		seq_printf(s, "msg %d: %llu first:", i, seen->count);
		for (w = 0; w < seen->words; w++)
			seq_printf(s, " %08x", seen->word[w]);
		seq_puts(s, "\n     last:");
		for (w = 0; w < seen->lastwords; w++)
			seq_printf(s, " %08x", seen->last[w]);
		seq_putc(s, '\n');
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nss_rx);

static int nss_wifili_show(struct seq_file *s, void *unused)
{
	return nss_wifili_start(s);
}
DEFINE_SHOW_ATTRIBUTE(nss_wifili);

static int nss_msgs_show(struct seq_file *s, void *unused)
{
	nss_msg_census_print(s);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nss_msgs);

static int nss_wifili_rx_show(struct seq_file *s, void *unused)
{
	nss_wifili_stats_print(s);
	nss_wifili_vdev_stats_print(s);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nss_wifili_rx);

static int nss_wifili_tx_show(struct seq_file *s, void *unused)
{
	return nss_wifili_tx(s);
}
DEFINE_SHOW_ATTRIBUTE(nss_wifili_tx);

/* Which switch ports the firmware routes through, as a mask of port indices.
 *
 * Arming a port is what puts its receive on the firmware's connection engines
 * instead of on the host, so it is what a pushed rule needs to match anything,
 * and it is a deliberate act for the same reason starting a core is.
 */
static int nss_phys_set(void *data, u64 val)
{
	struct nss_core *core = data;
	int ret;

	/* Outside the core's own lock because the switch driver wants it
	 * that way round, and arming a port is a call into the switch driver
	 * as much as into the firmware.
	 */
	rtnl_lock();
	mutex_lock(&core->lock);
	ret = nss_phys_arm(core, val);
	mutex_unlock(&core->lock);
	rtnl_unlock();

	return ret;
}

static int nss_phys_get(void *data, u64 *val)
{
	struct nss_core *core = data;

	guard(mutex)(&core->lock);
	*val = core->phys_armed;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(nss_phys_fops, nss_phys_get, nss_phys_set, "%#llx\n");

static int nss_flows_show(struct seq_file *s, void *unused)
{
	nss_flow_print(s);
	nss_bridge_print(s);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nss_flows);

static int nss_mcast_show(struct seq_file *s, void *unused)
{
	nss_mc_print(s);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nss_mcast);

static int nss_wifi_me_show(struct seq_file *s, void *unused)
{
	nss_wifili_me_print(s);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nss_wifi_me);

static int nss_redir_show(struct seq_file *s, void *unused)
{
	nss_redir_print(s);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nss_redir);

static int nss_mesh_show(struct seq_file *s, void *unused)
{
	nss_mesh_print(s);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nss_mesh);

static void nss_aux_clocks_put(void *data)
{
	struct nss_core *core = data;
	int i;

	for (i = 0; i < core->num_aux_clks; i++)
		clk_put(core->aux_clks[i]);
}

/* Clocks the firmware needs that belong to neither core, on the shared node. */
static int nss_aux_clocks_get(struct nss_core *core, struct device_node *cmn)
{
	int i, n;

	n = of_count_phandle_with_args(cmn, "clocks", "#clock-cells");
	if (n <= 0)
		return 0;

	core->aux_clks = devm_kcalloc(core->dev, n, sizeof(*core->aux_clks),
				      GFP_KERNEL);
	if (!core->aux_clks)
		return -ENOMEM;

	for (i = 0; i < n; i++) {
		core->aux_clks[i] = of_clk_get(cmn, i);
		if (IS_ERR(core->aux_clks[i])) {
			int ret = PTR_ERR(core->aux_clks[i]);

			core->num_aux_clks = i;
			nss_aux_clocks_put(core);
			return ret;
		}
	}
	core->num_aux_clks = n;

	return devm_add_action_or_reset(core->dev, nss_aux_clocks_put, core);
}

/* The region the core boots from is described once, on the node the two cores
 * share, and each core's load address points into it.
 */
static int nss_region_get(struct nss_core *core)
{
	struct device_node *cmn __free(device_node) =
		of_find_compatible_node(NULL, NULL, "qcom,nss-common");
	struct device_node *mem;
	struct reserved_mem *rmem;
	struct resource res;
	int ret;

	if (!cmn)
		return -ENODEV;

	ret = of_address_to_resource(cmn, 0, &res);
	if (ret)
		return ret;

	/* The reset register sits inside the range the clock controller
	 * claims, so it is mapped without being requested; asking for it
	 * would be refused.
	 */
	core->misc_reset = devm_ioremap(core->dev, res.start,
					resource_size(&res));
	if (!core->misc_reset)
		return -ENOMEM;

	mem = of_parse_phandle(cmn, "memory-region", 0);
	if (!mem)
		return -ENODEV;

	rmem = of_reserved_mem_lookup(mem);
	of_node_put(mem);
	if (!rmem)
		return -ENODEV;

	core->region_base = rmem->base;
	core->region_size = rmem->size;

	return nss_aux_clocks_get(core, cmn);
}

static ssize_t boot_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	u64 running;

	nss_boot_get(dev_get_drvdata(dev), &running);
	return sysfs_emit(buf, "%llu\n", running);
}

static ssize_t boot_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	bool value;
	int ret = kstrtobool(buf, &value);

	if (!ret)
		ret = nss_boot_set(dev_get_drvdata(dev), value);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(boot);

static ssize_t wifi_start_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct seq_file s = {};
	bool value;
	int ret = kstrtobool(buf, &value);

	if (ret || !value)
		return -EINVAL;
	ret = nss_wifili_start(&s);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(wifi_start);

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return nss_wifili_state(dev_get_drvdata(dev), buf);
}
static DEVICE_ATTR_RO(state);

static ssize_t flow_stats_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return nss_flow_summary(buf);
}
static DEVICE_ATTR_RO(flow_stats);

static struct attribute *nss_attrs[] = {
	&dev_attr_boot.attr,
	&dev_attr_wifi_start.attr,
	&dev_attr_state.attr,
	&dev_attr_flow_stats.attr,
	NULL,
};
ATTRIBUTE_GROUPS(nss);

static int nss_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nss_core *core;
	u32 load_addr, rate;
	struct resource *res;
	int ret;

	core = devm_kzalloc(dev, struct_size(core, clks, NSS_CLK_COUNT),
			    GFP_KERNEL);
	if (!core)
		return -ENOMEM;

	core->dev = dev;
	core->soc = of_machine_is_compatible("qcom,ipq6018") ? &nss_ipq6018 :
							       &nss_ipq8074;
	INIT_LIST_HEAD(&core->allocs);
	init_completion(&core->booted);
	INIT_WORK(&core->restart, nss_core_restart);
	INIT_WORK(&core->arm, nss_phys_work);
	INIT_WORK(&core->edma_reset, nss_edma_reset_work);

	spin_lock_init(&core->state_lock);
	ret = devm_mutex_init(dev, &core->lock);
	if (ret)
		return ret;

	/* The firmware addresses everything it is given with 32 bits, so
	 * every block handed to it has to come from below 4 GB.
	 */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "no 32-bit DMA\n");

	ret = of_property_read_u32(dev->of_node, "qcom,id", &core->id);
	if (ret)
		return dev_err_probe(dev, ret, "no qcom,id\n");

	/* WiFiLi and the flow engines currently bind one core. */
	if (core->id)
		return dev_err_probe(dev, -EOPNOTSUPP, "only NSS core 0 is supported\n");

	ret = of_property_read_u32(dev->of_node, "qcom,load-addr", &load_addr);
	if (ret)
		return dev_err_probe(dev, ret, "no qcom,load-addr\n");
	core->load_addr = load_addr;

	ret = of_property_read_u32(dev->of_node, "qcom,max-frequency", &rate);
	if (ret)
		return dev_err_probe(dev, ret, "no qcom,max-frequency\n");
	core->core_rate = rate;

	core->nphys = devm_platform_ioremap_resource_byname(pdev, "nphys");
	if (IS_ERR(core->nphys))
		return PTR_ERR(core->nphys);

	/* Shared with the other core and with the interrupt controller, so it
	 * is mapped rather than claimed.
	 */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "qgic-phys");
	if (!res)
		return -ENODEV;

	core->qgic = devm_ioremap(dev, res->start, resource_size(res));
	if (!core->qgic)
		return -ENOMEM;

	/* IPQ6018 has no instruction memory. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "vphys");
	if (res) {
		core->imem_base = res->start;
		core->imem_size = resource_size(res);
		core->imem = devm_ioremap_resource(dev, res);
		if (IS_ERR(core->imem))
			return PTR_ERR(core->imem);
	}

	ret = nss_clocks_get(core);
	if (ret)
		return ret;

	ret = devm_regulator_get_enable(dev, "npu");
	if (ret)
		return dev_err_probe(dev, ret, "npu supply\n");

	ret = nss_log_shadow_init(core);
	if (ret)
		return ret;

	ret = nss_msg_init(core);
	if (ret)
		return ret;

	ret = nss_region_get(core);
	if (ret)
		return dev_err_probe(dev, ret, "no boot region\n");

	if (core->load_addr < core->region_base ||
	    core->load_addr >= core->region_base + core->region_size)
		return dev_err_probe(dev, -ERANGE,
				     "load address is outside the region\n");

	/* Whatever ran before this driver, the block is reset before a boot. */
	core->conduit = nss_conduit_get();
	if (!core->conduit)
		return dev_err_probe(dev, -EPROBE_DEFER, "no EDMA conduit\n");
	core->edma_stale = true;

	platform_set_drvdata(pdev, core);
	nss_wifili_bind(core);
	nss_flow_bind(core);
	nss_mc_bind(core);
	nss_redir_bind(core);

	core->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	debugfs_create_file("boot", 0600, core->debugfs, core, &nss_boot_fops);
	debugfs_create_file("fault", 0200, core->debugfs, core, &nss_fault_fops);
	debugfs_create_file("log", 0400, core->debugfs, core, &nss_log_fops);
	if (IS_ENABLED(CONFIG_NSS_WIFI_DEBUG))
		debugfs_create_file("msg_probe", 0400, core->debugfs, core,
				    &nss_probe_fops);
	debugfs_create_file("rx", 0400, core->debugfs, core, &nss_rx_fops);
	if (IS_ENABLED(CONFIG_NSS_WIFI_DEBUG))
		debugfs_create_file("wifili_start", 0400, core->debugfs, core,
				    &nss_wifili_fops);
	debugfs_create_file("msgs", 0400, core->debugfs, core, &nss_msgs_fops);
	debugfs_create_file("wifili_rx", 0400, core->debugfs, core,
			    &nss_wifili_rx_fops);
	if (IS_ENABLED(CONFIG_NSS_WIFI_DEBUG))
		debugfs_create_file("wifili_tx", 0400, core->debugfs, core,
				    &nss_wifili_tx_fops);
	debugfs_create_file("fw_ports", 0600, core->debugfs, core,
			    &nss_phys_fops);
	debugfs_create_file("flows", 0400, core->debugfs, core,
			    &nss_flows_fops);
	debugfs_create_file("mcast", 0400, core->debugfs, core,
			    &nss_mcast_fops);
	debugfs_create_file("wifi_me", 0400, core->debugfs, core,
			    &nss_wifi_me_fops);
	debugfs_create_file("mesh", 0400, core->debugfs, core,
			    &nss_mesh_fops);
	debugfs_create_file("redir", 0400, core->debugfs, core,
			    &nss_redir_fops);

	return 0;
}

static void nss_remove(struct platform_device *pdev)
{
	struct nss_core *core = platform_get_drvdata(pdev);

	nss_wifili_bind(NULL);
	nss_flow_bind(NULL);
	nss_mc_bind(NULL);
	debugfs_remove_recursive(core->debugfs);
	WRITE_ONCE(core->up, false);
	scoped_guard(mutex, &core->lock)
		nss_core_halt(core);
	nss_wifili_halted(core);
	cancel_work_sync(&core->restart);
	flush_work(&core->arm);
	flush_work(&core->edma_reset);
	/* After the halt, which forgets the bound core's node pairs. */
	nss_redir_bind(NULL);
	dev_put(core->conduit);
	nss_clocks_disable(core);
}

static const struct of_device_id nss_of_match[] = {
	{ .compatible = "qcom,nss" },
	{ }
};
MODULE_DEVICE_TABLE(of, nss_of_match);

static struct platform_driver nss_driver = {
	.probe = nss_probe,
	.remove = nss_remove,
	.driver = {
		.name = "nss-wifi-drv",
		.of_match_table = nss_of_match,
		.dev_groups = nss_groups,
	},
};

/* The flow table's own state outlives any one core, and its registrations are
 * per module rather than per device, so it is set up and torn down here rather
 * than beside a device.
 */
static int __init nss_module_init(void)
{
	int ret;

	ret = nss_flow_init();
	if (ret)
		return ret;

	ret = nss_mc_init();
	if (ret)
		goto flow;

	ret = nss_redir_init();
	if (ret)
		goto mc;

	ret = platform_driver_register(&nss_driver);
	if (ret)
		goto redir;

	return 0;

redir:
	nss_redir_exit();
mc:
	nss_mc_exit();
flow:
	nss_flow_exit();

	return ret;
}
module_init(nss_module_init);

static void __exit nss_module_exit(void)
{
	platform_driver_unregister(&nss_driver);
	nss_redir_exit();
	nss_wifili_me_exit();
	nss_mesh_exit();
	nss_mc_exit();
	nss_flow_exit();
}
module_exit(nss_module_exit);

MODULE_DESCRIPTION("NSS firmware host driver for the Wi-Fi data plane");
MODULE_LICENSE("GPL");
