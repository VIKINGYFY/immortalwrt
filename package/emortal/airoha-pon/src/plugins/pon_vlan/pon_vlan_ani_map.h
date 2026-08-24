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
	pon_vlan_ani_map.h

	Abstract:

	Revision History:
	Who			When			What
	--------	----------		----------------------------------------------
	Name		Date			Modification logs
	Xi.Wang		2020/1/7	Create
*/

#ifndef  __PON_VLAN_ANI_MAP_H__
#define  __PON_VLAN_ANI_MAP_H__

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include "xpon_const.h"
#include "xpon_pon_vlan_ioctl.h"


#define GPON_ANI_MAP_MSG_NONE		0
#define GPON_ANI_MAP_MSG_ERR		(1 << 0)
#define GPON_ANI_MAP_MSG_WARN		(1 << 1)
#define GPON_ANI_MAP_MSG_TRACE		(1 << 2)

#define INVALID_GEM_ID_TO_INDEX		(0xFFFF) //max gem port number is CONFIG_GPON_10G_MAX_GEMPORT = 256
#define INVALID_ANI_ID			(0xFFFF) //max ani id is GPON_MAX_ANI_INTERFACE = 256
#define INVALID_GEM_ID			(0xFFFF) //G.987.3 chapter6.4 Table6-3 0xFFFF is Idle, reserved for Idle XGEM Port-ID

#define GPON_GEM_MAX_ANI_NUM		(8)

extern __u32 gponAniMapDbgLevel;
extern int gponAniMapEnableFlag;


typedef struct {
	__u16	gemIdToIndex[GPON_10G_MAX_GEM_ID]; //gemport to map index
	struct{
		__u16	gemId; //gemport id
		__u16	ani[GPON_GEM_MAX_ANI_NUM]; //ani list
	}map[GPON_GEMPORT_MAX_NUM];
} Gpon_Ani_Map_T ;


static inline void gpon_ani_map_print_msg_head(__u32 level)
{
	if((level) & GPON_ANI_MAP_MSG_ERR)
		printk("***ERROR***");
	else if((level) & GPON_ANI_MAP_MSG_WARN)
		printk("**WARNING**");
	else if((level) & GPON_ANI_MAP_MSG_TRACE)
		printk("***TRACE***");
	return;
}

#define GPON_ANI_MAP_MSG(level, F, B...) do{ \
		if(gponAniMapDbgLevel & (level) ){ \
			gpon_ani_map_print_msg_head(level); \
			printk("[%s:%d]: "F, __FUNCTION__, __LINE__,##B) ; \
		} \
	}while(0)


#endif /* __PON_VLAN_ANI_MAP_H__ */

