/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 StarField Xu <air_jinkela@163.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/of_mdio.h>
#include <linux/of_gpio.h>
#include <linux/of_net.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/sfp.h>
#include <linux/iopoll.h>
#include <linux/netdevice.h>
#include <net/rtnetlink.h>

#include "./rtl837x_common.h"

#include <linux/printk.h>

static DEFINE_MUTEX(rtl837x_sdk_mutex);

void rtl837x_sdk_lock(struct rtk_gsw *gsw)
{
	mutex_lock(&rtl837x_sdk_mutex);
	rtl_gbl_priv = gsw;
	gsw->sdk_select_count++;
}

void rtl837x_sdk_unlock(struct rtk_gsw *gsw)
{
	WARN_ON_ONCE(rtl_gbl_priv != gsw);
	rtl_gbl_priv = NULL;
	mutex_unlock(&rtl837x_sdk_mutex);
}

const uint8_t rtl8373_port_map[16] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, // 物理端口0-8
	0, 0, 0, 0, 0, 0, 0 // 填充
};

const uint8_t rtl8372_port_map[16] = {
	3, 4, 5, 6, 7, 8, // 物理端口3-8
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0 // 填充
};

static struct rtl837x_mib_counter rtl837x_mib_counters[] = { { 0, "ifInOctets" },	   { 2, "ifOutOctets" },	  { 4, "ifInUcastPkts" },	 { 6, "ifInMulticastPkts" },	{ 8, "ifInBroadcastPkts" },
							     { 0xA, "ifOutUcastPkts" },	   { 0xC, "ifOutMulticastPkts" }, { 0xE, "ifOutBroadcastPkts" }, { 0x10, "ifOutDiscards" },	{ 0x19, "InPauseFrames" },
							     { 0x1A, "OutPauseFrames" },   { 0x1C, "TxBroadcastPkts" },	  { 0x1D, "TxMulticastPkts" },	 { 0x20, "TxUndersizePkts" },	{ 0x21, "RxUndersizePkts" },
							     { 0x22, "TxOversizePkts" },   { 0x23, "RxOversizePkts" },	  { 0x24, "TxFragments" },	 { 0x25, "RxFragments" },	{ 0x26, "TxJabbers" },
							     { 0x27, "RxJabbers" },	   { 0x28, "TxCollisions" },	  { 0x29, "Tx64Octets" },	 { 0x2A, "Rx64Octets" },	{ 0x2B, "Tx65to127Bytes" },
							     { 0x2C, "Rx65to127Bytes" },   { 0x2D, "Tx128to255Bytes" },	  { 0x2E, "Rx128to255Bytes" },	 { 0x2F, "Tx256to511Bytes" },	{ 0x30, "Rx256to511Bytes" },
							     { 0x31, "Tx512to1023Bytes" }, { 0x32, "Rx512to1023Bytes" },  { 0x33, "Tx1024to1518Bytes" }, { 0x34, "Rx1024to1518Bytes" }, { 0x36, "RxUndersizedropPkts" },
							     { 0x37, "Tx1519toMaxBytes" }, { 0x38, "Rx1519toMaxBytes" },  { 0x39, "TxOverMaxBytes" },	 { 0x3A, "RxOverMaxBytes" } };

/* Match the validated 6.18 rtl8372n transport. The callback is invoked while
 * the bus lock is held, so use __mdiobus_* and poll the switch busy bit with a
 * bounded timeout. This keeps the SoC MDIO controller's own transaction and
 * error handling intact.
 */
static int rtl837x_mdio_wait_not_busy(struct rtk_gsw *priv, u16 *status)
{
	int val;
	int ret;

	ret = read_poll_timeout(__mdiobus_read, val, val < 0 || !(val & BIT(2)), 10, 5000, false, priv->bus, priv->mdio_addr, MDC_MDIO_CTRL_REG);
	if (ret) {
		priv->mdio_timeouts++;
		return ret;
	}
	if (val >= 0 && status)
		*status = val;

	return val < 0 ? val : 0;
}

static void rtl837x_mdio_record(struct rtk_gsw *priv, bool write, u32 reg, u32 val, int ret)
{
	priv->mdio_last_write = write;
	priv->mdio_last_reg = reg;
	priv->mdio_last_value = val;
	priv->mdio_last_error = ret;

	if (ret)
		dev_err_ratelimited(priv->dev, "MDIO indirect %s failed: reg=0x%04x err=%d timeouts=%llu\n", write ? "write" : "read", reg, ret, (unsigned long long)priv->mdio_timeouts);
}

static int rtl837x_mdio_write(void *ctx, u32 reg, u32 val)
{
	struct rtk_gsw *priv = ctx;
	struct mii_bus *bus = priv->bus;
	int ret;

	priv->mdio_writes++;
	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = rtl837x_mdio_wait_not_busy(priv, &priv->mdio_last_ctrl_before);
	if (ret)
		goto out_unlock;

	ret = __mdiobus_write(bus, priv->mdio_addr, MDC_MDIO_ADDR_REG, reg);
	if (ret)
		goto out_unlock;

	ret = __mdiobus_write(bus, priv->mdio_addr, MDC_MDIO_DATA_LOW, val & 0xffff);
	if (ret)
		goto out_unlock;

	ret = __mdiobus_write(bus, priv->mdio_addr, MDC_MDIO_DATA_HIGH, (val >> 16) & 0xffff);
	if (ret)
		goto out_unlock;

	ret = __mdiobus_write(bus, priv->mdio_addr, MDC_MDIO_CTRL_REG, MDC_MDIO_WRITE_CMD);
	if (ret)
		goto out_unlock;

	ret = rtl837x_mdio_wait_not_busy(priv, &priv->mdio_last_ctrl_after);
out_unlock:
	mutex_unlock(&bus->mdio_lock);
	rtl837x_mdio_record(priv, true, reg, val, ret);
	return ret;
}

static int rtl837x_mdio_read(void *ctx, u32 reg, u32 *val)
{
	struct rtk_gsw *priv = ctx;
	struct mii_bus *bus = priv->bus;
	int ret, val_l = -1, val_h = -1;

	*val = 0;
	priv->mdio_reads++;
	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = rtl837x_mdio_wait_not_busy(priv, &priv->mdio_last_ctrl_before);
	if (ret)
		goto out_unlock;

	ret = __mdiobus_write(bus, priv->mdio_addr, MDC_MDIO_ADDR_REG, reg);
	if (ret)
		goto out_unlock;

	ret = __mdiobus_write(bus, priv->mdio_addr, MDC_MDIO_CTRL_REG, MDC_MDIO_READ_CMD);
	if (ret)
		goto out_unlock;

	ret = rtl837x_mdio_wait_not_busy(priv, &priv->mdio_last_ctrl_after);
	if (ret)
		goto out_unlock;

	val_l = __mdiobus_read(bus, priv->mdio_addr, MDC_MDIO_DATA_LOW);
	if (val_l < 0) {
		ret = val_l;
		goto out_unlock;
	}

	val_h = __mdiobus_read(bus, priv->mdio_addr, MDC_MDIO_DATA_HIGH);
	if (val_h < 0) {
		ret = val_h;
		goto out_unlock;
	}

	*val = ((u32)(u16)val_h << 16) | (u16)val_l;
	priv->mdio_last_data_low = val_l;
	priv->mdio_last_data_high = val_h;
	ret = 0;

out_unlock:
	mutex_unlock(&bus->mdio_lock);
	rtl837x_mdio_record(priv, false, reg, *val, ret);
	return ret;
}

static void rtl837x_mdio_lock(void *ctx)
{
	struct rtk_gsw *priv = ctx;

	mutex_lock(&priv->map_lock);
}

static void rtl837x_mdio_unlock(void *ctx)
{
	struct rtk_gsw *priv = ctx;

	mutex_unlock(&priv->map_lock);
}

static const struct regmap_config rtl837x_mdio_regmap_config = {
	.reg_bits = 16,
	.val_bits = 32,
	.reg_stride = 4,

	.max_register = 0xffff,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.reg_read = rtl837x_mdio_read,
	.reg_write = rtl837x_mdio_write,
	.cache_type = REGCACHE_NONE,
	.lock = rtl837x_mdio_lock,
	.unlock = rtl837x_mdio_unlock,
};

static char *chipid_to_chip_name(switch_chip_t id)
{
	switch (id) {
	case CHIP_RTL8373:
		return "RTL8373";
	case CHIP_RTL8372:
		return "RTL8372";
	case CHIP_RTL8224:
		return "RTL8224";
	case CHIP_RTL8373N:
		return "RTL8373N";
	case CHIP_RTL8372N:
		return "RTL8372N";
	case CHIP_RTL8224N:
		return "RTL8224N";
	case CHIP_RTL8366U:
		return "RTL8366U";
	default:
		return "Unknow";
	}
}

static int rtl837x_cpu_port_to_sds(struct rtk_gsw *gsw)
{
	switch (gsw->chip_id) {
	case CHIP_RTL8372:
	case CHIP_RTL8372N:
	case CHIP_RTL8373:
	case CHIP_RTL8373N:
		/* The SDK and chip block diagram route SDS0 to MAC3 and SDS1
		 * to MAC8 throughout this supported chip family.
		 */
		if (gsw->cpu_port == UTP_PORT3)
			return 0;
		if (gsw->cpu_port == UTP_PORT8)
			return 1;
		break;
	default:
		break;
	}

	return -ENODEV;
}

static int rtl837x_switch_probe(struct rtk_gsw *gsw)
{
	switch_chip_t sw_chip;
	int ret;

	gsw->probe_attempts++;
	ret = switch_probe(&sw_chip);
	if (ret != RT_ERR_OK) {
		gsw->last_probe_error = ret;
		dev_err(gsw->dev, "switch detection failed: sdk=%d mdio=%d last-%s-reg=0x%04x timeouts=%llu\n", ret, gsw->mdio_last_error, gsw->mdio_last_write ? "write" : "read", gsw->mdio_last_reg, (unsigned long long)gsw->mdio_timeouts);
		goto CHIP_NOT_SUPPORTED;
	}

	switch (sw_chip) {
	case CHIP_RTL8372:
	case CHIP_RTL8372N:
		gsw->chip_name = chipid_to_chip_name(sw_chip);
		gsw->num_ports = 6;
		gsw->port_map = rtl8372_port_map;
		gsw->valid_port_mask = GENMASK(8, 3);
		gsw->dsa_num_ports = 9;
		goto END_DETECT_CHIP;
	case CHIP_RTL8373:
	case CHIP_RTL8373N:
		gsw->chip_name = chipid_to_chip_name(sw_chip);
		gsw->num_ports = 9;
		gsw->port_map = rtl8373_port_map;
		gsw->valid_port_mask = GENMASK(8, 0);
		gsw->dsa_num_ports = 9;
		goto END_DETECT_CHIP;
	default:
		goto CHIP_NOT_SUPPORTED;
	}

CHIP_NOT_SUPPORTED: {
	rtk_uint32 reg_value = 0;

	ret = rtl8373_getAsicReg(0x4, &reg_value);
	gsw->last_probe_id = reg_value;
	if (ret)
		dev_err(gsw->dev, "unable to read switch ID: sdk=%d mdio=%d last-%s-reg=0x%04x timeouts=%llu\n", ret, gsw->mdio_last_error, gsw->mdio_last_write ? "write" : "read", gsw->mdio_last_reg,
			(unsigned long long)gsw->mdio_timeouts);
	else
		dev_err(gsw->dev, "unsupported switch ID 0x%08x (mdio-data-hi=0x%04x lo=0x%04x)\n", reg_value, gsw->mdio_last_data_high, gsw->mdio_last_data_low);
}
	return RT_ERR_CHIP_NOT_SUPPORTED;

END_DETECT_CHIP:
	gsw->pMapper = dal_rtl8373_mapper_get();
	gsw->chip_id = sw_chip;
	gsw->last_probe_error = 0;

	if (!gsw->cpu_port_from_dsa) {
		if (gsw->legacy_cpu_port >= gsw->num_ports) {
			dev_err(gsw->dev, "legacy CPU port %u is out of logical range\n", gsw->legacy_cpu_port);
			return RT_ERR_INPUT;
		}

		gsw->cpu_port = PORT_MAPPED(gsw->legacy_cpu_port);
	}

	if (!(gsw->valid_port_mask & BIT(gsw->cpu_port))) {
		dev_err(gsw->dev, "CPU port %u is not valid for %s\n", gsw->cpu_port, gsw->chip_name);
		return RT_ERR_PORT_ID;
	}
	gsw->cpu_sds = rtl837x_cpu_port_to_sds(gsw);
	gsw->configured_port_mask = gsw->valid_port_mask;

	dev_dbg(gsw->dev, "Found Realtek RTL chip %s\n", gsw->chip_name);
	return RT_ERR_OK;
}

static int rtl837x_hw_reset(struct rtk_gsw *gsw)
{
	if (!gsw->reset_pin)
		return 0;

	dev_info(gsw->dev, "assert switch reset for %u us\n", gsw->reset_assert_us);
	gpiod_set_value_cansleep(gsw->reset_pin, 1);
	fsleep(gsw->reset_assert_us);
	gpiod_set_value_cansleep(gsw->reset_pin, 0);
	fsleep(gsw->reset_deassert_us);
	dev_info(gsw->dev, "switch reset released after %u us\n", gsw->reset_deassert_us);

	return 0;
}

/* The SDK reports every physically valid port, while a DSA instance may
 * expose only a subset. Use the DT port set for operational masks and keep
 * the SDK mask for chip/CPU validation. */
static int rtl837x_of_get_configured_port_mask(struct device_node *np, u32 valid_mask, u32 *configured_mask)
{
	struct device_node *ports, *child;
	u32 mask = 0;
	int ret = 0;

	ports = of_get_child_by_name(np, "ports");
	if (!ports)
		ports = of_get_child_by_name(np, "ethernet-ports");
	if (!ports) {
		*configured_mask = valid_mask;
		return 0;
	}

	for_each_available_child_of_node(ports, child) {
		u32 port;

		ret = of_property_read_u32(child, "reg", &port);
		if (ret || port >= RTK_MAX_NUM_OF_PORT || !(valid_mask & BIT(port))) {
			ret = ret ? ret : -EINVAL;
			of_node_put(child);
			break;
		}
		mask |= BIT(port);
	}
	of_node_put(ports);
	if (ret)
		return ret;
	if (!mask)
		return -EINVAL;

	*configured_mask = mask;
	return 0;
}

static const struct rtl837x_sdsmode_map _rtl837x_sdsmode[] = {
	{ SERDES_10GQXG, "10g-qxg" }, { SERDES_10GUSXG, "10g-usxg" },	  { SERDES_10GR, "10g-kr" },	  { SERDES_HSG, "hsgmii" },  { SERDES_2500BASEX, "2500base-x" },
	{ SERDES_SG, "sgmii" },	      { SERDES_1000BASEX, "1000base-x" }, { SERDES_100FX, "100base-fx" }, { SERDES_8221B, "8221b" },
};

static int rtl837x_sdsmode(const char *name, rtk_sds_mode_t *mode)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(_rtl837x_sdsmode); i++) {
		if (!strcmp(name, _rtl837x_sdsmode[i].name)) {
			*mode = _rtl837x_sdsmode[i].mode;
			return 0;
		}
	}

	return -1;
}

static int rtl837x_sds_mode_to_port_speed(rtk_sds_mode_t mode, rtk_port_speed_t *speed)
{
	switch (mode) {
	case SERDES_10GQXG:
	case SERDES_10GUSXG:
	case SERDES_10GR:
		*speed = PORT_SPEED_10G;
		return 0;
	case SERDES_HSG:
	case SERDES_2500BASEX:
		*speed = PORT_SPEED_2500M;
		return 0;
	case SERDES_SG:
	case SERDES_1000BASEX:
		*speed = PORT_SPEED_1000M;
		return 0;
	case SERDES_100FX:
		*speed = PORT_SPEED_100M;
		return 0;
	default:
		return -EINVAL;
	}
}

static int rtl8372n_igmp_init(struct rtk_gsw *gsw)
{
	unsigned int ret;

	ret = rtk_igmp_init();
	if (ret)
		return ret;

	ret = rtk_igmp_state_set(TRUE);
	if (ret)
		return ret;

	ret = rtk_igmp_fastLeave_set(TRUE);
	if (ret)
		return ret;

	if (gsw->num_ports > 0) {
		for (int port = 0; port < gsw->num_ports; port++) {
			rtk_uint32 phy_port = PORT_MAPPED(port);

			ret = rtk_igmp_maxGroup_set(phy_port, 255);
			if (ret) {
				dev_err(gsw->dev, "rtk_igmp_maxGroup_set failed, error:%d\n", ret);
				return ret;
			}
		}
	}
	return rtk_igmp_suppressionEnable_set(TRUE, TRUE);
}

static int of_extra_init(struct rtk_gsw *gsw)
{
	struct device_node *node = gsw->dev->of_node;
	const __be32 *list;
	int size, data_len, i, ret;
	u32 reg, mask, val;

	list = of_get_property(node, "extra-init", &size);
	if (!list || !size)
		return 0;
	if (size % (3 * sizeof(*list))) {
		dev_err(gsw->dev, "extra-init must contain register/mask/value triples\n");
		return -EINVAL;
	}

	data_len = size / (3 * sizeof(*list));
	for (i = 0; i < data_len; i++) {
		reg = be32_to_cpu(*list);
		list++;
		mask = be32_to_cpu(*list);
		list++;
		val = be32_to_cpu(*list);
		list++;
		ret = rtl8373_setAsicRegBits(reg, mask, val);
		if (ret) {
			dev_err(gsw->dev, "extra-init[%d] failed: reg=0x%x mask=0x%x value=0x%x error=%d\n", i, reg, mask, val, ret);
			return -EIO;
		}
	}

	return 0;
}

static int rtl837x_rtl8372n_led_init(struct rtk_gsw *gsw)
{
	rtk_uint32 glb_before = ~0, io_before = ~0, set_before = ~0;
	rtk_uint32 glb_after = ~0, io_after = ~0, set_after = ~0;
	int ret;

	if (!gsw->init_rtl8372n_leds)
		return 0;

	if (gsw->chip_id != CHIP_RTL8372N) {
		dev_err(gsw->dev, "realtek,rtl8372n-led-init is unsupported on %s\n", gsw->chip_name);
		return -EOPNOTSUPP;
	}

	(void)rtl8373_getAsicReg(RTL8373_LED_GLB_CTRL_ADDR, &glb_before);
	(void)rtl8373_getAsicReg(RTL8373_LED_GLB_IO_EN_ADDR, &io_before);
	(void)rtl8373_getAsicReg(RTL8373_LED1_0_SET0_CTRL0_ADDR, &set_before);

	ret = dal_rtl8373_led_default_init();
	if (ret != RT_ERR_OK)
		return ret;

	(void)rtl8373_getAsicReg(RTL8373_LED_GLB_CTRL_ADDR, &glb_after);
	(void)rtl8373_getAsicReg(RTL8373_LED_GLB_IO_EN_ADDR, &io_after);
	(void)rtl8373_getAsicReg(RTL8373_LED1_0_SET0_CTRL0_ADDR, &set_after);
	dev_info(gsw->dev, "RTL8372N LED init: glb=0x%08x->0x%08x io=0x%08x->0x%08x set0=0x%08x->0x%08x\n", glb_before, glb_after, io_before, io_after, set_before, set_after);

	return 0;
}

static int rtl837x_init_sds_regbits(struct rtk_gsw *gsw, u32 sds, u32 page, u32 reg, u32 mask, u32 value)
{
	int ret;

	ret = gsw->pMapper->rtl8373_sds_regbits_write(sds, page, reg, mask, value);
	if (!ret)
		return 0;

	dev_err(gsw->dev, "SerDes init write failed: sds=%u page=0x%x reg=0x%x mask=0x%x value=0x%x error=%d\n", sds, page, reg, mask, value, ret);
	return -EIO;
}

static int rtl837x_init_regbits(struct rtk_gsw *gsw, u32 reg, u32 mask, u32 value)
{
	int ret;

	ret = gsw->pMapper->rtl8373_setAsicRegBits(reg, mask, value);
	if (!ret)
		return 0;

	dev_err(gsw->dev, "switch init write failed: reg=0x%x mask=0x%x value=0x%x error=%d\n", reg, mask, value, ret);
	return -EIO;
}

int rtl8372n_hw_init(struct rtk_gsw *gsw, rtl837x_pnswap_cfg_t swap_cfg)
{
	rtk_port_ability_t cpu_ability;
	rtk_sds_mode_t active_sds_mode;
	rtk_sds_mode_t cpu_sds_mode;
	rtk_port_speed_t cpu_speed;
	int ret;

	if (!gsw->preserve_boot_config)
		rtl837x_hw_reset(gsw);
	ret = rtl837x_switch_probe(gsw);
	if (ret) {
		dev_err(gsw->dev, "rtl837x_switch_probe Fail, error:%d\n", ret);
		return -EPERM;
	}
	ret = rtl837x_of_get_configured_port_mask(gsw->dev->of_node, gsw->valid_port_mask, &gsw->configured_port_mask);
	if (ret) {
		dev_err(gsw->dev, "invalid DSA port mask, error:%d\n", ret);
		return ret;
	}

	if (gsw->preserve_boot_config) {
		ret = rtk_switch_attach();
		if (ret) {
			dev_err(gsw->dev, "rtk_switch_attach failed, error:%d\n", ret);
			return -EPERM;
		}
		ret = rtl837x_rtl8372n_led_init(gsw);
		if (ret) {
			dev_err(gsw->dev, "RTL8372N LED initialization failed, error:%d\n", ret);
			return ret;
		}
		if (gsw->reinit_cpu_serdes) {
			if (gsw->cpu_sds < 0) {
				dev_err(gsw->dev, "CPU port %u has no SerDes mapping for %s\n", gsw->cpu_port, gsw->chip_name);
				return -EINVAL;
			}

			cpu_sds_mode = gsw->cpu_sds ? gsw->sds1mode : gsw->sds0mode;
			if (cpu_sds_mode == SERDES_OFF) {
				dev_err(gsw->dev, "CPU SerDes %d reinitialization requested without a configured mode\n", gsw->cpu_sds);
				return -EINVAL;
			}

			ret = rtk_fw_reset_flow_tgr_tgx(gsw->cpu_sds);
			if (ret) {
				dev_err(gsw->dev, "CPU SerDes %d pre-reset failed, error:%d\n", gsw->cpu_sds, ret);
				return -EPERM;
			}

			ret = rtl837x_sds_mode_to_port_speed(cpu_sds_mode, &cpu_speed);
			if (ret) {
				dev_err(gsw->dev, "CPU SerDes %d mode %d has no fixed MAC speed mapping\n", gsw->cpu_sds, cpu_sds_mode);
				return ret;
			}

			ret = rtk_port_macForceLink_get(gsw->cpu_port, &cpu_ability);
			if (ret) {
				dev_err(gsw->dev, "CPU port %u force state read failed, error:%d\n", gsw->cpu_port, ret);
				return -EPERM;
			}
			cpu_ability.forcemode = ENABLED;
			cpu_ability.link = PORT_LINKUP;
			cpu_ability.duplex = PORT_FULL_DUPLEX;
			cpu_ability.speed = cpu_speed;
			ret = rtk_port_macForceLink_set(gsw->cpu_port, &cpu_ability);
			if (ret) {
				dev_err(gsw->dev, "CPU port %u force state update failed, error:%d\n", gsw->cpu_port, ret);
				return -EPERM;
			}

			ret = rtk_sdsMode_set(gsw->cpu_sds, cpu_sds_mode);
			if (ret) {
				dev_err(gsw->dev, "CPU SerDes %d mode %d initialization failed, error:%d\n", gsw->cpu_sds, cpu_sds_mode, ret);
				return -EPERM;
			}

			active_sds_mode = SERDES_END;
			ret = rtk_sdsMode_get(gsw->cpu_sds, &active_sds_mode);
			if (ret)
				dev_warn(gsw->dev, "CPU SerDes %d mode readback failed, error:%d\n", gsw->cpu_sds, ret);
			else
				dev_info(gsw->dev, "CPU SerDes %d SDK reinitialization complete: requested=%d active=%d cpu-port=%u speed=%d pause=%u/%u\n", gsw->cpu_sds, cpu_sds_mode, active_sds_mode, gsw->cpu_port, cpu_speed,
					 cpu_ability.rxpause, cpu_ability.txpause);
		}

		dev_info(gsw->dev, "preserving boot switch configuration: skip reset and SDK cold init%s\n", gsw->reinit_cpu_serdes ? "; CPU SerDes selectively reinitialized" : " and SerDes programming");
		return 0;
	}

	// Sx PN swap:
	//	RX:
	//		page0 reg0 bit9:1
	//		page6 reg2 bit13:1
	//	TX:
	//		page0 reg0 bit8:1
	//		page6 reg2 bit14:1
	if (swap_cfg.sds0_rx_swap) {
		ret = rtl837x_init_sds_regbits(gsw, 0, 0, 0, 0x200, 1);
		if (ret)
			return ret;
		ret = rtl837x_init_sds_regbits(gsw, 0, 6, 2, 0x2000, 1);
		if (ret)
			return ret;
	}

	if (swap_cfg.sds0_tx_swap) {
		ret = rtl837x_init_sds_regbits(gsw, 0, 0, 0, BIT(8), 1);
		if (ret)
			return ret;
		ret = rtl837x_init_sds_regbits(gsw, 0, 6, 2, BIT(14), 1);
		if (ret)
			return ret;
	}

	if (swap_cfg.sds1_rx_swap) {
		ret = rtl837x_init_sds_regbits(gsw, 1, 0, 0, 0x200, 1);
		if (ret)
			return ret;
		ret = rtl837x_init_sds_regbits(gsw, 1, 6, 2, 0x2000, 1);
		if (ret)
			return ret;
	}

	if (swap_cfg.sds1_tx_swap) {
		ret = rtl837x_init_sds_regbits(gsw, 1, 0, 0, BIT(8), 1);
		if (ret)
			return ret;
		ret = rtl837x_init_sds_regbits(gsw, 1, 6, 2, BIT(14), 1);
		if (ret)
			return ret;
	}

	if (swap_cfg.phy_mdi_reverse) {
		ret = rtl837x_init_regbits(gsw, RTL8373_CFG_PHY_MDI_REVERSE_ADDR, 0xf, 0xc);
		if (ret)
			return ret;
	}

	if (swap_cfg.phy_tx_polarity_swap) {
		ret = rtl837x_init_regbits(gsw, RTL8373_CFG_PHY_TX_POLARITY_SWAP_ADDR, 0xffff, 0x596a);
		if (ret)
			return ret;
	}

	ret = rtk_switch_init();
	if (ret) {
		dev_err(gsw->dev, "rtk_switch_init Fail, error:%d\n", ret);
		return -EPERM;
	}
	ret = rtl837x_rtl8372n_led_init(gsw);
	if (ret) {
		dev_err(gsw->dev, "RTL8372N LED initialization failed, error:%d\n", ret);
		return ret;
	}

	ret = of_extra_init(gsw);
	if (ret)
		return ret;

	ret = rtk_vlan_reset();
	if (ret) {
		dev_err(gsw->dev, "rtk_vlan_reset failed, error:%d\n", ret);
		return -EPERM;
	}

	ret = rtk_vlan_init();
	if (ret) {
		dev_err(gsw->dev, "rtk_vlan_init failed, error:%d\n", ret);
		return -EPERM;
	}

	ret = rtl8372n_igmp_init(gsw);
	if (ret) {
		dev_err(gsw->dev, "rtl8372n_igmp_init failed, error:%d\n", ret);
		return -EPERM;
	}

	rtk_rmaParam_t pRmacfg;

	ret = rtk_rma_get(2, &pRmacfg);
	if (ret) {
		dev_err(gsw->dev, "rtk_rma_get get rma failed, error:%d\n", ret);
		return -EPERM;
	}

	pRmacfg.operation = RMAOP_FORWARD; // 清零配置
	ret = rtk_rma_set(2, &pRmacfg);
	if (ret) {
		dev_err(gsw->dev, "rtk_rma_get set rma failed, error:%d\n", ret);
		return -EPERM;
	}

	for (int port = 0; port < gsw->num_ports; port++) {
		ret = rtk_eee_portTxRxEn_set(PORT_MAPPED(port), 0u, 0u);
		if (ret) {
			dev_err(gsw->dev, "rtk_eee_portTxRxEn_set failed, error:%d\n", ret);
			return -EPERM;
		}
	}

	ret = rtk_sdsMode_set(0, SERDES_10GR);
	if (ret)
		return -EPERM;
	ret = rtk_sdsMode_set(0, gsw->sds0mode);
	if (ret)
		return -EPERM;

	ret = rtk_sdsMode_set(1, SERDES_10GR);
	if (ret)
		return -EPERM;
	ret = rtk_sdsMode_set(1, gsw->sds1mode);
	if (ret)
		return -EPERM;

	ret = rtk_cpu_externalCpuPort_set(gsw->cpu_port);
	if (ret) {
		dev_err(gsw->dev, "rtk_cpu_externalCpuPort_set failed, error:%d\n", ret);
		return -EPERM;
	}

	// TODO
	// res = rtl8372n_igrAcl_init();
	// if (res != RT_ERR_OK){
	//	dev_err(gsw->dev, "ACL init failed, ret=%d\n", res);
	//	return res;
	// }

	// TODO
	// res = rtl837x_acl_add_u(a1);
	// if (res != RT_ERR_OK){
	//	dev_err(gsw->dev, "rtl837x_acl_add failed, ret=%d\n", res);
	//	return res;
	// }

	return 0;
}

/* unused */
static void rtl837x_sfp_attach(void *upstream, struct sfp_bus *bus)
{
	struct rtk_gsw *gsw = upstream;

	dev_info(gsw->dev, "SFP module attach\n");
}

/* unused */
static void rtl837x_sfp_detach(void *upstream, struct sfp_bus *bus)
{
	struct rtk_gsw *gsw = upstream;

	dev_info(gsw->dev, "SFP module detach\n");
}

static int rtl837x_sfp_module_insert(void *upstream, const struct sfp_eeprom_id *id)
{
	struct rtk_gsw *gsw = upstream;
	phy_interface_t iface;
	rtk_sds_mode_t old_mode = gsw->sds1mode;
	rtk_sds_mode_t new_mode;
	int ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
	const struct sfp_module_caps *caps;

	caps = sfp_get_module_caps(gsw->sfp_bus);
	iface = sfp_select_interface(gsw->sfp_bus, caps->link_modes);
#else
	__ETHTOOL_DECLARE_LINK_MODE_MASK(support) = {
		0,
	};
	DECLARE_PHY_INTERFACE_MASK(interfaces);

	sfp_parse_support(gsw->sfp_bus, id, support, interfaces);
	iface = sfp_select_interface(gsw->sfp_bus, support);
#endif

	dev_info(gsw->dev, "%s SFP module inserted\n", phy_modes(iface));

	switch (iface) {
	case PHY_INTERFACE_MODE_10GBASER:
		USE_SERDESMODE(1, SERDES_10GR);
		break;
	case PHY_INTERFACE_MODE_2500BASEX:
		USE_SERDESMODE(1, SERDES_2500BASEX);
		break;
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_SGMII:
		USE_SERDESMODE(1, SERDES_1000BASEX);
		break;
	case PHY_INTERFACE_MODE_100BASEX:
		USE_SERDESMODE(1, SERDES_100FX);
		break;
	default:
		dev_err(gsw->dev, "Incompatible SFP module inserted\n");
		return -EINVAL;
	}

	new_mode = gsw->sds1mode;
	rtl837x_sdk_lock(gsw);
	ret = rtk_sdsMode_set(1, new_mode);
	if (ret)
		gsw->sds1mode = old_mode;
	rtl837x_sdk_unlock(gsw);
	if (ret) {
		dev_err(gsw->dev, "failed to configure SFP SerDes mode %d: %d\n", new_mode, ret);
		return -EIO;
	}

	return 0;
}

static void rtl837x_sfp_module_remove(void *upstream)
{
	struct rtk_gsw *gsw = upstream;
	rtk_sds_mode_t old_mode = gsw->sds1mode;
	int ret;

	dev_info(gsw->dev, "SFP module remove\n");

	USE_SERDESMODE(1, SERDES_OFF);
	rtl837x_sdk_lock(gsw);
	ret = rtk_sdsMode_set(1, gsw->sds1mode);
	if (ret)
		gsw->sds1mode = old_mode;
	rtl837x_sdk_unlock(gsw);
	if (ret)
		dev_warn(gsw->dev, "failed to disable SFP SerDes: %d\n", ret);
}

static const struct sfp_upstream_ops sfp_ops = {
	.attach = rtl837x_sfp_attach,
	.detach = rtl837x_sfp_detach,
	.module_insert = rtl837x_sfp_module_insert,
	.module_remove = rtl837x_sfp_module_remove,
	// .module_start = rtl837x_sfp_module_start,
	// .module_stop = rtl837x_sfp_module_stop,
	// .link_up = rtl837x_sfp_link_up,
	// .link_down = rtl837x_sfp_link_down,
	// .connect_phy = rtl837x_sfp_connect_phy,
	// .disconnect_phy = rtl837x_sfp_disconnect_phy,
};

static int rtl837x_sfp_probe(struct rtk_gsw *gsw)
{
	struct sfp_bus *bus;
	int ret;

	bus = sfp_bus_find_fwnode(gsw->dev->fwnode);
	if (IS_ERR(bus))
		return dev_err_probe(gsw->dev, PTR_ERR(bus), "unable to find SFP bus\n");

	ret = sfp_bus_add_upstream(bus, gsw, &sfp_ops);
	if (!ret)
		gsw->sfp_bus = bus;
	sfp_bus_put(bus);

	return ret;
}

static int rtl837x_of_get_dsa_cpu_port(struct device_node *np, u32 *port, struct device_node **ethernet)
{
	struct device_node *ports, *child;
	int ret;

	*ethernet = NULL;

	ports = of_get_child_by_name(np, "ports");
	if (!ports)
		ports = of_get_child_by_name(np, "ethernet-ports");
	if (!ports)
		return -ENOENT;

	for_each_available_child_of_node(ports, child) {
		struct device_node *eth;

		eth = of_parse_phandle(child, "ethernet", 0);
		if (!eth)
			continue;

		ret = of_property_read_u32(child, "reg", port);
		if (ret) {
			of_node_put(eth);
			of_node_put(child);
			of_node_put(ports);
			return ret;
		}

		*ethernet = eth;
		of_node_put(child);
		of_node_put(ports);
		return 0;
	}

	of_node_put(ports);
	return -ENOENT;
}

static int rtl837x_check_conduit_ready(struct device *dev, struct device_node *ethernet, struct net_device **master)
{
	int ret = 0;

	*master = NULL;
	if (!ethernet)
		return 0;

	rtnl_lock();
	*master = of_find_net_device_by_node(ethernet);
	if (!*master) {
		dev_dbg(dev, "defer before MDIO: conduit netdev is not registered\n");
		ret = -EPROBE_DEFER;
	} else if ((*master)->reg_state != NETREG_REGISTERED || !netif_device_present(*master)) {
		dev_dbg(dev, "defer before MDIO: conduit %s reg-state=%u present=%u\n", (*master)->name, (*master)->reg_state, netif_device_present(*master));
		dev_put(*master);
		*master = NULL;
		ret = -EPROBE_DEFER;
	}
	rtnl_unlock();

	return ret;
}

static int rtl837x_check_init_dependency(struct device *dev)
{
	struct device_node *supplier_np;
	struct mdio_device *supplier;
	struct rtk_gsw *supplier_gsw;
	bool ready;

	supplier_np = of_parse_phandle(dev->of_node, "realtek,init-after", 0);
	if (!supplier_np)
		return 0;

	supplier = of_mdio_find_device(supplier_np);
	of_node_put(supplier_np);
	if (!supplier)
		return -EPROBE_DEFER;

	supplier_gsw = dev_get_drvdata(&supplier->dev);
	ready = supplier_gsw && READ_ONCE(supplier_gsw->dsa_registered);
	if (ready)
		dev_info(dev, "initialization dependency ready: %s\n", dev_name(&supplier->dev));
	put_device(&supplier->dev);

	return ready ? 0 : -EPROBE_DEFER;
}

static int rtl837x_early_quarantine(struct mdio_device *mdiodev, u32 cpu_port, bool cpu_port_from_dsa)
{
	struct device *dev = &mdiodev->dev;
	struct regmap_config rc;
	struct rtk_gsw *gsw;
	u32 user_ports;
	int port;
	int ret;

	gsw = kzalloc(sizeof(*gsw), GFP_KERNEL);
	if (!gsw)
		return -ENOMEM;

	gsw->dev = dev;
	gsw->bus = mdiodev->bus;
	gsw->mdio_addr = mdiodev->addr;
	gsw->cpu_port = cpu_port;
	gsw->legacy_cpu_port = cpu_port;
	gsw->cpu_port_from_dsa = cpu_port_from_dsa;
	mutex_init(&gsw->map_lock);

	rc = rtl837x_mdio_regmap_config;
	rc.lock_arg = gsw;
	gsw->map = regmap_init(dev, NULL, gsw, &rc);
	if (IS_ERR(gsw->map)) {
		ret = PTR_ERR(gsw->map);
		goto out_free;
	}

	rtl837x_sdk_lock(gsw);
	ret = rtl837x_switch_probe(gsw);
	if (ret)
		goto out_unlock;
	ret = rtl837x_of_get_configured_port_mask(dev->of_node, gsw->valid_port_mask, &gsw->configured_port_mask);
	if (ret)
		goto out_unlock;

	ret = rtk_switch_attach();
	if (ret)
		goto out_unlock;

	if (!(gsw->configured_port_mask & BIT(gsw->cpu_port))) {
		ret = -EINVAL;
		goto out_unlock;
	}
	user_ports = gsw->configured_port_mask & ~BIT(gsw->cpu_port);
	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!(user_ports & BIT(port)))
			continue;

		ret = rtk_port_isolation_set(port, BIT(gsw->cpu_port));
		if (ret)
			goto out_unlock;
	}

	ret = rtk_port_isolation_set(gsw->cpu_port, user_ports);
out_unlock:
	rtl837x_sdk_unlock(gsw);
	if (!ret)
		dev_dbg(dev, "early quarantine before conduit: user->cpu=0x%03x cpu->users=0x%03x reads=%llu writes=%llu\n", (u32)BIT(gsw->cpu_port), user_ports, (unsigned long long)gsw->mdio_reads, (unsigned long long)gsw->mdio_writes);
	regmap_exit(gsw->map);
out_free:
	kfree(gsw);
	return ret > 0 ? -EIO : ret;
}

// below are platform driver
static const struct of_device_id rtk_gsw_match[] = {
	{ .compatible = "realtek,rtl837x" },
	{},
};

MODULE_DEVICE_TABLE(of, rtk_gsw_match);

static int rtl837x_dsa_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct device_node *np = dev->of_node;
	struct rtk_gsw *gsw;
	struct device_node *ethernet;
	struct net_device *master;
	const char *sdsmode_name;
	rtk_sds_mode_t sdsmode;
	struct regmap_config rc;
	u32 cpu_port;
	u32 mdc_rate = 0;
	bool cpu_port_from_dsa = false;

	int ret;

	dev_dbg(dev, "start rtl837x_dsa_probe");

	ret = rtl837x_of_get_dsa_cpu_port(np, &cpu_port, &ethernet);
	if (ret == -ENOENT) {
		ethernet = of_parse_phandle(np, "ethernet", 0);

		ret = of_property_read_u32(np, "rtl837x,cpu-port", &cpu_port);
		if (ret) {
			dev_err(dev, "failed to get DSA CPU port or legacy rtl837x,cpu-port\n");
			if (ethernet)
				of_node_put(ethernet);
			return -EINVAL;
		}
	} else if (ret) {
		dev_err(dev, "failed to parse DSA CPU port: %d\n", ret);
		return ret;
	} else {
		cpu_port_from_dsa = true;
	}

	/* Quarantine is deliberately performed before waiting for the PPE
	 * conduit. It only touches the already running switch and closes the
	 * bootloader forwarding window while the host MAC is still coming up.
	 */
	if (of_property_read_bool(np, "realtek,quarantine-before-conduit")) {
		ret = rtl837x_early_quarantine(mdiodev, cpu_port, cpu_port_from_dsa);
		if (ret) {
			if (ethernet)
				of_node_put(ethernet);
			return dev_err_probe(dev, ret, "early switch quarantine failed\n");
		}
	}

	ret = rtl837x_check_conduit_ready(dev, ethernet, &master);
	if (ret) {
		if (ethernet)
			of_node_put(ethernet);
		return ret;
	}

	if (ethernet) {
		of_node_put(ethernet);
	} else {
		dev_warn(dev, "node 'ethernet' is not set\n");
	}

	ret = rtl837x_check_init_dependency(dev);
	if (ret) {
		if (master)
			dev_put(master);
		return dev_err_probe(dev, ret, "waiting for switch initialization dependency\n");
	}

	gsw = devm_kzalloc(dev, sizeof(struct rtk_gsw), GFP_KERNEL);
	if (!gsw) {
		if (master)
			dev_put(master);
		return -ENOMEM;
	}

	mutex_init(&gsw->map_lock);

	rc = rtl837x_mdio_regmap_config;
	rc.lock_arg = gsw;
	gsw->map = devm_regmap_init(dev, NULL, gsw, &rc);
	if (IS_ERR(gsw->map)) {
		ret = PTR_ERR(gsw->map);
		dev_err(dev, "regmap init failed: %d\n", ret);
		if (master)
			dev_put(master);
		return ret;
	}

	gsw->dev = dev;
	gsw->bus = mdiodev->bus;
	gsw->ethernet_master = master;
	gsw->conduit_ready = !!master;
	if (master)
		strscpy(gsw->conduit_name, master->name, sizeof(gsw->conduit_name));
	gsw->sds0mode = SERDES_OFF;
	gsw->sds1mode = SERDES_OFF;
	gsw->cpu_port = cpu_port;
	gsw->legacy_cpu_port = cpu_port;
	gsw->cpu_port_from_dsa = cpu_port_from_dsa;
	gsw->preserve_boot_config = of_property_read_bool(np, "realtek,preserve-boot-config");
	gsw->init_rtl8372n_leds = of_property_read_bool(np, "realtek,rtl8372n-led-init");
	gsw->quarantine_before_conduit = of_property_read_bool(np, "realtek,quarantine-before-conduit");
	gsw->reinit_cpu_serdes = of_property_read_bool(np, "realtek,reinit-cpu-serdes");
	gsw->dsa_svlan = of_property_read_bool(np, "realtek,dsa-svlan");
	if (gsw->reinit_cpu_serdes && !gsw->preserve_boot_config) {
		dev_err(dev, "realtek,reinit-cpu-serdes requires realtek,preserve-boot-config\n");
		if (master)
			dev_put(master);
		return -EINVAL;
	}

	gsw->reset_pin = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(gsw->reset_pin)) {
		ret = dev_err_probe(dev, PTR_ERR(gsw->reset_pin), "failed to get reset GPIO\n");
		if (master)
			dev_put(master);
		return ret;
	}
	gsw->reset_assert_us = 100000;
	gsw->reset_deassert_us = 100000;
	of_property_read_u32(np, "reset-assert-us", &gsw->reset_assert_us);
	of_property_read_u32(np, "reset-deassert-us", &gsw->reset_deassert_us);

	if (!of_property_read_string(np, "rtl837x,sds0mode", &sdsmode_name) && !rtl837x_sdsmode(sdsmode_name, &sdsmode))
		gsw->sds0mode = sdsmode;

	if (!of_property_read_string(np, "rtl837x,sds1mode", &sdsmode_name) && !rtl837x_sdsmode(sdsmode_name, &sdsmode))
		gsw->sds1mode = sdsmode;

	memset(&(gsw->swap_cfg), 0, sizeof(rtl837x_pnswap_cfg_t));
	if (of_property_read_bool(np, "sds0-rx-swap"))
		gsw->swap_cfg.sds0_rx_swap = 1;
	if (of_property_read_bool(np, "sds0-tx-swap"))
		gsw->swap_cfg.sds0_tx_swap = 1;
	if (of_property_read_bool(np, "sds1-rx-swap"))
		gsw->swap_cfg.sds1_rx_swap = 1;
	if (of_property_read_bool(np, "sds1-tx-swap"))
		gsw->swap_cfg.sds1_tx_swap = 1;

	if (of_property_read_bool(np, "phy-mdi-reverse"))
		gsw->swap_cfg.phy_mdi_reverse = 1;
	if (of_property_read_bool(np, "phy-tx-polarity-swap"))
		gsw->swap_cfg.phy_tx_polarity_swap = 1;

	gsw->mdio_addr = mdiodev->addr;
	gsw->mib_counters = rtl837x_mib_counters;
	gsw->num_mib_counters = ARRAY_SIZE(rtl837x_mib_counters);
	if (mdiodev->bus->parent && mdiodev->bus->parent->of_node)
		of_property_read_u32(mdiodev->bus->parent->of_node, "clock-frequency", &mdc_rate);

	dev_info(gsw->dev,
		 "rtl837x dev info:smi-addr:%d requested-mdc:%u configured-cpu-port:%u sds0:%d sds1:%d swap_cfg:0x%x preserve-boot:%u led-init:%u quarantine:%u reinit-cpu-serdes:%u dsa-svlan:%u\n",
		 gsw->mdio_addr, mdc_rate, gsw->cpu_port, gsw->sds0mode,
		 gsw->sds1mode, *(uint8_t *)&gsw->swap_cfg,
		 gsw->preserve_boot_config, gsw->init_rtl8372n_leds,
		 gsw->quarantine_before_conduit, gsw->reinit_cpu_serdes,
		 gsw->dsa_svlan);

	rtl837x_sdk_lock(gsw);
	ret = rtl8372n_hw_init(gsw, gsw->swap_cfg);
	rtl837x_sdk_unlock(gsw);
	if (ret) {
		dev_err(gsw->dev, "probe diagnostics: conduit=%s ready=%u attempts=%u raw-id=0x%08x last-error=%d mdio-reads=%llu writes=%llu timeouts=%llu\n", gsw->conduit_name[0] ? gsw->conduit_name : "none", gsw->conduit_ready,
			gsw->probe_attempts, gsw->last_probe_id, gsw->last_probe_error, (unsigned long long)gsw->mdio_reads, (unsigned long long)gsw->mdio_writes, (unsigned long long)gsw->mdio_timeouts);
		dev_err(gsw->dev, "rtl8372n_hw_init failed, ret=%d\n", ret);
		if (master)
			dev_put(master);
		return -ENODEV;
	}

	dev_info(gsw->dev, "RTL chip %s initialized: DSA cpu-port:%u cpu-sds:%d valid-port-mask:0x%x configured-port-mask:0x%x\n", gsw->chip_name, gsw->cpu_port, gsw->cpu_sds, gsw->valid_port_mask, gsw->configured_port_mask);
	dev_info(gsw->dev, "serialized RTL8373-family SDK context selected for MDIO address %u\n", gsw->mdio_addr);

	ret = rtl837x_dsa_register(gsw);
	if (ret) {
		dev_err(gsw->dev, "rtl837x_dsa_register failed, ret=%d\n", ret);
		if (master)
			dev_put(master);
		return ret;
	}
	dev_set_drvdata(dev, gsw);

#ifdef CONFIG_GPIOLIB
	if (of_property_read_bool(np, "gpio-controller")) {
		ret = rtl837x_gpiochip_init(gsw);
		if (ret)
			goto err_dsa_unregister;
	}
#endif /* CONFIG_GPIOLIB */

	ret = rtl837x_sfp_probe(gsw);
	if (ret)
		goto err_dsa_unregister;

	rtl837x_debug_proc_init(gsw);
	return 0;

err_dsa_unregister:
	rtl837x_dsa_unregister(gsw);
	dev_set_drvdata(dev, NULL);
	if (master)
		dev_put(master);
	return ret;
}

static void rtl837x_dsa_remove(struct mdio_device *mdiodev)
{
	struct rtk_gsw *gsw = dev_get_drvdata(&mdiodev->dev);

	if (!gsw)
		return;

	if (gsw->sfp_bus)
		sfp_bus_del_upstream(gsw->sfp_bus);

	rtl837x_dsa_unregister(gsw);
	rtl837x_debug_proc_deinit(gsw);
	if (gsw->ethernet_master) {
		dev_put(gsw->ethernet_master);
		gsw->ethernet_master = NULL;
	}
	dev_set_drvdata(&mdiodev->dev, NULL);
}

static void rtl837x_mdio_shutdown(struct mdio_device *mdiodev)
{
	struct rtk_gsw *gsw = dev_get_drvdata(&mdiodev->dev);

	if (!gsw)
		return;

	rtl837x_dsa_shutdown(gsw);
	if (gsw->ethernet_master) {
		dev_put(gsw->ethernet_master);
		gsw->ethernet_master = NULL;
	}

	dev_set_drvdata(&mdiodev->dev, NULL);
}

static struct mdio_driver rtl837x_mdio_driver = {
	.mdiodrv.driver = {
		.name = "rtl837x-dsa",
		.of_match_table = rtk_gsw_match,
	},
	.probe  = rtl837x_dsa_probe,
	.remove = rtl837x_dsa_remove,
	.shutdown = rtl837x_mdio_shutdown,
};
mdio_module_driver(rtl837x_mdio_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("StarField Xu <air_jinkela@163.com>");
MODULE_DESCRIPTION("RTL837x DSA switch driver");
