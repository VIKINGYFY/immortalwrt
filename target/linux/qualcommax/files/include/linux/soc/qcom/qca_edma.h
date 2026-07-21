/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Per-port datapath ownership, arbitrated by the qca-edma conduit driver.
 *
 * A delegate claims all injection into one PPE port's fabric path
 * (resolved from the DSA out-of-band tag): conduit TX for the port is
 * handed to the owner's xmit before the frame touches the host EDMA
 * rings, so an alternative data plane (the NSS firmware fast path)
 * transmits it instead. At any instant exactly one actor - the host
 * stack or the registered owner - may inject into a port's fabric path.
 *
 * The driver decides WHEN the owner may inject: entering a port
 * transition (link down, reconfiguration, port disable) it synchronously
 * revokes injection before the transition touches hardware, and
 * re-grants once link-up completes. A claim on a link-up port is granted
 * synchronously from the claim. While revoked, conduit TX for the port
 * is dropped.
 *
 * revoke/grant run in process context, may sleep, and must not call back
 * into the claim/release API. xmit runs in the conduit's ndo_start_xmit
 * context (BH, RCU read side) and takes ownership of the skb regardless
 * of its return value, except for NETDEV_TX_BUSY which requeues
 * upstream.
 */

#ifndef __LINUX_SOC_QCOM_QCA_EDMA_H__
#define __LINUX_SOC_QCOM_QCA_EDMA_H__

#include <linux/netdevice.h>

/* PPE port ids carried in the OOB tag; physical ports are 1..6 */
#define QCA_EDMA_DP_MAX_PORT	7

struct qca_edma_dp_owner {
	netdev_tx_t (*xmit)(struct sk_buff *skb, void *ctx);
	void (*revoke)(void *ctx);
	void (*grant)(void *ctx);
};

#if IS_REACHABLE(CONFIG_QCOM_EDMA)
bool qca_edma_netdev_is_conduit(const struct net_device *netdev);
int qca_edma_fw_baseline_restore(struct net_device *conduit);
int qca_edma_port_dp_claim(struct net_device *conduit, unsigned int port,
			   const struct qca_edma_dp_owner *owner, void *ctx);
int qca_edma_port_dp_release(struct net_device *conduit, unsigned int port);
void qca_edma_port_transition_begin(struct net_device *conduit,
				    unsigned int port);
void qca_edma_port_transition_end(struct net_device *conduit,
				  unsigned int port);
#else
static inline bool qca_edma_netdev_is_conduit(const struct net_device *netdev)
{
	return false;
}

static inline int qca_edma_fw_baseline_restore(struct net_device *conduit)
{
	return -ENODEV;
}

static inline int qca_edma_port_dp_claim(struct net_device *conduit,
					 unsigned int port,
					 const struct qca_edma_dp_owner *owner,
					 void *ctx)
{
	return -ENODEV;
}

static inline int qca_edma_port_dp_release(struct net_device *conduit,
					   unsigned int port)
{
	return -ENODEV;
}

static inline void qca_edma_port_transition_begin(struct net_device *conduit,
						  unsigned int port)
{
}

static inline void qca_edma_port_transition_end(struct net_device *conduit,
						unsigned int port)
{
}
#endif

#endif /* __LINUX_SOC_QCOM_QCA_EDMA_H__ */
