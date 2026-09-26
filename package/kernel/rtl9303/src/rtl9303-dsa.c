// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL9303 with an external CPU attached to a front-panel MAC, not port 28.
 * Register layouts follow OpenWrt's rtl83xx/rtl930x.c and the OEM register
 * descriptions. Management travels over the parent SPI regmap.
 *
 * RX service VLANs preserve ingress identity while sharing a bridge FID.
 * Separate, non-learning TX VLANs deliver CPU packets to exactly one port.
 * Multicast and VLAN-aware bridges are forwarded by the Linux bridge.
 */
#include <linux/bitfield.h>
#include <linux/if_bridge.h>
#include <linux/etherdevice.h>
#include <linux/unaligned.h>
#include <linux/if_vlan.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/pcs/pcs.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <net/dsa.h>

#define RTL9303_PORTS		28
#define RTL9303_VID_BASE		0xe00
#define RTL9303_TX_VID_BASE	0xf00
#define RTL9303_PROFILE		7
#define RTL9303_TX_PROFILE	6
#define RTL9303_FID_BASE	48
#define RTL9303_BRIDGES		3
#define RTL9303_L2_ENTRIES	16384
#define RTL9303_BR_FLAGS		(BR_LEARNING | BR_FLOOD | BR_BCAST_FLOOD | \
				 BR_MCAST_FLOOD | BR_ISOLATED)
#define RTL9303_TPID_SLOT		3
#define RTL9303_MAC_CTRL(p)	(0x3268 + (p) * 64)
#define RTL9303_MAC_MAX_LEN(p)	(0x326c + (p) * 64)
#define RTL9303_MAC_FORCE(p)	(0xca1c + (p) * 4)
#define RTL9303_MAC_RX_TX		GENMASK(1, 0)
#define RTL9303_FORCE_EN		BIT(0)
#define RTL9303_FORCE_LINK	BIT(1)
#define RTL9303_DUPLEX		BIT(2)
#define RTL9303_SPEED		GENMASK(6, 3)
#define RTL9303_TX_PAUSE		BIT(7)
#define RTL9303_RX_PAUSE		BIT(8)
#define RTL9303_FORCE_FC		BIT(9)
#define RTL9303_MAX_FRAME		12288

struct rtl9303_port {
	u8 bridge;
	u8 stp;
	unsigned long flags;
};

struct rtl9303 {
	struct dsa_switch ds;
	struct regmap *map;
	/* Serializes the shared table command/data registers. */
	struct mutex table_lock;
	struct rtl9303_port ports[RTL9303_PORTS];
	bool bridge_valid;
	u32 enabled;
	u32 user_mask;
	int cpu_port;
};

static u16 rtl9303_vid(struct dsa_switch *ds, int port)
{
	return RTL9303_VID_BASE | (ds->index << 5) | port;
}

static int rtl9303_table(struct rtl9303 *priv, int engine, int table,
			 int index, u32 *data, int words, bool write)
{
	unsigned int ctrl = engine == 2 ? 0xb320 : engine == 1 ? 0xce04 : 0xb340;
	unsigned int data_reg = engine == 2 ? 0xb334 : ctrl + 4;
	unsigned int bit = engine == 2 ? 18 : engine == 1 ? 14 : 16;
	u32 value, cmd = BIT(bit + 1) | (table << (engine == 2 ? 16 : 12)) | index;
	int i, ret;

	lockdep_assert_held(&priv->table_lock);
	ret = regmap_read_poll_timeout(priv->map, ctrl, value,
				       !(value & BIT(bit + 1)), 100, 20000);
	if (ret)
		return ret;
	if (write) {
		for (i = 0; i < words; i++) {
			ret = regmap_write(priv->map, data_reg + i * 4, data[i]);
			if (ret)
				return ret;
		}
		cmd |= BIT(bit);
	}
	ret = regmap_write(priv->map, ctrl, cmd);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(priv->map, ctrl, value,
				       !(value & BIT(bit + 1)), 100, 20000);
	if (ret || write)
		return ret;
	for (i = 0; i < words; i++) {
		ret = regmap_read(priv->map, data_reg + i * 4, &data[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int rtl9303_isolate(struct rtl9303 *priv, int port, u32 members)
{
	u32 value = members << 3;

	return rtl9303_table(priv, false, 6, port, &value, 1, true);
}

static int rtl9303_vlan(struct rtl9303 *priv, u16 vid, u32 members,
			int fid, bool shared, int profile)
{
	u32 data[2] = {
		(members << 3) | (fid >> 3),
		((u32)fid << 29) | (profile << 24) | (shared ? BIT(28) : 0),
	};
	u32 untagged = (members & priv->user_mask) << 3;
	int ret;

	/* Unicast uses the bridge FID; multicast remains private to each RX
	 * VID and is sent only to Linux by the RX VLAN profile.
	 */
	ret = rtl9303_table(priv, 0, 1, vid, data, 2, true);
	if (ret)
		return ret;
	return rtl9303_table(priv, 1, 0, vid, &untagged, 1, true);
}

static int rtl9303_stp(struct rtl9303 *priv, int port, int msti, unsigned int state)
{
	unsigned int index = 1 - (port + 3) / 16;
	unsigned int shift = 2 * ((port + 3) % 16);
	u32 data[2];
	int ret;

	ret = rtl9303_table(priv, 0, 4, msti, data, 2, false);
	if (ret)
		return ret;
	data[index] &= ~(3 << shift);
	data[index] |= state << shift;
	return rtl9303_table(priv, 0, 4, msti, data, 2, true);
}

/* Dynamic entries only; static bridge/host entries are owned by DSA. */
static int rtl9303_flush_port(struct rtl9303 *priv, int port)
{
	u32 value;
	int ret;

	ret = regmap_read_poll_timeout(priv->map, 0x9404, value,
				       !(value & BIT(30)), 100, 20000);
	if (ret)
		return ret;
	ret = regmap_write(priv->map, 0x9408, port << 11);
	if (ret)
		return ret;
	ret = regmap_write(priv->map, 0x9404, BIT(30) | BIT(26));
	if (ret)
		return ret;
	return regmap_read_poll_timeout(priv->map, 0x9404, value,
				       !(value & BIT(30)), 100, 20000);
}

static int rtl9303_learning(struct rtl9303 *priv, int port, bool enable)
{
	unsigned int shift = (port % 16) * 2;

	/* SALRN: 0 = hardware learning, 2 = disabled (OEM SDK). */
	return regmap_update_bits(priv->map, 0x8fec + (port / 16) * 4,
				 3 << shift, (enable ? 0 : 2) << shift);
}

static void rtl9303_stop(struct rtl9303 *priv)
{
	int p;

	priv->bridge_valid = false;
	for (p = 0; p < RTL9303_PORTS; p++)
		if (priv->user_mask & BIT(p))
			regmap_update_bits(priv->map, RTL9303_MAC_CTRL(p),
					   RTL9303_MAC_RX_TX, 0);
}

/* RTNL serializes configuration; table_lock also excludes FDB work. Stop
 * user MACs while changing membership so no intermediate state can bridge
 * two domains. On an SPI error leave them stopped until rollback succeeds.
 */
static int rtl9303_apply_bridge(struct rtl9303 *priv)
{
	int p, q, ret;

	lockdep_assert_held(&priv->table_lock);
	priv->bridge_valid = false;
	for (p = 0; p < RTL9303_PORTS; p++) {
		if (!(priv->user_mask & BIT(p)))
			continue;
		ret = regmap_update_bits(priv->map, RTL9303_MAC_CTRL(p), RTL9303_MAC_RX_TX, 0);
		if (ret)
			goto fail;
		ret = rtl9303_learning(priv, p, false);
		if (ret)
			goto fail;
	}
	for (p = 0; p < RTL9303_PORTS; p++) {
		struct rtl9303_port *port = &priv->ports[p];
		u32 members = BIT(p) | BIT(priv->cpu_port);
		u32 forward = BIT(priv->cpu_port);
		bool learning;

		if (!(priv->user_mask & BIT(p)))
			continue;
		for (q = 0; q < RTL9303_PORTS; q++) {
			struct rtl9303_port *other = &priv->ports[q];

			if (!(priv->user_mask & BIT(q)) || q == p ||
			    !port->bridge || port->bridge != other->bridge)
				continue;
			members |= BIT(q);
			if (port->stp == BR_STATE_FORWARDING &&
			    other->stp == BR_STATE_FORWARDING &&
			    !((port->flags & other->flags) & BR_ISOLATED))
				forward |= BIT(q);
		}
		ret = rtl9303_vlan(priv, rtl9303_vid(&priv->ds, p), members,
				   port->bridge ? RTL9303_FID_BASE + port->bridge : 0,
				   !!port->bridge, RTL9303_PROFILE);
		if (ret)
			goto fail;
		/* CPU-originated frames must never be looked up in a bridge FID:
		 * even a broadcast is delivered to this one user port only.
		 */
		ret = rtl9303_vlan(priv, RTL9303_TX_VID_BASE | (priv->ds.index << 5) | p,
				   BIT(p) | BIT(priv->cpu_port), 0, false, RTL9303_TX_PROFILE);
		if (ret)
			goto fail;
		ret = rtl9303_isolate(priv, p, forward);
		if (ret)
			goto fail;
		ret = rtl9303_flush_port(priv, p);
		if (ret)
			goto fail;
		learning = port->bridge && (port->flags & BR_LEARNING) &&
			   (port->stp == BR_STATE_LEARNING || port->stp == BR_STATE_FORWARDING);
		ret = rtl9303_learning(priv, p, learning);
		if (ret)
			goto fail;
	}
	for (p = 0; p < RTL9303_PORTS; p++) {
		if (!(priv->enabled & BIT(p)))
			continue;
		ret = regmap_update_bits(priv->map, RTL9303_MAC_CTRL(p),
					 RTL9303_MAC_RX_TX, RTL9303_MAC_RX_TX);
		if (ret)
			goto fail;
	}
	priv->bridge_valid = true;
	return 0;
fail:
	rtl9303_stop(priv);
	return ret;
}

static void rtl9303_rollback(struct rtl9303 *priv)
{
	int ret = rtl9303_apply_bridge(priv);

	if (ret)
		dev_err(priv->ds.dev, "bridge rollback failed: %d; LAN MAC stop requested\n", ret);
}

static int rtl9303_bridge_join(struct dsa_switch *ds, int port,
			       struct dsa_bridge bridge, bool *tx_fwd_offload,
			       struct netlink_ext_ack *extack)
{
	struct rtl9303 *priv = ds->priv;
	struct net_device *dev = dsa_to_port(ds, port)->user;
	struct rtl9303_port old;
	int ret;

	/* Returning EOPNOTSUPP here makes DSA retain software bridging. */
	if (br_vlan_enabled(bridge.dev)) {
		NL_SET_ERR_MSG_MOD(extack, "RTL9303 VLAN-aware bridges use software forwarding");
		return -EOPNOTSUPP;
	}
	if (!br_port_flag_is_set(dev, BR_FLOOD) ||
	    !br_port_flag_is_set(dev, BR_BCAST_FLOOD) ||
	    br_port_flag_is_set(dev, BR_HAIRPIN_MODE | BR_PORT_LOCKED))
		return -EOPNOTSUPP;
	if (!bridge.num || bridge.num > RTL9303_BRIDGES)
		return -EOPNOTSUPP;

	mutex_lock(&priv->table_lock);
	old = priv->ports[port];
	priv->ports[port].bridge = bridge.num;
	priv->ports[port].stp = br_port_get_stp_state(dev);
	priv->ports[port].flags = BR_FLOOD | BR_BCAST_FLOOD | BR_MCAST_FLOOD;
	if (br_port_flag_is_set(dev, BR_LEARNING))
		priv->ports[port].flags |= BR_LEARNING;
	if (br_port_flag_is_set(dev, BR_ISOLATED))
		priv->ports[port].flags |= BR_ISOLATED;
	ret = rtl9303_apply_bridge(priv);
	if (ret) {
		priv->ports[port] = old;
		rtl9303_rollback(priv);
	}
	mutex_unlock(&priv->table_lock);
	*tx_fwd_offload = false;
	return ret;
}

static void rtl9303_bridge_leave(struct dsa_switch *ds, int port, struct dsa_bridge bridge)
{
	struct rtl9303 *priv = ds->priv;
	int ret;

	mutex_lock(&priv->table_lock);
	priv->ports[port].bridge = 0;
	priv->ports[port].stp = BR_STATE_FORWARDING;
	priv->ports[port].flags = BR_FLOOD | BR_BCAST_FLOOD | BR_MCAST_FLOOD;
	ret = rtl9303_apply_bridge(priv);
	if (ret)
		dev_err(ds->dev, "bridge leave failed: %d; LAN MAC stop requested\n", ret);
	mutex_unlock(&priv->table_lock);
}

static void rtl9303_stp_state_set(struct dsa_switch *ds, int port, u8 state)
{
	struct rtl9303 *priv = ds->priv;
	int ret;

	mutex_lock(&priv->table_lock);
	priv->ports[port].stp = state;
	/* Use isolation for the data path, leaving CPU delivery possible for
	 * BPDUs even on blocked ports. Hardware traps would go to port 28.
	 */
	ret = rtl9303_apply_bridge(priv);
	if (ret)
		dev_err(ds->dev, "STP update failed: %d; LAN MAC stop requested\n", ret);
	mutex_unlock(&priv->table_lock);
}

static int rtl9303_pre_bridge_flags(struct dsa_switch *ds, int port,
				    struct switchdev_brport_flags flags,
				    struct netlink_ext_ack *extack)
{
	if (flags.mask & ~RTL9303_BR_FLAGS)
		return -EOPNOTSUPP;
	if (flags.mask & ~flags.val & (BR_FLOOD | BR_BCAST_FLOOD)) {
		NL_SET_ERR_MSG_MOD(extack, "RTL9303 hardware bridges require unicast and broadcast flooding");
		return -EINVAL;
	}
	return 0;
}

static int rtl9303_bridge_flags(struct dsa_switch *ds, int port,
				struct switchdev_brport_flags flags,
				struct netlink_ext_ack *extack)
{
	struct rtl9303 *priv = ds->priv;
	unsigned long old;
	int ret;

	ret = rtl9303_pre_bridge_flags(ds, port, flags, extack);
	if (ret)
		return ret;
	mutex_lock(&priv->table_lock);
	old = priv->ports[port].flags;
	priv->ports[port].flags = (old & ~flags.mask) | flags.val;
	ret = rtl9303_apply_bridge(priv);
	if (ret) {
		priv->ports[port].flags = old;
		rtl9303_rollback(priv);
	}
	mutex_unlock(&priv->table_lock);
	return ret;
}

static int rtl9303_vlan_filtering(struct dsa_switch *ds, int port, bool filtering,
				  struct netlink_ext_ack *extack)
{
	if (!filtering)
		return 0;
	/* Do not silently keep hardware forwarding after bridge VLAN filtering
	 * is enabled. A bridge created with filtering enabled falls back at join.
	 */
	NL_SET_ERR_MSG_MOD(extack,
			   "Recreate the RTL9303 bridge with VLAN filtering enabled for software forwarding");
	return -EBUSY;
}

static void rtl9303_fast_age(struct dsa_switch *ds, int port)
{
	struct rtl9303 *priv = ds->priv;
	int ret;

	mutex_lock(&priv->table_lock);
	ret = rtl9303_flush_port(priv, port);
	if (ret) {
		rtl9303_stop(priv);
		dev_err(ds->dev, "FDB flush failed: %d; LAN MAC stop requested\n", ret);
	}
	mutex_unlock(&priv->table_lock);
}

static int rtl9303_ageing_time(struct dsa_switch *ds, unsigned int msec)
{
	struct rtl9303 *priv = ds->priv;

	return regmap_update_bits(priv->map, 0x8fdc, GENMASK(20, 0),
				 min_t(u32, (msec / 100 + 6) / 7, 0x1fffff));
}

/* RTL930x uses two four-way hash buckets. Match the OEM hashIdx_get,
 * including bits 55..59 in BOTH algorithms (some older upstream versions
 * mistakenly used seed << 55 for the second algorithm).
 */
static void rtl9303_hash(u64 seed, u32 algo, u16 bucket[2])
{
	u32 h1 = (seed >> 11) & 0x7ff, h2 = (seed >> 33) & 0x7ff;
	u32 common = ((seed >> 55) & 0x1f) ^ ((seed >> 44) & 0x7ff) ^
		     ((seed >> 22) & 0x7ff) ^ (seed & 0x7ff);
	u32 k0 = common ^ h1 ^ h2;
	u32 k1 = common ^ ((h1 & 0x1f) << 6 | h1 >> 5) ^
		 ((h2 & 0x3f) << 5 | h2 >> 6);

	bucket[0] = algo & BIT(0) ? k1 : k0;
	bucket[1] = (algo & BIT(1) ? k1 : k0) + 2048;
}

static int rtl9303_fdb_find(struct rtl9303 *priv, const unsigned char *addr,
			   u16 fid, bool add, u32 entry[3])
{
	u64 seed = ether_addr_to_u64(addr) | ((u64)fid << 48);
	u32 mac0 = get_unaligned_be32(addr), mac1 = get_unaligned_be16(addr + 4);
	u16 bucket[2];
	u32 algo;
	int i, index, empty = -1, ret;

	ret = regmap_read(priv->map, 0x8fd8, &algo);
	if (ret)
		return ret;
	rtl9303_hash(seed, algo, bucket);
	for (i = 0; i < 8; i++) {
		index = bucket[i / 4] * 4 + i % 4;
		ret = rtl9303_table(priv, 2, 0, index, entry, 3, false);
		if (ret)
			return ret;
		if (!(entry[2] & BIT(31))) {
			if (empty < 0)
				empty = index;
		} else if (entry[0] == mac0 && entry[1] == ((mac1 << 16) | fid)) {
			return index;
		}
	}
	if (algo & BIT(12)) {
		for (i = 0; i < 64; i++) {
			ret = rtl9303_table(priv, 2, 1, i, entry, 3, false);
			if (ret)
				return ret;
			if (!(entry[2] & BIT(31))) {
				if (empty < 0)
					empty = RTL9303_L2_ENTRIES + i;
			} else if (entry[0] == mac0 && entry[1] == ((mac1 << 16) | fid)) {
				return RTL9303_L2_ENTRIES + i;
			}
		}
	}
	/* Search all eight candidates and enabled CAM before using a slot, otherwise
	 * a MAC already in the second bucket could be installed twice.
	 */
	if (!add)
		return -ENOENT;
	if (empty < 0)
		return -ENOSPC;
	memset(entry, 0, 3 * sizeof(*entry));
	return empty;
}

static int rtl9303_fdb_update(struct dsa_switch *ds, int port,
			      const unsigned char *addr, u16 vid,
			      struct dsa_db db, bool add)
{
	struct rtl9303 *priv = ds->priv;
	u32 entry[3];
	u16 fid;
	int index, ret;

	/* Standalone traffic is deliberately flooded to the external CPU.
	 * There is no hardware bridge FID for DSA_DB_PORT host entries, so
	 * treating these requests as successful avoids rejecting normal host
	 * learning notifications from the software bridge.
	 */
	if (db.type == DSA_DB_PORT)
		return 0;
	if (db.type != DSA_DB_BRIDGE)
		return -EOPNOTSUPP;
	/* A VLAN-unaware bridge still reports its software PVID (normally 1).
	 * The ASIC uses the private service VID and the bridge FID instead.
	 */
	if (br_vlan_enabled(db.bridge.dev) || !db.bridge.num ||
	    db.bridge.num > RTL9303_BRIDGES)
		return -EOPNOTSUPP;
	fid = RTL9303_FID_BASE + db.bridge.num;
	mutex_lock(&priv->table_lock);
	index = rtl9303_fdb_find(priv, addr, fid, add, entry);
	if (index < 0) {
		ret = !add && index == -ENOENT ? 0 : index;
		goto out;
	}
	/* A delayed delete for the old port must not delete a moved entry. */
	if (!add && ((entry[2] >> 20) & 0x3ff) != (u32)port) {
		ret = 0;
		goto out;
	}
	entry[0] = add ? get_unaligned_be32(addr) : 0;
	entry[1] = add ? ((u32)get_unaligned_be16(addr + 4) << 16) | fid : 0;
	entry[2] = add ? BIT(31) | (port << 20) | (3 << 17) | BIT(14) : 0;
	ret = rtl9303_table(priv, 2, index / RTL9303_L2_ENTRIES,
			    index % RTL9303_L2_ENTRIES, entry, 3, true);
out:
	mutex_unlock(&priv->table_lock);
	return ret;
}

static int rtl9303_fdb_add(struct dsa_switch *ds, int port, const unsigned char *addr,
			  u16 vid, struct dsa_db db)
{
	return rtl9303_fdb_update(ds, port, addr, vid, db, true);
}

static int rtl9303_fdb_del(struct dsa_switch *ds, int port, const unsigned char *addr,
			  u16 vid, struct dsa_db db)
{
	return rtl9303_fdb_update(ds, port, addr, vid, db, false);
}

static int rtl9303_fdb_dump(struct dsa_switch *ds, int port,
			   dsa_fdb_dump_cb_t *cb, void *data)
{
	struct rtl9303 *priv = ds->priv;
	unsigned char addr[ETH_ALEN];
	u32 entry[3];
	int i, ret = 0;
	u16 fid;

	mutex_lock(&priv->table_lock);
	if (!priv->ports[port].bridge)
		goto out;
	fid = RTL9303_FID_BASE + priv->ports[port].bridge;
	for (i = 0; i < RTL9303_L2_ENTRIES + 64; i++) {
		ret = rtl9303_table(priv, 2, i / RTL9303_L2_ENTRIES,
				    i % RTL9303_L2_ENTRIES, entry, 3, false);
		if (ret)
			break;
		if (!(entry[2] & BIT(31)) || (entry[0] & BIT(24)) ||
		    (entry[2] & (BIT(30) | BIT(12))) ||
		    ((entry[2] >> 20) & 0x3ff) != (u32)port || (entry[1] & 0xfff) != fid)
			continue;
		put_unaligned_be32(entry[0], addr);
		put_unaligned_be16(entry[1] >> 16, addr + 4);
		ret = cb(addr, 0, !!(entry[2] & BIT(14)), data);
		if (ret)
			break;
		cond_resched();
	}
out:
	mutex_unlock(&priv->table_lock);
	return ret;
}

/* Clean only our reserved forwarding domains, including stale static/MC
 * entries after a driver reload. Leave unrelated switch domains untouched.
 * The normal SRAM table and the enabled overflow CAM are both supported.
 */
static int rtl9303_fdb_reset(struct rtl9303 *priv)
{
	u32 entry[3];
	int engine, i, p, ret;
	u16 fid;
	bool owned;

	for (engine = 0; engine < 2; engine++) {
		for (i = 0; i < (engine ? 64 : RTL9303_L2_ENTRIES); i++) {
			ret = rtl9303_table(priv, 2, engine, i, entry, 3, false);
			if (ret)
				return ret;
			if (!(entry[2] & BIT(31)))
				continue;
			fid = entry[1] & 0xfff;
			owned = fid > RTL9303_FID_BASE && fid <= RTL9303_FID_BASE + RTL9303_BRIDGES;
			for (p = 0; p < RTL9303_PORTS && !owned; p++)
				if (priv->user_mask & BIT(p))
					owned = fid == rtl9303_vid(&priv->ds, p) ||
						fid == (RTL9303_TX_VID_BASE | (priv->ds.index << 5) | p);
			if (!owned)
				continue;
			memset(entry, 0, sizeof(entry));
			ret = rtl9303_table(priv, 2, engine, i, entry, 3, true);
			if (ret)
				return ret;
			cond_resched();
		}
	}
	return 0;
}

static int rtl9303_port_mtu(struct dsa_switch *ds, int port, int mtu)
{
	struct rtl9303 *priv = ds->priv;
	u32 len = mtu + ETH_HLEN + 2 * VLAN_HLEN + ETH_FCS_LEN;

	if (len > RTL9303_MAX_FRAME)
		return -EINVAL;
	return regmap_update_bits(priv->map, RTL9303_MAC_MAX_LEN(port),
				 GENMASK(27, 0), len | (len << 14));
}

static int rtl9303_port_max_mtu(struct dsa_switch *ds, int port)
{
	return RTL9303_MAX_FRAME - ETH_HLEN - 2 * VLAN_HLEN - ETH_FCS_LEN;
}

static int rtl9303_config_port(struct rtl9303 *priv, int port)
{
	struct regmap *map = priv->map;
	bool cpu = port == priv->cpu_port;
	unsigned int shift = (port % 16) * 2;
	int ret;

	ret = regmap_update_bits(map, RTL9303_MAC_CTRL(port), RTL9303_MAC_RX_TX, 0);
	if (ret)
		return ret;
	/* Recognize the private S-tag only on the CPU link. Customer VLANs
	 * remain opaque, even when a customer sends an 802.1ad frame.
	 */
	ret = regmap_update_bits(map, 0x327c + port * 64, 0xf, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, 0x3280 + port * 64, 0xf,
				 cpu ? BIT(RTL9303_TPID_SLOT) : 0);
	if (ret)
		return ret;
	/* Accept only service-tagged traffic from the CPU; user packets have
	 * no recognized outer tag and receive their own service PVID.
	 */
	ret = regmap_update_bits(map, 0x8260 + port * 4, 0xf, cpu ? 7 : 0xb);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, 0x82d8 + port * 4, GENMASK(27, 14),
				 cpu ? 0 : (rtl9303_vid(&priv->ds, port) << 16));
	if (ret)
		return ret;
	ret = regmap_update_bits(map, 0x834c + port * 4, 0xf, 0xf);
	if (ret)
		return ret;
	/* Keep the source service VID on CPU egress, rather than rewriting it
	 * from the learned destination's aggregation VID.
	 */
	ret = regmap_update_bits(map, 0x8e60 + port * 4, 7, 0);
	if (ret)
		return ret;
	/* Keep customer data unchanged; outer tag insertion/removal follows
	 * the VLAN membership/untag table. Force our chosen outer TPID.
	 */
	ret = regmap_update_bits(map, 0xce24 + port * 4, 0xff, BIT(2) | BIT(0));
	if (ret)
		return ret;
	ret = regmap_update_bits(map, 0xce98 + port * 4, 0x3f,
				 (RTL9303_TPID_SLOT << 4) | BIT(0));
	if (ret)
		return ret;
	ret = regmap_update_bits(map, 0x83c0 + (port / 16) * 4,
				 3 << shift, BIT(shift));
	if (ret)
		return ret;
	ret = regmap_update_bits(map, 0x83c8, BIT(port), BIT(port));
	if (ret)
		return ret;
	/* Start with learning disabled until bridge state is installed. */
	ret = regmap_update_bits(map, 0x8fec + (port / 16) * 4, 3 << shift, 2 << shift);
	if (ret)
		return ret;
	shift = (port % 10) * 3;
	ret = regmap_update_bits(map, 0x8ff4 + (port / 10) * 4, 7 << shift, 0);
	if (ret)
		return ret;
	/* Moving dynamic MACs must relearn without trapping to internal CPU 28.
	 * Static entries stay under bridge/FDB control.
	 */
	ret = regmap_update_bits(map, 0x9000 + (port / 10) * 4, 7 << shift, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, 0x900c, BIT(port), BIT(port));
	if (ret)
		return ret;
	ret = regmap_update_bits(map, 0x9010 + (port / 10) * 4, 7 << shift, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, 0x901c, BIT(port), 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, 0x904c, BIT(port), 0);
	if (ret)
		return ret;
	/* Management traps target the internal CPU (28), which isn't wired
	 * to Linux. Forward these packets through the service VLAN instead.
	 */
	ret = regmap_update_bits(map, 0x9e7c + (port / 10) * 4, 7 << shift, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, 0x9efc + (port / 10) * 4, 7 << shift, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, 0x9f08 + (port / 10) * 4, 7 << shift, 0);
	if (ret)
		return ret;
	ret = rtl9303_port_mtu(&priv->ds, port, ETH_DATA_LEN);
	if (ret)
		return ret;
	ret = rtl9303_isolate(priv, port, cpu ? priv->user_mask : BIT(priv->cpu_port));
	if (ret)
		return ret;
	return rtl9303_stp(priv, port, 0, 3);
}

static int rtl9303_setup(struct dsa_switch *ds)
{
	struct rtl9303 *priv = ds->priv;
	struct dsa_port *dp;
	u32 mask;
	int ret, i, msti;

	if (ds->index > 7)
		return -EINVAL;
	priv->cpu_port = -1;
	dsa_switch_for_each_port(dp, ds) {
		if (dp->index >= RTL9303_PORTS)
			return -EINVAL;
		if (dsa_port_is_cpu(dp)) {
			if (priv->cpu_port >= 0)
				return -EINVAL;
			priv->cpu_port = dp->index;
		} else if (dsa_port_is_user(dp)) {
			priv->user_mask |= BIT(dp->index);
		}
	}
	if (priv->cpu_port < 0 || !priv->user_mask)
		return dev_err_probe(ds->dev, -EINVAL, "CPU and user ports are required\n");
	mask = priv->user_mask | BIT(priv->cpu_port);
	ds->fdb_isolation = true;
	ds->assisted_learning_on_cpu_port = true;
	ds->max_num_bridges = RTL9303_BRIDGES;
	mutex_lock(&priv->table_lock);
	for (i = 0; i < RTL9303_PORTS; i++) {
		if (!(mask & BIT(i)))
			continue;
		ret = regmap_update_bits(priv->map, RTL9303_MAC_CTRL(i), RTL9303_MAC_RX_TX, 0);
		if (ret)
			goto out;
	}
	ret = regmap_update_bits(priv->map, 0xc7ac + RTL9303_TPID_SLOT * 4,
				 GENMASK(31, 16), (u32)ETH_P_8021AD << 16);
	if (ret)
		goto out;
	ret = regmap_update_bits(priv->map, 0x9064, mask, mask);
	if (ret)
		goto out;
	ret = regmap_update_bits(priv->map, 0x9068, mask, mask);
	if (ret)
		goto out;
	/* RX multicast goes only to Linux for snooping/MDB replication. TX
	 * multicast reaches the sole user member of its directed TX VLAN.
	 * Neither profile enables L3 routing or a management CPU trap.
	 */
	for (i = 0; i < 5; i++) {
		ret = regmap_write(priv->map, 0x9c60 + RTL9303_PROFILE * 20 + i * 4,
				   i < 2 ? 0 : BIT(priv->cpu_port));
		if (ret)
			goto out;
		ret = regmap_write(priv->map, 0x9c60 + RTL9303_TX_PROFILE * 20 + i * 4,
				   i < 2 ? 0 : mask);
		if (ret)
			goto out;
	}
	/* Max learning count, forward on a full table. The OEM register
	 * description places CONSTRT_NUM at bit 3, not bit 2.
	 */
	ret = regmap_update_bits(priv->map, 0x909c, GENMASK(17, 0), 0x7fff << 3);
	if (ret)
		goto out;
	ret = regmap_update_bits(priv->map, 0x8fd8, GENMASK(10, 8), 0);
	if (ret)
		goto out;
	ret = regmap_update_bits(priv->map, 0x8fdc, BIT(25) | BIT(21), 0);
	if (ret)
		goto out;
	for (i = 0; i < RTL9303_PORTS; i++) {
		if (!(mask & BIT(i)))
			continue;
		ret = rtl9303_config_port(priv, i);
		if (ret)
			goto out;
		priv->ports[i].stp = BR_STATE_FORWARDING;
		priv->ports[i].flags = BR_FLOOD | BR_BCAST_FLOOD | BR_MCAST_FLOOD;
		for (msti = 1; msti <= RTL9303_BRIDGES; msti++) {
			ret = rtl9303_stp(priv, i, RTL9303_FID_BASE + msti, 3);
			if (ret)
				goto out;
		}
		ret = regmap_update_bits(priv->map, 0x90a4 + i * 4,
					 GENMASK(17, 0), 0x7fff << 3);
		if (ret)
			goto out;
		ret = regmap_update_bits(priv->map, 0x8fe0, BIT(i), BIT(i));
		if (ret)
			goto out;
	}
	ret = rtl9303_fdb_reset(priv);
	if (ret)
		goto out;
	ret = rtl9303_apply_bridge(priv);
	if (ret)
		goto out;
	ret = regmap_update_bits(priv->map, RTL9303_MAC_CTRL(priv->cpu_port),
				 RTL9303_MAC_RX_TX, RTL9303_MAC_RX_TX);
out:
	if (ret)
		for (i = 0; i < RTL9303_PORTS; i++)
			if (mask & BIT(i))
				regmap_update_bits(priv->map, RTL9303_MAC_CTRL(i), RTL9303_MAC_RX_TX, 0);
	mutex_unlock(&priv->table_lock);
	return ret;
}

static enum dsa_tag_protocol rtl9303_tag_protocol(struct dsa_switch *ds, int port,
						enum dsa_tag_protocol conduit_proto)
{
	return DSA_TAG_PROTO_RTL9303_8021AD;
}

static int rtl9303_fill_pcs(struct phylink_config *config,
			    struct phylink_pcs **pcs, unsigned int count)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);

	return fwnode_phylink_pcs_parse(of_fwnode_handle(dp->dn), pcs, count);
}

static void rtl9303_get_caps(struct dsa_switch *ds, int port,
			     struct phylink_config *config)
{
	struct dsa_port *dp = dsa_to_port(ds, port);
	phy_interface_t interface;
	int ret;

	config->mac_capabilities = MAC_SYM_PAUSE | MAC_ASYM_PAUSE |
		MAC_10 | MAC_100 | MAC_1000FD | MAC_2500FD;
	ret = of_get_phy_mode(dp->dn, &interface);
	if (!ret && (interface == PHY_INTERFACE_MODE_USXGMII ||
		     interface == PHY_INTERFACE_MODE_10GBASER)) {
		config->mac_capabilities |= MAC_5000FD | MAC_10000FD;
		__set_bit(PHY_INTERFACE_MODE_USXGMII, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_10GBASER, config->supported_interfaces);
	} else {
		__set_bit(PHY_INTERFACE_MODE_SGMII, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX, config->supported_interfaces);
	}
	config->num_possible_pcs = fwnode_phylink_pcs_count(of_fwnode_handle(dp->dn));
	if (config->num_possible_pcs) {
		config->fill_available_pcs = rtl9303_fill_pcs;
		bitmap_copy(config->pcs_interfaces, config->supported_interfaces,
			    PHY_INTERFACE_MODE_MAX);
	}
}

static void rtl9303_mac_config(struct phylink_config *config, unsigned int mode,
			       const struct phylink_link_state *state)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct rtl9303 *priv = dp->ds->priv;
	int ret;

	/* The shared Otto PCS provider programs and calibrates the SerDes.  Drop
	 * any stale force mode before an in-band link is negotiated.
	 */
	ret = regmap_write(priv->map, RTL9303_MAC_FORCE(dp->index), 0);
	if (ret)
		dev_err_ratelimited(dp->ds->dev, "port %d MAC configuration failed: %d\n",
				    dp->index, ret);
}

static void rtl9303_mac_link_down(struct phylink_config *config, unsigned int mode,
				  phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct rtl9303 *priv = dp->ds->priv;
	int ret;

	mutex_lock(&priv->table_lock);
	/* The USXGMII PCS reads the MAC link-status mirror.  Forcing this
	 * mirror down would make phylink wait forever for PCS link-up.  Stop
	 * traffic using the RX/TX gates and let in-band status reach the MAC.
	 */
	ret = regmap_update_bits(priv->map, RTL9303_MAC_CTRL(dp->index),
				 RTL9303_MAC_RX_TX, 0);
	if (!ret)
		ret = regmap_update_bits(priv->map, RTL9303_MAC_FORCE(dp->index),
					 RTL9303_FORCE_EN | RTL9303_FORCE_LINK, 0);
	if (!ret)
		ret = rtl9303_flush_port(priv, dp->index);
	if (ret)
		dev_err_ratelimited(dp->ds->dev, "port %d link-down failed: %d\n", dp->index, ret);
	mutex_unlock(&priv->table_lock);
}

static void rtl9303_mac_link_up(struct phylink_config *config, struct phy_device *phy,
				unsigned int mode, phy_interface_t interface,
				int speed, int duplex, bool tx_pause, bool rx_pause)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct rtl9303 *priv = dp->ds->priv;
	u32 value = RTL9303_FORCE_LINK | RTL9303_FORCE_FC;
	int code, ret;

	if (dp->index == priv->cpu_port || phy)
		value |= RTL9303_FORCE_EN;

	switch (speed) {
	case SPEED_10:
		code = 0;
		break;
	case SPEED_100:
		code = 1;
		break;
	case SPEED_1000:
		code = 2;
		break;
	case SPEED_2500:
		code = 5;
		break;
	case SPEED_5000:
		code = 6;
		break;
	case SPEED_10000:
		code = 4;
		break;
	default:
		return;
	}
	value |= FIELD_PREP(RTL9303_SPEED, code);
	if (duplex == DUPLEX_FULL)
		value |= RTL9303_DUPLEX;
	if (tx_pause)
		value |= RTL9303_TX_PAUSE;
	if (rx_pause)
		value |= RTL9303_RX_PAUSE;
	ret = regmap_update_bits(priv->map, RTL9303_MAC_FORCE(dp->index), GENMASK(9, 0), value);
	if (!ret)
		ret = regmap_update_bits(priv->map, RTL9303_MAC_CTRL(dp->index),
					 RTL9303_MAC_RX_TX, RTL9303_MAC_RX_TX);
	if (ret)
		dev_err_ratelimited(dp->ds->dev, "port %d link-up failed: %d\n", dp->index, ret);
}

static int rtl9303_port_enable(struct dsa_switch *ds, int port, struct phy_device *phy)
{
	struct rtl9303 *priv = ds->priv;

	int ret;

	mutex_lock(&priv->table_lock);
	if (!priv->bridge_valid) {
		ret = rtl9303_apply_bridge(priv);
		if (ret)
			goto out;
	}
	ret = regmap_update_bits(priv->map, RTL9303_MAC_CTRL(port),
				 RTL9303_MAC_RX_TX, RTL9303_MAC_RX_TX);
	if (!ret)
		priv->enabled |= BIT(port);
out:
	mutex_unlock(&priv->table_lock);
	return ret;
}

static void rtl9303_port_disable(struct dsa_switch *ds, int port)
{
	struct rtl9303 *priv = ds->priv;
	int ret;

	/* DSA also calls this for every unused port, including MoCA. */
	if (!(priv->user_mask & BIT(port)) && port != priv->cpu_port)
		return;
	mutex_lock(&priv->table_lock);
	priv->enabled &= ~BIT(port);
	ret = regmap_update_bits(priv->map, RTL9303_MAC_CTRL(port), RTL9303_MAC_RX_TX, 0);
	if (!ret)
		ret = rtl9303_flush_port(priv, port);
	if (ret)
		dev_err_ratelimited(ds->dev, "port %d disable failed: %d\n", port, ret);
	mutex_unlock(&priv->table_lock);
}

static const struct phylink_mac_ops rtl9303_mac_ops = {
	.mac_config = rtl9303_mac_config,
	.mac_link_down = rtl9303_mac_link_down,
	.mac_link_up = rtl9303_mac_link_up,
};

static const struct dsa_switch_ops rtl9303_switch_ops = {
	.get_tag_protocol = rtl9303_tag_protocol,
	.setup = rtl9303_setup,
	.port_bridge_join = rtl9303_bridge_join,
	.port_bridge_leave = rtl9303_bridge_leave,
	.port_stp_state_set = rtl9303_stp_state_set,
	.port_pre_bridge_flags = rtl9303_pre_bridge_flags,
	.port_bridge_flags = rtl9303_bridge_flags,
	.port_vlan_filtering = rtl9303_vlan_filtering,
	.port_fast_age = rtl9303_fast_age,
	.set_ageing_time = rtl9303_ageing_time,
	.port_fdb_add = rtl9303_fdb_add,
	.port_fdb_del = rtl9303_fdb_del,
	.port_fdb_dump = rtl9303_fdb_dump,
	.phylink_get_caps = rtl9303_get_caps,
	.port_enable = rtl9303_port_enable,
	.port_disable = rtl9303_port_disable,
	.port_change_mtu = rtl9303_port_mtu,
	.port_max_mtu = rtl9303_port_max_mtu,
};

static int rtl9303_dsa_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *ports;
	struct rtl9303 *priv;
	struct phy_device *phy;
	int ret = 0;

	/* DSA can disable a user port permanently on a missing PHY. Wait for
	 * all three MDIO devices before registering any network interfaces.
	 */
	ports = of_get_child_by_name(dev->of_node, "ethernet-ports");
	if (!ports)
		return -EINVAL;
	for_each_available_child_of_node_scoped(ports, port) {
		struct device_node *phy_np = of_parse_phandle(port, "phy-handle", 0);

		if (!phy_np)
			continue;
		phy = of_phy_find_device(phy_np);
		of_node_put(phy_np);
		if (!phy) {
			ret = -EPROBE_DEFER;
			break;
		}
		put_device(&phy->mdio.dev);
	}
	of_node_put(ports);
	if (ret)
		return dev_err_probe(dev, ret, "waiting for LAN PHYs\n");

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->map = dev_get_regmap(dev->parent, NULL);
	if (!priv->map)
		return -ENODEV;
	mutex_init(&priv->table_lock);
	priv->ds.dev = dev;
	priv->ds.priv = priv;
	priv->ds.num_ports = RTL9303_PORTS;
	priv->ds.ops = &rtl9303_switch_ops;
	priv->ds.phylink_mac_ops = &rtl9303_mac_ops;
	platform_set_drvdata(pdev, priv);
	return dsa_register_switch(&priv->ds);
}

static void rtl9303_dsa_remove(struct platform_device *pdev)
{
	struct rtl9303 *priv = platform_get_drvdata(pdev);

	dsa_unregister_switch(&priv->ds);
}

static void rtl9303_dsa_shutdown(struct platform_device *pdev)
{
	struct rtl9303 *priv = platform_get_drvdata(pdev);

	dsa_switch_shutdown(&priv->ds);
}

static struct platform_driver rtl9303_dsa_driver = {
	.driver.name = "rtl9303-dsa",
	.probe = rtl9303_dsa_probe,
	.remove = rtl9303_dsa_remove,
	.shutdown = rtl9303_dsa_shutdown,
};
module_platform_driver(rtl9303_dsa_driver);

MODULE_ALIAS("platform:rtl9303-dsa");
MODULE_DESCRIPTION("RTL9303 external CPU DSA switch");
MODULE_LICENSE("GPL");
