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
	pon_vlan_filter.c

	Abstract:

	Revision History:
	Who			When			What
	--------	----------		----------------------------------------------
	Name		Date			Modification logs
	Wayne.lee	2012/8/14	Create
*/
#ifdef TCSUPPORT_PON_VLAN_FILTER
//#include <linux/skbuff.h>
//#include <linux/netdevice.h>
//#include <linux/slab.h>
//#include <linux/string.h>
//#include <linux/inet.h>
//#include <linux/in.h>
//#include <linux/ipv6.h>
#include "pon_vlan_filter.h"
#include "pon_vlan.h"

#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include "linux/libcompileoption.h"
#include <lan_port/lan_port_info.h>
#include <ecnt_hook/ecnt_hook_xpon_mapping.h>

#ifdef TCSUPPORT_XPON_IGMP
extern int (*xpon_sfu_up_multicast_vlan_hook)(struct sk_buff *skb, int clone);
extern int (*xpon_sfu_multicast_protocol_hook)(struct sk_buff *skb);
#endif
extern pon_vlan_all pon_vlan_all_data;


#define VLAN_FILTER_IP_HOST_PORT_ID 10
#define VLAN_FILTER_XFI_LAN_PORT_ID MAX_ECNT_ETHER_PORT_NUM


/*******************************************************************************************************************************
globle variable

********************************************************************************************************************************/
struct pon_vlan_list gPon_vlan_list;

__u8 gponVlanFilterDbgFlag = GPON_VLAN_FILTER_DEBUG_LEVEL_NO_MSG;
extern pon_vlan_all pon_vlan_all_data;

/*******************************************************************************************************************************
general function

********************************************************************************************************************************/
#if 0
#define isdigit(x)	((x)>='0'&&(x)<='9')
static inline int atoi(char *s)
{
        int i=0;

	if(s == NULL)
	{
		printk("\r\ns is NULL ====> atoi in pon vlan");
		return -1;
	}
	
        while (isdigit(*s)) {
                i = i*10 + *(s++) - '0';
        }
        return i;
}


static void dump_filter_skb(void* ptr,int len)
{
	unsigned char* str = (unsigned  char*) ptr;
	int i;
	
	printk("\n Dump ptr = %x, len=%d \n", (int)ptr,len);
	for(i=0;i<len;i++)
	{
		printk("%02x ",str[i]);
		if ((i&0x0f)==0x0f)
			printk("\n");
	}
	return ;
}
#endif


/*******************************************************************************************
**function name
	convertVlanFilterKernelStruct
**description:
	convert ioctl vlan filter structure to kernetl vlan fitler structure
 **retrun :
 	GPON_VLAN_FILTER_SUCCESS:	success
 	GPON_VLAN_FILTER_FAIL:	failure
**parameter:
  	inRule_ptr :	ioctrl structure
  	outRule_ptr: output kernel structure
********************************************************************************************/
int convertVlanFilterKernelStruct(IN gponVlanFilterIoctl_ptr inRule_ptr, OUT gponVlanFilterKernel_ptr outRule_ptr){
	int ret = GPON_VLAN_FILTER_FAIL;

	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n convertVlanFilterKernelStruct->start");
	if(inRule_ptr == NULL || outRule_ptr == NULL){
		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
			printk("\r\n convertVlanFilterKernelStruct-->inRule_ptr == NULL || outRule_ptr == NULL");
		goto end;
	}
	outRule_ptr->port = inRule_ptr->port;
	outRule_ptr->portType = inRule_ptr->portType;
	outRule_ptr->type = inRule_ptr->type;
	outRule_ptr->untaggedAction = inRule_ptr->untaggedAction;
	outRule_ptr->taggedAction = inRule_ptr->taggedAction;
	outRule_ptr->maxValidVlanListNum = inRule_ptr->maxValidVlanListNum;
	//outRule_ptr->reserved = inRule_ptr->cleanFlag;
	memcpy(outRule_ptr->vlanList, inRule_ptr->vlanList, MAX_GPON_VLAN_FILTER_LIST_BYTES);
	
	ret = GPON_VLAN_FILTER_SUCCESS;
end:
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n convertVlanFilterKernelStruct-->ret = 0x%02x",ret);
	return ret;
}
/*******************************************************************************************
**function name
	checkGponVlanFilterRule
**description:
	check vlan filter rule
 **retrun :
 	GPON_VLAN_FILTER_SUCCESS:	success
 	GPON_VLAN_FILTER_FAIL:	failure
**parameter:
  	vlanFilterRule_ptr :	rule content
********************************************************************************************/
int checkGponVlanFilterRule(IN gponVlanFilterKernel_ptr vlanFilterRule_ptr){
	int ret = GPON_VLAN_FILTER_FAIL;

	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n checkGponVlanFilterRule-->start");
	if(vlanFilterRule_ptr == NULL){
		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
			printk("\r\n checkGponVlanFilterRule-->vlanFilterRule_ptr == NULL");
		goto end;
	}

	if((vlanFilterRule_ptr->portType != GPON_VLAN_FILTR_PORT_TYPE_LAN)
		&& (vlanFilterRule_ptr->portType != GPON_VLAN_FILTR_PORT_TYPE_ANI)){
		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
			printk("\r\n checkGponVlanFilterRule-->vlanFilterRule_ptr->portType == 0x%02x, fail",vlanFilterRule_ptr->portType);
		goto end;
	}

	if((vlanFilterRule_ptr->type & GPON_VLAN_FILTR_TYPE_UNTAGGED)
		&& (vlanFilterRule_ptr->untaggedAction != GPON_VLAN_FILTER_ACTION_BRIDGE)
		&& (vlanFilterRule_ptr->untaggedAction != GPON_VLAN_FILTER_ACTION_DISCARD)){
		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
			printk("\r\n checkGponVlanFilterRule-->vlanFilterRule_ptr->untaggedAction == 0x%02x, fail",vlanFilterRule_ptr->untaggedAction);
		goto end;
	}

	if(vlanFilterRule_ptr->type & GPON_VLAN_FILTR_TYPE_TAGGED){
		switch(vlanFilterRule_ptr->taggedAction){
			case GPON_VLAN_FILTER_ACTION_BRIDGE:
			case GPON_VLAN_FILTER_ACTION_DISCARD:
			case GPON_VLAN_FILTER_ACTION_G_VID:
			case GPON_VLAN_FILTER_ACTION_G_PBIT:
			case GPON_VLAN_FILTER_ACTION_G_TCI:
			case GPON_VLAN_FILTER_ACTION_H_VID:
			case GPON_VLAN_FILTER_ACTION_H_PBIT:
			case GPON_VLAN_FILTER_ACTION_H_TCI:
			case GPON_VLAN_FILTER_ACTION_J_VID:			
			case GPON_VLAN_FILTER_ACTION_J_PBIT:
			case GPON_VLAN_FILTER_ACTION_J_TCI:
				break;
			default:
		  		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
					printk("\r\n checkGponVlanFilterRule-->vlanFilterRule_ptr->taggedAction == 0x%02x, fail",vlanFilterRule_ptr->taggedAction);
				goto end;
		}
	}
	if(vlanFilterRule_ptr->maxValidVlanListNum > MAX_GPON_VLAN_FILTER_LIST){
		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
			printk("\r\n checkGponVlanFilterRule-->vlanFilterRule_ptr->maxValidVlanListNum == 0x%02x, fail",vlanFilterRule_ptr->maxValidVlanListNum);
		goto end;
	}
	
	
	ret = GPON_VLAN_FILTER_SUCCESS;
end:
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n checkGponVlanFilterRule-->ret = 0x%02x",ret);
	return ret;
}

/*******************************************************************************************
**function name
	findGponVlanFilterRuleInKernel
**description:
	according the port and portType, find the rule
 **retrun :
 	rule:	success
 	NULL:	failure
**parameter:
  	port :	port id
  	portType: 0:UNI, 1:ANI
********************************************************************************************/
static inline gponVlanFilterKernel_ptr findGponVlanFilterRuleInKernel(IN __u16 port, IN __u8 portType){
	gponVlanFilterKernel_ptr filterRule_ptr = NULL;

	list_for_each_entry_rcu(filterRule_ptr, &gPon_vlan_list.vlanFilterList, list){
		if ((filterRule_ptr->port == port) &&
			(filterRule_ptr->portType == portType)){
			if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
				printk("\r\n findGponVlanFilterRuleInKernel-->find");
			return filterRule_ptr;
		}
	}
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n findGponVlanFilterRuleInKernel-->not find");
	return NULL;
}

/*******************************************************************************************
**function name
	createGponVlanFilterRuleInKernel
**description:
	add vlan filter rule
 **retrun :
 	rule:	success
 	NULL:	failure
**parameter:
   	rule_ptr :	new rule information
********************************************************************************************/
static gponVlanFilterKernel_ptr createGponVlanFilterRuleInKernel(gponVlanFilterKernel_ptr rule_ptr){
	gponVlanFilterKernel_ptr newRule_ptr = NULL;

	newRule_ptr = kmalloc(sizeof(gponVlanFilterKernel_t), GFP_ATOMIC);
	if (newRule_ptr){
		newRule_ptr->port = rule_ptr->port;
		newRule_ptr->portType = rule_ptr->portType;
		newRule_ptr->type = rule_ptr->type;
		newRule_ptr->untaggedAction = rule_ptr->untaggedAction;
		newRule_ptr->taggedAction = rule_ptr->taggedAction;
		newRule_ptr->maxValidVlanListNum = rule_ptr->maxValidVlanListNum;
		memcpy(newRule_ptr->vlanList, rule_ptr->vlanList, MAX_GPON_VLAN_FILTER_LIST_BYTES);
		list_add_tail_rcu(&newRule_ptr->list, &gPon_vlan_list.vlanFilterList);	
	}
	
	return newRule_ptr;
}
/*******************************************************************************************
**function name
	addGponVlanFilterRuleInKernel
**description:
	add a vlan filter rule to gpon vlan filter list
 **retrun :
 	GPON_VLAN_FILTER_SUCCESS:	success
 	GPON_VLAN_FILTER_FAIL:	failure
	GPON_VLAN_FILTER_ENTRY_EXIST:	this rule is exists.
	others : invalid.
**parameter:
  	vlanFilterIoctlRule_ptr :	rule content
********************************************************************************************/
int addGponVlanFilterRuleInKernel(IN gponVlanFilterIoctl_ptr vlanFilterIoctlRule_ptr){
	int ret = GPON_VLAN_FILTER_FAIL;
	gponVlanFilterKernel_ptr newRule_ptr = NULL;
	gponVlanFilterKernel_t vlanFilterRule;
	gponVlanFilterKernel_ptr vlanFilterRule_ptr = NULL;
//	__u8 findFlag = NOT_FIND_GPON_VLAN_FILTER_RULE;
//	__u8 vlanFilterStructSize = 0;

	spin_lock_bh(&gPon_vlan_list.lock);
	 if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n addGponVlanFilterRuleInKernel->start");
	 if(vlanFilterIoctlRule_ptr == NULL){
		 if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
			printk("\r\n addGponVlanFilterRuleInKernel->vlanFilterIoctlRule_ptr == NULL, fail");
		 goto end;
	 }
	/*convert structrure*/
	vlanFilterRule_ptr = &vlanFilterRule;
	memset(vlanFilterRule_ptr, 0, sizeof(gponVlanFilterKernel_t));
	convertVlanFilterKernelStruct(vlanFilterIoctlRule_ptr, vlanFilterRule_ptr);
	
	/*check vlan rule content*/
	if(checkGponVlanFilterRule(vlanFilterRule_ptr) != GPON_VLAN_FILTER_SUCCESS){
		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
			printk("\r\n addGponVlanFilterRuleInKernel-->checkGponVlanFilterRule, fail");
		goto end;
	}

	newRule_ptr = findGponVlanFilterRuleInKernel(vlanFilterRule_ptr->port, vlanFilterRule_ptr->portType);
	if(newRule_ptr){
		newRule_ptr->type = vlanFilterRule_ptr->type;
		newRule_ptr->untaggedAction = vlanFilterRule_ptr->untaggedAction;
		newRule_ptr->taggedAction = vlanFilterRule_ptr->taggedAction;
		newRule_ptr->maxValidVlanListNum = vlanFilterRule_ptr->maxValidVlanListNum;
		memcpy(newRule_ptr->vlanList, vlanFilterRule_ptr->vlanList, MAX_GPON_VLAN_FILTER_LIST_BYTES);
		ret = GPON_VLAN_FILTER_ENTRY_EXIST;
	}else{
		newRule_ptr = createGponVlanFilterRuleInKernel(vlanFilterRule_ptr);
		if(newRule_ptr){
			ret = GPON_VLAN_FILTER_SUCCESS;
		}
	}
	
end:
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n addGponVlanFilterRuleInKernel-->ret=0x%02x",ret);

	spin_unlock_bh(&gPon_vlan_list.lock);

	return ret;

}
/*******************************************************************************************
**function name
	getGponVlanFilterRuleInKernel
**description:
	get gpon vlan filter rule in this port.
 **retrun :
 	GPON_VLAN_FILTER_SUCCESS:	success
 	GPON_VLAN_FILTER_FAIL:	failure
**parameter:
  	rule_ptr: return this vlan filter rule
********************************************************************************************/
int getGponVlanFilterRuleInKernel(gponVlanFilterIoctl_ptr ioctlRule_ptr){
	int ret = GPON_VLAN_FILTER_FAIL;
	gponVlanFilterKernel_ptr tempVlanFilter_ptr = NULL;

	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n getGponVlanFilterRuleInKernel->start");
	if(ioctlRule_ptr == NULL){
		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
			printk("\r\n getGponVlanFilterRuleInKernel-->ioctlRule_ptr == NULL");
		goto end;
	}
	rcu_read_lock();
	tempVlanFilter_ptr = findGponVlanFilterRuleInKernel(ioctlRule_ptr->port, ioctlRule_ptr->portType);
	if(tempVlanFilter_ptr){
		ioctlRule_ptr->port = tempVlanFilter_ptr->port;
		ioctlRule_ptr->portType = tempVlanFilter_ptr->portType;
		ioctlRule_ptr->type = tempVlanFilter_ptr->type;
		ioctlRule_ptr->untaggedAction = tempVlanFilter_ptr->untaggedAction;
		ioctlRule_ptr->taggedAction = tempVlanFilter_ptr->taggedAction;
		ioctlRule_ptr->maxValidVlanListNum = tempVlanFilter_ptr->maxValidVlanListNum;
		//ioctlRule_ptr->cleanFlag = tempVlanFilter_ptr->reserved;
		memcpy(ioctlRule_ptr->vlanList, tempVlanFilter_ptr->vlanList, MAX_GPON_VLAN_FILTER_LIST_BYTES);
		ret = GPON_VLAN_FILTER_SUCCESS;
	}
	rcu_read_unlock();
end:
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n getGponVlanFilterRuleInKernel-->ret=0x%02x",ret);
	return ret;
}


/*******************************************************************************************
**function name
	RcuFreevlanFilterRule
**description:
	free space
 **retrun :
**parameter:
********************************************************************************************/
static void RcuFreevlanFilterRule(struct rcu_head *head){
	
	gponVlanFilterKernel_ptr ent
		= container_of(head, struct gponVlanFilterKernel_s, rcu);
	kfree(ent);	
	return;
}


/*******************************************************************************************
**function name
	deleteVlanFilterEntry
**description:
	del this entry.
 **retrun :

**parameter:
  	rule_ptr: del this  vlan filter rule
********************************************************************************************/
static inline void deleteVlanFilterEntry(gponVlanFilterKernel_ptr rule_ptr){
	
	list_del_rcu(&rule_ptr->list);
	call_rcu(&rule_ptr->rcu, RcuFreevlanFilterRule);
	
	return;
}

/*******************************************************************************************
**function name
	delGponVlanFilterRuleInKernel
**description:
	del gpon vlan filter rule via the port id.
 **retrun :

**parameter:
  	port :	port id
  	portType: lan or wan (0/1)
********************************************************************************************/
void delGponVlanFilterRuleInKernel(IN __u16 port, IN __u8 portType){
//	int ret = GPON_VLAN_FILTER_FAIL;
	gponVlanFilterKernel_ptr currVlanFilter_ptr = NULL;

	spin_lock_bh(&gPon_vlan_list.lock);
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n delGponVlanFilterRuleInKernel->");
	
	list_for_each_entry_rcu(currVlanFilter_ptr, &gPon_vlan_list.vlanFilterList, list) {
		if((currVlanFilter_ptr->port != port)
			|| (currVlanFilter_ptr->portType != portType)){
			continue;
		}
		deleteVlanFilterEntry(currVlanFilter_ptr);
	}
	spin_unlock_bh(&gPon_vlan_list.lock);

	return;
}


/*******************************************************************************************
**function name
	cleanGponVlanFilterRuleInKernel
**description:
	clean vlan filter rule
 **retrun :
**parameter:
********************************************************************************************/
void cleanGponVlanFilterRuleInKernel(void){
	gponVlanFilterKernel_ptr currVlanFilter_ptr = NULL;

	spin_lock_bh(&gPon_vlan_list.lock);
	list_for_each_entry_rcu(currVlanFilter_ptr, &gPon_vlan_list.vlanFilterList, list) {
		deleteVlanFilterEntry(currVlanFilter_ptr);
	}
	spin_unlock_bh(&gPon_vlan_list.lock);
	
	return;
}
/*******************************************************************************************
**function name
	displayAllGponVlanFilterRuleInKernel
**description:
	display all vlan filter rule information in vlan fitler list
 **retrun :
**parameter:
********************************************************************************************/
void displayAllGponVlanFilterRuleInKernel(void){
	gponVlanFilterKernel_ptr currVlanFilter_ptr = NULL;
	int index= 0;
	int i = 0;
	
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n displayAllGponVlanFilterRuleInKernel->start");
	printk("\r\n port: LAN port 0~3, ANI port: 0~%d",(GPON_MAX_ANI_INTERFACE-1));
	printk("\r\n portType:0:LAN port, 1:ANI port");
	printk("\r\n type:bit0::set for untagged frame, bit1:set for tagged frame");
	printk("\r\n untaggedAction:0:bridge, 1:discard");
	printk("\r\n taggedAction:0:bridge, 1:discard,21:vid(g),22:pbit(g),23:tci(g)");
	printk("\r\n 31:vid(h),32:pbit(h),33:tci(h)");
	printk("\r\n 41:vid(j),42:pbit(j),43:tci(j)");
	printk("\r\n maxValidVlanListNum: max valid num in vlan list");
	printk("\r\n reserved: this colum is invalid.");
	printk("\r\n vlanList: vlan list \n");

	printk("\r\nindex \tport \tportType \ttype \tuntagAction \ttagAction \tmaxValidNum \treserved\n");
	printk("\r\n vlanlist:");
	rcu_read_lock();	
	list_for_each_entry_rcu(currVlanFilter_ptr, &gPon_vlan_list.vlanFilterList, list) {
		printk("\r\n%d", index);
		printk(" \t0x%02x", currVlanFilter_ptr->port);
		printk(" \t0x%02x", currVlanFilter_ptr->portType);
		printk(" \t0x%02x", currVlanFilter_ptr->type);
		printk(" \t%d", currVlanFilter_ptr->untaggedAction);
		printk(" \t%d", currVlanFilter_ptr->taggedAction);
		printk(" \t%d", currVlanFilter_ptr->maxValidVlanListNum);
		printk(" \t%d", currVlanFilter_ptr->reserved);
		printk("\r\n vlanList: ");
		for(i=0; i<MAX_GPON_VLAN_FILTER_LIST; i++){
			printk("\r\n vlanList[%d]=0x%02x", i, currVlanFilter_ptr->vlanList[i]);
		}
		index++;
	}
	rcu_read_unlock();
	
	printk("\r\n");	
	return;
}

/*******************************************************************************************
**function name
	initVlanList
**description:
	init vlan list
 **retrun :
 	0:	success
 	-1:	failure
**parameter:
********************************************************************************************/
int initVlanList(void){
	spin_lock_init(&gPon_vlan_list.lock);
	INIT_LIST_HEAD(&gPon_vlan_list.vlanFilterList);
	return 0;
}


/*******************************************************************************************
**function name
	setGponVlanFilterDbgLeverInKernel
**description:
	set debug level for vlan filter
 **retrun :
 	0:	success
 	1:	failure
**parameter:
	dbgLevel: debug level
********************************************************************************************/
int setGponVlanFilterDbgLeverInKernel(IN __u8 dbgLevel){

	printk("\r\n setGponVlanFilterDbgLeverInKernel->dbgLevel=0x%02x",dbgLevel);
	switch(dbgLevel){
		case GPON_VLAN_FILTER_DEBUG_LEVEL_NO_MSG:
		case GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR:
		case GPON_VLAN_FILTER_DEBUG_LEVEL_WARN:
		case GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE:
			break;
		default:
			return GPON_VLAN_FILTER_FAIL;
	}
	gponVlanFilterDbgFlag = dbgLevel;
	
	printk("\r\n setGponVlanFilterDbgLeverInKernel->current gponVlanFilterDbgFlag=0x%02x",gponVlanFilterDbgFlag);
	return GPON_VLAN_FILTER_SUCCESS;
	
}

/*******************************************************************************************
**function name
	matchVlanFilterListByType
**description:
	find matched entry in vlanlist by type
 **retrun :
 	NOT_FIND_GPON_VLAN_FILTER_RULE:	failure
 	FIND_GPON_VLAN_FILTER_RULE:	success
**parameter:
	vlanList: vlan list
	maxValidVlanListNum: max valid num in vlan list
	tagType: 0:vid, 1:pbit, 2:tci
	vlan_tag: match this vlan tag
********************************************************************************************/
int matchVlanFilterListByType(IN __u16 * vlanList, IN __u8  maxValidVlanListNum, IN __u8 tagType, IN __u16 vlan_tag){
	int findFlag = NOT_FIND_GPON_VLAN_FILTER_RULE;
	int i = 0;
	__u16 temp1 = 0;
	__u16 temp2 = 0;
	
	if(vlanList == NULL){
		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
		printk("\r\n matchVlanFilterListByType->vlanList == NULL, fail");
		goto end;
	}
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE){
		printk("\r\n matchVlanFilterListByType->maxValidVlanListNum=0x%02x", maxValidVlanListNum);
		printk("\r\n matchVlanFilterListByType->tagType=0x%02x", tagType);
		printk("\r\n matchVlanFilterListByType->vlan_tag=0x%02x", vlan_tag);
	}
	switch(tagType){
		case GPON_VLAN_FILTER_VLAN_TAG_VID:
			temp2 = (vlan_tag & GPON_VLAN_FILTR_RULE_VID_FIELD);
			break;
		case GPON_VLAN_FILTER_VLAN_TAG_PBIT:
			temp2 = (vlan_tag & GPON_VLAN_FILTR_RULE_PBIT_FIELD);
			break;
		case GPON_VLAN_FILTER_VLAN_TAG_TCI:	
//			temp2 = vlan_tag;
			temp2 = (vlan_tag & GPON_VLAN_FILTR_RULE_PBIT_VID_FIELD);
			break;
		default:
			if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
				printk("\r\n matchVlanFilterListByType->type=0x%02x, fail", tagType);
			goto end;
	}
	
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE){
		printk("\r\n matchVlanFilterListByType->temp2=0x%02x", temp2);
	}
	for(i = 0; i<maxValidVlanListNum; i++){
		if(tagType == GPON_VLAN_FILTER_VLAN_TAG_VID){
			temp1 = (vlanList[i] & GPON_VLAN_FILTR_RULE_VID_FIELD);
			if(temp1 == temp2){
				findFlag = FIND_GPON_VLAN_FILTER_RULE;
				break;
			}		
		}else if(tagType == GPON_VLAN_FILTER_VLAN_TAG_PBIT){
			temp1 = (vlanList[i] & GPON_VLAN_FILTR_RULE_PBIT_FIELD);
			if(temp1 == temp2){
				findFlag = FIND_GPON_VLAN_FILTER_RULE;
				break;
			}
		}else if(tagType == GPON_VLAN_FILTER_VLAN_TAG_TCI){
//			temp1 = vlanList[i] ;
			temp1 = (vlanList[i] & GPON_VLAN_FILTR_RULE_PBIT_VID_FIELD);
			if(temp1 == temp2){
				findFlag = FIND_GPON_VLAN_FILTER_RULE;
				break;
			}
		}else{
			//nothing
		}
	}
	
end:
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n matchVlanFilterListByType-->findFlag=0x%02x",findFlag);
	return findFlag;
}
/*******************************************************************************************
**function name
	matchVlanFilterRuleOp
**description:
	match outer vlan tag  with the vlan filter list rule
 **retrun :
 	GPON_VLAN_FILTER_SUCCESS:	success
 	GPON_VLAN_FILTER_FAIL:	failure
**parameter:
	port: LAN port 0~3, ANI port: 0~xx
	portType: 0:LAN port, 1:ANI port
	type: (1<<0): untagged frame, (1<<1):tagged frame
	vlan_tag: outer vlan tag
	dir: 0:rx, 1:tx
********************************************************************************************/
int matchVlanFilterRuleOp(IN __u16 port, IN __u8 portType, IN __u8 type, IN __u16 vlan_tag, IN __u8 dir ){
	int ret = GPON_VLAN_FILTER_FAIL;
	gponVlanFilterKernel_ptr tempVlanFilter_ptr = NULL;
	__u16  rulePortId = 0;
	int actionFlag = 0;	

	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE){
		printk("\r\n matchVlanFilterRuleOp->start");
		printk("\r\n matchVlanFilterRuleOp->port=0x%02x, portType=0x%02x, type=0x%02x, vlantag=0x%02x, dir=0x%02x", port, portType, type,vlan_tag,dir);

	}
	rcu_read_lock();
	if((type != GPON_VLAN_FILTR_TYPE_UNTAGGED) && (type != GPON_VLAN_FILTR_TYPE_TAGGED)){
		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
			printk("\r\n matchVlanFilterRuleOp->type = 0x%02x, fail",type);
		goto end;
	}
	if((dir != GPON_VLAN_FILTR_RULE_DIR_RX) && (dir != GPON_VLAN_FILTR_RULE_DIR_TX)){
		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
			printk("\r\n matchVlanFilterRuleOp->dir = 0x%02x, fail",dir);
		goto end;
	}

	
	/*get port id in rule, according to the port id in packet*/
	if(portType == GPON_VLAN_FILTR_PORT_TYPE_ANI){
		rulePortId = port;
	}else if(portType == GPON_VLAN_FILTR_PORT_TYPE_LAN){
		rulePortId = port;
	}else{
		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
			printk("\r\n matchVlanFilterRuleOp->port type = 0x%02x, fail",portType);
		goto end;
	}
	
	/*find vlan filter match rule in vlan filter list*/
	list_for_each_entry_rcu(tempVlanFilter_ptr, &gPon_vlan_list.vlanFilterList, list){
		if((tempVlanFilter_ptr->portType == portType)
			&& (tempVlanFilter_ptr->port == rulePortId)){//find the port
			if(tempVlanFilter_ptr->type & type){//find match type(untagged or tagged)
				if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE){
					printk("\r\n matchVlanFilterRuleOp->Find match rule");
				}
				if(type == GPON_VLAN_FILTR_TYPE_UNTAGGED){//untagged rule action
					if(tempVlanFilter_ptr->untaggedAction == GPON_VLAN_FILTER_ACTION_BRIDGE){
						actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FORWARD_TYPE;
					}else if(tempVlanFilter_ptr->untaggedAction == GPON_VLAN_FILTER_ACTION_DISCARD){
						actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE;
					}
				}else if(type == GPON_VLAN_FILTR_TYPE_TAGGED){//tagged rule action
					switch(tempVlanFilter_ptr->taggedAction){
						case GPON_VLAN_FILTER_ACTION_BRIDGE:
							actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FORWARD_TYPE;
							break;
						case GPON_VLAN_FILTER_ACTION_DISCARD:
							actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE;
							break;
						case GPON_VLAN_FILTER_ACTION_G_VID://TX,match: discard, others: forward
							if(dir == GPON_VLAN_FILTR_RULE_DIR_TX){
								if(matchVlanFilterListByType(tempVlanFilter_ptr->vlanList, tempVlanFilter_ptr->maxValidVlanListNum, GPON_VLAN_FILTER_VLAN_TAG_VID, vlan_tag) == FIND_GPON_VLAN_FILTER_RULE){
									actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE;
								}else{
									actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FORWARD_TYPE;
								}
							}else{
								//nothing
							}
							break;
						case GPON_VLAN_FILTER_ACTION_G_PBIT://TX,match: discard, others: forward
							if(dir == GPON_VLAN_FILTR_RULE_DIR_TX){
								if(matchVlanFilterListByType(tempVlanFilter_ptr->vlanList, tempVlanFilter_ptr->maxValidVlanListNum, GPON_VLAN_FILTER_VLAN_TAG_PBIT, vlan_tag) == FIND_GPON_VLAN_FILTER_RULE){
									actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE;
								}else{
									actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FORWARD_TYPE;
								}
							}else{
								//nothing
							}
							break;
						case GPON_VLAN_FILTER_ACTION_G_TCI://TX,match: discard, others: forward
							if(dir == GPON_VLAN_FILTR_RULE_DIR_TX){
								if(matchVlanFilterListByType(tempVlanFilter_ptr->vlanList, tempVlanFilter_ptr->maxValidVlanListNum, GPON_VLAN_FILTER_VLAN_TAG_TCI, vlan_tag) == FIND_GPON_VLAN_FILTER_RULE){
									actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE;
								}else{
									actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FORWARD_TYPE;
								}
							}else{
								//nothing
							}
							break;
						case GPON_VLAN_FILTER_ACTION_H_VID://TX or RX, match: forward, others: discard
							if(matchVlanFilterListByType(tempVlanFilter_ptr->vlanList, tempVlanFilter_ptr->maxValidVlanListNum, GPON_VLAN_FILTER_VLAN_TAG_VID, vlan_tag) == FIND_GPON_VLAN_FILTER_RULE){
								actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FORWARD_TYPE;
							}else{
								actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE;
							}
							break;
						case GPON_VLAN_FILTER_ACTION_H_PBIT://TX or RX, match: forward, others: discard
							if(matchVlanFilterListByType(tempVlanFilter_ptr->vlanList, tempVlanFilter_ptr->maxValidVlanListNum, GPON_VLAN_FILTER_VLAN_TAG_PBIT, vlan_tag) == FIND_GPON_VLAN_FILTER_RULE){
								actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FORWARD_TYPE;
							}else{
								actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE;
							}
							break;
						case GPON_VLAN_FILTER_ACTION_H_TCI://TX or RX, match: forward, others: discard
							if(matchVlanFilterListByType(tempVlanFilter_ptr->vlanList, tempVlanFilter_ptr->maxValidVlanListNum, GPON_VLAN_FILTER_VLAN_TAG_TCI, vlan_tag) == FIND_GPON_VLAN_FILTER_RULE){
								actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FORWARD_TYPE;
							}else{
								actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE;
							}
							break;
						case GPON_VLAN_FILTER_ACTION_J_VID:	//TX, match: forward, others: discard
							if(dir == GPON_VLAN_FILTR_RULE_DIR_TX){
								if(matchVlanFilterListByType(tempVlanFilter_ptr->vlanList, tempVlanFilter_ptr->maxValidVlanListNum, GPON_VLAN_FILTER_VLAN_TAG_VID, vlan_tag) == FIND_GPON_VLAN_FILTER_RULE){
									actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FORWARD_TYPE;
								}else{
									actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE;
								}
							}else{
								//nothing
							}
							break;
						case GPON_VLAN_FILTER_ACTION_J_PBIT://TX, match: forward, others: discard
							if(dir == GPON_VLAN_FILTR_RULE_DIR_TX){
								if(matchVlanFilterListByType(tempVlanFilter_ptr->vlanList, tempVlanFilter_ptr->maxValidVlanListNum, GPON_VLAN_FILTER_VLAN_TAG_PBIT, vlan_tag) == FIND_GPON_VLAN_FILTER_RULE){
									actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FORWARD_TYPE;
								}else{
									actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE;
								}
							}else{
								//nothing
							}
							break;
						case GPON_VLAN_FILTER_ACTION_J_TCI://TX, match: forward, others: discard
							if(dir == GPON_VLAN_FILTR_RULE_DIR_TX){
								if(matchVlanFilterListByType(tempVlanFilter_ptr->vlanList, tempVlanFilter_ptr->maxValidVlanListNum, GPON_VLAN_FILTER_VLAN_TAG_TCI, vlan_tag) == FIND_GPON_VLAN_FILTER_RULE){
									actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FORWARD_TYPE;
								}else{
									actionFlag = GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE;
								}
							}else{
								//nothing
							}
							break;
						default:
					  		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
								printk("\r\n matchVlanFilterRuleOp-->tempVlanFilter_ptr->taggedAction == 0x%02x, fail",tempVlanFilter_ptr->taggedAction);
							goto end;
					}
				}
			}
			break;
		}
	}
	ret = GPON_VLAN_FILTER_SUCCESS;
	if(actionFlag != 0){
		ret = actionFlag;
	}
end:
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n matchVlanFilterRuleOp-->ret=0x%02x",ret);
		
	rcu_read_unlock();	

	return ret;
}

EXPORT_SYMBOL(matchVlanFilterRuleOp);
extern int pon_vlan_get_mode(void);
int get_lan_port_index(struct sk_buff *skb)
{
	return 0;
}
#define PON_VLAN_SKB_IS_MULTICAST(skb) ( ((skb)->data[0] == 0x01) &&\
                                           ((skb)->data[1] == 0x00) &&\
                                           ((skb)->data[2] == 0x5e))
/*******************************************************************************************
**function name
	matchVlanFilterRule
**description:
	match valn filter rule  for vlan filter function
 **retrun :
 	GPON_VLAN_FILTER_SUCCESS:	success
 	GPON_VLAN_FILTER_FAIL:	failure
**parameter:
	skb: pkt buffer
	ponVlanFilterDirFlag: set pon Vlan filter type.
	discardFlag:
********************************************************************************************/
int matchVlanFilterRule(struct sk_buff *skb, __u8 ponVlanFilterDirFlag, __u8 * discardFlag){
	int ret = GPON_VLAN_FILTER_FAIL;
	int result = 0;
	__u16 currUniPort = 0;
	__u16 currAniPort = 0;

	__u8 type = 0;
	__u16 vlan_tag = 0;


	if(skb == NULL || skb->dev == NULL || skb->dev->name[0] == '\0' || discardFlag==NULL ){
		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
		printk("\r\n matchVlanFilterRule-->parameter is NULL, fail");
		goto end;
	}
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n matchVlanFilterRule-->skb->pon_tag_num=%d",skb->pon_tag_num);
	if(skb->pon_tag_num == GPON_VLAN_FILTR_VLAN_TAG_NUM_0){
		type = GPON_VLAN_FILTR_TYPE_UNTAGGED;
	}else{
		type = GPON_VLAN_FILTR_TYPE_TAGGED;
		vlan_tag = skb->pon_vlan_tci[skb->pon_tag_num-1];
	}
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		printk("\r\n matchVlanFilterRule-->vlan_tag= 0x%02x",vlan_tag);
	
	if((ponVlanFilterDirFlag & GPON_VLAN_FILTER_HANDLE_ANI_RX_VLAN_TAG)
		&& (ponVlanFilterDirFlag & GPON_VLAN_FILTER_HANDLE_UNI_TX_VLAN_TAG)){//downstream

		if(TCSUPPORT_VNPTT_VAL && !(vlan_tag & 0x0fff))  //VNPTT && vlan 0, do not discard, 
		{
		   if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
		      printk("\r\n Down stream : TCSUPPORT_VNPTT_VAL || vlan_tag= 0x%x, will do not do VLAN Filter",vlan_tag);					

		   ret = GPON_VLAN_FILTER_SUCCESS;
		   goto end;
		}

		/*******************************************************************************************
								ANI RX DOWNSTREAM VLAN Filter
		*****************************************************************************************************/

			currAniPort = skb->v_if;
            
			if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE){
				printk("\r\n matchVlanFilterRule currAniPort=0x%02x",currAniPort);
			}
            if(0 == (skb->pon_vlan_flag & PON_MULTICAST_ANI_FILTER_FLAG) && 
                ((!PON_VLAN_SKB_IS_MULTICAST(skb)) ||
                (pon_vlan_all_data.multi_filter_enable)))
            {
                result = matchVlanFilterRuleOp(currAniPort, GPON_VLAN_FILTR_PORT_TYPE_ANI, type, vlan_tag, GPON_VLAN_FILTR_RULE_DIR_RX);
                if(result == GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE)  
                {
                   if(gponVlanFilterDbgFlag >=	GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
                	  printk("\r\n ANI:matchVlanFilterRule-->vlan_tag= 0x%x, result=FILTER_TYPE",vlan_tag); 				
				
                	//vlanFilterOp = PON_PKT_VLAN_FILTER_DISCARD;				   
                	*discardFlag = GPON_PKT_VLAN_FILTER_DISCARD;
                	ret = GPON_VLAN_FILTER_SUCCESS;
                	goto end;
                }
            }

		/*******************************************************************************************
								UNI TX DOWNSTREAM VLAN Filter
		*****************************************************************************************************/

			if(pon_vlan_all_data.uni_filter_enable_flag == DISABLE){
				if(gponVlanFilterDbgFlag >= GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE){
					printk("\r\n downstream UNI TX VLAN Filter Disable\n");
				}
				
				*discardFlag = GPON_PKT_VLAN_FILTER_FWD;
				ret = GPON_VLAN_FILTER_SUCCESS;
				goto end;
			}
			if(TCSUPPORT_PON_IP_HOST_VAL && (skb->pon_vlan_flag & PON_PKT_VOIP_RX)){
				currUniPort = VLAN_FILTER_IP_HOST_PORT_ID;
			}else{
				if(TCSUPPORT_CMCCV2_VAL && PON_VLAN_SKB_IS_MULTICAST(skb))
                {
                	ret = GPON_VLAN_FILTER_SUCCESS;
                    goto end;
                }
				/*match  UNI TX port*/
				if(pon_vlan_get_mode() == MODE_HGU)
				{
						currUniPort = 0;
				}
				else
				{
					if(skb->dev->name[0] != 'e'){
						return GPON_VLAN_FILTER_FAIL;
					}
					ret = ENCT_HOOK_XPON_ETH_MAP_DEV_NAME_TO_PORT(skb->dev->name);
					if(ret == -1){
						if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR){
							printk("get port id fail by dev name=%s, do not do VLAN Filter",skb->dev->name);
						}
						ret = GPON_VLAN_FILTER_FAIL;
                    	goto end;
					}
					currUniPort = ret;
				}
			}

			if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE){
				printk("\r\n matchVlanFilterRule-->skb->dev->name= %s, currUniPort=0x%02x",skb->dev->name,currUniPort);	
			}
			
			result = matchVlanFilterRuleOp(currUniPort, GPON_VLAN_FILTR_PORT_TYPE_LAN, type, vlan_tag, GPON_VLAN_FILTR_RULE_DIR_TX);
			if(result == GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE){
				*discardFlag = GPON_PKT_VLAN_FILTER_DISCARD;
				ret = GPON_VLAN_FILTER_SUCCESS;
				goto end;
			}
	}
	else if((ponVlanFilterDirFlag & GPON_VLAN_FILTER_HANDLE_UNI_RX_VLAN_TAG)
		&& (ponVlanFilterDirFlag & GPON_VLAN_FILTER_HANDLE_ANI_TX_VLAN_TAG)){//upstream	

		/*******************************************************************************************
								UNI RX UPSTREAM VLAN Filter
		*****************************************************************************************************/
			/*match  UNI RX port*/
			if(!(skb->pon_vlan_flag & PON_PKT_SEND_TO_WAN)){
				if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE){
					printk("\r\nbegin to do up stream filter");
					printk("\r\n matchVlanFilterRule, currUniPort=0x%02x",currUniPort);
				}
				if(pon_vlan_get_mode() == MODE_HGU)
				{
					currUniPort = 0;
				}
				else
				{
					if(skb->pon_vlan_flag & PON_PKT_FROM_LAN)
					{
						ret = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
						if(ret == -1){
							if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR){
								printk("get port id fail by dev name=%s, do not do VLAN Filter",skb->dev->name);
							}
							ret = GPON_VLAN_FILTER_FAIL;
	                    	goto end;
						}
						currUniPort = ret;
					}
					else
					{
						if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE)
							printk("\r\nreturn filter fail because sfu packet is not from lan");
						return GPON_VLAN_FILTER_FAIL;
					}
				}

				if(gponVlanFilterDbgFlag >= GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
					printk("\r\n upstream UNI:skb->pon_vlan_flag == %d skb->original_dev->name == %s",skb->pon_vlan_flag,skb->original_dev->name);
		
				if( pon_vlan_all_data.uni_filter_enable_flag == DISABLE){
					*discardFlag = GPON_PKT_VLAN_FILTER_FWD;
					ret = GPON_VLAN_FILTER_SUCCESS;
					if(gponVlanFilterDbgFlag >= GPON_VLAN_FILTER_DEBUG_LEVEL_WARN)
						printk("\r\n pon_vlan_all_data.uni_filter_enable_flag == DISABLE"); 
				}else{
					result = matchVlanFilterRuleOp(currUniPort, GPON_VLAN_FILTR_PORT_TYPE_LAN, type, vlan_tag, GPON_VLAN_FILTR_RULE_DIR_RX);
					if(result == GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE){
						*discardFlag = GPON_PKT_VLAN_FILTER_DISCARD;
						ret = GPON_VLAN_FILTER_SUCCESS;
						goto end;
					}
				}
			}
			else{

			/*******************************************************************************************
								ANI TX UPSTREAM VLAN Filter
		*****************************************************************************************************/

				if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE){
					
					printk("\r\n matchVlanFilterRule-->skb->dev->name= %s, currAniPort=0x%02x",skb->dev->name,currAniPort);	
				}
				currAniPort = skb->v_if;
				result = matchVlanFilterRuleOp(currAniPort, GPON_VLAN_FILTR_PORT_TYPE_ANI, type, vlan_tag, GPON_VLAN_FILTR_RULE_DIR_TX);
				if(result == GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE){
					*discardFlag = GPON_PKT_VLAN_FILTER_DISCARD;
					ret = GPON_VLAN_FILTER_SUCCESS;
					goto end;
				}
			}
	}else{
		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
			printk("\r\n matchVlanFilterRule-->ponVlanFilterDirFlag=%d, fail",ponVlanFilterDirFlag);
		goto end;
	}
	ret = GPON_VLAN_FILTER_SUCCESS;
end:
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE){
		printk("\r\n matchVlanFilterRule-->ret=0x%02x",ret);
	}
	return ret;
}

/*******************************************************************************************
**function name
	pon_vlan_uni_filter_switch_option
**description:
	set uni vlan filter enable/disable
 **retrun :
 	0:	success
 	1:	failure
**parameter:
	pon_vlan_ioctl * data
********************************************************************************************/
int pon_vlan_uni_filter_switch_option(pon_vlan_ioctl * data,void * arg)
{
	if(data->option_flag == OPT_GET)
	{
		data->uni_filter_enable = pon_vlan_all_data.uni_filter_enable_flag;
		if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
		{
			printk("\r\ncopy to user error ====> pon vlan Switch opt");
			return -1;
		}
		return 0;
	}
	else if(data->option_flag == OPT_SET)
	{
		pon_vlan_all_data.uni_filter_enable_flag = data->uni_filter_enable;
		return 0;
	}
	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
		printk("\r\n option error ====> pon vlan uni filter Switch opt");
	return -1;
}

#endif
