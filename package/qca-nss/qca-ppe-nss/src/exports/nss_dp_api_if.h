/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nss_dp_api_if.h
 *	NSS data-plane glue API for the qca_edma/qca_ppe ethernet stack.
 *
 * Drop-in replacement for the header of the same name exported by the
 * removed qca-nss-dp driver. qca-nss-drv compiles against this header
 * unmodified; the implementation lives in qca-ppe-nss.ko, which binds
 * the NSS data-plane override to the DSA user ports created by qca_ppe
 * instead of nss-dp's own netdevs.
 *
 * Constants below are the IPQ807x values from qca-nss-dp
 * hal/soc_ops/ipq807x/nss_ipq807x.h (NSS phys_if numbers 1..6).
 */

#ifndef __NSS_DP_API_IF_H
#define __NSS_DP_API_IF_H

#include <linux/netdevice.h>
#include <linux/skbuff.h>

/*
 * NSS DP status
 */
#define NSS_DP_SUCCESS	0
#define NSS_DP_FAILURE	-1

/*
 * IPQ807x platform defines (from nss-dp nss_ipq807x.h)
 */
#define NSS_DP_HAL_MAX_PORTS	6
#define NSS_DP_HAL_START_IFNUM	1
#define NSS_DP_START_IFNUM	NSS_DP_HAL_START_IFNUM
#define NSS_DP_MAX_INTERFACES	(NSS_DP_HAL_MAX_PORTS + NSS_DP_START_IFNUM)
#define NSS_DP_PREHEADER_SIZE	32

/*
 * NSS PTP service code
 */
#define NSS_PTP_EVENT_SERVICE_CODE	0x9

/**
 * nss_dp_data_plane_ctx
 *	Data plane context base class.
 */
struct nss_dp_data_plane_ctx {
	struct net_device *dev;
};

/*
 * Empty on IPQ807x, exactly as in nss-dp (hal/soc_ops/ipq807x): EDMA
 * stats do not flow through phys_if, but nss-drv embeds this by value
 * in struct nss_data_plane_param so it must be a complete type.
 */
struct nss_dp_hal_gmac_stats {
};

struct nss_dp_gmac_stats {
	struct nss_dp_hal_gmac_stats stats;
};

/**
 * nss_dp_data_plane_ops
 *	Per data-plane ops structure, installed by qca-nss-drv via
 *	nss_dp_override_data_plane().
 */
struct nss_dp_data_plane_ops {
	int (*init)(struct nss_dp_data_plane_ctx *dpc);
	int (*open)(struct nss_dp_data_plane_ctx *dpc, uint32_t tx_desc_ring,
		    uint32_t rx_desc_ring, uint32_t mode);
	int (*close)(struct nss_dp_data_plane_ctx *dpc);
	int (*link_state)(struct nss_dp_data_plane_ctx *dpc,
			  uint32_t link_state);
	int (*mac_addr)(struct nss_dp_data_plane_ctx *dpc, uint8_t *addr);
	int (*change_mtu)(struct nss_dp_data_plane_ctx *dpc, uint32_t mtu);
	netdev_tx_t (*xmit)(struct nss_dp_data_plane_ctx *dpc,
			    struct sk_buff *os_buf);
	void (*set_features)(struct nss_dp_data_plane_ctx *dpc);
	int (*pause_on_off)(struct nss_dp_data_plane_ctx *dpc,
			    uint32_t pause_on);
	int (*vsi_assign)(struct nss_dp_data_plane_ctx *dpc, uint32_t vsi);
	int (*vsi_unassign)(struct nss_dp_data_plane_ctx *dpc, uint32_t vsi);
	int (*rx_flow_steer)(struct nss_dp_data_plane_ctx *dpc,
			     struct sk_buff *skb, uint32_t cpu, bool is_add);
	void (*get_stats)(struct nss_dp_data_plane_ctx *dpc,
			  struct nss_dp_gmac_stats *stats);
	int (*deinit)(struct nss_dp_data_plane_ctx *dpc);
};

/*
 * API consumed by qca-nss-drv (see nss_data_plane/nss_data_plane.c)
 */
struct net_device *nss_dp_get_netdev_by_nss_if_num(int if_num);
bool nss_dp_is_in_open_state(struct net_device *netdev);
int nss_dp_override_data_plane(struct net_device *netdev,
			       struct nss_dp_data_plane_ops *dp_ops,
			       struct nss_dp_data_plane_ctx *dpc);
void nss_dp_start_data_plane(struct net_device *netdev,
			     struct nss_dp_data_plane_ctx *dpc);
void nss_dp_restore_data_plane(struct net_device *netdev);
void nss_dp_receive(struct net_device *netdev, struct sk_buff *skb,
		    struct napi_struct *napi);
void nss_phy_tstamp_rx_buf(void *app_data, struct sk_buff *skb);

/*
 * Probe gate: qca-nss-drv calls this at the top of its platform probe.
 * Returns 0 when the data plane is armed (firmware boot may proceed) or
 * -EPROBE_DEFER while no port is armed; deferred devices are re-attached
 * when fw_mask first becomes non-zero. Keeps Wi-Fi's module dependency on
 * qca-nss-drv from booting the NSS firmware unarmed at boot.
 */
int nss_dp_probe_gate(struct device *dev);

#endif /* __NSS_DP_API_IF_H */
