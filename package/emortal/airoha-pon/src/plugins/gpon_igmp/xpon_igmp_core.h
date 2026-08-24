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
#include <ecnt_hook/ecnt_hook_multicast_general.h>
#include <lan_port/lan_port_info.h>
#include "ponvlan_port_info.h"
#include "xpon_igmp_ioctl.h"
#include "xpon_ioctl_if.h"

#define XPONIGMP_PORT_ETH_START         1               //for PPTP start id
#define XPONIGMP_PORT_VEIP_ID           21               //for VEIP id
#define XPONIGMP_PORT_ETH_NUM           4

// the white list entry used for access control of IGMP upstream flow  
#define XPON_MASK_IGMPV1 1
#define XPON_MASK_IGMPV2 2
#define XPON_MASK_IGMPV3 4
#define XPON_MASK_MLDV1  16
#define XPON_MASK_MLDV2  32
 

#define XPON_MODE_INCLUDE  0x100
#define XPON_MODE_EXCLUDE  0x200


#define IGMP_TRANSPATENT 0


#define XPON_IGMP_ROUTER_PORT (1 << 0)
#define XPON_MLD_ROUTER_PORT (1 << 1)
#define XPON_BRIDGE_PORT  (1 << 2)
#define XPON_INVALID_PORT  (1 << 3)
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

#define MULTICAST_IPV4_START_ADDR  0xdf
#define MULTICAST_IPV4_END_ADDR    0xf0
#define MULTICAST_IPV6_ADDR        0xff

#define FWD_RULE_TYPE_ALL      0
#define FWD_RULE_TYPE_DYNAMIC  1
#define FWD_RULE_TYPE_STATIC   2

#define FWD_STATIC_ADD_RATE   10  //add static rule to mtk_mutlicast once per num of pkt

#ifndef TRUE
#define TRUE     1
#endif
#ifndef FALSE
#define FALSE   0
#endif

#define SFU 1
#define HGU 2

#define TCSUPPORT_XPON_IGMP_CTC
/*************************************************************************************/
#define MULTICAST_UP_STREAM      1
#define MULTICAST_DOWN_STREAM    2
#define MAX_PORT_NUM             4
#define UNTAG_FRAME              0
#define TAG_FRAME                1
#define GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE   4
#define GPON_VLAN_FILTR_TYPE_UNTAGGED	            (1<<0)
#define GPON_VLAN_FILTR_TYPE_TAGGED		            (1<<1)
#define OMCI_MAC_BRIDGE_PORT_LAN_PORT_BASIC			1

#define GPON_VLAN_FILTR_PORT_TYPE_LAN	    0
#define GPON_VLAN_FILTR_PORT_TYPE_ANI	    1

#define	GPON_VLAN_FILTR_RULE_DIR_RX			0
#define	GPON_VLAN_FILTR_RULE_DIR_TX			1

#define SIGNAL_LAN_PORT          "eth0"
#define MULTI_LAN_PORT_FORMAT    "eth0.%d"
#define XPON_VEIP_PORT          "veip1"
#define PON_ITF                 "pon"

#define NOT_MATCH_STATIC_ACL 0
#define MATCH_STATIC_ACL 1

#define NOT_MATCH_DYNAMIC_ACL 0
#define MATCH_DYNAMIC_ACL 1

#define MULTICAST_OP_VEIP_PORT_ID 1

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

typedef  enum e_xpon_igmp_result
{
    XPON_IGMP_RET_CONTINUE   = 0,
    XPON_IGMP_RET_FWD_END,
    XPON_IGMP_RET_DROP_END,
}e_xpon_igmp_result_t;

typedef  enum {
	MC_API_OP_TYPE_ADD = 1,
	MC_API_OP_TYPE_UPDATE,
	MC_API_OP_TYPE_DELETE,
	MC_API_OP_TYPE_GET,
}MC_API_OP;

typedef enum{
    DYNAMIC_WHITE_LIST =1 ,
    STATIC_WHITE_LIST,
}WHITE_LIST_TYPE;

typedef enum{
    FWD_OPT_TYPE_ADD = 1,
    FWD_OPT_TYPE_DEL,
    FWD_OPT_TYPE_UPDATE,
}FWD_OPT_TYPE;

extern int g_DS_MCAST_BW_RATE_LIMIT_ENABLE;
extern e_multicast_debug_level g_MULTICAST_DEBUG_LEVEL;
extern int g_care_ver_dynlist_stalist_op;


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

typedef struct xPON_WhiteList_Entry_s
{
	struct list_head  list;
	short int flag;
	int index;
	int type;
	int gemid;
	int vlanid;
	int bandwidth;
	unsigned char srcip[16];
	unsigned char grpstart[16];
	unsigned char grpend[16];
	unsigned char srcipv6[64];
	unsigned char grpstartv6[64];
	unsigned char grpendv6[64];	
	xPON_DynPreview_t preview_info;
}xPON_WhiteList_Entry_t;

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
	ECNT_VLAN_ACTION vlan_action;
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
	xPON_WhiteList_Entry_t * dyn_list_entry;
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
	int port_id;  		
	char port_name[8]; 	
	int  port_flag;
	xPON_PortConf_t* port_conf;
	xPON_PortVLan_t* port_vlan;
	xPON_PortStatus_t* port_status;
}xPON_PortEntry_t;

// define MVLAN config including binding port, snooping switch,etc
#define XPON_MVLAN_SNOOPING_DISABLED  1
typedef struct xPON_MVLanEntry_s
{
	short int mvlan_id;
	short int mvlan_flag;
}xPON_MVLanEntry_t;


//define local forwarding table entry based on CTC EPON Spec 
typedef struct xPON_FwdEntry_s
{
	struct list_head  list;
	struct rcu_head rcu;

	// 1st byte: IGMPV1=1 IGMPV2=2 IGMPV3=3 MLDV1=5 MLDV2=6
	// 2st byte: IGMPV3.Include = 1,Exclude = 2
	int flag;

	// used for forwarding multicast flow	
	int port;
	int type;
	int vid;
	int ruleType;  //1: dynamic,  2 static
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
	unsigned char grp_addr[16];
	unsigned char src_addr[16];
	unsigned char mac_da[6];
	struct timer_list age_timer;
}xPON_IGMP_HWNATEntry_t;


//define xPON Multicast Control structure
#define XPON_PORT_CNT 8
#define XPON_MVLAN_CNT 16

#define XPON_IGMP_DISABLED 0x01
#define XPON_IGMP_MVLAN_DISABLED  0x02
#define XPON_IGMP_SNOOPING_DISABLED  0x04
#define XPON_MLD_SNOOPING_DISABLED  0x08
//#define XPON_HWNAT_DISABLED 0x10
//#define XPON_HWNAT_DROP_DISABLED  0x20
#define XPON_CONTROL_MULTCAST_DISABLE  0x40

typedef struct xPON_IGMPConf_s
{
	int flag;
	int dbglevel; 
	unsigned char dsbwctrl;
	int xpon_mode;
	int onu_type;
	int ani_num;
	int uni_num;
	int veip_num;
	int mvlan_num;
#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_IGMP_SET_GROUP)
	int group_num;
#endif/*TCSUPPORT_COMPILE*/
	int veip_acl;
	int empty_dynlist_pass;
	xPON_AniEntry_t* ani_port; 
	xPON_PortEntry_t* uni_port;
	xPON_PortEntry_t * veip_port;
	xPON_MVLanEntry_t* mul_vlan;
	xPON_FwdTable_t* fwd_tbl;
	struct list_head hwnat_igmp;
	struct list_head hwnat_drop;
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
	xPON_IGMP_Vlan_Action_t vlan_action[MAX_PORT_NUM];
}xPON_IGMP_Multi_Vlan_t;

//-------------------------xpon_igmp_itf.c-----------------------------------//
extern struct xPON_IGMPConf_s* xpon_get_igmp_conf(void); 
extern xPON_PortEntry_t* xpon_port_entry_by_id(int port);
extern xPON_PortEntry_t* xpon_port_entry_by_name(char* name);
extern  xPON_PortConf_t* xpon_port_conf_by_name(char* name);
extern  xPON_PortConf_t* xpon_port_conf_by_id(int port);
extern xPON_PortVLan_t* xpon_port_vlan_by_name(char* name);
extern xPON_PortVLan_t* xpon_port_vlan_by_id(int port);
extern int xpon_port_vlan_find(int port,int vid);
extern  int xpon_port_index_by_id(int port);
extern int xpon_port_index_by_name(char* name);
extern int xpon_is_uni_port(char* name);
extern int xpon_is_ani_port(char* name);
extern xPON_AniEntry_t* xpon_get_ani_entry(char* name);
extern xPON_AniEntry_t* xpon_ani_entry_by_id(int id);
extern void xpon_free(void* ptr);
extern void* xpon_alloc(int size);
extern xPON_MVLanEntry_t* xpon_mvlan_find(int mvid);
extern int xpon_is_non_zero(unsigned char* addr,int len);
extern int xpon_sfu_down_vlan_access_control(struct sk_buff* skb,char* dev_name);
extern int xpon_igmp_get_veip_acl(void);
extern int xpon_igmp_get_empty_dynlist_pass(void);
extern int mtk_multicast_fwdtbl_opration(int port, int vid, unsigned char* grp_addr,unsigned char* src_ip,int isIpv6, int op_type);
extern int xpon_port_conf_clear(void);
extern int xpon_user_subscribe_clear(int port);
extern int xpon_fwdtbl_clear(void);
extern int xpon_port_status_clear(void);

//-------------------------xpon_igmp_core.c-----------------------------------//
void xpon_ip4_to_mac(unsigned char*  addr_ip4, unsigned char *mac);
void xpon_ip6_to_mac(unsigned char* addr_ip6, unsigned char *mac);
int xpon_fwdtbl_operate_entry(int op,xPON_FwdEntry_t*  entry);
xPON_FwdEntry_t* xpon_fwdtbl_find(int type, int port, int vid,unsigned char* grp_addr,unsigned char* src_ip,int proto);
xPON_FwdEntry_t* xpon_fwdtbl_find_ext(int port, int vid,unsigned char* grp_mac,unsigned char* grp_addr,unsigned char* src_ip,int proto);
xPON_FwdEntry_t* xpon_fwdtbl_add(int type, int port, int vid,unsigned char* grp_addr,unsigned char* src_addr,int flag,unsigned char* client_ip);
int xpon_fwdtbl_del(int type, int port, int vid,unsigned char* grp_addr,unsigned char* src_ip);
int xpon_fwdtbl_update(xPON_FwdEntry_t* entry);
struct list_head*  xpon_get_forward_list(void);
int xpon_is_non_zero(unsigned char* addr,int len);
int xpon_igmp_incoming_hook(struct sk_buff* skb,int clone);
int xpon_igmp_debug_on(void);
void xpon_igmp_debug(int level,char* fmt,...);
int xpon_pass_access_control(xPON_FwdEntry_t*  fwd_entry);
void xpon_rate_control_init(void);
int xpon_get_dest_mac(unsigned char* mac,struct sk_buff* skb);
int xpon_get_vlan_id(struct sk_buff* skb);
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
xPON_User_Subscribe_Entry_t* xpon_user_subscribe_find(int port, int index, unsigned char* src_ip, unsigned char* program_ip);
int xpon_is_igmp_pkt(struct sk_buff* skb);
int xpon_get_igmp_grpaddr(unsigned char* grp_addr,struct sk_buff* skb);
bool xpon_upstream_dyn_whitelist_access(unsigned char* grp_addr,int port_id);
int xpon_is_data_pkt(struct sk_buff* skb);
xPON_PortStatus_t* xpon_port_status_by_id(int port);
int xpon_check_hwfwd_list_threshold(void);
void xpon_droptbl_del_by_port_vid(int port, int vid);
int xpon_igmp_get_fwdmode(void);
int xpon_igmp_get_max_playgroup(int port);
int xpon_grp_addr_between(unsigned char* grp_addr,unsigned char* start, unsigned char* end);
int static_acl_ctr(int port, int vid, int gem_portid, unsigned char* dest_addr , unsigned char* src_addr, int hw_flag);

//**************************************************************************//
extern int xpon_igmp_protocol_pack(struct sk_buff* skb);
extern int xpon_up_igmp_uni_vlan_filter(struct sk_buff* skb);
extern int xpon_up_igmp_ani_vlan_filter(struct sk_buff* skb);
extern int isVlanOperationInMulticastModule(struct sk_buff* skb);
extern int xpon_upstream_vlan_handle_hook(struct sk_buff* skb, int clone);
extern int xpon_up_send_multicast_frame_hook(struct sk_buff* skb,int clone);
extern int xpon_up_igmp_incoming_hook(struct sk_buff* skb,int clone);
extern int xpon_hybrid_down_igmp_incoming_hook(struct sk_buff* skb,int clone);
extern int xpon_get_up_vlan_operation_point(int port, e_vlan_operation_point_t *point);
extern int xpon_get_down_vlan_operation_point(int port, e_vlan_operation_point_t *point);
extern int xpon_get_vlan_tci(struct sk_buff* skb);
extern int multicast_data_pack(struct sk_buff* skb);

void init_xpon_igmp_macro_compatible(void);
//int is_veip_acl_list_enable(void);

/*EXPORT_SYMBOL*/
extern void tc3162wdog_kick(void);
extern u32 random32(void);
extern struct sk_buff *__vlan_put_tag(struct sk_buff *skb, u16 vlan_tci);

#endif
