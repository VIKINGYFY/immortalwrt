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
	pon_vlan_filter.h

	Abstract:

	Revision History:
	Who			When			What
	--------	----------		----------------------------------------------
	Name		Date			Modification logs
	Wayne.lee	2012/8/14	Create
*/
#ifndef  _PON_VLAN_FILTER_H_
#ifdef TCSUPPORT_PON_VLAN_FILTER
#define _PON_VLAN_FILTER_H_

#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>

#include <linux/netdevice.h>
#include <linux/if_bridge.h>
#include "pon_vlan.h"
#include "xpon_const.h"

#ifndef OUT
#define OUT
#endif
#ifndef IN
#define IN
#endif

struct pon_vlan_list{
	struct list_head		vlanFilterList;
	spinlock_t			lock;	
};
/**debug level ***/
#define GPON_VLAN_FILTER_DEBUG_LEVEL_NO_MSG		0
#define GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR		1
#define GPON_VLAN_FILTER_DEBUG_LEVEL_WARN		2
#define GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE		3

#define	GPON_PKT_VLAN_FILTER_DISCARD	1
#define	GPON_PKT_VLAN_FILTER_FWD		0

#define 	GPON_VLAN_FILTER_HANDLE_ANI_RX_VLAN_TAG	(1<<0)//handle original vlan tag
#define 	GPON_VLAN_FILTER_HANDLE_ANI_TX_VLAN_TAG	(1<<1)//handel the modified vlan tag
#define 	GPON_VLAN_FILTER_HANDLE_UNI_RX_VLAN_TAG	(1<<2)//handel the modified vlan tag	
#define 	GPON_VLAN_FILTER_HANDLE_UNI_TX_VLAN_TAG	(1<<3)//handle original vlan tag



#define	OMCI_MAC_BRIDGE_PORT_ETH_LAN_MAX_PORT_NUM	MAX_ECNT_ETHER_PORT_NUM
#define 	GPON_VLAN_FILTER_PREFIX_PON_IFC_NAME 			"nas"
#define 	GPON_VLAN_FILTER_PREFIX_PON_IFC_NAME_LEN		3
#define 	GPON_VLAN_FILTER_PREFIX_ETH_LAN_IFC_NAME 		"eth0."
#define 	GPON_VLAN_FILTER_PREFIX_ETH_LAN_IFC_NAME_LEN	5

#define	GPON_VLAN_FILTR_VLAN_TAG_NUM_0		0
#define	GPON_VLAN_FILTR_RULE_DIR_RX			0
#define	GPON_VLAN_FILTR_RULE_DIR_TX			1

#define 	GPON_VLAN_FILTER_VLAN_TAG_VID		0
#define 	GPON_VLAN_FILTER_VLAN_TAG_PBIT		1
#define 	GPON_VLAN_FILTER_VLAN_TAG_TCI		2

#define	GPON_VLAN_FILTR_RULE_PBIT_FIELD			0xe000
#define	GPON_VLAN_FILTR_RULE_VID_FIELD			0x0fff
#define	GPON_VLAN_FILTR_RULE_PBIT_VID_FIELD		0xefff

#define FIND_GPON_VLAN_FILTER_RULE		1
#define NOT_FIND_GPON_VLAN_FILTER_RULE	0


/*
**********************************************************************************
vlan filter  data structure.
**********************************************************************************
*/
typedef struct gponVlanFilterKernel_s{
	__u16 port; //LAN port 0~3, ANI port: 0~31
	__u8 portType;//0:LAN port, 1:ANI port
	__u8 type; //bit0::set for untagged frame, bit1:set for tagged frame
	__u8 untaggedAction; //0:bridge, 1:discard
	/*
		0:bridge, 
		1:discard, 		
		21: when egress frame match vid in vlan list, then filter, others is forward.(Action g)
		22: when egress frame match pbit in vlan list, then filter, others is forward.(Action g)
		23: when egress frame match TCI in vlan list, then filter, others is forward.(Action g)		
		31: when ingress frame match vid in vlan list, then bridge, others is filter.(Action h)
		     when egress frame match vid in vlan list, then bridge, others is filter.
		32:when ingress frame match pbit in vlan list, then bridge, others is filter.(Action h)
		     when egress frame match pbit in vlan list, then bridge, others is filter.
		33:when ingress frame match TCI in vlan list, then bridge, others is filter.(Action h)
		     when egress frame match TCI in vlan list, then bridge, others is filter.
		41:when egress frame match vid in vlan list, then forward, others is filter.(Action j)
		42:when egress frame match pbit in vlan list, then forward, others is filter.(Action j)
		43:when egress frame match TCI in vlan list, then forward, others is filter.(Action j)
	*/
	__u8 taggedAction;
	__u8 maxValidVlanListNum; //max valid number in vlan list.
	__u8 reserved;
	__u16 vlanList[MAX_GPON_VLAN_FILTER_LIST];
	struct list_head	list;
	struct rcu_head		rcu;
}gponVlanFilterKernel_t, *gponVlanFilterKernel_ptr;

int convertVlanFilterKernelStruct(IN gponVlanFilterIoctl_ptr inRule_ptr, OUT gponVlanFilterKernel_ptr outRule_ptr);
int checkGponVlanFilterRule(IN gponVlanFilterKernel_ptr vlanFilterRule_ptr);
int addGponVlanFilterRuleInKernel(IN gponVlanFilterIoctl_ptr vlanFilterIoctlRule_ptr);
int getGponVlanFilterRuleInKernel(gponVlanFilterIoctl_ptr ioctlRule_ptr);
void delGponVlanFilterRuleInKernel(IN __u16 port, IN __u8 portType);
void cleanGponVlanFilterRuleInKernel(void);
void displayAllGponVlanFilterRuleInKernel(void);
int initVlanList(void);
int setGponVlanFilterDbgLeverInKernel(IN __u8 dbgLevel);
int matchVlanFilterListByType(IN __u16 * vlanList, IN __u8  maxValidVlanListNum, IN __u8 tagType, IN __u16 vlan_tag);
int matchVlanFilterRuleOp(IN __u16 port, IN __u8 portType, IN __u8 type, IN __u16 vlan_tag, IN __u8 dir );
int matchVlanFilterRule(struct sk_buff *skb, __u8 ponVlanFilterDirFlag, __u8 * discardFlag);
int pon_vlan_uni_filter_switch_option(pon_vlan_ioctl * data,void * arg);

extern __u8 gponVlanFilterDbgFlag ;
#endif
#endif
