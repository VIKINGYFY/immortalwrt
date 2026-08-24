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
	xpon_mapping.h

	Abstract:

	Revision History:
	Who			When			What
	--------	----------		----------------------------------------------
	Name		Date			Modification logs
	andy.Yi		2013/3/20	Create
*/
#ifndef _XPON_MAP_MODULE_INCLUDE_
#define _XPON_MAP_MODULE_INCLUDE_
#include <linux/version.h>
#include "mapping.h"
#include <lan_port/lan_port_info.h>

#define setQueueID(x,y) 				do{(x) &= (~0xf0); (x)  |= (((y)&0x07) << 4);}while(0)
#define getQueueID(x)  				(((x) & 0xf0) >> 4)

#if defined(TCSUPPORT_GPON_MAPPING)

#define OMCI_CONFIG 				0
#define LOCAL_CONFIG 				1

#define ETH_ALEN 					6

#define LEVEL_ETHER     				2
#define LEVEL_IP        				3
#define LEVEL_TRANS     				4

#define DEV_NAME_LEN                8

#define getVID(x) 					((x) & 0x0FFF)
#define getPbit(x) 					(((x) & 0xE000) >> 13)
#define setTse(x,y) 					(x = (y ? (x|QOS_TSE_MARK) : (x & ~QOS_TSE_MARK)))
#define getTse(x)  					((x & QOS_TSE_MARK) ? 1 : 0)
#define setTsID(x,y) 					do{(x) &= (~QOS_TSID_MARK); (x)  |= ((y)&QOS_TSID_MARK);}while(0)
#define getTsID(x) 					(x & QOS_TSID_MARK)


#define PONQOS

#define setNewPriority(x,y) do{(x) &= (0xff00); (x) |= ((y)&0xff);}while(0)
#define getNewPriority(x) (((x)&0xff))

#ifndef TIMER_FUN_PAAM
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0) 
#define TIMER_FUN_PAAM unsigned long
#else
#define TIMER_FUN_PAAM struct timer_list *
#endif
#endif


typedef struct {
	int DoFlag;
	int timerFlag;
	struct timer_list mappingDoneTimer;
}MappingCfgStatus_t;

enum gponmap_user_port{
	GPON_MAP_VEIP_PORT=0,
	GPON_MAP_XFI_LAN_PORT=MAX_ECNT_ETHER_PORT_NUM,
	GPON_MAP_IPHOST_VOICE_PORT=10
};

#endif
#endif

