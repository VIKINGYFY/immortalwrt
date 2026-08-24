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
	Reid.Ma		2013/3/22	Create
*/

#ifndef PON_MAC_FILTER_H_
#define PON_MAC_FILTER_H_
#include "xpon_mac_filter_ioctl.h"

#define PKT_FORWARD             0
#define PKT_DISCARD             1
#define PORT_INDEX_LEN          10


typedef struct pon_mac_filter_s
{
	__u8 port_index[PORT_INDEX_LEN];
	__u8 unicast_rule_counter;
	__u8 multicast_rule_counter;
	
	pon_mac_filter_rule * unicast_rule;
	pon_mac_filter_rule * multicast_rule;
}pon_mac_filter, *pon_mac_filter_p;


typedef struct pon_mac_filter_all_s
{
	__u8 enable_flag;
	__u8 onu_mode;

	__u8 lan_port_count;
	__u8 wlan_port_count;
	__u8 usb_port_count;
	__u8 total_port_count;//lan port counter

	__u16 padding;
	
	pon_mac_filter * upstream_data;
	pon_mac_filter * downstream_data;
	
}pon_mac_filter_all,*pon_mac_filter_all_p;




#endif

