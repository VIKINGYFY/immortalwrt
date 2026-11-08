/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * What a WLAN driver tells the NSS firmware about its rings.
 *
 * The firmware reaps the WLAN hardware's rings itself, so it has to be told
 * where they are. This is the whole of what it needs: the description is
 * filled from whatever the WLAN driver already knows and handed over once,
 * which keeps the firmware driver free of that driver's internals and of the
 * kernel version they change with.
 *
 * Registration is not a handover. The description is kept until something
 * asks for the firmware to be started, because a running core takes the
 * hardware with it and that stays a deliberate act.
 *
 * The firmware runs one instance for the radios inside the SoC and up to two
 * for radios on their own chip, such as a PCIe card. Registering a
 * description returns the instance it was given, and every later call about
 * that hardware names it.
 */

#ifndef __LINUX_SOC_QCOM_NSS_WIFI_H__
#define __LINUX_SOC_QCOM_NSS_WIFI_H__

#include <linux/if_ether.h>
#include <linux/types.h>

struct napi_struct;
struct net_device;
struct sk_buff;

struct flow_block_offload;

#define NSS_WIFI_MAX_TCL_RINGS	4
#define NSS_WIFI_MAX_REO_RINGS	4

/*
 * One hardware ring. Everything here is a property of the ring the WLAN
 * driver allocated; the firmware derives the rest. entry_size is in 32-bit
 * words, and hwreg[1] is the absolute address of the register the firmware
 * moves the ring's pointer through.
 */
struct nss_wifi_ring {
	u8 id;
	u8 dir;
	u32 num_entries;
	u32 entry_size;
	u32 flags;
	u32 base;
	u32 hwreg[2];
};

struct nss_wifi_pdev {
	struct nss_wifi_ring rxdma;
	u8 radio_id;
	u8 lmac_id;
	u8 target_pdev_id;
	u32 num_rx_swdesc;
};

/* What the firmware counted for one peer since its previous report. */
struct nss_wifi_peer_stats {
	u32 tx_packets;
	u32 tx_bytes;
	u32 tx_acked;
	u32 tx_retries;
	u32 tx_failed;
	u32 rx_packets;
	u32 rx_bytes;
};

enum nss_wifi_wds_event {
	NSS_WIFI_WDS_LEARN,
	NSS_WIFI_WDS_MEC,
	NSS_WIFI_WDS_4ADDR,
	NSS_WIFI_WDS_ACTIVE,
	NSS_WIFI_WDS_MEC_EXPIRE,
};

struct nss_wifi_wds_info {
	u16 peer_id;
	u16 ast_idx;
	u8 radio;
	bool addr4;
	u8 addr[ETH_ALEN];
};

enum nss_wifi_wds_cmd {
	NSS_WIFI_WDS_ADD,
	NSS_WIFI_WDS_DEL,
	NSS_WIFI_WDS_MAP,
	NSS_WIFI_WDS_UPDATE,
};

struct nss_wifi_wds_peer {
	u16 vdev_id;
	u16 peer_id;
	u16 ast_idx;
	u8 radio;
	u8 type;
	u8 peer[ETH_ALEN];
	u8 addr[ETH_ALEN];
};

struct nss_wifi_soc {
	u32 dev_base;
	u32 shadow_rd;
	u32 shadow_wr;
	u32 lmac_ring_start;
	u32 target_type;
	u16 tlv_size;
	u16 rx_buf_len;
	u8 num_tcl;
	u8 num_reo;
	/* Radios that follow with nss_wifi_pdev_register(). */
	u8 num_pdevs;
	/* A radio on its own chip rather than inside the SoC. */
	bool external;
	struct nss_wifi_ring tcl[NSS_WIFI_MAX_TCL_RINGS];
	struct nss_wifi_ring txcomp[NSS_WIFI_MAX_TCL_RINGS];
	struct nss_wifi_ring reo_dest[NSS_WIFI_MAX_REO_RINGS];
	struct nss_wifi_ring reo_reinject;
	struct nss_wifi_ring rx_rel;
	struct nss_wifi_ring reo_exception;

	/*
	 * The firmware reaps rings holding MSDU-link descriptors it cannot
	 * put back, because the ring that returns one to the idle list is not
	 * among those it is told about. It hands each one back by message,
	 * and this is the way home. Called from softirq, may not sleep.
	 */
	void *priv;
	void (*link_desc_return)(void *priv, const u32 *buf_addr_info);

	/*
	 * Stop and restart the WLAN hardware, unregistering and describing
	 * the rings again on the way. The firmware's buffers sit in those
	 * rings, so a firmware that stops is freed only behind this.
	 */
	void (*reset)(void *priv);

	/*
	 * A frame a virtual device handed up, before it is taken for
	 * ethernet. Returns true if it took the frame. The hardware receive
	 * descriptor is at skb->head. Called from softirq.
	 */
	bool (*rx_80211)(void *priv, struct net_device *dev,
			 struct sk_buff *skb, struct napi_struct *napi, bool mcbc);

	/* Special receive notifications; softirq, may not sleep. */
	void (*wds_event)(void *priv, struct net_device *dev,
			  enum nss_wifi_wds_event ev,
			  const struct nss_wifi_wds_info *info);

	/*
	 * Offloaded frames never pass the WLAN driver, so the firmware's
	 * per-peer counts are the only record of a station's traffic.
	 * Called from softirq.
	 */
	void (*peer_stats)(void *priv, u16 peer_id,
			   const struct nss_wifi_peer_stats *st);
};

/*
 * 802.11s, on the firmware lines that forward it themselves. A mesh
 * interface is a virtual device for its frames plus two firmware nodes that
 * hold the path tables; the subsystem's path table is mirrored into them,
 * and the firmware reports back what only it sees.
 */
enum nss_wifi_mesh_cmd {
	NSS_WIFI_MESH_MPATH_ADD,
	NSS_WIFI_MESH_MPATH_UPDATE,
	NSS_WIFI_MESH_MPATH_DEL,
	NSS_WIFI_MESH_MPP_ADD,
	NSS_WIFI_MESH_MPP_UPDATE,
	NSS_WIFI_MESH_MPP_DEL,
};

enum nss_wifi_mesh_event {
	NSS_WIFI_MESH_EV_REFRESH,	/* path in use and due for a refresh */
	NSS_WIFI_MESH_EV_NOT_FOUND,	/* no path to da; ta set if forwarded */
	NSS_WIFI_MESH_EV_MPATH_DEL,
	NSS_WIFI_MESH_EV_EXPIRY,
	NSS_WIFI_MESH_EV_MPP_LEARN,
	NSS_WIFI_MESH_EV_MPP_ADD,
	NSS_WIFI_MESH_EV_MPP_UPDATE,	/* also: proxy still in use */
};

#define NSS_WIFI_MESH_PATH_ACTIVE	0x01
#define NSS_WIFI_MESH_PATH_RESOLVING	0x02
#define NSS_WIFI_MESH_PATH_RESOLVED	0x04
#define NSS_WIFI_MESH_PATH_FIXED	0x08

struct nss_wifi_mesh_path {
	u8 mesh_da[ETH_ALEN];	/* path destination, or the proxy of da */
	u8 da[ETH_ALEN];
	u8 next_hop[ETH_ALEN];
	u8 old_next_hop[ETH_ALEN];
	u8 ta[ETH_ALEN];
	u32 metric;
	u32 exp_ms;
	u8 hop_count;
	u8 flags;		/* NSS_WIFI_MESH_PATH_* */
	bool gate;
};

/* Both are called from softirq. rx gets an 802.11 frame at skb->data with
 * the hardware receive descriptor at skb->head.
 */
struct nss_wifi_mesh_ops {
	void (*event)(void *priv, enum nss_wifi_mesh_event ev,
		      const struct nss_wifi_mesh_path *p);
	void (*rx)(void *priv, struct sk_buff *skb, struct napi_struct *napi);
};

#if IS_REACHABLE(CONFIG_NSS_WIFI_DRV)
int nss_wifi_soc_register_v2(const struct nss_wifi_soc *soc);
void nss_wifi_soc_unregister(int soc);
int nss_wifi_pdev_register(int soc, const struct nss_wifi_pdev *pdev);
int nss_wifi_vdev_register(int soc, struct net_device *dev, u8 radio,
			   u32 vdev_id, const u8 *mac, bool ap, u8 encap,
			   u8 decap);
void nss_wifi_vdev_unregister(struct net_device *dev);
int nss_wifi_wds_vdev_register(struct net_device *dev, struct net_device *parent,
			       u16 peer_id, const u8 *peer);
int nss_wifi_wds_peer(int soc, enum nss_wifi_wds_cmd cmd,
		      const struct nss_wifi_wds_peer *peer);
int nss_wifi_vdev_tx(int if_num, struct sk_buff *skb);
int nss_wifi_peer_create(int soc, u32 vdev_id, const u8 *mac, u16 peer_id,
			 u16 hw_ast_idx, u32 tx_ast_hash);
void nss_wifi_peer_delete(int soc, u32 vdev_id, const u8 *mac, u16 peer_id);
int nss_wifi_vdev_security(int if_num, u32 cipher);
int nss_wifi_vdev_ap_isolate(int if_num, bool isolate);
int nss_wifi_peer_security(int soc, u16 peer_id, bool group, u32 cipher,
			   const u8 *mic_key);
int nss_wifi_peer_authorize(int soc, u16 peer_id, bool authorized);
int nss_flow_setup_block(struct net_device *dev, struct flow_block_offload *f);
int nss_wifi_mesh_register(int soc, struct net_device *dev, u8 radio,
			   u32 vdev_id, const u8 *mac, u8 mode,
			   const struct nss_wifi_mesh_ops *ops, void *priv);
int nss_wifi_mesh_path(int if_num, enum nss_wifi_mesh_cmd cmd,
		       const struct nss_wifi_mesh_path *p);
int nss_wifi_mesh_config(int if_num, u8 ttl, u32 refresh_ms, bool forward);
#else
static inline int nss_wifi_soc_register_v2(const struct nss_wifi_soc *soc)
{
	return -ENODEV;
}

static inline void nss_wifi_soc_unregister(int soc)
{
}

static inline int nss_wifi_pdev_register(int soc,
					 const struct nss_wifi_pdev *pdev)
{
	return -ENODEV;
}

static inline int nss_wifi_vdev_register(int soc, struct net_device *dev,
					 u8 radio, u32 vdev_id, const u8 *mac,
					 bool ap, u8 encap, u8 decap)
{
	return -ENODEV;
}

static inline void nss_wifi_vdev_unregister(struct net_device *dev)
{
}

static inline int nss_wifi_wds_vdev_register(struct net_device *dev,
					     struct net_device *parent,
					     u16 peer_id, const u8 *peer)
{
	return -ENODEV;
}

static inline int nss_wifi_wds_peer(int soc, enum nss_wifi_wds_cmd cmd,
				    const struct nss_wifi_wds_peer *peer)
{
	return -ENODEV;
}

static inline int nss_wifi_vdev_tx(int if_num, struct sk_buff *skb)
{
	return -ENODEV;
}

static inline int nss_wifi_peer_create(int soc, u32 vdev_id, const u8 *mac,
				       u16 peer_id, u16 hw_ast_idx,
				       u32 tx_ast_hash)
{
	return -ENODEV;
}

static inline void nss_wifi_peer_delete(int soc, u32 vdev_id, const u8 *mac,
					u16 peer_id)
{
}

static inline int nss_wifi_vdev_security(int if_num, u32 cipher)
{
	return -ENODEV;
}

static inline int nss_wifi_vdev_ap_isolate(int if_num, bool isolate)
{
	return -ENODEV;
}

static inline int nss_wifi_peer_security(int soc, u16 peer_id, bool group,
					 u32 cipher, const u8 *mic_key)
{
	return -ENODEV;
}

static inline int nss_wifi_peer_authorize(int soc, u16 peer_id,
					  bool authorized)
{
	return -ENODEV;
}

static inline int nss_flow_setup_block(struct net_device *dev,
				       struct flow_block_offload *f)
{
	return -EOPNOTSUPP;
}

static inline int nss_wifi_mesh_register(int soc, struct net_device *dev,
					 u8 radio, u32 vdev_id, const u8 *mac,
					 u8 mode,
					 const struct nss_wifi_mesh_ops *ops,
					 void *priv)
{
	return -EOPNOTSUPP;
}

static inline int nss_wifi_mesh_path(int if_num, enum nss_wifi_mesh_cmd cmd,
				     const struct nss_wifi_mesh_path *p)
{
	return -EOPNOTSUPP;
}

static inline int nss_wifi_mesh_config(int if_num, u8 ttl, u32 refresh_ms,
				       bool forward)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* __LINUX_SOC_QCOM_NSS_WIFI_H__ */
