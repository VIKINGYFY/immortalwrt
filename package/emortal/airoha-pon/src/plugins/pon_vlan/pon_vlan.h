/*
 ***************************************************************************
 * MediaTeK Inc.
 * 4F, No. 2 Technology	5th	Rd.
 * Science-based Industrial	Park
 * Hsin-chu, Taiwan, R.O.C.
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
	pon_vlan.h

	Abstract:

	Revision History:
	Who			When			What
	--------	----------		----------------------------------------------
	Name		Date			Modification logs
	Reid.Ma		2013/2/4	Create
*/


#ifndef PON_VLAN_H_
#define PON_VLAN_H_
#include "xpon_pon_vlan_ioctl.h"


#define VLAN_RULE_LIMIT		80
#define INVALID_VLAN_RULE_NUM		(VLAN_RULE_LIMIT + 1)
#define DEFAULT_RULE_NOT_MATCH      (VLAN_RULE_LIMIT + 2)

extern unsigned int pon_vlan_dev_offset;

#define MAX_PON_IF			64
#define MAX_PORT			16
#define MAX_MAC_VID			32

#define MODE_HGU 0
#define MODE_SFU 1

#define setPbit(x,y) ((x) = (((x) & 0x1FFF) | (y) << 13))
#define getPbit(x) (((x) & 0xE000) >> 13)
#define setDEI(x,y) ((x) = (((x) & 0xEFFF) | (y) << 12))
#define getDEI(x) (((x) & 0x1000) >> 12)
#define setVID(x,y) ((x) = (((x) & 0xF000) | (y)))
#define getVID(x) ((x) & 0x0FFF)


#define HIGH_PRIORITY 0
#define LOW_PRIORITY 1
#define PRIORITY_DEFAULT_ENTRY 2


#define IGMP_MODE_PASS_THROUGH 0
#define IGMP_MODE_ADD_TAG 1
#define IGMP_MODE_MODIFY_TCI 2
#define IGMP_MODE_MODIFY_VID 3


#define HANDLE_TAG_LIMIT 4
#define PROTOCOL_IPV4 4
#define PROTOCOL_IPV6 6

#define UNKNOWN_STEAM 0
#define UPSTREAM 1
#define DOWNSTREAM 2

#define ADD_FIRST_TAG 1
#define ADD_SECOND_TAG 2
#define ADD_AND_CHANGE_OUTER_TAG 23
#define DEL_AND_CHANGE_INNER_TAG 33

#define FILTER_FIRST_TAG 1
#define FILTER_SECONE_TAG 2

#define CHANGE_INNER_TAG 0
#define CHANGE_OUTER_TAG 1

#define MATCH_FIRST_TAG 1
#define MATCH_SECOND_TAG 2
#define MATCH_THIRD_TAG 3
#define MATCH_FOURTH_TAG 4

#define FILTER_TPID_DO_NOT_CARE 0
#define FILTER_TPID_8100 1
#define FILTER_TPID_EQUAL_TO_INPUT_TPID 2
#define FILTER_TPID_EQUAL_TO_OUTPUT_TPID 3

#define FILTER_PRI_DO_NOT_CARE 8

#define FILTER_DEI_DO_NOT_CARE 2

#define FILTER_VID_DO_NOT_CARE 4096

#define FILTER_ETP_DO_NOT_CARE 0

#define TREAT_TPID_8100 0
#define TREAT_TPID_OUTPUT_TPID 1
#define TREAT_TPID_COPY_FROM_INNER 2
#define TREAT_TPID_COPY_FROM_OUTER 3
#define TREAT_TPID_INPUT_TPID 4


#define TREAT_PRI_COPY_FROM_INNER 8
#define TREAT_PRI_COPY_FROM_OUTER 9
#define TREAT_PRI_BASED_ON_DSCP 10

#define TREAT_DEI_COPY_FROM_INNER 2
#define TREAT_DEI_COPY_FROM_OUTER 3

#define TREAT_VID_COPY_FROM_INNER 4096
#define TREAT_VID_COPY_FROM_OUTER 4097


#define METHOD_TRANSPARENT 0
#define METHOD_BLOCK 1
#define METHOD_ADD_TAG 2
#define METHOD_DEL_TAG 3
#define METHOD_CHANGE_TAG 4

#define TRANSPARENT_MODE 	0	// 0:transparent 				
#define INVERSE_MODE 		1	// 1: inverse operation to upstream
#define DOWNSTEAM_MODE_2	2 	// 2: inverse VID and P-bit, unmatch forward
#define DOWNSTEAM_MODE_3 	3 	// 3: inverse VID, unmatch forward
#define DOWNSTEAM_MODE_4 	4 	// 4: inverse P-bit, unmatch forward
#define DOWNSTEAM_MODE_5	5 	// 5: inverse VID and P-bit, unmatch discard
#define DOWNSTEAM_MODE_6 	6 	// 6: inverse VID, unmatch discard
#define DOWNSTEAM_MODE_7 	7 	// 7: inverse P-bit, unmatch discard
#define DOWNSTEAM_MODE_8 	8 	// 8: Discard all downstream packet
#define STRIPPED_MODE 		9	// strip a tag

#define DOWNSTREAM_MODE_VID 	(1<<0)
#define DOWNSTREAM_MODE_DEI 	(1<<1)
#define DOWNSTREAM_MODE_PRI 	(1<<2) 
#define DOWNSTREAM_MODE_TPID 	(1<<3) 
#define DOWNSTREAM_MODE_MATCH_ALL 0xf  //TPID,PRI,DEV,VID

#define DOWNSTREAM_MODE_UNMATCH_FORWARD 0
#define DOWNSTREAM_MODE_UNMATCH_DISCARD 1

#define IF_VLAN_TRANSPARENT 4096
#define IF_VLAN_BLOCK 4097

#define RULE_MATCH_TOTAL 1
#define RULE_MATCH_VID	2
#define RULE_MATCH_PRI	3
#define RULE_MATCH_PRI_VID	4


#define EPON_PRI_REMARK_DEFAULT_VALUE  0xff
#define ECNT_1GETHER_PORT_NUM      4

#define PONVLAN_RESORT_TIMER		(2000)

typedef struct pon_vlan_sub_sule_s
{
	__u32 tpid;
	__u8 pri;
	__u8 dei;
	__u16 vid;
}pon_vlan_sub_rule, *pon_vlan_sub_rule_p;

typedef struct mac_vid_pair_s
{
	__u8 src_mac[6];
	__u16 vid_mask;
	__u16 org_outer_vid;
	__u16 org_inner_vid;
	__u16 rs_outer_vid;
	__u16 rs_inner_vid;
	__u8 uprule_index;
	__u8 org_inner_pbit;
	__u8 org_outer_pbit;

#define ORG_OUTER_PBIT	(1<<6)
#define ORG_INNER_PBIT  (1<<5)
#define RULE_ENABLE		(1<<4)
#define ORG_OUTER_VID	(1<<3)
#define ORG_INNER_VID	(1<<2)
#define RS_OUTER_VID	(1<<1)
#define RS_INNER_VID	(1<<0)

	long last_time;
}mac_vid_pair, *mac_vid_pair_p;

typedef struct ds_vlan_info_s
{
	__u8  pon_tag_num;
	__u16 pon_vlan_tpid[2];
	__u16 pon_vlan_tci[2];
	__u16 ethertype;
	
}ds_vlan_info, *ds_vlan_info_p;

typedef struct pon_vlan_s
{
	struct timer_list		resort_timer;
	__u8 up_rule_count;
	__u8 down_rule_count;
	__u8 rule_limit;
	__u16 port_index;
	__u8 enable_default_rule;
	__u8 down_stream_mode;

	__u16 input_tpid;
	__u16 output_tpid;
	__u8 dscp_map[64];

	__u8 igmp_mode;
	__u16 igmp_tci;

	mac_vid_pair mac_vid[MAX_MAC_VID];
	__u8 mac_bind_vlan_enable;
	__u32 user_group;
	pon_vlan_rule * up_rule;
	pon_vlan_rule * down_rule;
	
	uint8_t downstream_mode_mask;	//Downstram mode mask, TPID/PRI/DEI/VID: 3/2/1/0
	uint8_t downstream_unmatch_oper;	//0: foward, 1: discard 	
}pon_vlan,*pon_vlan_p;

typedef struct pon_vlan_all_s
{
	int vlan_enable_flag;
	int veip_enable_flag;
	int uni_filter_enable_flag;
	int ds_bcast_1toN_flag;
	__u8 virtual_port_count;
	__u8 lan_port_count;
	//__u8 xfi_lan_port_count;	
	__u8 wlan_port_count;
	__u8 wlan_ac_port_count;
	__u8 usb_port_count;
	__u8 ipHost_port_count;
    __u8 multi_filter_enable;

	__u16 total_port_count;

	__u16 onu_mode;
	__u16 xpon_mode;	/* 0,auto mode; 1,gpon; 2, epon */

	int total_special_tpid;
	__u16 tpid_counter;
	struct packet_type * pon_vlan_type;

	/*
		when running PCP drop precedence,cdmrx decode frames and 
		cdmtx encode frames in HW forwarding path.
		In the SW path,we need to decode frames as cdmrx, so that frame
		can be encoded correctly by cdmtx.
	*/
	__u16 pcp_mode;//0:8P0D  1:7P1D  2:6P2D  3:5P3D
		
	int igmp_enable_flag;
	pon_vlan * pon_vlan_data;

	int mac_vlan_time;
	__u8 user_group_enable_flag;
	__u8 if_vlan_bind_enable_flag;
	__u8 resort_enable_flag;
	__u16 pon_if_vlan_pair_data[MAX_PON_IF];
	
}pon_vlan_all,*pon_vlan_all_p;

typedef struct original_tag_s
{
	__u32 tag_num;
	
	__u16 inner_tpid;
	__u16 inner_tci;

	__u16 outer_tpid;
	__u16 outer_tci;

}original_tag,*original_tag_p;

#define CHECK_MARK_NUM        4
#define CHECK_DST_MAC         1<<0
#define CHECK_SRC_MAC         1<<1
#define CHECK_OUTER_VID       1<<2
#define CHECK_INNER_VID       1<<3


#define     PONVLAN_MSG_ERR         1<<0
#define     PONVLAN_MSG_WARNING     1<<1
#define     PONVLAN_MSG_TRACE       1<<2
#define     PONVLAN_DROP_TRACE      1<<3
#define     PONVLAN_DROP_DUMP_UP    1<<4
#define     PONVLAN_DROP_DUMP_DOWN  1<<5


#define PONVLAN_PRINT(level, F, B...) do{ \
                                                if((level) & DBG_Level) \
                                                    printk("[%s:%d]: " F "\r\n",  __FUNCTION__, __LINE__, ##B) ;\
                                    }while(0)
												
 typedef void (*ponvlanTimerCallback)(TIMER_FUN_PAAM);                                   
int pon_vlan_create_timer(struct timer_list *timer, ponvlanTimerCallback callback, unsigned long param,unsigned long expire);
#define PONVLAN_CREATE_TIMER(timer,func,para,expire) pon_vlan_create_timer(timer,func,para,expire)
#define PONVLAN_START_TIMER(timer,para)  { timer.expires = para; mod_timer(&timer, (jiffies + ((timer.expires*HZ)/1000))) ; }


extern int gpon_init_ani_map(void);
extern int gpon_ani_opt_dispatch(unsigned int cmd, void* arg);
extern int gpon_ani_map_get_by_gem(__u16 gem_id,__u16* ani_ptr);

#endif

