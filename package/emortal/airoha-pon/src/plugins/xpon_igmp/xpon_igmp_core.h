/*
 ***************************************************************************
 * MediaTeK Inc.
 * 4F, No. 2 Technology	5th	Rd.
 * Science-based Industrial	Park
 *
 * (c) Copyright 2002-2012, MTK.
 *
 * All rights reserved.	MediaTeK's source	code is	an unpublished work	and	the
 * use of a	copyright notice does not imply	otherwise. This	source code
 * contains	confidential trade secret material of MediaTeK Tech. Any attemp
 * or participation	in deciphering,	decoding, reverse engineering or in	any
 * way altering	the	source code	is stricitly prohibited, unless	the	prior
 * written consent of MediaTeK, Inc. is obtained.
 ***************************************************************************
	Module Name:
	xpon_igmp_core.h
	
	Abstract:

	Revision History:
	Who			When			What
	--------	----------		----------------------------------------------
	Name				Date			Modification logs
	lidong.hu		2012/7/28	Create
*/

#ifndef XPON_IGMP_CORE_H_
#define  XPON_IGMP_CORE_H_

#include <ecnt_hook/ecnt_hook_pon_mac.h>
#include <ecnt_hook/ecnt_hook_pon_vlan.h>

#include "xpon_igmp_ioctl.h"
#include "xpon_ioctl_if.h"
#include <lan_port/lan_port_info.h>
#include <ecnt_hook/ecnt_hook_xpon_mapping.h>


// the white list entry used for access control of IGMP upstream flow  
#define XPON_MASK_IGMPV1 1
#define XPON_MASK_IGMPV2 2
#define XPON_MASK_IGMPV3 4
#define XPON_MASK_MLDV1  16
#define XPON_MASK_MLDV2  32
 

#define XPON_MODE_INCLUDE  0x100
#define XPON_MODE_EXCLUDE  0x200


#define IGMP_TRANSPATENT 0

#define MAC_HEADER_LEN		12
#define ETHER_TYPE_LEN		2
#define TCI_LEN				2
#define IEEE8021AD_TAG_LEN		(ETHER_TYPE_LEN+TCI_LEN)
#define XPON_IGMP_ROUTER_PORT 1
#define XPON_MLD_ROUTER_PORT 2
#define XPON_BRIDGE_PORT 4
#define MAX_VLAN_COUNT		4
//#define XPON_IGMP_DEBUG 0x8000


#define  PACKET_IPV4 0x0800
#define  PACKET_IPV6 0x86dd

#define PROTOCOL_IGMP 0x02
#define PROTOCOL_UDP 0x11
#define PROTOCOL_TCP 0x06

#define PROTOCOL_ICMPV6  0x3a
#define PROTOCOL_UDP6  0x11
#define PROTOCOL_TCP6 0x02


#define XPON_IGMP_DEBUG_ERROR 1
#define XPON_IGMP_DEBUG_TRACE 2
#define XPON_IGMP_DEBUG_HW 4

#define MULTICAST_IPV4_START_ADDR  0xdf
#define MULTICAST_IPV4_END_ADDR    0xf0
#define MULTICAST_IPV6_ADDR        0xff

#define DEFAULT_LEAVE_RETRY_CNT     2

typedef enum {
	E_XPON_IGMP_DIRECTION_DOWNSTREAM = 0,
	E_XPON_IGMP_DIRECTION_UPSTREAM
} ENUM_XPON_IGMP_DIRECTION_t;


#ifndef TRUE
#define TRUE     1
#endif
#ifndef FALSE
#define FALSE   0
#endif


#define TCSUPPORT_XPON_IGMP_CTC
/*************************************************************************************/
#define MULTICAST_UP_STREAM      1
#define MULTICAST_DOWN_STREAM    2
#define	MAX_UNI_PORT_NUM	     (MAX_ECNT_ETHER_PORT_NUM+1)
#define UNTAG_FRAME              0
#define TAG_FRAME                1
#define GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE   4
#define GPON_VLAN_FILTR_TYPE_UNTAGGED	            (1<<0)
#define GPON_VLAN_FILTR_TYPE_TAGGED		            (1<<1)

#define GPON_VLAN_FILTR_PORT_TYPE_LAN	    0
#define GPON_VLAN_FILTR_PORT_TYPE_ANI	    1

#define	GPON_VLAN_FILTR_RULE_DIR_RX			0
#define	GPON_VLAN_FILTR_RULE_DIR_TX			1

#define SIGNAL_LAN_PORT          "eth0"
#define MULTI_LAN_PORT_FORMAT    "eth0.%d"

#define NOT_MATCH_STATIC_ACL 0
#define MATCH_STATIC_ACL 1

#define NOT_MATCH_DYNAMIC_ACL 0
#define MATCH_DYNAMIC_ACL 1

#define MULTICAST_OP_VEIP_PORT_ID 1

#define XFI_NAME_TMP_LEN	10


typedef  enum _vlan_operation_point
{
    vlan_operation_in_vlan_module      = 0,
        
    vlan_operation_in_multicast_module = 1,
    
}e_vlan_operation_point_t;

typedef enum _multicast_debug_level
{
    E_NO_INFO_LEVEL      = 0,
    E_ERROR_INFO_LEVLE   = 1 << 0,
    E_CRITIC_INFO_LEVLE  = 1 << 1,
    E_NOTICE_INFO_LEVLE  = 1 << 2,
    E_WARN_INFO_LEVLE    = 1 << 3,
    E_DEBUG_INFO_LEVLE   = 1 << 4,
    E_TRACE_INFO_LEVLE   = 1 << 5,
}e_multicast_debug_level;

typedef enum{
    FWD_OPT_TYPE_ADD = 1,
    FWD_OPT_TYPE_DEL,
    FWD_OPT_TYPE_UPDATE,
}FWD_OPT_TYPE;

#define XPON_HWNAT_AGE_TIME 3000
#define XPON_HWNAT_FWD_THRESHOLD 600

//define xPON Multicast Control structure
#define XPON_PORT_CNT 8
#define XPON_MVLAN_CNT 16

#define XPON_IGMP_DISABLED 0x01
#define XPON_IGMP_MVLAN_DISABLED  0x02
#define XPON_IGMP_SNOOPING_DISABLED  0x04
#define XPON_MLD_SNOOPING_DISABLED  0x08
#define XPON_HWNAT_DISABLED 0x10
#define XPON_HWNAT_DROP_DISABLED  0x20
#define XPON_CONTROL_MULTCAST_DISABLE  0x40
#define XPON_PROOCOL_AUTO_THROUGH  0x100	/* auto through for special packets with multicast dst mac, eg. igmp, llmnr. */

#define XPON_IGMPv1_DISCARD       0x200



#define MULTICAST_ERROR_INFO(fmt, ...)  do{ \
                                            if(g_MULTICAST_DEBUG_LEVEL & E_ERROR_INFO_LEVLE)\
                                                printk("error info, func: %s, line:%d  " fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
                                        }while(0)

#define MULTICAST_CRITIC_INFO(fmt, ...) do{ \
                                            if(g_MULTICAST_DEBUG_LEVEL & E_CRITIC_INFO_LEVLE)\
                                                printk("critic info, func: %s, line:%d  " fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
                                        }while(0)

#define MULTICAST_NOTICE_INFO(fmt, ...) do{ \
                                            if(g_MULTICAST_DEBUG_LEVEL & E_NOTICE_INFO_LEVLE) \
                                                printk("notice info, func: %s, line:%d  " fmt,__FUNCTION__, __LINE__, ##__VA_ARGS__); \
                                        }while(0)

#define MULTICAST_WARN_INFO(fmt, ...)  do{ \
                                            if(g_MULTICAST_DEBUG_LEVEL & E_WARN_INFO_LEVLE) \
                                                printk("WARN info, func: %s, line:%d  " fmt,__FUNCTION__, __LINE__, ##__VA_ARGS__); \
                                        }while(0)

#define MULTICAST_DEBUG_INFO(fmt, ...)  do{ \
                                            if(g_MULTICAST_DEBUG_LEVEL & E_DEBUG_INFO_LEVLE) \
                                                printk("Debug info, func: %s, line:%d\t  " fmt,__FUNCTION__, __LINE__, ##__VA_ARGS__); \
                                        }while(0)

#define MULTICAST_TRACE_INFO(fmt, ...)  do{ \
                                            if(g_MULTICAST_DEBUG_LEVEL & E_TRACE_INFO_LEVLE) \
                                                printk("[%s][LN%d]\t" fmt,__FUNCTION__, __LINE__, ##__VA_ARGS__); \
                                        }while(0)
                                        
#define DUMP_PKT(now, from, data, size)\
{\
    if (g_MULTICAST_DEBUG_LEVEL & E_TRACE_INFO_LEVLE)\
    {\
        int idx = 0;\
        printk("<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<\n");\
        printk("[%s][LN%d][LEN = %d][%s -> %s]\n",__FUNCTION__,__LINE__,size,from,now);\
        for (idx = 0; idx < size; idx++)\
        {\
            printk("%02x ",*(data + idx));\
            if (!((idx + 1) % 16))\
                printk("\n");\
        }\
        printk("\n");\
    }\
}

#define DBG_PRINT_CLIENT_INFO(type, vid, src_ip, dest_ip){\
    if (g_MULTICAST_DEBUG_LEVEL & E_TRACE_INFO_LEVLE)\
    {\
        if (type == PACKET_IPV4)\
        {\
            MULTICAST_TRACE_INFO("(Client)vlan = %d, src_ip = "NIPQUAD_FMT", dest_ip = "NIPQUAD_FMT"\n",vid,NIPQUAD(src_ip),NIPQUAD(dest_ip));\
        }\
    }\
}

#define DBG_PRINT_ACL_ENTRY_INFO(type, idx, vid, src_ip, start_ip, end_ip){\
    if (g_MULTICAST_DEBUG_LEVEL & E_TRACE_INFO_LEVLE)\
    {\
        if (type == PACKET_IPV4)\
        {\
            MULTICAST_TRACE_INFO("=>[%d]vid = %d, src = "NIPQUAD_FMT", start = "NIPQUAD_FMT", end = "NIPQUAD_FMT"\n",idx,vid,NIPQUAD(src_ip),NIPQUAD(start_ip),NIPQUAD(end_ip));\
        }\
    }\
}

#define IS_IGMPv1_DISCARD     (XPON_IGMPv1_DISCARD & igmp_conf->flag)

#ifndef TIMER_FUN_PAAM
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0) 
#define TIMER_FUN_PAAM unsigned long
#else
#define TIMER_FUN_PAAM struct timer_list *
#endif
#endif

/*************************************************************************************/
typedef struct xPON_DynPreview_s
{
	unsigned short int pre_len;
	unsigned short int pre_rep_time;
	unsigned short int pre_rep_cnt;
	unsigned short int pre_rep_cnt_left;/*preview repeat left*/
	unsigned short int pre_rst_time;
	struct timer_list rep_interval_timer;
	unsigned char rep_interval_flag;
}xPON_DynPreview_t;

typedef struct xPON_WhiteList_Entry_List_s
{
	struct list_head  list;
	short int flag;
	short int index;
	int type;
	short int gemid;
	short int vlanid;
	int bandwidth;
	unsigned char srcip[16];
	unsigned char grpstart[16];
	unsigned char grpend[16];
	unsigned char srcipv6[64];
	unsigned char grpstartv6[64];
	unsigned char grpendv6[64];	
	xPON_DynPreview_t preview_info;
}xPON_WhiteList_Entry_List_t;

/*
typedef struct xPON_WhiteList_s
{
	int nCount;
	xPON_WhiteList_Entry_List_t * entry;
}xPON_WhiteList_t;
*/

typedef struct xPON_PortConf_s
{
	//supported Multicast Protocol: IGMPv2/IGMPv3/MLDv1/MLDv2
	//the low4 bits of the lowest byte is for V4, the high 4 bits is for V6
	short int proto_mask;
	short int work_mode;
	short int fastleave;
	short int robust;
	short int unauthor;
	short int up_vlan_tci;
	short int up_vlan_mode;
	short int down_vlan_tci;
	short int down_vlan_mode;
	short int tagstrip;
	char queryip[16];
	int queryinterval;
	int maxresp;
	int lastinterval;
	int maxrate; 
	int maxgroup;
	int maxbw;/*Max Multicast Bandwidth*/
	unsigned char bw_enforce;/*Bandwidth Enforcement*/
////////////////	
	int dyn_cnt;
	int sta_cnt;
	//white list for access control
	struct list_head   dyn_list;
	struct list_head   sta_list;
	struct list_head   dyn_list_ipv6;
}xPON_PortConf_t;


//define supported MVLan group per port
#define XPON_PORT_VLAN_CNT 8
typedef struct xPON_PortVLan_s
{
	short int vlan_flag;
	short int vlan_num;
	short int vlan_id[XPON_PORT_VLAN_CNT];
	short int vlan_trans[XPON_PORT_VLAN_CNT];
}xPON_PortVLan_t;

#define XPON_PORT_USER_SUBSCRIBE_DONT_CARE_INDEX    -1
#define XPON_PORT_USER_SUBSCRIBE_FOUND_SAME_ENTRY    2
#define XPON_PORT_USER_SUBSCRIBE_FOUND_SAME_PROGRAM  1
#define XPON_PORT_USER_SUBSCRIBE_NOT_FOUND           0

typedef struct xPON_User_Subscribe_Entry_s
{
    short int index;
	unsigned char srcip[16];
	unsigned char program_ip[16];
	xPON_WhiteList_Entry_List_t * dyn_list_entry;
    struct list_head list;	
}xPON_User_Subscribe_Entry_t;

//define performance monitor in G.988 9.3.29
typedef struct xPON_PortPMCount_s
{
	unsigned int current_mcast_bw;
	unsigned int join_msg;
	unsigned int bw_exceeded;	
}xPON_PortPMCount_t;
typedef struct xPON_PortStatus_s
{
	xPON_PortPMCount_t pmc;	
	int user_subscribe_cnt;
	struct list_head user_subscribe_list;
}xPON_PortStatus_t;

//defines relation between the UNI port and dev name 
typedef struct xPON_PortEntry_s
{
	int  port_flag;
	xPON_PortConf_t port_conf;
	xPON_PortVLan_t port_vlan;
	xPON_PortStatus_t port_status;
}xPON_PortEntry_t;

// define MVLAN config including binding port, snooping switch,etc
#define XPON_MVLAN_SNOOPING_DISABLED  1
typedef struct xPON_MVLanEntry_s
{
	short int mvlan_id;
	short int mvlan_flag;
}xPON_MVLanEntry_t;


// define local forward table entry type based on CTC EPON Spec
/*
typedef struct xPON_FwdEntryType_0_s
{
	unsigned char mac[6];
}xPON_FwdEntryType_0_t;

typedef struct xPON_FwdEntryType_1_s
{
	short vid;
	unsigned char mac[6];
}xPON_FwdEntryType_1_t;

typedef struct xPON_FwdEntryType_2_s
{
	unsigned char mac[6];
	int ip;
}xPON_FwdEntryType_2_t;

typedef struct xPON_FwdEntryType_3_s
{
	short vid;
	int ip;
}xPON_FwdEntryType_3_t;

typedef struct xPON_FwdEntryType_4_s
{
	short vid;
	unsigned char ip6[16];
}xPON_FwdEntryType_4_t;

typedef struct xPON_FwdEntryType_5_s
{
	unsigned char mac[6];
	unsigned char ip6[16];
}xPON_FwdEntryType_5_t;

typedef struct xPON_FwdEntryType_6_s
{
	short vid;
	unsigned char mac[6];
	unsigned char ip6[16];
}xPON_FwdEntryType_6_t;
*/

/*
	union 
	{
		struct xPON_FwdEntryType_0_s type0;
		struct xPON_FwdEntryType_1_s type1;
		struct xPON_FwdEntryType_2_s type2;
		struct xPON_FwdEntryType_3_s type3;
		struct xPON_FwdEntryType_4_s type4;
		struct xPON_FwdEntryType_5_s type5;
		struct xPON_FwdEntryType_6_s type6;
	}fwd_entry;
*/

//define local forwarding table entry based on CTC EPON Spec 
typedef struct xPON_FwdEntry_s
{
	struct list_head  list;
	struct rcu_head rcu;

//	1st byte: IGMPV1=1 IGMPV2=2 IGMPV3=3 MLDV1=5 MLDV2=6
// 2st byte: IGMPV3.Include = 1,Exclude = 2
	int flag;

// used for forwarding multicast flow	
	int port;
	int type;
	int vid;
	unsigned char grp_addr[16];
	unsigned char src_addr[16];

// used for leave timer
	struct timer_list leave_ageing_timer;
	int leave_count;
	unsigned char client_ip[16];
	unsigned long join_time;
	struct timer_list preview_timer;
} xPON_FwdEntry_t;

// define local forwarding table 
typedef struct xPON_FwdTable_s
{
	short fwd_num;
	short int fwd_mode;
	struct list_head  fwd_list;
}xPON_FwdTable_t;

typedef struct xPON_AniEntry_s
{
	int ani_id;
	char ani_name[12];
	int ani_flag;
}xPON_AniEntry_t;


typedef struct xPON_IGMP_HWNATEntry_s
{
	struct list_head  list;
	int hwnat_type;   //ipv6 or ipv4
	int hwnat_index;   //get from skb buff
	int gem_port_id;
	unsigned int  src_vlan;  //source VLAN info, TPID + TCI
	short int  hwnat_vid; 
	short int  hwnat_mask;   //port mask
	short int  sw_mask;   //port mask for external switch
	unsigned char grp_addr[16];
	unsigned char src_addr[16];
	unsigned char mac_da[6];
//	struct timer_list age_timer;
}xPON_IGMP_HWNATEntry_t;

typedef struct xPON_IGMPConf_s
{
	int flag;
	int dbglevel; 
	unsigned char dsbwctrl;
	int xpon_mode;
	int onu_type;
	int ani_num;
	int uni_num;
	int mvlan_num;
#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_IGMP_SET_GROUP)
	int group_num;
#endif/*TCSUPPORT_COMPILE*/
	xPON_AniEntry_t* ani_port; 
	xPON_PortEntry_t* uni_port;
	xPON_MVLanEntry_t* mul_vlan;
	xPON_FwdTable_t* fwd_tbl;
	struct list_head hwnat_igmp;
	struct list_head hwnat_drop;
	struct timer_list addFlwTm;
	struct timer_list addDrpTm;
}xPON_IGMPConf_t;
////////////////////////////////////////////////////////////////
typedef struct xPON_IGMP_Up_Vlan_s
{
	int port;
	int vlan_tci;
	unsigned char grp_addr[16];
	struct list_head up_vlan_list;
}xPON_IGMP_Up_Vlan_t;

typedef struct xPON_IGMP_Vlan_Action_s
{
    int switch_port;
	int mode;
	int vid;
}xPON_IGMP_Vlan_Action_t;

typedef struct xPON_IGMP_Multi_Vlan_s
{
	int port_num;
	xPON_IGMP_Vlan_Action_t vlan_action[MAX_UNI_PORT_NUM];
}xPON_IGMP_Multi_Vlan_t;
//-------------------------xpon_igmp_itf.c-----------------------------------//
int xpon_get_trans_vid(int port,int vid);
struct xPON_IGMPConf_s* xpon_get_igmp_conf(void); 
xPON_PortEntry_t* xpon_port_entry_by_id(int port);
xPON_PortConf_t* xpon_port_conf_by_id(int port);
xPON_PortVLan_t* xpon_port_vlan_by_id(int port);
int xpon_port_vlan_find(int port,int vid);
int xpon_is_ani_port(char* name);
xPON_AniEntry_t* xpon_get_ani_entry(char* name);
xPON_AniEntry_t* xpon_ani_entry_by_id(int id);
void xpon_free(void* ptr);
void* xpon_alloc(int size);
xPON_MVLanEntry_t* xpon_mvlan_find(int mvid);
int xpon_is_non_zero(unsigned char* addr,int len);
int xpon_sfu_down_vlan_access_control(struct sk_buff* skb,int port);
//-------------------------xpon_igmp_core.c-----------------------------------//
void xpon_ip4_to_mac(unsigned char*  addr_ip4, unsigned char *mac);
void xpon_ip6_to_mac(unsigned char* addr_ip6, unsigned char *mac);
int xpon_fwdtbl_operate_entry(int op,xPON_FwdEntry_t*  entry);
int xpon_should_forward_flow(int port,int vid,unsigned char* dest_mac ,unsigned char* dest_addr ,unsigned char* src_addr);
xPON_FwdEntry_t* xpon_fwdtbl_find(int type, int port, int vid,unsigned char* grp_addr,unsigned char* src_ip,int proto);
xPON_FwdEntry_t* xpon_fwdtbl_find_ext(int port, int vid,unsigned char* grp_mac,unsigned char* grp_addr,unsigned char* src_ip,int proto);
xPON_FwdEntry_t* xpon_fwdtbl_add(int type, int port, int vid,unsigned char* grp_addr,unsigned char* src_addr,int flag,unsigned char* client_ip);
int xpon_fwdtbl_del(int type, int port, int vid,unsigned char* grp_addr,unsigned char* src_ip);
struct list_head*  xpon_get_forward_list(void);
int xpon_is_non_zero(unsigned char* addr,int len);
int xpon_igmp_port_hook(struct net_device* dev,int op);
int xpon_igmp_incoming_hook(struct sk_buff* skb,int clone);
int xpon_igmp_debug_on(void);
void xpon_igmp_debug(int level,char* fmt,...);
int xpon_pass_access_control(xPON_FwdEntry_t*  fwd_entry);
void xpon_rate_control_init(void);
int xpon_get_dest_mac(unsigned char* mac,struct sk_buff* skb);
int xpon_get_vlan_id(struct sk_buff* skb,uint16_t * vlanId, unsigned char * tagNum);
int xpon_get_outmost_vid(struct sk_buff* skb);
int xpon_get_vlan_tci(struct sk_buff* skb);
int xpon_get_src_addr(unsigned char* src,struct sk_buff* skb);
int xpon_get_dest_addr(unsigned char* src,struct sk_buff* skb);
int xpon_get_packet_type(struct sk_buff* skb);
int xpon_get_igmp_port(struct sk_buff* skb);
int xpon_get_igmp_port_by_original_dev(struct sk_buff* skb);
int xpon_is_multicast_addr(unsigned char* addr);
int xpon_get_downstream_grpaddr(unsigned char* grp_addr,struct sk_buff* skb);
bool xpon_is_general_query(struct sk_buff* skb);
void xpon_clear_multicast_hw_table(void);
//**************************************************************************//
int xpon_igmp_protocol_pack(struct sk_buff* skb);
int xpon_up_igmp_uni_vlan_filter(struct sk_buff* skb);
int xpon_up_igmp_ani_vlan_filter(struct sk_buff* skb);
int isVlanOperationInMulticastModule(struct sk_buff* skb);
int xpon_upstream_vlan_handle_hook(struct sk_buff* skb, int clone);
int xpon_up_send_multicast_frame_hook(struct sk_buff* skb,int clone);
int xpon_up_igmp_incoming_hook(struct sk_buff* skb,int clone);
int xpon_down_igmp_incoming_hook(struct sk_buff* skb,int clone);
int xpon_get_up_vlan_operation_point(int port, e_vlan_operation_point_t *point);
int xpon_get_down_vlan_operation_point(int port, e_vlan_operation_point_t *point);
int xpon_get_vlan_tci(struct sk_buff* skb);
int multicast_data_pack(struct sk_buff* skb);
int xpon_hgu_downstream_vlan_handle(struct sk_buff* skb);
//-------------------------xpon_igmp_hw.c-----------------------------------//
int xpon_switch_update_entry(int vid,int port,int mode ,int newvid);
int xpon_switch_update_port(int port);
int xpon_hwnat_flow_read(char *buf, char **start, off_t off, int count,int *eof, void *data);
int xpon_hwnat_drop_read(char *buf, char **start, off_t off, int count,int *eof, void *data);
int xpon_hwnat_update_flow_by_fwd(xPON_FwdEntry_t* entry);
//int xpon_switch_clear_mulvlan(int port);
int xpon_hwnat_learn_flow(struct sk_buff * skb);
int xpon_hwnat_drop_multicast(struct sk_buff * skb);
int xpon_hwnat_wan_mvlan_change(void);
int xpon_hwnat_clear_flows(void);
int xpon_hwnat_clear_all_drop(void);
int xpon_hwnat_delate_drop_by_fwd(xPON_FwdEntry_t* entry);
int xpon_hwnat_clear_drop_by_grpip(unsigned char is_ipv6,unsigned char* grp_ip);
void xpon_hwnat_show_hwnat_list(void);
//------------------ ----------others---------------------------------------//
int debug_show_xpon_hwnat_list(void);
int static_acl_ctr(int port, int vid, int gem_portid, unsigned char* dest_addr , unsigned char* src_addr, int hw_flag);
int xpon_igmp_acl_filter(struct sk_buff* skb);
void init_xpon_igmp_macro_compatible(void);
void add_flow_timer_func(TIMER_FUN_PAAM arg);
void add_drop_timer_func(TIMER_FUN_PAAM arg);
int xpon_hwnat_update_flow_by_hw(xPON_IGMP_HWNATEntry_t* entry);
int xpon_hwnat_update_external_switch(xPON_IGMP_HWNATEntry_t* entry, unsigned int sw_mask);
xPON_User_Subscribe_Entry_t* xpon_user_subscribe_find(int port, int index, unsigned char* src_ip, unsigned char* program_ip);
int xpon_fwdtbl_clear(void);
int xpon_port_conf_clear(void);
int xpon_port_status_clear(void);
int xpon_set_port_conf(int mode);
int xpon_switch_set_mode(int vid,int mode);
int xpon_igmp_has_control_entry(void);
int xpon_hwnat_del_hw_flow(xPON_FwdEntry_t*  fwd_entry);
int xpon_multi_should_passthrough(struct sk_buff* skb);
int xpon_grp_addr_between(unsigned char* grp_addr,unsigned char* start, unsigned char* end);
int xpon_is_igmp_pkt(struct sk_buff* skb);
int xpon_get_igmp_grpaddr(unsigned char* grp_addr,struct sk_buff* skb);
bool xpon_upstream_dyn_whitelist_access(unsigned char* grp_addr,int port_id);
int xpon_is_data_pkt(struct sk_buff* skb);
xPON_PortStatus_t* xpon_port_status_by_id(int port);
int xpon_check_hwfwd_list_threshold(void);
void xpon_droptbl_del_by_port_vid(int port, int vid);
int xpon_igmp_get_fwdmode(void);
int xpon_igmp_get_max_playgroup(int port);
void xpon_fwdtbl_del_by_grp(int type, int port, int vid,unsigned char* grp_addr);





/*EXPORT_SYMBOL*/
extern void tc3162wdog_kick(void);
extern int igmp_hwnat_clear_flows(void);
#ifdef TCSUPPORT_CPU_ARMV8
extern char get_onutype(void);
#endif
extern u32 random32(void);
extern struct sk_buff *__vlan_put_tag(struct sk_buff *skb, u16 vlan_tci);


extern int g_DS_MCAST_BW_RATE_LIMIT_ENABLE;
extern e_multicast_debug_level g_MULTICAST_DEBUG_LEVEL;
extern int g_care_ver_dynlist_stalist_op;
extern xPON_IGMPConf_t igmp_conf;



#endif
