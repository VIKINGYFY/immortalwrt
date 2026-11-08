/* SPDX-License-Identifier: GPL-2.0-only */
/* The NSS firmware's message ABI, as far as this driver speaks it.
 *
 * Every layout here is fixed by the firmware. Space the driver never reads or
 * writes is kept as reserved bytes, and a message union carries only the
 * messages the driver uses, sized so that the whole message keeps the size
 * the firmware's own union gives it. The layouts are those of 12.5; struct
 * nss_fw_line holds where 11.4 differs.
 */

#ifndef __NSS_FW_H
#define __NSS_FW_H

#include <linux/if_ether.h>
#include <linux/types.h>

/* Common message header. */

#define NSS_CMN_RESPONSE_ACK	0
#define NSS_CMN_RESPONSE_EMSG	4
#define NSS_CMN_RESPONSE_NOTIFY	5

/* The top byte of an interface number the firmware reports names the core. */
#define NSS_INTERFACE_NUM_GET(interface)	((interface) & 0xffffff)

struct nss_cmn_msg {
	u16 version;
	u16 len;
	u32 interface;
	u32 response;
	u32 type;
	u32 error;
	u32 reserved;
	u64 cb;
	u64 app_data;
};

struct nss_cmn_node_stats {
	u32 rx_packets;
	u32 rx_bytes;
	u32 tx_packets;
	u32 tx_bytes;
	u32 rx_dropped[4];
};

/* Node statistics of the Ethernet receive, IPv4 and IPv6 nodes: the common
 * block, then 3 (Ethernet), 20 (IPv4) or 21 (IPv6) counters of their own,
 * then one counter per exception event, cumulative.
 */
#define NSS_IPV4_NODE_EXCEPTIONS_OFF	(sizeof(struct nss_cmn_node_stats) + 20 * 4)
#define NSS_IPV6_NODE_EXCEPTIONS_OFF	(sizeof(struct nss_cmn_node_stats) + 21 * 4)
#define NSS_ETH_RX_NODE_EXCEPTIONS_OFF	(sizeof(struct nss_cmn_node_stats) + 3 * 4)
#define NSS_NODE_EXCEPTIONS_MAX		96
#define NSS_ETH_RX_NODE_STATS_SYNC_MSG	0

/* N2H statistics prefix from QSDK R50 nss_n2h.h. The free counts are
 * snapshots; allocation-failure fields are deltas since the last sync.
 */
struct nss_n2h_pbuf_stats {
	u32 total;
	u32 free;
	u32 alloc_with_payload_failed;
	u32 alloc_without_payload_failed;
};

struct nss_n2h_buffer_stats {
	struct nss_cmn_node_stats node;
	u32 queue_dropped;
	u32 total_ticks;
	u32 worst_ticks;
	u32 iterations;
	struct nss_n2h_pbuf_stats ocm;
	struct nss_n2h_pbuf_stats ddr;
	u32 payload_alloc_failed;
	u32 payload_free;
};

/* Dynamic interfaces. */

#define NSS_DYNAMIC_INTERFACE_ALLOC_NODE	0
#define NSS_DYNAMIC_INTERFACE_DEALLOC_NODE	1
#define NSS_DYNAMIC_INTERFACE_MAX		2

enum nss_dynamic_interface_type {
	NSS_DYNAMIC_INTERFACE_TYPE_VAP = 6,
	NSS_DYNAMIC_INTERFACE_TYPE_PPPOE = 8,
	NSS_DYNAMIC_INTERFACE_TYPE_VLAN = 17,
	NSS_DYNAMIC_INTERFACE_TYPE_WIFILI_INTERNAL = 19,
	NSS_DYNAMIC_INTERFACE_TYPE_GENERIC_REDIR_N2H = 32,
	NSS_DYNAMIC_INTERFACE_TYPE_GENERIC_REDIR_H2N = 33,
	NSS_DYNAMIC_INTERFACE_TYPE_WIFILI_EXTERNAL0 = 61,
	NSS_DYNAMIC_INTERFACE_TYPE_WIFILI_EXTERNAL1 = 62,
	NSS_DYNAMIC_INTERFACE_TYPE_WIFI_EXT_VDEV_WDS = 66,
	NSS_DYNAMIC_INTERFACE_TYPE_WIFI_MESH_INNER = 70,
	NSS_DYNAMIC_INTERFACE_TYPE_WIFI_MESH_OUTER = 71,
};

struct nss_dynamic_interface_alloc_node_msg {
	u32 type;
	s32 if_num;
};

struct nss_dynamic_interface_dealloc_node_msg {
	u32 type;
	s32 if_num;
};

struct nss_dynamic_interface_msg {
	struct nss_cmn_msg cm;
	union {
		struct nss_dynamic_interface_alloc_node_msg alloc_node;
		struct nss_dynamic_interface_dealloc_node_msg dealloc_node;
	} msg;
};

/* Generic interface messages, understood by every node. */

#define NSS_IF_OPEN		0
#define NSS_IF_CLOSE		1
#define NSS_IF_LINK_STATE_NOTIFY	2
#define NSS_IF_MTU_CHANGE	3
#define NSS_IF_MAC_ADDR_SET	4

#define NSS_IF_DATA_ALIGN_2BYTE	0

struct nss_if_open {
	u32 tx_desc_ring;
	u32 rx_desc_ring;
	u32 rx_forward_if;
	u32 alignment_mode;
};

struct nss_if_close {
	u32 reserved;
};

struct nss_if_link_state_notify {
	u32 state;
};

struct nss_if_mtu_change {
	u16 min_buf_size;
};

struct nss_if_mac_address_set {
	u8 mac_addr[ETH_ALEN];
};

struct nss_if_msg {
	struct nss_cmn_msg cm;
	u8 msg[56];
};

/* The configuration of each node of a generic redirect pair. */

#define NSS_VIRT_IF_TX_CONFIG_MSG	10000

struct nss_virt_if_config_msg {
	u32 flags;
	u32 sibling;
	u32 nexthop;
	u8 mac_addr[ETH_ALEN];
};

/* IPv4 and IPv6 connection rules. Addresses and ports are in host order. */

#define NSS_IPV4_TX_CREATE_RULE_MSG		0
#define NSS_IPV4_TX_DESTROY_RULE_MSG		1
#define NSS_IPV4_RX_CONN_STATS_SYNC_MSG		3
#define NSS_IPV4_RX_NODE_STATS_SYNC_MSG		4
#define NSS_IPV4_TX_CONN_CFG_RULE_MSG		5
#define NSS_IPV4_TX_CREATE_MC_RULE_MSG		6
#define NSS_IPV4_TX_CONN_STATS_SYNC_MANY_MSG	7
#define NSS_IPV4_TX_ACCEL_MODE_CFG_MSG		8
#define NSS_IPV4_TX_CONN_TABLE_SIZE_MSG		10

/* R50 connection-table negotiation uses network order for counts/sizes,
 * but native 32-bit DMA addresses for the host-owned CE/CME tables.
 * IPv4 and IPv6 use the same layouts and message numbers.
 */
struct nss_conn_table_size_msg {
	struct nss_cmn_msg cm;
	__be32 num_conn;
	__be32 ce_table_size;
	__be32 cme_table_size;
};

struct nss_conn_cfg_msg {
	struct nss_cmn_msg cm;
	__be32 num_conn;
	u32 ce_mem;
	u32 cme_mem;
};

/* Where the IPv4 and IPv6 engines accelerate: in the cores, or handed to the
 * switch, which they boot with.
 */
#define NSS_ACCEL_MODE_CORE			0

struct nss_accel_mode_cfg {
	struct nss_cmn_msg cm;
	__be32 mode;
};

#define NSS_IPV4_RULE_SYNC_REASON_FLUSH		1
#define NSS_IPV4_RULE_SYNC_REASON_EVICT		2
#define NSS_IPV4_CR_BUFFER_ALLOC_FAIL_ERROR	4
#define NSS_IPV4_DR_NO_CONNECTION_ENTRY_ERROR	5

#define NSS_IPV4_RULE_CREATE_FLAG_NO_SEQ_CHECK	0x01
#define NSS_IPV4_RULE_CREATE_FLAG_BRIDGE_FLOW	0x02
#define NSS_IPV4_RULE_CREATE_FLAG_ROUTED	0x04

#define NSS_IPV4_RULE_CREATE_CONN_VALID		0x001
#define NSS_IPV4_RULE_CREATE_TCP_VALID		0x002
#define NSS_IPV4_RULE_CREATE_PPPOE_VALID	0x004
#define NSS_IPV4_RULE_CREATE_QOS_VALID		0x008
#define NSS_IPV4_RULE_CREATE_VLAN_VALID		0x010
#define NSS_IPV4_RULE_CREATE_NEXTHOP_VALID	0x100

#define NSS_IPV4_MC_RULE_CREATE_FLAG_MC_UPDATE		0x01
#define NSS_IPV4_MC_RULE_CREATE_FLAG_INGRESS_VLAN_VALID	0x04
#define NSS_IPV4_MC_RULE_CREATE_FLAG_INGRESS_PPPOE	0x08

#define NSS_IPV4_MC_RULE_CREATE_IF_FLAG_VLAN_VALID	0x01
#define NSS_IPV4_MC_RULE_CREATE_IF_FLAG_ROUTED_FLOW	0x02
#define NSS_IPV4_MC_RULE_CREATE_IF_FLAG_JOIN		0x04
#define NSS_IPV4_MC_RULE_CREATE_IF_FLAG_LEAVE		0x08

#define NSS_IPV6_TX_CREATE_RULE_MSG		0
#define NSS_IPV6_TX_DESTROY_RULE_MSG		1
#define NSS_IPV6_RX_CONN_STATS_SYNC_MSG		3
#define NSS_IPV6_RX_NODE_STATS_SYNC_MSG		4
#define NSS_IPV6_TX_CREATE_MC_RULE_MSG		6
#define NSS_IPV6_TX_CONN_STATS_SYNC_MANY_MSG	7
#define NSS_IPV6_TX_ACCEL_MODE_CFG_MSG		8

#define NSS_IPV6_RULE_SYNC_REASON_FLUSH		1
#define NSS_IPV6_RULE_SYNC_REASON_EVICT		2
#define NSS_IPV6_CR_BUFFER_ALLOC_FAIL_ERROR	4
#define NSS_IPV6_DR_NO_CONNECTION_ENTRY_ERROR	5

#define NSS_IPV6_RULE_CREATE_FLAG_NO_SEQ_CHECK	0x01
#define NSS_IPV6_RULE_CREATE_FLAG_BRIDGE_FLOW	0x02
#define NSS_IPV6_RULE_CREATE_FLAG_ROUTED	0x04

#define NSS_IPV6_RULE_CREATE_CONN_VALID		0x001
#define NSS_IPV6_RULE_CREATE_TCP_VALID		0x002
#define NSS_IPV6_RULE_CREATE_PPPOE_VALID	0x004
#define NSS_IPV6_RULE_CREATE_QOS_VALID		0x008
#define NSS_IPV6_RULE_CREATE_VLAN_VALID		0x010
#define NSS_IPV6_RULE_CREATE_NEXTHOP_VALID	0x100

#define NSS_IPV6_MC_RULE_CREATE_FLAG_MC_UPDATE		0x01
#define NSS_IPV6_MC_RULE_CREATE_FLAG_INGRESS_VLAN_VALID	0x04
#define NSS_IPV6_MC_RULE_CREATE_FLAG_INGRESS_PPPOE	0x08

#define NSS_IPV6_MC_RULE_CREATE_IF_FLAG_VLAN_VALID	0x01
#define NSS_IPV6_MC_RULE_CREATE_IF_FLAG_ROUTED_FLOW	0x02
#define NSS_IPV6_MC_RULE_CREATE_IF_FLAG_JOIN		0x04
#define NSS_IPV6_MC_RULE_CREATE_IF_FLAG_LEAVE		0x08

#define NSS_MC_IF_MAX	16

struct nss_ipv4_5tuple {
	u32 flow_ip;
	u32 flow_ident;
	u32 return_ip;
	u32 return_ident;
	u8 protocol;
	u8 reserved[3];
};

struct nss_ipv4_connection_rule {
	u16 flow_mac[3];
	u16 return_mac[3];
	s32 flow_interface_num;
	s32 return_interface_num;
	u32 flow_mtu;
	u32 return_mtu;
	u32 flow_ip_xlate;
	u32 return_ip_xlate;
	u32 flow_ident_xlate;
	u32 return_ident_xlate;
};

struct nss_ipv4_protocol_tcp_rule {
	u32 flow_max_window;
	u32 return_max_window;
	u32 flow_end;
	u32 return_end;
	u32 flow_max_end;
	u32 return_max_end;
	u8 flow_window_scale;
	u8 return_window_scale;
	u16 reserved;
};

struct nss_ipv4_pppoe_rule {
	u32 flow_if_exist;
	s32 flow_if_num;
	u32 return_if_exist;
	s32 return_if_num;
};

struct nss_ipv4_qos_rule {
	u32 flow_qos_tag;
	u32 return_qos_tag;
};

struct nss_ipv4_vlan_rule {
	u32 ingress_vlan_tag;
	u32 egress_vlan_tag;
};

struct nss_ipv4_nexthop {
	s32 flow_nexthop;
	s32 return_nexthop;
};

struct nss_ipv4_rule_create_msg {
	u16 valid_flags;
	u16 rule_flags;
	struct nss_ipv4_5tuple tuple;
	struct nss_ipv4_connection_rule conn_rule;
	struct nss_ipv4_protocol_tcp_rule tcp_rule;
	struct nss_ipv4_pppoe_rule pppoe_rule;
	struct nss_ipv4_qos_rule qos_rule;
	u32 rsvd0;
	struct nss_ipv4_vlan_rule vlan_primary_rule;
	struct nss_ipv4_vlan_rule vlan_secondary_rule;
	u32 rsvd1[4];
	struct nss_ipv4_nexthop nexthop_rule;
	u32 rsvd2[8];
};

struct nss_ipv4_rule_destroy_msg {
	struct nss_ipv4_5tuple tuple;
};

struct nss_ipv4_mc_if_rule {
	u16 rule_flags;
	u16 valid_flags;
	u32 xlate_src_ip;
	u32 xlate_src_ident;
	u32 egress_vlan_tag[2];
	s32 pppoe_if_num;
	u32 if_num;
	u32 if_mtu;
	u16 if_mac[3];
	u8 reserved[2];
};

struct nss_ipv4_mc_rule_create_msg {
	struct nss_ipv4_5tuple tuple;
	u32 rule_flags;
	u32 valid_flags;
	u32 src_interface_num;
	u32 ingress_vlan_tag[2];
	u32 qos_tag;
	u16 dest_mac[3];
	u16 if_count;
	u8 egress_dscp;
	u8 reserved[1];
	u16 igs_qos_tag;
	struct nss_ipv4_mc_if_rule if_rule[NSS_MC_IF_MAX];
};

struct nss_ipv4_conn_sync {
	u32 reserved;
	u8 protocol;
	u32 flow_ip;
	u32 flow_ip_xlate;
	u32 flow_ident;
	u32 flow_ident_xlate;
	u32 flow_max_window;
	u32 flow_end;
	u32 flow_max_end;
	u32 flow_rx_packet_count;
	u32 flow_rx_byte_count;
	u32 flow_tx_packet_count;
	u32 flow_tx_byte_count;
	u32 return_ip;
	u32 return_ip_xlate;
	u32 return_ident;
	u32 return_ident_xlate;
	u32 return_max_window;
	u32 return_end;
	u32 return_max_end;
	u32 return_rx_packet_count;
	u32 return_rx_byte_count;
	u32 return_tx_packet_count;
	u32 return_tx_byte_count;
	u32 inc_ticks;
	u32 reason;
	u8 flags;
	u32 qos_tag;
	u32 cause;
};

struct nss_ipv4_conn_sync_many_msg {
	u16 index;
	u16 size;
	u16 next;
	u16 count;
	struct nss_ipv4_conn_sync conn_sync[];
};

struct nss_ipv4_msg {
	struct nss_cmn_msg cm;
	union {
		struct nss_ipv4_rule_create_msg rule_create;
		struct nss_ipv4_rule_destroy_msg rule_destroy;
		struct nss_ipv4_mc_rule_create_msg mc_rule_create;
		struct nss_ipv4_conn_sync_many_msg conn_stats_many;
		struct nss_ipv4_conn_sync conn_stats;
	} msg;
};

struct nss_ipv6_5tuple {
	u32 flow_ip[4];
	u32 flow_ident;
	u32 return_ip[4];
	u32 return_ident;
	u8 protocol;
	u8 reserved[3];
};

struct nss_ipv6_connection_rule {
	u16 flow_mac[3];
	u16 return_mac[3];
	s32 flow_interface_num;
	s32 return_interface_num;
	u32 flow_mtu;
	u32 return_mtu;
};

/* Not the IPv4 order: the flow direction's three words come first. */
struct nss_ipv6_protocol_tcp_rule {
	u32 flow_max_window;
	u32 flow_end;
	u32 flow_max_end;
	u32 return_max_window;
	u32 return_end;
	u32 return_max_end;
	u8 flow_window_scale;
	u8 return_window_scale;
	u16 reserved;
};

struct nss_ipv6_pppoe_rule {
	u32 flow_if_exist;
	s32 flow_if_num;
	u32 return_if_exist;
	s32 return_if_num;
};

struct nss_ipv6_qos_rule {
	u32 flow_qos_tag;
	u32 return_qos_tag;
};

struct nss_ipv6_vlan_rule {
	u32 ingress_vlan_tag;
	u32 egress_vlan_tag;
};

struct nss_ipv6_nexthop {
	s32 flow_nexthop;
	s32 return_nexthop;
};

struct nss_ipv6_rule_create_msg {
	u16 valid_flags;
	u16 rule_flags;
	struct nss_ipv6_5tuple tuple;
	struct nss_ipv6_connection_rule conn_rule;
	struct nss_ipv6_protocol_tcp_rule tcp_rule;
	struct nss_ipv6_pppoe_rule pppoe_rule;
	struct nss_ipv6_qos_rule qos_rule;
	u32 rsvd0;
	struct nss_ipv6_vlan_rule vlan_primary_rule;
	struct nss_ipv6_vlan_rule vlan_secondary_rule;
	u32 rsvd1[4];
	struct nss_ipv6_nexthop nexthop_rule;
	u32 rsvd2[8];
};

struct nss_ipv6_rule_destroy_msg {
	struct nss_ipv6_5tuple tuple;
};

struct nss_ipv6_mc_if_rule {
	u16 rule_flags;
	u16 valid_flags;
	u32 egress_vlan_tag[2];
	s32 pppoe_if_num;
	u32 if_num;
	u32 if_mtu;
	u16 if_mac[3];
	u8 reserved[2];
};

struct nss_ipv6_mc_rule_create_msg {
	struct nss_ipv6_5tuple tuple;
	u32 rule_flags;
	u32 valid_flags;
	u32 src_interface_num;
	u32 ingress_vlan_tag[2];
	u32 qos_tag;
	u16 dest_mac[3];
	u16 if_count;
	u8 egress_dscp;
	u8 reserved[1];
	u16 igs_qos_tag;
	struct nss_ipv6_mc_if_rule if_rule[NSS_MC_IF_MAX];
};

struct nss_ipv6_conn_sync {
	u32 reserved;
	u8 protocol;
	u32 flow_ip[4];
	u32 flow_ident;
	u32 flow_max_window;
	u32 flow_end;
	u32 flow_max_end;
	u32 flow_rx_packet_count;
	u32 flow_rx_byte_count;
	u32 flow_tx_packet_count;
	u32 flow_tx_byte_count;
	u32 return_ip[4];
	u32 return_ident;
	u32 return_max_window;
	u32 return_end;
	u32 return_max_end;
	u32 return_rx_packet_count;
	u32 return_rx_byte_count;
	u32 return_tx_packet_count;
	u32 return_tx_byte_count;
	u32 inc_ticks;
	u32 reason;
	u8 flags;
	u32 qos_tag;
	u32 cause;
};

struct nss_ipv6_conn_sync_many_msg {
	u16 index;
	u16 size;
	u16 next;
	u16 count;
	struct nss_ipv6_conn_sync conn_sync[];
};

struct nss_ipv6_msg {
	struct nss_cmn_msg cm;
	union {
		struct nss_ipv6_rule_create_msg rule_create;
		struct nss_ipv6_rule_destroy_msg rule_destroy;
		struct nss_ipv6_mc_rule_create_msg mc_rule_create;
		struct nss_ipv6_conn_sync_many_msg conn_stats_many;
		struct nss_ipv6_conn_sync conn_stats;
	} msg;
};

/* The wifili node: the Wi-Fi SoC and its radios. */

#define NSS_WIFILI_INIT_MSG			0
#define NSS_WIFILI_SOC_RESET_MSG		1
#define NSS_WIFILI_PDEV_INIT_MSG		2
#define NSS_WIFILI_PDEV_DEINIT_MSG		3
#define NSS_WIFILI_START_MSG			4
#define NSS_WIFILI_STOP_MSG			5
#define NSS_WIFILI_PEER_CREATE_MSG		6
#define NSS_WIFILI_PEER_DELETE_MSG		7
#define NSS_WIFILI_STATS_MSG			10
#define NSS_WIFILI_PEER_STATS_MSG		12
#define NSS_WIFILI_STATS_CFG_MSG		17
#define NSS_WIFILI_LINK_DESC_INFO_MSG		20
#define NSS_WIFILI_PEER_SECURITY_TYPE_MSG	21
#define NSS_WIFILI_RADIO_BUF_CFG		23
#define NSS_WIFILI_STATS_V2_CFG_MSG		32
#define NSS_WIFILI_ISOLATION_MSG		39
#define NSS_WIFILI_PEER_UPDATE_AUTH_FLAG	44
#define NSS_WIFILI_SEND_MESH_CAPABILITY_INFO	45

#define NSS_WIFILI_MAX_TCL_DATA_RINGS_MSG	4
#define NSS_WIFILI_MAX_REO_DATA_RINGS_MSG	4
#define NSS_WIFILI_MAX_PDEV_NUM_MSG		3

struct nss_wifili_hal_srng_info {
	u8 ring_id;
	u8 mac_id;
	u8 resv[2];
	u32 ring_base_paddr;
	u32 num_entries;
	u32 flags;
	u32 ring_dir;
	u32 entry_size;
	u32 low_threshold;
	u32 hwreg_base[2];
};

struct nss_wifili_hal_srng_soc_msg {
	u32 dev_base_addr;
	u32 shadow_rdptr_mem_addr;
	u32 shadow_wrptr_mem_addr;
	u32 lmac_rings_start_id;
};

#define NSS_WIFILI_TX_DESC_PAGES	32

struct nss_wifili_tx_desc_init_msg {
	u32 num_tx_desc;
	u32 num_tx_desc_ext;
	u32 num_pool;
	u32 memory_addr[NSS_WIFILI_TX_DESC_PAGES];
	u32 memory_size[NSS_WIFILI_TX_DESC_PAGES];
	u32 num_memaddr;
	u32 ext_desc_page_num;
	u32 num_tx_desc_2;
	u32 num_tx_desc_ext_2;
	u32 num_tx_desc_3;
	u32 num_tx_desc_ext_3;
	u32 num_tx_device_limit;
};

struct nss_wifili_rx_init_param {
	u16 tlv_size;
	u16 rx_buf_len;
};

struct nss_wifili_init_msg {
	struct nss_wifili_hal_srng_soc_msg hssm;
	u8 num_tcl_data_rings;
	u8 num_reo_dest_rings;
	u8 flags;
	u8 soc_mem_profile;
	struct nss_wifili_hal_srng_info tcl_ring_info[NSS_WIFILI_MAX_TCL_DATA_RINGS_MSG];
	struct nss_wifili_hal_srng_info tx_comp_ring[NSS_WIFILI_MAX_TCL_DATA_RINGS_MSG];
	struct nss_wifili_hal_srng_info reo_dest_ring[NSS_WIFILI_MAX_REO_DATA_RINGS_MSG];
	struct nss_wifili_hal_srng_info reo_exception_ring;
	struct nss_wifili_hal_srng_info rx_rel_ring;
	struct nss_wifili_hal_srng_info reo_reinject_ring;
	struct nss_wifili_tx_desc_init_msg wtdim;
	u32 target_type;
	struct nss_wifili_rx_init_param wrip;
	u32 rsvd[129];
	u32 tx_sw_internode_queue_size;
};

struct nss_wifili_pdev_init_msg {
	struct nss_wifili_hal_srng_info rxdma_ring;
	u32 radio_id;
	u32 hwmode;
	u32 lmac_id;
	u32 num_rx_swdesc;
	u32 target_pdev_id;
	u8 scheme_id;
	u8 reserved[3];
};

/* Names the radio by its index in the instance, not by its node. */
struct nss_wifili_pdev_deinit_msg {
	u32 ifnum;
};

struct nss_wifili_peer_msg {
	u8 peer_mac_addr[ETH_ALEN];
	u16 vdev_id;
	u16 peer_id;
	u16 hw_ast_idx;
	u8 is_nawds;
	u8 pext_stats_valid;
	u16 psta_vdev_id;
	u32 nss_peer_mem;
	u32 tx_ast_hash;
	u32 pext_stats_mem;
	u32 flags;
	u32 peer_memory_size;	/* not on 11.4 */
};

struct nss_wifili_peer_security_type_msg {
	u16 peer_id;
	u8 pkt_type;
	u8 security_type;
	u8 mic_key[8];
};

struct nss_wifili_peer_update_auth_flag {
	u16 peer_id;
	u8 auth_flag;
	u8 reserved;
};

struct nss_wifili_peer_isolation_msg {
	u16 peer_id;
	u16 isolation;
};

struct nss_wifili_rx_reo_ring_stats {
	u32 ring_error;
	u32 ring_reaped;
	u32 invalid_cookie;
	u32 defrag_reaped;
};

struct nss_wifili_rx_sw_pool_stats {
	u32 rx_no_pb;
	u32 desc_alloc;
	u32 desc_alloc_fail;
};

struct nss_wifili_rx_dma_ring_stats {
	u32 rx_hw_desc_unavailable;
	u32 rx_buf_replenished;
};

struct nss_wifili_rx_wbm_ring_stats {
	u32 invalid_buf_mgr;
	u32 err_src_rxdma;
	u32 err_src_rxdma_code_inv;
	u32 err_src_reo;
	u32 err_src_reo_code_nullq;
	u32 err_src_reo_code_inv;
	u32 err_src_invalid;
	u32 err_reo_codes[15];
	u32 err_dma_codes[14];
	u32 err_internal_codes[5];
};

/* The transmit counters that lead the block are 32 bytes shorter on 11.4. */
struct nss_wifili_device_stats {
	u32 rsvd0[115];
	struct nss_wifili_rx_reo_ring_stats rxreo_stats[NSS_WIFILI_MAX_REO_DATA_RINGS_MSG];
	struct nss_wifili_rx_sw_pool_stats rx_sw_pool_stats[NSS_WIFILI_MAX_PDEV_NUM_MSG];
	u32 rsvd1[72];
	struct nss_wifili_rx_dma_ring_stats rxdma_stats[NSS_WIFILI_MAX_PDEV_NUM_MSG];
	struct nss_wifili_rx_wbm_ring_stats rxwbm_stats;
	u32 rsvd2[7];
};

struct nss_wifili_stats_sync_msg {
	struct nss_wifili_device_stats stats;
	u32 target_type;
};

struct nss_wifili_stats_cfg_msg {
	u32 cfg;
};

struct nss_wifili_tx_ctrl_stats {
	u32 ofdma;
	u32 non_amsdu_cnt;
	u32 amsdu_cnt;
	u32 tx_mcast_cnt;
	u32 tx_mcast_bytes;
	u32 tx_ucast_cnt;
	u32 tx_ucast_bytes;
	u32 tx_bcast_bytes;
	u32 tx_bcast_cnt;
	u32 drop_stats[7];
	u32 tx_nawds_mcast_drop_cnt;
	u32 tx_success_cnt;
	u32 tx_success_bytes;
	u32 tx_nawds_mcast_cnt;
	u32 tx_nawds_mcast_bytes;
	u32 retries;
};

/* Both lines agree up to here; 12.5 appends more per peer. */
struct nss_wifili_rx_ctrl_stats {
	u32 mic_err;
	u32 decrypt_err;
	u32 multipass_rx_pkt_drop;
	u32 peer_unauth_rx_pkt_drop;
	u32 reserved_type[2];
	u32 non_amsdu_cnt;
	u32 amsdu_cnt;
	u32 mcast_rcv_cnt;
	u32 mcast_rcv_bytes;
	u32 rx_recvd;
	u32 rx_recvd_bytes;
};

struct nss_wifili_peer_ctrl_stats {
	u32 peer_id;
	struct nss_wifili_tx_ctrl_stats tx;
	struct nss_wifili_rx_ctrl_stats rx;
};

/* The message declares the length of a message of one peer; that less npeers
 * is the stride between peers.
 */
struct nss_wifili_peer_stats_msg {
	u32 npeers;
	struct nss_wifili_peer_ctrl_stats wpcs[];
};

struct nss_wifili_soc_linkdesc_buf_info_msg {
	u32 buffer_addr_low;
	u32 buffer_addr_high;
};

struct nss_wifili_radio_buf_cfg_msg {
	u32 buf_cnt;
	u32 range;
};

struct nss_wifili_radio_cfg_msg {
	u32 radio_if_num;
	union {
		struct nss_wifili_radio_buf_cfg_msg radiobufcfgmsg;
	} radiomsg;
};

struct nss_wifili_mesh_capability_info {
	u8 mesh_enable;
};

struct nss_wifili_msg {
	struct nss_cmn_msg cm;
	union {
		struct nss_wifili_init_msg init;
		struct nss_wifili_pdev_init_msg pdevmsg;
		struct nss_wifili_pdev_deinit_msg pdevdeinit;
		struct nss_wifili_stats_sync_msg wlsoc_stats;
		struct nss_wifili_stats_cfg_msg scm;
		struct nss_wifili_radio_cfg_msg radiocfgmsg;
		struct nss_wifili_soc_linkdesc_buf_info_msg linkdescinfomsg;
		struct nss_wifili_mesh_capability_info cap_info;
		u8 rsvd[2052];
	} msg;
};

/* WDS message layouts shared by firmware lines 11.4 and 12.5. */
#define NSS_WIFILI_WDS_PEER_ADD_MSG	13
#define NSS_WIFILI_WDS_PEER_DEL_MSG	14
#define NSS_WIFILI_WDS_PEER_MAP_MSG	15
#define NSS_WIFILI_WDS_ACTIVE_INFO_MSG	16
#define NSS_WIFILI_WDS_PEER_UPDATE_MSG	31
#define NSS_WIFILI_PEER_MEC_AGEOUT_MSG	37
#define NSS_WIFILI_PEER_4ADDR_EVENT_MSG	42

struct nss_wifili_wds_peer_msg {
	u8 dest_mac[ETH_ALEN];
	u8 peer_mac[ETH_ALEN];
	u8 ast_type;
	u8 pdev_id;
	u16 peer_id;
};

struct nss_wifili_wds_peer_map_msg {
	u8 dest_mac[ETH_ALEN];
	u16 peer_id;
	u16 ast_idx;
	u16 vdev_id;
};

struct nss_wifili_peer_wds_4addr_allow_msg {
	u32 peer_id;
	u32 if_num;
	u8 enable;
	u8 reserved[3];
};

#define NSS_WIFI_EXT_VDEV_CONFIGURE_IF	10000
#define NSS_WIFI_EXT_VDEV_CONFIGURE_WDS	10001
#define NSS_WIFI_EXT_VDEV_SET_NEXT_HOP	10002
#define NSS_WIFI_EXT_VDEV_STATS_SYNC	10003

struct nss_wifi_ext_vdev_configure_if_msg {
	u8 mac_addr[ETH_ALEN];
	u16 radio_ifnum;
	u16 pvap_ifnum;
};

struct nss_wifi_ext_vdev_wds_msg {
	u16 wds_peer_id;
	u8 mac_addr[ETH_ALEN];
};

/* Wi-Fi virtual devices. */

#define NSS_WIFI_VDEV_INTERFACE_CONFIGURE_MSG		10000
#define NSS_WIFI_VDEV_INTERFACE_UP_MSG			10001
#define NSS_WIFI_VDEV_INTERFACE_DOWN_MSG		10002
#define NSS_WIFI_VDEV_INTERFACE_CMD_MSG			10003
#define NSS_WIFI_VDEV_SNOOPLIST_GRP_LIST_CREATE_MSG	10004
#define NSS_WIFI_VDEV_SNOOPLIST_GRP_LIST_DELETE_MSG	10005
#define NSS_WIFI_VDEV_SNOOPLIST_GRP_MEMBER_ADD_MSG	10006
#define NSS_WIFI_VDEV_SNOOPLIST_GRP_MEMBER_REMOVE_MSG	10007
#define NSS_WIFI_VDEV_STATS_MSG				10021
#define NSS_WIFI_VDEV_SET_NEXT_HOP			10022

#define NSS_WIFI_VDEV_ENCAP_TYPE_CMD		1
#define NSS_WIFI_VDEV_DECAP_TYPE_CMD		2
#define NSS_WIFI_VDEV_ENABLE_ME_CMD		3
#define NSS_WIFI_VDEV_CFG_AP_BRIDGE_CMD		14
#define NSS_WIFI_VDEV_SECURITY_TYPE_CMD		15
#define NSS_WIFI_VDEV_CFG_WDS_BACKHAUL_CMD	21
#define NSS_WIFI_VDEV_CFG_MCBC_EXC_TO_HOST_CMD	22

#define NSS_WIFI_VDEV_SNOOPTABLE_GRP_LIST_EXIST		17
#define NSS_WIFI_VDEV_SNOOPTABLE_GRP_MEMBER_EXIST	24

#define WIFI_VDEV_EXT_MODE_MESH_LINK	1

/* Per-packet metadata in the headroom of a buffer handed up as PACKET_EXT. */
#define NSS_WIFI_VDEV_PER_PACKET_METADATA_OFFSET		4
#define NSS_WIFI_VDEV_EXT_DATA_PKT_TYPE_WDS_LEARN			12
#define NSS_WIFI_VDEV_EXT_DATA_PKT_TYPE_4ADDR			17

struct nss_wifi_vdev_wds_per_packet_metadata {
	u16 peer_id;
	u8 is_sa_valid;
	u8 reserved;
	u32 wds_type;
	u8 addr4_valid;
	u8 rsvd;
	u16 sa_idx;
	u16 sa_sw_peer_id;
	u16 pad;
};

struct nss_wifi_vdev_addr4_data_metadata {
	u16 peer_id;
	u8 sa_valid;
	u8 addr4_valid;
};

#define NSS_WIFI_VDEV_EXT_DATA_PKT_TYPE_MCBC_RX			14
#define NSS_WIFI_VDEV_MESH_EXT_DATA_PKT_TYPE_RX_MCAST_EXC	16

struct nss_wifi_vdev_per_packet_metadata {
	u32 pkt_type;
	u32 rsvd[16];
};

struct nss_wifi_vdev_config_msg {
	u8 mac_addr[ETH_ALEN];
	u16 radio_ifnum;
	u32 vdev_id;
	u32 epid;
	u32 downloadlen;
	u32 hdrcachelen;
	u32 hdrcache[6];
	u32 opmode;
	u32 mesh_mode_en;
	u8 is_mpsta;
	u8 is_psta;
	u8 special_vap_mode;
	u8 smartmesh_mode_en;
	u8 is_wrap;
	u8 is_nss_qwrap_en;
	u8 tx_per_pkt_vdev_id_check;
	u8 align_pad;
	u32 vap_ext_mode;
};

struct nss_wifi_vdev_enable_msg {
	u8 mac_addr[ETH_ALEN];
	u8 reserved[2];
};

struct nss_wifi_vdev_disable_msg {
	u32 reserved;
};

struct nss_wifi_vdev_cmd_msg {
	u32 cmd;
	u32 value;
};

struct nss_wifi_vdev_set_next_hop_msg {
	u32 ifnumber;
};

/* Group addresses are in network order. */
union nss_wifi_vdev_grpaddr {
	__be32 grpaddr_ip4;
	__be32 grpaddr_ip6[4];
};

struct nss_wifi_vdev_me_snptbl_grp_create_msg {
	u32 ether_type;
	union nss_wifi_vdev_grpaddr u;
	u8 grp_addr[ETH_ALEN];
};

struct nss_wifi_vdev_me_snptbl_grp_delete_msg {
	u32 ether_type;
	union nss_wifi_vdev_grpaddr u;
	u8 grp_addr[ETH_ALEN];
};

struct nss_wifi_vdev_me_snptbl_grp_mbr_add_msg {
	u32 ether_type;
	union nss_wifi_vdev_grpaddr u;
	u32 peer_id;
	u8 grp_addr[ETH_ALEN];
	u8 grp_member_addr[ETH_ALEN];
	u8 mode;
	u8 nsrcs;
	u8 src_ip_addr[64];
	u8 rsvd[7];
};

struct nss_wifi_vdev_me_snptbl_grp_mbr_delete_msg {
	u32 ether_type;
	union nss_wifi_vdev_grpaddr u;
	u8 grp_addr[ETH_ALEN];
	u8 grp_member_addr[ETH_ALEN];
};

struct nss_wifi_vdev_mcast_enhance_stats {
	u32 mcast_rcvd;
	u32 mcast_ucast_converted;
	u32 mcast_alloc_fail;
	u32 mcast_pbuf_enq_fail;
	u32 mcast_pbuf_copy_fail;
	u32 mcast_peer_flow_ctrl_send_fail;
	u32 mcast_loopback_err;
	u32 mcast_dst_address_err;
	u32 mcast_no_enhance_drop_cnt;
	u32 mcast_rcvd_bytes;
	u32 igmp_rcvd;
	u32 igmp_ucast_converted;
	u32 mcast_mbr_alloc_failed;	/* not on 11.4 */
};

struct nss_wifi_vdev_stats_sync_msg {
	u32 dropped;
	u32 tx_enqueue_cnt;
	u32 tx_enqueue_fail_cnt;
	u32 tx_intra_bss_enqueue_cnt;
	u32 tx_intra_bss_enqueue_fail_cnt;
	u32 tx_intra_bss_mcast_send_cnt;
	u32 tx_intra_bss_mcast_send_fail_cnt;
	u32 tx_enqueue_bytes;
	u32 rx_enqueue_cnt;
	u32 rx_enqueue_fail_cnt;
	u32 rx_except_enqueue_cnt;
	u32 rx_except_enqueue_fail_cnt;
	u32 rx_enqueue_bytes;
	u32 rx_wds_learn_send_cnt;
	u32 rx_wds_learn_send_fail_cnt;
	struct nss_wifi_vdev_mcast_enhance_stats wvmes;
	u32 num_tx_exception;
	u32 tx_dma_map_fail;
	u32 tx_desc_alloc_fail;
	u32 tx_hw_ring_full;
	u32 tx_tso_pkt;
	u32 tx_num_seg;
	u32 tx_rcvd;
	u32 tx_rcvd_bytes;
	u32 cce_classified;
	u32 cce_classified_raw;
	u32 tx_eapol_cnt;
	u32 nawds_tx_mcast_cnt;
	u32 nawds_tx_mcast_bytes;
	u32 per_pkt_vdev_check_fail;
	u32 rx_mcast_cnt;
	u32 rx_mcast_bytes;
	u32 rx_decrypt_err;
	u32 rx_mic_err;
	u32 mcbc_exc_host_fail_cnt;
	/* not on 11.4 */
	u32 addr4_exc_fail;
	u32 addr4_exc_pass;
	u32 eapol_over_nl_exc_fail_cnt;
	u32 eapol_over_nl_exc_pass_cnt;
};

struct nss_wifi_vdev_msg {
	struct nss_cmn_msg cm;
	union {
		struct nss_wifi_vdev_cmd_msg vdev_cmd;
		struct nss_wifi_vdev_stats_sync_msg vdev_stats;
		struct nss_wifi_vdev_set_next_hop_msg next_hop;
		u8 rsvd[1028];
	} msg;
};

/* Wi-Fi mesh nodes. */

#define NSS_WIFI_MESH_MSG_INTERFACE_CONFIGURE	10000
#define NSS_WIFI_MESH_MSG_MPATH_ADD		10001
#define NSS_WIFI_MESH_MSG_MPATH_DELETE		10002
#define NSS_WIFI_MESH_MSG_MPATH_UPDATE		10003
#define NSS_WIFI_MESH_MSG_PROXY_PATH_LEARN	10004
#define NSS_WIFI_MESH_MSG_PROXY_PATH_ADD	10005
#define NSS_WIFI_MESH_MSG_PROXY_PATH_DELETE	10006
#define NSS_WIFI_MESH_MSG_PROXY_PATH_UPDATE	10007
#define NSS_WIFI_MESH_MSG_PATH_NOT_FOUND	10008
#define NSS_WIFI_MESH_MSG_PATH_REFRESH		10009
#define NSS_WIFI_MESH_MSG_PATH_EXPIRY		10010
#define NSS_WIFI_MESH_MSG_PROXY_PATH_TABLE_DUMP	10012
#define NSS_WIFI_MESH_MSG_STATS_SYNC		10013

#define NSS_WIFI_MESH_PATH_UPDATE_FLAG_NEXTHOP			0x01
#define NSS_WIFI_MESH_PATH_UPDATE_FLAG_HOPCOUNT			0x02
#define NSS_WIFI_MESH_PATH_UPDATE_FLAG_METRIC			0x04
#define NSS_WIFI_MESH_PATH_UPDATE_FLAG_MESH_FLAGS		0x08
#define NSS_WIFI_MESH_PATH_UPDATE_FLAG_BLOCK_MESH_FWD		0x40
#define NSS_WIFI_MESH_PATH_UPDATE_FLAG_METADATA_ENABLE_VALID	0x80

#define NSS_WIFI_MESH_PROXY_PATH_UPDATE_FLAG_MDA	0x1
#define NSS_WIFI_MESH_PROXY_PATH_UPDATE_FLAG_MESH	0x2

#define NSS_WIFI_MESH_CONFIG_FLAG_TTL_VALID			0x01
#define NSS_WIFI_MESH_CONFIG_FLAG_MPATH_REFRESH_VALID		0x02
#define NSS_WIFI_MESH_CONFIG_FLAG_LOCAL_MAC_VALID		0x08
#define NSS_WIFI_MESH_CONFIG_FLAG_MPP_LEARNING_MODE_VALID	0x10
#define NSS_WIFI_MESH_CONFIG_FLAG_SIBLING_IF_NUM_VALID		0x20
#define NSS_WIFI_MESH_CONFIG_FLAG_BLOCK_MESH_FWD_VALID		0x40

#define NSS_WIFI_MESH_MPP_LEARNING_MODE_INDEPENDENT_NSS	0
#define NSS_WIFI_MESH_PRE_HEADER_NONE			0xdcb

#define NSS_WIFI_MESH_ENCAP_METADATA_OFFSET_TYPE		4
#define NSS_WIFI_MESH_ENCAP_EXT_DATA_PKT_TYPE_MPATH_NOT_FOUND_EXC	1

struct nss_wifi_mesh_encap_ext_pkt_metadata {
	u16 pkt_type;
};

struct nss_wifi_mesh_config_msg {
	u8 local_mac_addr[ETH_ALEN];
	u16 reserved;
	u32 ttl;
	u32 mesh_path_refresh_time;
	u32 config_flags;
	u32 sibling_ifnum;
	u8 mpp_learning_mode;
	u8 block_mesh_forwarding;
	u8 reserved_2[2];
	u32 metadata_type;
};

struct nss_wifi_mesh_mpath_add_msg {
	u8 dest_mac_addr[ETH_ALEN];
	u8 next_hop_mac_addr[ETH_ALEN];
	u32 metric;
	u32 link_vap_id;
	u32 expiry_time;
	u8 hop_count;
	u8 path_flags;
	u8 is_mesh_gate;
	u8 block_mesh_fwd;
	u32 metadata_type;
};

struct nss_wifi_mesh_mpath_del_msg {
	u32 link_vap_id;
	u8 mesh_dest_mac_addr[ETH_ALEN];
	u8 next_hop_mac_addr[ETH_ALEN];
};

struct nss_wifi_mesh_mpath_update_msg {
	u8 dest_mac_addr[ETH_ALEN];
	u8 next_hop_mac_addr[ETH_ALEN];
	u8 old_next_hop_mac_addr[ETH_ALEN];
	u32 metric;
	u32 link_vap_id;
	u32 expiry_time;
	u8 hop_count;
	u8 path_flags;
	u8 is_mesh_gate;
	u8 update_flags;
	u8 block_mesh_fwd;
	u8 reserved[3];
	u8 metadata_type;
};

struct nss_wifi_mesh_proxy_path_learn_msg {
	u8 mesh_dest_mac[ETH_ALEN];
	u8 dest_mac_addr[ETH_ALEN];
	u8 path_flags;
	u8 is_update;
};

struct nss_wifi_mesh_proxy_path_add_msg {
	u8 mesh_dest_mac[ETH_ALEN];
	u8 dest_mac_addr[ETH_ALEN];
	u8 path_flags;
};

struct nss_wifi_mesh_proxy_path_update_msg {
	u8 mesh_dest_mac[ETH_ALEN];
	u8 dest_mac_addr[ETH_ALEN];
	u8 path_flags;
	u8 bitmap;
};

struct nss_wifi_mesh_proxy_path_del_msg {
	u8 mesh_dest_mac_addr[ETH_ALEN];
	u8 dest_mac_addr[ETH_ALEN];
};

struct nss_wifi_mesh_mpath_not_found_msg {
	u8 dest_mac_addr[ETH_ALEN];
	u8 transmitter_mac_addr[ETH_ALEN];
	u32 link_vap_id;
	u8 is_mesh_forward_path;
};

struct nss_wifi_mesh_path_refresh_msg {
	u8 dest_mac_addr[ETH_ALEN];
	u8 next_hop_mac_addr[ETH_ALEN];
	u32 link_vap_id;
	u8 path_flags;
};

struct nss_wifi_mesh_path_expiry_msg {
	u8 mesh_dest_mac_addr[ETH_ALEN];
	u8 next_hop_mac_addr[ETH_ALEN];
	u32 link_vap_id;
	u8 path_flags;
};

struct nss_wifi_mesh_proxy_path_dump_entry {
	u8 dest_mac_addr[ETH_ALEN];
	u8 mesh_dest_mac[ETH_ALEN];
	u8 flags;
	u8 reserved[3];
	u32 time_diff;
};

struct nss_wifi_mesh_proxy_path_table_dump {
	u32 num_entries;
	u32 more_events;
	struct nss_wifi_mesh_proxy_path_dump_entry path_entry[];
};

struct nss_wifi_mesh_encap_stats {
	u32 expiry_notify_sent;
	u32 mc_count;
	u32 mp_not_found;
	u32 mp_active;
	u32 mpp_not_found;
	u32 mpp_found;
	u32 encap_hdr_fail;
	u32 mp_del_notify_fail;
	u32 link_enqueue;
	u32 link_enq_fail;
	u32 ra_lup_fail;
	u32 dummy_add_count;
	u32 encap_mp_add_notify_fail;
	u32 dummy_add_fail;
	u32 dummy_lup_fail;
	u32 send_to_host_failed;
	u32 sent_to_host;
	u32 expiry_notify_fail;
	u32 no_headroom;
	u32 path_refresh_sent;
	u32 linearise_failed;
	u32 mp_exception_event_rl_dropped;
};

struct nss_wifi_mesh_decap_stats {
	u32 path_refresh_sent;
	u32 reserved;
	u32 mc_drop;
	u32 ttl_0;
	u32 mpp_lup_fail;
	u32 decap_hdr_fail;
	u32 rx_fwd_fail;
	u32 rx_fwd_success;
	u32 mp_fwd_lookup_fail;
	u32 mp_fwd_inactive;
	u32 nxt_mnode_fwd_success;
	u32 nxt_mnode_fwd_fail;
	u32 mpp_add_fail;
	u32 mpp_add_event2host_fail;
	u32 mpp_upate_fail;
	u32 mpp_update_even2host_fail;
	u32 mpp_learn2host_fail;
	u32 block_mesh_fwd_packets;
	u32 no_headroon;
	u32 linearise_failed;
	u32 mpp_learn_events_rate_limited;
	u32 mp_missing_events_rate_limited;
};

struct nss_wifi_mesh_stats_sync_msg {
	struct nss_cmn_node_stats pnode_stats;
	struct nss_wifi_mesh_encap_stats mesh_encap_stats;
	struct nss_wifi_mesh_decap_stats mesh_decap_stats;
	u32 rsvd[44];
};

struct nss_wifi_mesh_msg {
	struct nss_cmn_msg cm;
	u8 msg[384];
};

#endif /* __NSS_FW_H */
