/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 StarField Xu <air_jinkela@163.com>
 */

#include <linux/bitops.h>
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/kernel.h>
#include <linux/phylink.h>
#include <linux/phy.h>
#include <linux/string.h>
#include <linux/dsa/8021q.h>
#include <net/dsa.h>
#include <net/switchdev.h>

#include "./rtl837x_common.h"
#include "./rtk-api/l2.h"
#include "./rtk-api/dal/rtl8373/dal_rtl8373_port.h"
#include "./rtk-api/dal/rtl8373/dal_rtl8373_stp.h"
#include "./rtk-api/dal/rtl8373/dal_rtl8373_svlan.h"

static int rtl837x_to_errno(int ret)
{
	return ret == RT_ERR_OK ? 0 : -EIO;
}

static int rtl837x_mdio_setup(struct dsa_switch *ds);
static void rtl837x_mdio_teardown(struct dsa_switch *ds);

static DEFINE_MUTEX(rtl837x_phy_driver_lock);
static unsigned int rtl837x_phy_driver_users;

static bool rtl837x_valid_port(struct rtk_gsw *gsw, int port)
{
	return port >= 0 && port < RTK_MAX_NUM_OF_PORT && (gsw->configured_port_mask & BIT(port));
}

static bool rtl837x_user_port(struct rtk_gsw *gsw, int port)
{
	return rtl837x_valid_port(gsw, port) && port != gsw->cpu_port;
}

static u32 rtl837x_user_ports(struct rtk_gsw *gsw)
{
	return gsw->configured_port_mask & ~BIT(gsw->cpu_port);
}

static u32 rtl837x_bridge_ports(struct rtk_gsw *gsw, int port)
{
	struct net_device *bridge;
	struct dsa_port *dp;
	u32 members = 0;
	int other;

	dp = dsa_to_port(&gsw->ds, port);
	bridge = dsa_port_bridge_dev_get(dp);
	if (!bridge)
		return 0;

	for (other = 0; other < RTK_MAX_NUM_OF_PORT; other++) {
		if (other == port || !rtl837x_user_port(gsw, other))
			continue;

		dp = dsa_to_port(&gsw->ds, other);
		if (dsa_port_bridge_dev_get(dp) == bridge)
			members |= BIT(other);
	}

	return members;
}

static int rtl837x_update_isolation(struct rtk_gsw *gsw)
{
	u32 permit;
	int port, ret;

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!rtl837x_valid_port(gsw, port))
			continue;

		if (!gsw->dsa_svlan) {
			permit = gsw->configured_port_mask;
		} else if (port == gsw->cpu_port) {
			permit = rtl837x_user_ports(gsw);
		} else {
			permit = BIT(gsw->cpu_port) |
				 rtl837x_bridge_ports(gsw, port);
		}

		ret = rtk_port_isolation_set(port, permit);
		if (ret)
			return rtl837x_to_errno(ret);
	}

	return 0;
}

static int rtl837x_commit_pvid(struct rtk_gsw *gsw, int port)
{
	struct dsa_port *dp = dsa_to_port(&gsw->ds, port);
	bool valid = gsw->tag8021q_pvid_valid[port];
	u16 vid = gsw->tag8021q_pvid[port];
	int ret;

	if (gsw->dsa_svlan) {
		valid = true;
		vid = 1;
	}

	if (dsa_port_is_vlan_filtering(dp) && gsw->bridge_pvid_valid[port]) {
		vid = gsw->bridge_pvid[port];
		valid = true;
	}

	ret = rtk_vlan_portPvid_set(port, valid ? vid : 0);
	if (ret)
		return rtl837x_to_errno(ret);

	gsw->port_pvid[port] = valid ? vid : 0;
	return 0;
}

static int rtl837x_set_stp_state(struct rtk_gsw *gsw, int port, u8 state)
{
	u32 mstp_state;
	int ret;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	switch (state) {
	case BR_STATE_DISABLED:
		mstp_state = MSTP_DISABLE;
		break;
	case BR_STATE_BLOCKING:
	case BR_STATE_LISTENING:
		mstp_state = MSTP_BLOCKING;
		break;
	case BR_STATE_LEARNING:
		mstp_state = MSTP_LEARNING;
		break;
	case BR_STATE_FORWARDING:
		mstp_state = MSTP_FORWARDING;
		break;
	default:
		return -EINVAL;
	}

	ret = dal_rtl8373_asicMstpPortStatus_set(0, port, mstp_state);
	return rtl837x_to_errno(ret);
}

static u64 rtl837x_read_stat(int port, u32 counter)
{
	rtk_stat_counter_t value = 0;

	if (rtk_stat_port_get(port, counter, &value))
		return 0;

	return value;
}

static int rtl837x_write_vlan(u16 vid, u16 mbr, u16 untag)
{
	rtk_vlan_entry_t vlan = { 0 };
	int ret;

	vlan.mbr.bits[0] = mbr;
	vlan.untag.bits[0] = untag;
	vlan.fid_msti = 0;
	vlan.svlan_chk_ivl_svl = 0;
	vlan.ivl_svl = 1;

	ret = rtk_vlan_set(vid, &vlan);
	return rtl837x_to_errno(ret);
}

static bool rtl837x_vlan_has_user(struct rtk_gsw *gsw, u16 mbr)
{
	return mbr & rtl837x_user_ports(gsw);
}

static void rtl837x_rollback_vlan_pvid(struct rtk_gsw *gsw, int port, u16 vid,
				       const struct rtl837x_vlan_entry *old_vlan,
				       u16 *pvid_state, bool *pvid_valid,
				       u16 old_pvid, bool old_pvid_valid,
				       bool service)
{
	int ret;

	*pvid_state = old_pvid;
	*pvid_valid = old_pvid_valid;
	if (service && gsw->dsa_svlan) {
		u16 restore_vid = old_pvid_valid ? old_pvid : 0;

		ret = dal_rtl8373_svlanDfltSvlan_set(port, restore_vid);
		ret = rtl837x_to_errno(ret);
	} else {
		ret = rtl837x_commit_pvid(gsw, port);
	}
	if (ret)
		dev_err(gsw->dev, "failed to roll back port %d PVID state: %d\n", port, ret);

	ret = rtl837x_write_vlan(vid, old_vlan->mbr, old_vlan->untag);
	if (ret)
		dev_err(gsw->dev, "failed to roll back VLAN %u state: %d\n", vid, ret);
}

static int rtl837x_seed_vlan_table(struct rtk_gsw *gsw)
{
	struct rtl837x_vlan_entry vlan1 = {
		.mbr = gsw->configured_port_mask,
		.untag = gsw->configured_port_mask,
	};
	int port, ret;

	memset(gsw->vlan_table, 0, sizeof(gsw->vlan_table));

	ret = rtl837x_write_vlan(1, vlan1.mbr, vlan1.untag);
	if (ret)
		return ret;
	gsw->vlan_table[1] = vlan1;

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!rtl837x_valid_port(gsw, port))
			continue;

		ret = rtk_vlan_portPvid_set(port, 1);
		if (ret)
			return rtl837x_to_errno(ret);
		gsw->port_pvid[port] = 1;

		ret = rtk_vlan_portIgrFilterEnable_set(port, DISABLED);
		if (ret)
			return rtl837x_to_errno(ret);

		ret = rtk_vlan_portAcceptFrameType_set(port, ACCEPT_FRAME_TYPE_ALL);
		if (ret)
			return rtl837x_to_errno(ret);
	}

	return 0;
}

static enum dsa_tag_protocol rtl837x_get_tag_protocol(struct dsa_switch *ds, int port, enum dsa_tag_protocol mprot)
{
	struct rtk_gsw *gsw = ds->priv;

	if (gsw->dsa_svlan)
		return DSA_TAG_PROTO_RTL837X_8021AD;

	/* A standard VLAN header remains parseable by host checksum engines. */
	return DSA_TAG_PROTO_VSC73XX_8021Q;
}

static int __rtl837x_tag_8021q_vlan_add(struct dsa_switch *ds, int port, u16 vid, u16 flags)
{
	struct rtk_gsw *gsw = ds->priv;
	struct rtl837x_vlan_entry old_vlan, new_vlan;
	bool untagged = flags & BRIDGE_VLAN_INFO_UNTAGGED;
	bool pvid = flags & BRIDGE_VLAN_INFO_PVID;
	bool old_pvid_valid;
	u16 old_pvid;
	int ret;

	if (!rtl837x_valid_port(gsw, port) || !vid || vid > RTK_VID_MAX)
		return -EINVAL;
	old_vlan = gsw->vlan_table[vid];
	new_vlan = old_vlan;
	old_pvid = gsw->tag8021q_pvid[port];
	old_pvid_valid = gsw->tag8021q_pvid_valid[port];

	new_vlan.mbr |= BIT(port);

	if (untagged)
		new_vlan.untag |= BIT(port);
	else
		new_vlan.untag &= ~BIT(port);

	ret = rtl837x_write_vlan(vid, new_vlan.mbr, new_vlan.untag);
	if (ret)
		return ret;

	if (pvid) {
		gsw->tag8021q_pvid[port] = vid;
		gsw->tag8021q_pvid_valid[port] = true;
		if (gsw->dsa_svlan)
			ret = rtl837x_to_errno(dal_rtl8373_svlanDfltSvlan_set(port, vid));
		else
			ret = rtl837x_commit_pvid(gsw, port);
		if (ret) {
			rtl837x_rollback_vlan_pvid(gsw, port, vid, &old_vlan,
						   &gsw->tag8021q_pvid[port],
						   &gsw->tag8021q_pvid_valid[port],
						   old_pvid, old_pvid_valid, true);
			return ret;
		}

		dev_dbg(gsw->dev, "tag_8021q identity add: port=%d vid=%u svlan=%u mbr=0x%03x untag=0x%03x\n",
			port, vid, gsw->dsa_svlan, new_vlan.mbr, new_vlan.untag);
	}
	gsw->vlan_table[vid] = new_vlan;

	return 0;
}

static int rtl837x_tag_8021q_vlan_add(struct dsa_switch *ds, int port, u16 vid, u16 flags)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	rtl837x_sdk_lock(gsw);
	ret = __rtl837x_tag_8021q_vlan_add(ds, port, vid, flags);
	rtl837x_sdk_unlock(gsw);

	return ret;
}

static int __rtl837x_tag_8021q_vlan_del(struct dsa_switch *ds, int port, u16 vid)
{
	struct rtk_gsw *gsw = ds->priv;
	struct rtl837x_vlan_entry old_vlan, new_vlan;
	bool old_pvid_valid;
	u16 old_pvid;
	int ret;

	if (!rtl837x_valid_port(gsw, port) || !vid || vid > RTK_VID_MAX)
		return -EINVAL;
	old_vlan = gsw->vlan_table[vid];
	new_vlan = old_vlan;
	old_pvid = gsw->tag8021q_pvid[port];
	old_pvid_valid = gsw->tag8021q_pvid_valid[port];

	if (!old_vlan.mbr)
		return 0;

	new_vlan.mbr &= ~BIT(port);
	new_vlan.untag &= ~BIT(port);

	ret = rtl837x_write_vlan(vid, new_vlan.mbr, new_vlan.untag);
	if (ret)
		return ret;

	if (gsw->tag8021q_pvid_valid[port] && gsw->tag8021q_pvid[port] == vid) {
		gsw->tag8021q_pvid_valid[port] = false;
		if (gsw->dsa_svlan)
			ret = rtl837x_to_errno(dal_rtl8373_svlanDfltSvlan_set(port, 0));
		else
			ret = rtl837x_commit_pvid(gsw, port);
		if (ret) {
			rtl837x_rollback_vlan_pvid(gsw, port, vid, &old_vlan,
						   &gsw->tag8021q_pvid[port],
						   &gsw->tag8021q_pvid_valid[port],
						   old_pvid, old_pvid_valid, true);
			return ret;
		}

		dev_info(gsw->dev, "tag_8021q identity del: port=%d vid=%u svlan=%u\n",
			 port, vid, gsw->dsa_svlan);
	}
	gsw->vlan_table[vid] = new_vlan;

	return 0;
}

static int rtl837x_tag_8021q_vlan_del(struct dsa_switch *ds, int port, u16 vid)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	rtl837x_sdk_lock(gsw);
	ret = __rtl837x_tag_8021q_vlan_del(ds, port, vid);
	rtl837x_sdk_unlock(gsw);

	return ret;
}

static int rtl837x_setup_hardware(struct rtk_gsw *gsw)
{
	int port, ret;

	ret = rtk_cpu_externalCpuPort_set(gsw->cpu_port);
	if (ret)
		return rtl837x_to_errno(ret);

	/* Port identity is encoded in a standard 802.1Q tag, not 0x8899. */
	ret = rtk_cpuTag_enable_set(EXTERNAL_CPU, DISABLED);
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtk_l2_init();
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtk_l2_table_clear();
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtk_l2_aging_set(300);
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtk_stat_global_reset();
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtk_vlan_reset();
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtk_vlan_init();
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtk_vlan_egrFilterEnable_set(ENABLED);
	if (ret)
		return rtl837x_to_errno(ret);

	ret = rtl837x_seed_vlan_table(gsw);
	if (ret)
		return ret;

	if (gsw->dsa_svlan) {
		ret = dal_rtl8373_svlanInit();
		if (ret)
			return rtl837x_to_errno(ret);

		ret = dal_rtl8373_svlanServicePort_add(gsw->cpu_port);
		if (ret)
			return rtl837x_to_errno(ret);

		ret = dal_rtl8373_svlanUnassignAction_set(UNASSIGN_PBSVID);
		if (ret)
			return rtl837x_to_errno(ret);
	}

	memset(gsw->tag8021q_pvid, 0, sizeof(gsw->tag8021q_pvid));
	memset(gsw->tag8021q_pvid_valid, 0, sizeof(gsw->tag8021q_pvid_valid));
	memset(gsw->bridge_pvid, 0, sizeof(gsw->bridge_pvid));
	memset(gsw->bridge_pvid_valid, 0, sizeof(gsw->bridge_pvid_valid));
	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!rtl837x_valid_port(gsw, port))
			continue;

		ret = rtk_stat_port_reset(port);
		if (ret)
			return rtl837x_to_errno(ret);

		ret = rtl837x_set_stp_state(gsw, port, port == gsw->cpu_port ? BR_STATE_FORWARDING : BR_STATE_DISABLED);
		if (ret)
			return ret;
	}

	return 0;
}

static void rtl837x_log_forwarding_state(struct rtk_gsw *gsw)
{
	rtk_cpuTag_insertMode_t insert_mode = CPU_INSERT_END;
	rtk_enable_t cpu_tag = RTK_ENABLE_END;
	rtk_enable_t egr_filter = RTK_ENABLE_END;
	rtk_enable_t igr_filter[RTK_MAX_NUM_OF_PORT];
	rtk_vlan_entry_t vlan1 = { 0 };
	rtk_portmask_t aware = { 0 };
	rtk_vlan_t pvid[RTK_MAX_NUM_OF_PORT];
	u32 isolation[RTK_MAX_NUM_OF_PORT];
	u32 ext_cpu = U32_MAX;
	u32 tpid = U32_MAX;
	u32 svlan_tpid = U32_MAX;
	int port;

	memset(pvid, 0xff, sizeof(pvid));
	memset(igr_filter, 0xff, sizeof(igr_filter));
	memset(isolation, 0xff, sizeof(isolation));

	rtl837x_sdk_lock(gsw);
	rtk_cpu_externalCpuPort_get(&ext_cpu);
	rtk_cpuTag_tpid_get(&tpid);
	rtk_cpuTag_enable_get(EXTERNAL_CPU, &cpu_tag);
	rtk_cpuTag_insertMode_get(EXTERNAL_CPU, &insert_mode);
	rtk_cpuTag_awarePort_get(&aware);
	rtk_vlan_egrFilterEnable_get(&egr_filter);
	rtk_vlan_get(1, &vlan1);
	if (gsw->dsa_svlan)
		dal_rtl8373_svlanTpid_get(&svlan_tpid);

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!rtl837x_valid_port(gsw, port))
			continue;

		rtk_vlan_portPvid_get(port, &pvid[port]);
		rtk_vlan_portIgrFilterEnable_get(port, &igr_filter[port]);
		rtk_port_isolation_get(port, &isolation[port]);
	}
	rtl837x_sdk_unlock(gsw);

	dev_info(gsw->dev, "tag_8021q ready: ext-cpu=%u private-tag=%u insert=%u tpid=0x%04x dsa-svlan=%u svlan-tpid=0x%04x aware=0x%03x egr-filter=%u vlan1=0x%03x/0x%03x\n",
		 ext_cpu, cpu_tag, insert_mode, tpid, gsw->dsa_svlan, svlan_tpid,
		 aware.bits[0], egr_filter, vlan1.mbr.bits[0], vlan1.untag.bits[0]);

	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!rtl837x_valid_port(gsw, port))
			continue;

		dev_dbg(gsw->dev, "tag_8021q port state: port=%d role=%s pvid=%u tag-pvid=%u/%u bridge-pvid=%u/%u ingress-filter=%u isolation=0x%03x\n", port, port == gsw->cpu_port ? "cpu" : "user", pvid[port], gsw->tag8021q_pvid[port],
			 gsw->tag8021q_pvid_valid[port], gsw->bridge_pvid[port], gsw->bridge_pvid_valid[port], igr_filter[port], isolation[port]);
	}
}

static int rtl837x_setup(struct dsa_switch *ds)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	rtl837x_sdk_lock(gsw);
	ret = rtl837x_setup_hardware(gsw);
	rtl837x_sdk_unlock(gsw);
	if (ret)
		return ret;

	ret = rtl837x_mdio_setup(ds);
	if (ret)
		return ret;

	rtnl_lock();
	ret = dsa_tag_8021q_register(ds, htons(gsw->dsa_svlan ?
						      ETH_P_8021AD : ETH_P_8021Q));
	rtnl_unlock();
	if (ret) {
		rtl837x_mdio_teardown(ds);
		return ret;
	}

	rtl837x_sdk_lock(gsw);
	ret = rtl837x_update_isolation(gsw);
	rtl837x_sdk_unlock(gsw);
	if (ret) {
		rtnl_lock();
		dsa_tag_8021q_unregister(ds);
		rtnl_unlock();
		rtl837x_mdio_teardown(ds);
		return ret;
	}
	dev_info(gsw->dev, "DSA VLAN isolation initialized after tag setup: mode=%s configured=0x%03x\n",
		 gsw->dsa_svlan ? "bridge-matrix" : "vlan-membership",
		 gsw->configured_port_mask);

	rtl837x_log_forwarding_state(gsw);

	return 0;
}

static void rtl837x_teardown(struct dsa_switch *ds)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	rtnl_lock();
	dsa_tag_8021q_unregister(ds);
	rtnl_unlock();

	rtl837x_mdio_teardown(ds);
	rtl837x_sdk_lock(gsw);
	ret = rtk_cpuTag_enable_set(EXTERNAL_CPU, DISABLED);
	if (!ret && gsw->dsa_svlan)
		ret = dal_rtl8373_svlanServicePort_del(gsw->cpu_port);
	rtl837x_sdk_unlock(gsw);
	if (ret)
		dev_err(gsw->dev, "failed to disable CPU tag during teardown: %d\n", ret);
}

static int __rtl837x_mdio_read_c45(struct mii_bus *bus, int port, int devad, int regnum)
{
	struct rtk_gsw *gsw = bus->priv;
	u32 value = 0;
	int ret;

	if (!rtl837x_user_port(gsw, port))
		return -EOPNOTSUPP;

	ret = rtk_port_phyReg_get(port, devad, regnum, &value);
	/* MDIO bus probing treats negative reads as fatal bus errors. */
	if (ret)
		return 0xffff;

	return value & 0xffff;
}

static int rtl837x_mdio_read_c45(struct mii_bus *bus, int port, int devad, int regnum)
{
	struct rtk_gsw *gsw = bus->priv;
	int ret;

	rtl837x_sdk_lock(gsw);
	ret = __rtl837x_mdio_read_c45(bus, port, devad, regnum);
	rtl837x_sdk_unlock(gsw);

	return ret;
}

static int __rtl837x_mdio_write_c45(struct mii_bus *bus, int port, int devad, int regnum, u16 val)
{
	struct rtk_gsw *gsw = bus->priv;
	int ret;

	if (!rtl837x_user_port(gsw, port))
		return -EOPNOTSUPP;

	ret = rtk_port_phyReg_set(BIT(port), devad, regnum, val);
	return rtl837x_to_errno(ret);
}

static int rtl837x_mdio_write_c45(struct mii_bus *bus, int port, int devad, int regnum, u16 val)
{
	struct rtk_gsw *gsw = bus->priv;
	int ret;

	rtl837x_sdk_lock(gsw);
	ret = __rtl837x_mdio_write_c45(bus, port, devad, regnum, val);
	rtl837x_sdk_unlock(gsw);

	return ret;
}

static int rtl837x_phy_speed_to_ethtool(u32 speed)
{
	switch (speed) {
	case PORT_SPEED_10M:
		return SPEED_10;
	case PORT_SPEED_100M:
		return SPEED_100;
	case PORT_SPEED_1000M:
		return SPEED_1000;
	case PORT_SPEED_10G:
		return SPEED_10000;
	case PORT_SPEED_2500M:
		return SPEED_2500;
	case PORT_SPEED_5G:
		return SPEED_5000;
	default:
		return SPEED_UNKNOWN;
	}
}

static int rtl837x_phy_match(struct phy_device *phydev, const struct phy_driver *phydrv)
{
	struct mii_bus *bus = phydev->mdio.bus;
	struct rtk_gsw *gsw;

	if (!bus || bus->read_c45 != rtl837x_mdio_read_c45)
		return 0;

	gsw = bus->priv;
	return gsw && rtl837x_user_port(gsw, phydev->mdio.addr);
}

static int rtl837x_phy_probe(struct phy_device *phydev)
{
	phydev->is_internal = true;
	phydev->port = PORT_TP;

	return 0;
}

static int rtl837x_phy_get_features(struct phy_device *phydev)
{
	linkmode_zero(phydev->supported);

	linkmode_set_bit(ETHTOOL_LINK_MODE_TP_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_MII_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Autoneg_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Half_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Pause_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT, phydev->supported);

	return 0;
}

static int __rtl837x_phy_read_status(struct phy_device *phydev)
{
	rtk_port_status_t status = { 0 };
	struct rtk_gsw *gsw = phydev->mdio.bus->priv;
	int ret;

	gsw->phy_status_count++;

	ret = rtk_port_macStatus_get(phydev->mdio.addr, &status);
	if (ret)
		return rtl837x_to_errno(ret);

	phydev->link = !!status.link;
	phydev->autoneg_complete = phydev->link;

	if (!phydev->link) {
		phydev->speed = SPEED_UNKNOWN;
		phydev->duplex = DUPLEX_UNKNOWN;
		return 0;
	}

	phydev->speed = rtl837x_phy_speed_to_ethtool(status.speed);
	phydev->duplex = status.duplex ? DUPLEX_FULL : DUPLEX_HALF;

	linkmode_mod_bit(ETHTOOL_LINK_MODE_Pause_BIT, phydev->lp_advertising, status.rxpause && status.txpause);
	linkmode_mod_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT, phydev->lp_advertising, status.rxpause != status.txpause);

	return 0;
}

static int rtl837x_phy_read_status(struct phy_device *phydev)
{
	struct rtk_gsw *gsw = phydev->mdio.bus->priv;
	int ret;

	rtl837x_sdk_lock(gsw);
	ret = __rtl837x_phy_read_status(phydev);
	rtl837x_sdk_unlock(gsw);

	return ret;
}

static struct phy_driver rtl837x_phy_driver = {
	.name = "RTL837x internal PHY",
	.match_phy_device = rtl837x_phy_match,
	.probe = rtl837x_phy_probe,
	.get_features = rtl837x_phy_get_features,
	.read_status = rtl837x_phy_read_status,
};

static int rtl837x_phy_driver_get(void)
{
	int ret = 0;

	mutex_lock(&rtl837x_phy_driver_lock);

	if (!rtl837x_phy_driver_users) {
		ret = phy_drivers_register(&rtl837x_phy_driver, 1, THIS_MODULE);
		if (ret)
			goto out;
	}

	rtl837x_phy_driver_users++;

out:
	mutex_unlock(&rtl837x_phy_driver_lock);
	return ret;
}

static void rtl837x_phy_driver_put(void)
{
	mutex_lock(&rtl837x_phy_driver_lock);

	if (rtl837x_phy_driver_users && !--rtl837x_phy_driver_users)
		phy_drivers_unregister(&rtl837x_phy_driver, 1);

	mutex_unlock(&rtl837x_phy_driver_lock);
}

static int rtl837x_mdio_setup(struct dsa_switch *ds)
{
	struct rtk_gsw *gsw = ds->priv;
	struct mii_bus *bus;
	int ret;

	ret = rtl837x_phy_driver_get();
	if (ret)
		return ret;

	bus = mdiobus_alloc();
	if (!bus) {
		rtl837x_phy_driver_put();
		return -ENOMEM;
	}

	ds->user_mii_bus = bus;
	bus->priv = gsw;
	bus->name = "rtl837x slave mii";
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s-mii", dev_name(gsw->dev));
	bus->read_c45 = rtl837x_mdio_read_c45;
	bus->write_c45 = rtl837x_mdio_write_c45;
	bus->parent = gsw->dev;
	bus->phy_mask = ~ds->phys_mii_mask;

	ret = mdiobus_register(bus);
	if (ret) {
		dev_err(gsw->dev, "failed to register slave MDIO bus: %d\n", ret);
		mdiobus_free(bus);
		ds->user_mii_bus = NULL;
		rtl837x_phy_driver_put();
		return ret;
	}

	return 0;
}

static void rtl837x_mdio_teardown(struct dsa_switch *ds)
{
	if (!ds->user_mii_bus)
		return;

	mdiobus_unregister(ds->user_mii_bus);
	mdiobus_free(ds->user_mii_bus);
	ds->user_mii_bus = NULL;
	rtl837x_phy_driver_put();
}

static void rtl837x_phylink_get_caps(struct dsa_switch *ds, int port, struct phylink_config *config)
{
	struct rtk_gsw *gsw = ds->priv;

	if (!rtl837x_valid_port(gsw, port))
		return;

	config->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE | MAC_10 | MAC_100 | MAC_1000FD | MAC_2500FD | MAC_5000FD | MAC_10000FD;

	__set_bit(PHY_INTERFACE_MODE_INTERNAL, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_GMII, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_SGMII, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_100BASEX, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_1000BASEX, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_2500BASEX, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_5GBASER, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_10GBASER, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_10GKR, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_USXGMII, config->supported_interfaces);
}

static void rtl837x_get_strings(struct dsa_switch *ds, int port, u32 stringset, uint8_t *data)
{
	struct rtk_gsw *gsw = ds->priv;
	int i;

	if (stringset != ETH_SS_STATS || !rtl837x_valid_port(gsw, port))
		return;

	for (i = 0; i < gsw->num_mib_counters; i++)
		strscpy(data + i * ETH_GSTRING_LEN, gsw->mib_counters[i].name, ETH_GSTRING_LEN);
}

static void __rtl837x_get_ethtool_stats(struct dsa_switch *ds, int port, uint64_t *data)
{
	struct rtk_gsw *gsw = ds->priv;
	int i;

	if (!rtl837x_valid_port(gsw, port))
		return;

	for (i = 0; i < gsw->num_mib_counters; i++)
		data[i] = rtl837x_read_stat(port, gsw->mib_counters[i].base);
}

static void rtl837x_get_ethtool_stats(struct dsa_switch *ds, int port, uint64_t *data)
{
	struct rtk_gsw *gsw = ds->priv;

	rtl837x_sdk_lock(gsw);
	__rtl837x_get_ethtool_stats(ds, port, data);
	rtl837x_sdk_unlock(gsw);
}

static int rtl837x_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	struct rtk_gsw *gsw = ds->priv;

	if (sset != ETH_SS_STATS)
		return 0;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	return gsw->num_mib_counters;
}

static void __rtl837x_get_pause_stats(struct dsa_switch *ds, int port, struct ethtool_pause_stats *pause_stats)
{
	struct rtk_gsw *gsw = ds->priv;

	if (!rtl837x_valid_port(gsw, port))
		return;

	pause_stats->rx_pause_frames = rtl837x_read_stat(port, dot3InPauseFrames);
	pause_stats->tx_pause_frames = rtl837x_read_stat(port, dot3OutPauseFrames);
}

static void rtl837x_get_pause_stats(struct dsa_switch *ds, int port, struct ethtool_pause_stats *pause_stats)
{
	struct rtk_gsw *gsw = ds->priv;

	rtl837x_sdk_lock(gsw);
	__rtl837x_get_pause_stats(ds, port, pause_stats);
	rtl837x_sdk_unlock(gsw);
}

static int rtl837x_set_ageing_time(struct dsa_switch *ds, unsigned int msecs)
{
	struct rtk_gsw *gsw = ds->priv;
	unsigned int secs = DIV_ROUND_UP(msecs, 1000);
	int ret;

	secs = clamp_t(unsigned int, secs, 14, 800);
	rtl837x_sdk_lock(gsw);
	ret = rtl837x_to_errno(rtk_l2_aging_set(secs));
	rtl837x_sdk_unlock(gsw);

	return ret;
}

static int __rtl837x_port_enable(struct dsa_switch *ds, int port, struct phy_device *phy)
{
	struct rtk_gsw *gsw = ds->priv;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	gsw->port_enable_count++;

	return rtl837x_set_stp_state(gsw, port, BR_STATE_FORWARDING);
}

static int rtl837x_port_enable(struct dsa_switch *ds, int port, struct phy_device *phy)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	rtl837x_sdk_lock(gsw);
	ret = __rtl837x_port_enable(ds, port, phy);
	rtl837x_sdk_unlock(gsw);

	return ret;
}

static void __rtl837x_port_disable(struct dsa_switch *ds, int port)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	if (!rtl837x_valid_port(gsw, port))
		return;

	gsw->port_disable_count++;
	ret = rtl837x_set_stp_state(gsw, port, BR_STATE_DISABLED);
	if (ret)
		dev_err(gsw->dev, "failed to disable port %d: %d\n", port, ret);
}

static void rtl837x_port_disable(struct dsa_switch *ds, int port)
{
	struct rtk_gsw *gsw = ds->priv;

	rtl837x_sdk_lock(gsw);
	__rtl837x_port_disable(ds, port);
	rtl837x_sdk_unlock(gsw);
}

static void __rtl837x_port_stp_state_set(struct dsa_switch *ds, int port, u8 state)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	if (!rtl837x_valid_port(gsw, port))
		return;

	ret = rtl837x_set_stp_state(gsw, port, state);
	if (ret)
		dev_err(gsw->dev, "failed to set STP state %u on port %d: %d\n",
			state, port, ret);
}

static void rtl837x_port_stp_state_set(struct dsa_switch *ds, int port, u8 state)
{
	struct rtk_gsw *gsw = ds->priv;

	rtl837x_sdk_lock(gsw);
	__rtl837x_port_stp_state_set(ds, port, state);
	rtl837x_sdk_unlock(gsw);
}

static void __rtl837x_port_fast_age(struct dsa_switch *ds, int port)
{
	struct rtk_gsw *gsw = ds->priv;
	rtk_l2_flushCfg_t cfg = { 0 };
	int ret;

	if (!rtl837x_user_port(gsw, port))
		return;

	cfg.flushByPort = ENABLED;
	cfg.portmask = BIT(port);
	cfg.flushStaticAddr = DISABLED;
	cfg.flushAddrOnAllPorts = DISABLED;

	ret = rtk_l2_ucastAddr_flush(&cfg);
	if (ret)
		dev_err(gsw->dev, "failed to flush FDB for port %d: %d\n", port,
			ret);
}

static void rtl837x_port_fast_age(struct dsa_switch *ds, int port)
{
	struct rtk_gsw *gsw = ds->priv;

	rtl837x_sdk_lock(gsw);
	__rtl837x_port_fast_age(ds, port);
	rtl837x_sdk_unlock(gsw);
}

static int __rtl837x_port_change_mtu(struct dsa_switch *ds, int port, int new_mtu)
{
	struct rtk_gsw *gsw = ds->priv;
	int tag_overhead = gsw->dsa_svlan ? 2 * VLAN_HLEN : VLAN_HLEN;
	int frame_size = new_mtu + ETH_HLEN + tag_overhead + ETH_FCS_LEN;
	int ret;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	if (frame_size > RTK_SWITCH_MAX_PKTLEN)
		return -ERANGE;

	ret = dal_rtl8373_portMaxLen_set(port, 0, frame_size);
	if (ret)
		return rtl837x_to_errno(ret);

	ret = dal_rtl8373_portMaxLen_set(port, 1, frame_size);
	if (ret)
		return rtl837x_to_errno(ret);

	ret = dal_rtl8373_portMaxLenIncTag_set(port, ENABLED);
	if (ret)
		return rtl837x_to_errno(ret);

	if (port == gsw->cpu_port)
		dev_info(gsw->dev, "DSA MTU configured: cpu-port=%d user-mtu=%d conduit-mtu=%d frame=%d tag-overhead=%d\n",
			 port, new_mtu,
			 gsw->ethernet_master ? gsw->ethernet_master->mtu : -1,
			 frame_size, tag_overhead);

	return 0;
}

static int rtl837x_port_change_mtu(struct dsa_switch *ds, int port, int new_mtu)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	rtl837x_sdk_lock(gsw);
	ret = __rtl837x_port_change_mtu(ds, port, new_mtu);
	rtl837x_sdk_unlock(gsw);

	return ret;
}

static int rtl837x_port_max_mtu(struct dsa_switch *ds, int port)
{
	struct rtk_gsw *gsw = ds->priv;
	int tag_overhead = gsw->dsa_svlan ? 2 * VLAN_HLEN : VLAN_HLEN;

	return RTK_SWITCH_MAX_PKTLEN - ETH_HLEN - tag_overhead - ETH_FCS_LEN;
}

static int rtl837x_port_bridge_join(struct dsa_switch *ds, int port, struct dsa_bridge bridge, bool *tx_fwd_offload, struct netlink_ext_ack *extack)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	if (!rtl837x_user_port(gsw, port))
		return -EINVAL;
	if (gsw->dsa_svlan) {
		rtl837x_sdk_lock(gsw);
		ret = rtl837x_update_isolation(gsw);
		rtl837x_sdk_unlock(gsw);
		if (ret)
			return ret;

		*tx_fwd_offload = true;
		dev_info(gsw->dev,
			 "SVLAN bridge join: port=%d bridge=%s source-svid=%u\n",
			 port, bridge.dev->name, gsw->tag8021q_pvid[port]);
		return 0;
	}

	ret = dsa_tag_8021q_bridge_join(ds, port, bridge, tx_fwd_offload, extack);
	if (ret)
		return ret;

	dev_info(gsw->dev, "tag_8021q bridge join: port=%d bridge=%s tx-fwd-offload=%u pvid=%u\n", port, bridge.dev->name, *tx_fwd_offload, gsw->tag8021q_pvid[port]);

	return 0;
}

static void rtl837x_port_bridge_leave(struct dsa_switch *ds, int port, struct dsa_bridge bridge)
{
	struct rtk_gsw *gsw = ds->priv;

	if (!rtl837x_user_port(gsw, port))
		return;
	if (gsw->dsa_svlan) {
		int ret;

		rtl837x_sdk_lock(gsw);
		ret = rtl837x_update_isolation(gsw);
		rtl837x_sdk_unlock(gsw);
		if (ret)
			dev_err(gsw->dev,
				"failed to update isolation after bridge leave on port %d: %d\n",
				port, ret);
		return;
	}

	dsa_tag_8021q_bridge_leave(ds, port, bridge);

	dev_info(gsw->dev, "tag_8021q bridge leave: port=%d bridge=%s pvid=%u\n", port, bridge.dev->name, gsw->tag8021q_pvid[port]);
}

static int __rtl837x_port_vlan_filtering(struct dsa_switch *ds, int port, bool vlan_filtering, struct netlink_ext_ack *extack)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	if (!rtl837x_user_port(gsw, port))
		return 0;

	ret = rtk_vlan_portIgrFilterEnable_set(port, vlan_filtering ? ENABLED : DISABLED);
	if (ret)
		return rtl837x_to_errno(ret);

	return rtl837x_commit_pvid(gsw, port);
}

static int rtl837x_port_vlan_filtering(struct dsa_switch *ds, int port, bool vlan_filtering, struct netlink_ext_ack *extack)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	rtl837x_sdk_lock(gsw);
	ret = __rtl837x_port_vlan_filtering(ds, port, vlan_filtering, extack);
	rtl837x_sdk_unlock(gsw);

	return ret;
}

static int __rtl837x_port_vlan_add(struct dsa_switch *ds, int port, const struct switchdev_obj_port_vlan *vlan, struct netlink_ext_ack *extack)
{
	struct rtk_gsw *gsw = ds->priv;
	struct rtl837x_vlan_entry old_vlan, new_vlan;
	bool untagged = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;
	bool pvid = vlan->flags & BRIDGE_VLAN_INFO_PVID;
	bool old_pvid_valid;
	u16 vid = vlan->vid;
	u16 old_pvid;
	int ret;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	if (!vid)
		return 0;

	if (vid > RTK_VID_MAX) {
		NL_SET_ERR_MSG_MOD(extack, "VLAN ID out of range");
		return -EINVAL;
	}
	old_vlan = gsw->vlan_table[vid];
	new_vlan = old_vlan;
	old_pvid = gsw->bridge_pvid[port];
	old_pvid_valid = gsw->bridge_pvid_valid[port];

	new_vlan.mbr |= BIT(port);
	new_vlan.untag &= ~BIT(port);

	if (untagged)
		new_vlan.untag |= BIT(port);

	if (port != gsw->cpu_port) {
		new_vlan.mbr |= BIT(gsw->cpu_port);
		new_vlan.untag &= ~BIT(gsw->cpu_port);
	}

	ret = rtl837x_write_vlan(vid, new_vlan.mbr, new_vlan.untag);
	if (ret) {
		NL_SET_ERR_MSG_MOD(extack, "failed to program VLAN");
		return ret;
	}

	if (pvid && port != gsw->cpu_port) {
		gsw->bridge_pvid[port] = vid;
		gsw->bridge_pvid_valid[port] = true;
		ret = rtl837x_commit_pvid(gsw, port);
		if (ret) {
			NL_SET_ERR_MSG_MOD(extack, "failed to program VLAN PVID");
			rtl837x_rollback_vlan_pvid(gsw, port, vid, &old_vlan,
						   &gsw->bridge_pvid[port],
						   &gsw->bridge_pvid_valid[port],
						   old_pvid, old_pvid_valid, false);
			return ret;
		}
	}
	gsw->vlan_table[vid] = new_vlan;

	return 0;
}

static int rtl837x_port_vlan_add(struct dsa_switch *ds, int port, const struct switchdev_obj_port_vlan *vlan, struct netlink_ext_ack *extack)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	rtl837x_sdk_lock(gsw);
	ret = __rtl837x_port_vlan_add(ds, port, vlan, extack);
	rtl837x_sdk_unlock(gsw);

	return ret;
}

static int __rtl837x_port_vlan_del(struct dsa_switch *ds, int port, const struct switchdev_obj_port_vlan *vlan)
{
	struct rtk_gsw *gsw = ds->priv;
	struct rtl837x_vlan_entry old_vlan, new_vlan;
	bool old_pvid_valid;
	u16 vid = vlan->vid;
	u16 old_pvid;
	int ret;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	if (!vid || vid > RTK_VID_MAX)
		return 0;
	old_vlan = gsw->vlan_table[vid];
	if (!old_vlan.mbr)
		return 0;
	new_vlan = old_vlan;
	old_pvid = gsw->bridge_pvid[port];
	old_pvid_valid = gsw->bridge_pvid_valid[port];

	new_vlan.mbr &= ~BIT(port);
	new_vlan.untag &= ~BIT(port);

	if (!rtl837x_vlan_has_user(gsw, new_vlan.mbr)) {
		new_vlan.mbr &= ~BIT(gsw->cpu_port);
		new_vlan.untag &= ~BIT(gsw->cpu_port);
	}

	ret = rtl837x_write_vlan(vid, new_vlan.mbr, new_vlan.untag);
	if (ret)
		return ret;

	if (port != gsw->cpu_port && gsw->bridge_pvid_valid[port] && gsw->bridge_pvid[port] == vid) {
		gsw->bridge_pvid_valid[port] = false;
		ret = rtl837x_commit_pvid(gsw, port);
		if (ret) {
			rtl837x_rollback_vlan_pvid(gsw, port, vid, &old_vlan,
						   &gsw->bridge_pvid[port],
						   &gsw->bridge_pvid_valid[port],
						   old_pvid, old_pvid_valid, false);
			return ret;
		}
	}
	gsw->vlan_table[vid] = new_vlan;

	return 0;
}

static int rtl837x_port_vlan_del(struct dsa_switch *ds, int port, const struct switchdev_obj_port_vlan *vlan)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	rtl837x_sdk_lock(gsw);
	ret = __rtl837x_port_vlan_del(ds, port, vlan);
	rtl837x_sdk_unlock(gsw);

	return ret;
}

static int rtl837x_fdb_isolation_vid(struct rtk_gsw *gsw, u16 *vid,
				     struct dsa_db db)
{
	if (*vid)
		return 0;
	if (gsw->dsa_svlan) {
		*vid = 1;
		return 0;
	}

	switch (db.type) {
	case DSA_DB_PORT:
		*vid = dsa_tag_8021q_standalone_vid(db.dp);
		break;
	case DSA_DB_BRIDGE:
		*vid = dsa_tag_8021q_bridge_vid(db.bridge.num);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int __rtl837x_port_fdb_add(struct dsa_switch *ds, int port, const unsigned char *addr, u16 vid, struct dsa_db db)
{
	struct rtk_gsw *gsw = ds->priv;
	rtk_l2_ucastAddr_t l2 = { 0 };
	rtk_mac_t mac = { 0 };
	int ret;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	/* Host-bound unknown unicast is already flooded to the CPU port. */
	if (gsw->chip_id == CHIP_RTL8372N && port == gsw->cpu_port)
		return 0;
	ret = rtl837x_fdb_isolation_vid(gsw, &vid, db);
	if (ret)
		return ret;

	memcpy(mac.octet, addr, ETH_ALEN);
	memcpy(l2.mac.octet, addr, ETH_ALEN);
	l2.ivl = 1;
	l2.vid_fid = vid;
	l2.port = port;
	l2.auth = 1;
	l2.is_static = 1;

	ret = rtk_l2_addr_add(&mac, &l2);
	return rtl837x_to_errno(ret);
}

static int rtl837x_port_fdb_add(struct dsa_switch *ds, int port, const unsigned char *addr, u16 vid, struct dsa_db db)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	rtl837x_sdk_lock(gsw);
	ret = __rtl837x_port_fdb_add(ds, port, addr, vid, db);
	rtl837x_sdk_unlock(gsw);

	return ret;
}

static int __rtl837x_port_fdb_del(struct dsa_switch *ds, int port, const unsigned char *addr, u16 vid, struct dsa_db db)
{
	struct rtk_gsw *gsw = ds->priv;
	rtk_l2_ucastAddr_t l2 = { 0 };
	rtk_mac_t mac = { 0 };
	int ret;

	if (!rtl837x_valid_port(gsw, port))
		return -EINVAL;

	if (gsw->chip_id == CHIP_RTL8372N && port == gsw->cpu_port)
		return 0;
	ret = rtl837x_fdb_isolation_vid(gsw, &vid, db);
	if (ret)
		return ret;

	memcpy(mac.octet, addr, ETH_ALEN);
	memcpy(l2.mac.octet, addr, ETH_ALEN);
	l2.ivl = 1;
	l2.vid_fid = vid;
	l2.port = port;
	l2.is_static = 1;

	ret = rtk_l2_addr_del(&mac, &l2);
	if (ret == RT_ERR_L2_ENTRY_NOTFOUND)
		return 0;

	return rtl837x_to_errno(ret);
}

static int rtl837x_port_fdb_del(struct dsa_switch *ds, int port, const unsigned char *addr, u16 vid, struct dsa_db db)
{
	struct rtk_gsw *gsw = ds->priv;
	int ret;

	rtl837x_sdk_lock(gsw);
	ret = __rtl837x_port_fdb_del(ds, port, addr, vid, db);
	rtl837x_sdk_unlock(gsw);

	return ret;
}

static const struct dsa_switch_ops rtl837x_dsa_ops = {
	.get_tag_protocol = rtl837x_get_tag_protocol,
	.setup = rtl837x_setup,
	.teardown = rtl837x_teardown,
	.phylink_get_caps = rtl837x_phylink_get_caps,
	.get_strings = rtl837x_get_strings,
	.get_ethtool_stats = rtl837x_get_ethtool_stats,
	.get_sset_count = rtl837x_get_sset_count,
	.get_pause_stats = rtl837x_get_pause_stats,
	.set_ageing_time = rtl837x_set_ageing_time,
	.port_enable = rtl837x_port_enable,
	.port_disable = rtl837x_port_disable,
	.port_bridge_join = rtl837x_port_bridge_join,
	.port_bridge_leave = rtl837x_port_bridge_leave,
	.port_stp_state_set = rtl837x_port_stp_state_set,
	.port_fast_age = rtl837x_port_fast_age,
	.port_change_mtu = rtl837x_port_change_mtu,
	.port_max_mtu = rtl837x_port_max_mtu,
	.port_vlan_filtering = rtl837x_port_vlan_filtering,
	.port_vlan_add = rtl837x_port_vlan_add,
	.port_vlan_del = rtl837x_port_vlan_del,
	.port_fdb_add = rtl837x_port_fdb_add,
	.port_fdb_del = rtl837x_port_fdb_del,
	.tag_8021q_vlan_add = rtl837x_tag_8021q_vlan_add,
	.tag_8021q_vlan_del = rtl837x_tag_8021q_vlan_del,
};

int rtl837x_dsa_register(struct rtk_gsw *gsw)
{
	struct dsa_switch *ds = &gsw->ds;
	int ret;

	ds->dev = gsw->dev;
	ds->priv = gsw;
	ds->ops = &rtl837x_dsa_ops;
	ds->num_ports = gsw->dsa_num_ports;
	ds->phys_mii_mask = rtl837x_user_ports(gsw);
	ds->configure_vlan_while_not_filtering = true;
	ds->untag_bridge_pvid = true;
	ds->fdb_isolation = true;
	ds->max_num_bridges = DSA_TAG_8021Q_MAX_NUM_BRIDGES;
	ds->ageing_time_min = 14000;
	ds->ageing_time_max = 800000;

	ret = dsa_register_switch(ds);
	if (ret)
		return ret;

	gsw->dsa_registered = true;
	return 0;
}

void rtl837x_dsa_unregister(struct rtk_gsw *gsw)
{
	if (!gsw->dsa_registered)
		return;

	dsa_unregister_switch(&gsw->ds);
	gsw->dsa_registered = false;
}

void rtl837x_dsa_shutdown(struct rtk_gsw *gsw)
{
	if (!gsw->dsa_registered)
		return;

	dsa_switch_shutdown(&gsw->ds);
	gsw->dsa_registered = false;
}
