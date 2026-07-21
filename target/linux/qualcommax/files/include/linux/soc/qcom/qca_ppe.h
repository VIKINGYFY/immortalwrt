/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PPE state exported by the qca-ppe DSA driver to the NSS data-plane
 * glue.
 *
 * qca-ppe is the single writer of all PPE tables; other modules
 * observe its decisions (or request firmware-side resources) through
 * these calls instead of touching the registers themselves.
 */

#ifndef __LINUX_SOC_QCOM_QCA_PPE_H__
#define __LINUX_SOC_QCOM_QCA_PPE_H__

#include <linux/netdevice.h>

int qca_ppe_port_fw_vsi_get(struct net_device *netdev);
int qca_ppe_port_fw_vsi_refresh(struct net_device *netdev);

/*
 * Bridge offload helpers for the NSS bridge manager. qca-ppe owns the shared
 * per-bridge VSI (allocated when DSA ports join the Linux bridge); the bridge
 * manager reuses it for the firmware bridge interface and asks qca-ppe to
 * re-assert masks / set STP / flush FDB rather than touching PPE tables itself.
 */
int qca_ppe_bridge_vsi_get(struct net_device *br_dev);
int qca_ppe_bridge_vsi_refresh(struct net_device *br_dev);
int qca_ppe_fdb_del(const unsigned char *addr, u32 vsi);

/*
 * Port VLAN helpers for the NSS VLAN manager: qca-ppe allocates the VSI and
 * programs the ingress translation and egress tagging for a single-tagged
 * 802.1Q upper of a DSA user port; the manager attaches the returned VSI to
 * the firmware VLAN interface and re-asserts the masks after the attach.
 */
int qca_ppe_port_vlan_vsi_get(struct net_device *netdev, u16 vid);
int qca_ppe_port_vlan_vsi_refresh(struct net_device *netdev, u16 vid);
int qca_ppe_port_vlan_vsi_put(struct net_device *netdev, u16 vid);

#endif /* __LINUX_SOC_QCOM_QCA_PPE_H__ */
