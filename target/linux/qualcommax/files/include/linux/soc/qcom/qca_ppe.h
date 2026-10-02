/* SPDX-License-Identifier: GPL-2.0-or-later OR MIT */
#ifndef __LINUX_SOC_QCOM_QCA_PPE_H__
#define __LINUX_SOC_QCOM_QCA_PPE_H__
#include <linux/netdevice.h>
#include <linux/skbuff.h>

/* Dedicated native WLAN CPU-return service. Unlike QSDK's no-edit RFS
 * configuration, this service must retain route/NAT editing and publish its
 * marker in the EDMA preheader before handing the frame to WLAN TX. */
#define QCA_PPE_WIFI_SERVICE_CODE 21
#define QCA_PPE_WIFI_INGRESS_VID_BASE 0xfd0
#define QCA_PPE_WIFI_INGRESS_SLOTS 8

/* QSDK's NOEDIT_REDIR_COREx queue map uses the common redirect profile. */
#define QCA_PPE_REDIRECT_PROFILE_ID 9

/* Only marked, checksum-valid CPU-return frames reach this consuming API. */
void qca_ppe_wifi_xmit(struct sk_buff *skb, u8 iport);

/* EDMA owns the callback; unregister waits for active ingress readers.
 * xmit takes a caller-owned, writable linear skb with headroom bytes free.
 * Return 0 consumes the skb: completion may free it before xmit returns.
 * Errors retain caller ownership and restore data/len after any EDMA push;
 * the caller is responsible for undoing its own Ethernet/S-tag preparation. */
struct qca_ppe_wifi_inject_ops {
	struct net_device *dev;
	unsigned int headroom;
	int (*xmit)(struct net_device *dev, struct sk_buff *skb);
};
void qca_ppe_wifi_inject_register(const struct qca_ppe_wifi_inject_ops *ops);
void qca_ppe_wifi_inject_unregister(const struct qca_ppe_wifi_inject_ops *ops);
bool qca_ppe_wifi_ingress_return(struct sk_buff *skb, u8 source_port);
#endif
