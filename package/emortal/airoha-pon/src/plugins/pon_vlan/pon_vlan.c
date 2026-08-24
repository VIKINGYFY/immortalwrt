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
	pon_vlan.c

	Abstract:

	Revision History:
	Who			When			What
	--------	----------		----------------------------------------------
	Name		Date			Modification logs
	Reid.Ma		2013/2/4	Create
*/

#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/proc_fs.h>
#include <linux/if_vlan.h>
#include <linux/if_ether.h>
#include <linux/time.h>
#include <linux/version.h>
#include <linux/module.h>
#include "pon_vlan.h"
#include <linux/sort.h>
#ifdef  TCSUPPORT_PON_VLAN_FILTER
#include "pon_vlan_filter.h"
#endif
#include "pon_vlan_ani_map.h"
#include "linux/libcompileoption.h"
#include <ecnt_hook/ecnt_hook_pon_vlan.h>
#include <ecnt_hook/ecnt_hook_xpon_igmp.h>
#include <ecnt_hook/ecnt_hook_multicast_general.h>
#include <ecnt_hook/ecnt_hook_pon_mac.h>
#include <lan_port/lan_port_info.h>
#include "lan_port/ponvlan_port_info.h"
#include <macro_compatible/ecnt_macro_compatible.h>
#include <ecnt_hook/ecnt_hook_bbf247.h>

#include <ecnt_hook/ecnt_hook_xpon_mapping.h>


MODULE_DESCRIPTION("Pon_VLAN");
MODULE_LICENSE("GPL");

#define KERNEL_2_6_36 		(LINUX_VERSION_CODE > KERNEL_VERSION(2,6,31))
#define PON_VLAN_MULT_FILTER_DEFAULT  ENABLE

#define MAC_ADDR_LEN_BYTE 6  //6*8bits = 48 bits
#define VLAN_TAG_LEN_BYTE 4 

#define PON_VLAN_OFFSET_DEFAULT_GPON 1
#define PON_VLAN_OFFSET_DEFAULT_EPON 0

#define COPY_TO_USER(user,kernel,size,ret) \
                        if(0 == ret){\
                            ret = copy_to_user(user,kernel,size);\
                        	if(0 != ret){\
                        		printk("func:%s line:%d copy_to_user failed\n",__func__,__LINE__);\
                        	}\
                        }\
                                        
#define COPY_FROM_USER(kernel,user,size,ret) \
                        ret = copy_from_user(kernel,user,size);\
                        if(0 != ret){\
                            printk("func:%s line:%d copy_from_user failed\n",__func__,__LINE__);\
                            break;\
                        }\

extern struct sk_buff *__pon_vlan_put_tag(struct sk_buff *skb, u16 tpid,unsigned short vlan_tci);
extern int pon_check_pack(__u16 type);

extern int (*pon_vlan_is_ds_1_to_N_hook)(struct sk_buff **pskb, int *count);
extern int (*pon_vlan_ds_1_to_N_handler_hook)(struct sk_buff **pskb, int index);
extern int (*pon_insert_tag_hook)(struct sk_buff **pskb);
extern int (*pon_vlan_get_mode_hook)(void);
extern int (*pon_store_tag_hook)(struct sk_buff *skb, struct net_device *orig_dev);
extern int (*pon_check_vlan_hook)(struct net_device *dev, struct sk_buff *skb);
extern int (*pon_check_tpid_hook)(__u16 * buf);
extern int (*pon_check_user_group_hook)(struct sk_buff *skb);
extern int (*pon_PCP_decode_hook)(struct sk_buff **pskb);
#if defined(TCSUPPORT_RA_HWNAT) && defined(TCSUPPORT_RA_HWNAT_ENHANCE_HOOK)
extern int (*ra_sw_nat_hook_drop_packet) (struct sk_buff * skb);
extern int (*ra_sw_nat_hook_clean_table) (void);
#endif
 
extern int (*ra_sw_nat_hook_tls_vtag_handle_hook)(struct sk_buff** pskb);
extern int (*ra_sw_nat_hook_xfer) (struct sk_buff *skb, const struct sk_buff *prev_p);
extern int (*hwnat_clean_entry_by_dst_mac_hook)(unsigned char*mac);
extern int (*xpon_hgu_down_multicast_vlan_tci_hook)(struct sk_buff* skb);


extern int macMT7530GetPortBrgInd(u8 port, u8 *Ind);
extern int macMT7530SetPortBrgInd(u8 port, u8 Ind);
extern unsigned int macMT7530LanPortMap2Switch(unsigned int portId);

#define isdigit(x)	((x)>='0'&&(x)<='9')

pon_vlan_all pon_vlan_all_data;
pon_vlan_trace_drop trace_pkt_info;

static int DBG_Level = 0;
/* Hybrid Mode */
#define HYBRID_LAN_COUNT_MAX       4
#define HYBRID_ACTION_CHANGE_MODE  1
#define HYBRID_ACTION_CHANGE_PORT  2
#define VLAN_LAN_PORT_ID_BASE      11


int hy_enable = 0;
int hy_lan_count = 0;
char hy_br_wan[PON_VLAN_ITF_NAME_SIZE] = {0};
char hy_lan[HYBRID_LAN_COUNT_MAX][PON_VLAN_ITF_NAME_SIZE] = {{0}};
unsigned int hy_port_mask = 0x0f;
int bbf247_ignore_ds_pbit = 0;           // , if 247 is loaded,match ds pbit

extern int (*pon_hybrid_sfu_lan_check_hook)(struct sk_buff **pskb);
extern int (*pon_hybrid_sfu_wan_check_hook)(struct sk_buff **pskb);

int pon_check_hybrid_sfu_lan(struct sk_buff **pskb);
int pon_check_hybrid_sfu_fwd_lan(struct sk_buff **pskb);
int pon_vlan_hybrid_mode_ioctl(int cmd,pon_vlan_ioctl *data, void* arg);
int get_stream_direction(__u32 pon_vlan_flag);

unsigned int        pon_vlan_dev_offset = DEV_OFFSET;
unsigned int    pon_vlan_wlan_dev_offset = WLAN_DEV_OFFSET;
unsigned int pon_vlan_wlan_ac_dev_offset = WLAN_AC_DEV_OFFSET;
unsigned int     pon_vlan_usb_dev_offset = USB_DEV_OFFSET;

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,35)
long ponvlan_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
#else
int ponvlan_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg);
#endif

int ponvlan_open(struct inode *inode, struct file *filp);
int pon_check_untag(struct sk_buff *skb);


static inline int atoi(char *s)
{
	int i = 0;

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


static inline pon_vlan * get_pon_vlan_by_port(int port)
{
	int i = 0;
	pon_vlan * tmp;
	
	for(i = 0; i < pon_vlan_all_data.total_port_count; i++)
	{
		tmp = &(pon_vlan_all_data.pon_vlan_data[i]);
		PONVLAN_PRINT(PONVLAN_MSG_TRACE,"frame port is %d,vlan data port is %d ====>get pon vlan by port",port,tmp->port_index);
		if(tmp->port_index == port)
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"find port %d ====>get pon vlan by port",port);
			return tmp;
		}
	}
	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"can't find port ====>get pon vlan by port");
	return NULL;
}


static void pon_vlan_set_downstream_mode_mask(pon_vlan *data) {
	if (NULL == data) {
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Pon_vlan_set_downstream_mode_mask data is NULL\n");
		return;
	}

	switch(data->down_stream_mode)
	{
		case TRANSPARENT_MODE:
			data->downstream_mode_mask = DOWNSTREAM_MODE_MATCH_ALL;
			data->downstream_unmatch_oper = DOWNSTREAM_MODE_UNMATCH_FORWARD;
			break;
		case INVERSE_MODE:
			data->downstream_mode_mask = DOWNSTREAM_MODE_MATCH_ALL;
			//data->downstream_unmatch_oper = DOWNSTREAM_MODE_UNMATCH_FORWARD;
			data->downstream_unmatch_oper = DOWNSTREAM_MODE_UNMATCH_DISCARD;
			break;
		case DOWNSTEAM_MODE_2:
			data->downstream_mode_mask = DOWNSTREAM_MODE_VID | DOWNSTREAM_MODE_PRI;
			data->downstream_unmatch_oper = DOWNSTREAM_MODE_UNMATCH_FORWARD;
			break;
		case DOWNSTEAM_MODE_3:
			data->downstream_mode_mask = DOWNSTREAM_MODE_VID;
			data->downstream_unmatch_oper = DOWNSTREAM_MODE_UNMATCH_FORWARD;
			break;
		case DOWNSTEAM_MODE_4:
			data->downstream_mode_mask = DOWNSTREAM_MODE_PRI;
			data->downstream_unmatch_oper = DOWNSTREAM_MODE_UNMATCH_FORWARD;
			break;
		case DOWNSTEAM_MODE_5:
			data->downstream_mode_mask =  DOWNSTREAM_MODE_VID | DOWNSTREAM_MODE_PRI;
			data->downstream_unmatch_oper = DOWNSTREAM_MODE_UNMATCH_DISCARD;
			break;
		case DOWNSTEAM_MODE_6:
			data->downstream_mode_mask = DOWNSTREAM_MODE_VID;
			data->downstream_unmatch_oper = DOWNSTREAM_MODE_UNMATCH_DISCARD;
			break;
		case DOWNSTEAM_MODE_7:
			data->downstream_mode_mask = DOWNSTREAM_MODE_PRI;
			data->downstream_unmatch_oper = DOWNSTREAM_MODE_UNMATCH_DISCARD;
			break;
		case DOWNSTEAM_MODE_8:
			data->downstream_mode_mask = DOWNSTREAM_MODE_MATCH_ALL;
			data->downstream_unmatch_oper = DOWNSTREAM_MODE_UNMATCH_DISCARD;
			break;
		default:
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"Pon_vlan_set_downstream_mode_mask down_stream_mode error\n");
			break;
	}
	
}


int is_vlan(__u16 * buf)
{
	int i = 0;
	
	if(*buf == htons(ETH_P_8021Q) || *buf == htons(ETH_P_QinQ_88a8) || *buf == htons(ETH_P_QinQ_9100))
		return 1;

	for(i = 0; i < pon_vlan_all_data.tpid_counter; i++)
		{
			if(*buf == pon_vlan_all_data.pon_vlan_type[i].type)
				return 1;
		}
	return 0;
}

static inline int get_type_index(__u16 * buf)
{
	int i = 0;
	
	if(*buf == htons(ETH_P_8021Q))
		return 32;

	for(i = 0; i < pon_vlan_all_data.tpid_counter; i++)
		{
			if(*buf == pon_vlan_all_data.pon_vlan_type[i].type)
				return i;
		}
	return -1;
}


static inline int add_tpid(__u16 type)
{
	if(pon_vlan_all_data.tpid_counter >= pon_vlan_all_data.total_special_tpid)
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"too many special tpid,can't add any more====>Add tpid");
		return -1;
	}
	
	pon_vlan_all_data.pon_vlan_type[pon_vlan_all_data.tpid_counter].type = type;
	dev_add_pack(&pon_vlan_all_data.pon_vlan_type[pon_vlan_all_data.tpid_counter]);
	pon_vlan_all_data.tpid_counter++;
	return 0;
}

static inline int remove_tpid(__u16 type)
{
	__u16 tmp = type;
	int index = 0;
	
	index = get_type_index(&tmp);
	if(index == -1)
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"TPID not found, ====> pon vlan remove tpid");
		return -1;
	}
	
	dev_remove_pack(&pon_vlan_all_data.pon_vlan_type[index]);
	pon_vlan_all_data.tpid_counter--;

	//just move the last element to the position which just remove.
	//if we remove the last element,do nothing here.
	if(index != pon_vlan_all_data.tpid_counter)
	{
		dev_remove_pack(&pon_vlan_all_data.pon_vlan_type[pon_vlan_all_data.tpid_counter]);
		pon_vlan_all_data.pon_vlan_type[index].type = pon_vlan_all_data.pon_vlan_type[pon_vlan_all_data.tpid_counter].type;
		pon_vlan_all_data.pon_vlan_type[pon_vlan_all_data.tpid_counter].type = 0;
		dev_add_pack(&pon_vlan_all_data.pon_vlan_type[index]);
	}
	return 0;
}



/*
	add a tag or modify tci for igmp frames when send to wan.
*/
static int handle_igmp_tag(struct sk_buff ** pskb,int mode, __u16 tci)
{
	struct sk_buff *skb = NULL;
	struct net_device *out_dev = NULL;
	__u16 newTCI = tci;

	if(pskb == NULL)
	{
		printk("\r\npskb is NULL pointer,return -1 ====> Handle IGMP Tag");
		return -1;
	}
	if(*pskb == NULL)
	{
		printk("\r\n*pskb is NULL pointer,return -1 ====> Handle IGMP Tag");
		return -1;
	}

	skb = *pskb;
	out_dev = skb->dev;
	
	if(!((skb->pon_vlan_flag & PON_PKT_FROM_IGMP) && strcmp(out_dev->name,"pon") == 0))	
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"vlan flag error or out_dev isn't nasx return -1====>Handle IGMP Tag");
		return -1;
	}

	switch(mode)
	{
		case IGMP_MODE_PASS_THROUGH://do nothing
			PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"handle igmp packet with mode 0:transparent====>Handle IGMP Tag");
			return 0;

		case IGMP_MODE_ADD_TAG:
			PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"handle igmp packet with mode 1:add tag====>Handle IGMP Tag");
			goto add_tag;

		case IGMP_MODE_MODIFY_TCI:
		case IGMP_MODE_MODIFY_VID:
			PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"handle igmp packet with mode 2&3:modify tci or vid====>Handle IGMP Tag");
			if(skb->pon_tag_num == 0)
				goto add_tag;
			else
				goto modify_TCI;
		
		default:
			PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"mode error ====> Handle IGMP Tag");
			return -1;
	}

add_tag:
	skb->pon_vlan_tpid[skb->pon_tag_num] = ETH_P_8021Q;
	skb->pon_vlan_tci[skb->pon_tag_num] = newTCI;
	skb->pon_tag_num ++;
	PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"add tag success,tci is %x====>Handle IGMP Tag",skb->pon_vlan_tci[skb->pon_tag_num]);
	return 0;

modify_TCI:
	if(mode == IGMP_MODE_MODIFY_TCI)
		skb->pon_vlan_tci[skb->pon_tag_num - 1] = newTCI;
	else		// mode == IGMP_MODE_MODIFY_VID
		skb->pon_vlan_tci[skb->pon_tag_num - 1] = (skb->pon_vlan_tci[skb->pon_tag_num - 1] & 0xF000) | (newTCI & 0x0FFF);

	PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"modify tci success,tci is %x====>Handle IGMP Tag",skb->pon_vlan_tci[skb->pon_tag_num - 1]);
	return 0;
	
}

/************************************************/
/* 	mark junwei.ren:	20180418
	ether name :         11 ~ 14
	wlan name :    	21 ~ 28
	wlan AC name :     31 ~ 38
	USB name :	   	41
*/
/************************************************/
static int skb_mark_convert_port_num(struct sk_buff *skb)
{
	int mark = 0;
	int num  = -1;
	int skb_mark = 0;
	int skb_mark2 = 0;	
	skb_mark = skb->mark;
	skb_mark2 = skb->mark2;

	mark = GET_LAN_ITF_MARK(skb_mark);
	if( (pon_vlan_wlan_dev_offset <= mark) \
		&& ((pon_vlan_wlan_dev_offset + MAX_ECNT_WALN_PORT_NUM - 1)  >= mark) )
	{
		num = (PONVLAN_PORT_OFFSET_WLAN + mark - MAX_ECNT_ETHER_PORT_NUM);
	}
	else if( (pon_vlan_wlan_ac_dev_offset <= mark) \
		&& ((pon_vlan_wlan_ac_dev_offset + MAX_ECNT_WALNAC_PORT_NUM - 1)  >= mark) )
	{
		num = (PONVLAN_PORT_OFFSET_WLANAC + mark - MAX_ECNT_ETHER_PORT_NUM - MAX_ECNT_WALN_PORT_NUM);
	}
	else if(pon_vlan_usb_dev_offset == mark)
	{
		num = (PONVLAN_PORT_OFFSET_USB + mark - MAX_ECNT_ETHER_PORT_NUM - MAX_ECNT_WALN_PORT_NUM - MAX_ECNT_WALNAC_PORT_NUM);
	}
	else
	{

		num = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
		if(num > 0)
		{
			num += PONVLAN_PORT_OFFSET_ETH;
		}else{
			num = -1;
		}
	}
	
	return num;
}

static int devname_convert_port_num(char *dev_name)
{
	int num  = -1;
		
	if((NULL == dev_name) || (0 >= strlen(dev_name)))
	{
		return -1;
	}
	
	switch(dev_name[0])
	{
		case 'e':
		{
			if(pon_vlan_all_data.lan_port_count == 1)
			{
				num = PONVLAN_PORT_OFFSET_ETH + 1;
			}
			else
			{
				num = ENCT_HOOK_XPON_ETH_MAP_DEV_NAME_TO_PORT(dev_name);
				if(num > 0){
					num += PONVLAN_PORT_OFFSET_ETH;
				}else{
					num = -1;
				}
			}
			break;
		}
		case 'r':
		{
			/*wlan AC itf name "rai0"*/
			if(NULL != strstr(dev_name, WLAN_AC_ITF_NAME_FORMAT))
			{
				num = 0;
				sscanf(dev_name, WLAN_AC_ITF_NAME_FORMAT"%d", &num);
				if(num > 0 && num <= MAX_ECNT_WALNAC_PORT_NUM)
				{
					num = num + PONVLAN_PORT_START_WLANAC;
				}
				else{
					num = -1;
				}
			}
			/*wlan itf name "ra0"*/
			else if(NULL != strstr(dev_name, WLAN_ITF_NAME_FORMAT))
			{
				num = 0;
				sscanf(dev_name, WLAN_ITF_NAME_FORMAT"%d", &num);
				if(num > 0 && num <= MAX_ECNT_WALN_PORT_NUM)
				{
					num = num + PONVLAN_PORT_START_WLAN;
				}
				else{
					num = -1;
				}
			}
			break;
		}
		case 'u':
		{
			/*usb itf name: usb0*/
			if(NULL != strstr(dev_name, USB_ITF_NAME_FORMAT))
			{
				num = 0;
				sscanf(dev_name, USB_ITF_NAME_FORMAT"%d", &num);
				if(num > 0 && num <= MAX_ECNT_USB_PORT_NUM)
				{
					num = num + PONVLAN_PORT_START_USB;
				}
				else{
					num = -1;
				}
			}
			break;
		}
		default:
		{
			break;
		}
	}

	return num;
}
//PortNumDefine
//00:CPE	01~09 reserved
//11~14 lan port 1~4.	10,15~19 reserved
//21~24 wlan port 1~4. 20,25~29 reserved
//30 usb?not uesd
//-1 error

//Modified by Fred, 20170523
//Assign Port 25-28 with 5g interface(11AC)

static inline int get_port_num(struct sk_buff *skb)
{
	int num = -1;
	int i = 0;
	
	struct net_device *out_dev = NULL;

	if(skb == NULL)
	{
		printk("\r\nskb is NULL ====> get port num");
		return -1;
	}

	out_dev = skb->dev;
	
	if(skb->pon_vlan_flag & PON_PKT_FROM_CPE)
	{
		/* only support one ip host now */
		if(TCSUPPORT_PON_IP_HOST_VAL && (skb->pon_vlan_flag & PON_PKT_VOIP_TX)){
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"voip packet return 60====>get port num");
			return PONVLAN_PORT_OFFSET_IPHOST;
		}
		else
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"packet from cpe return 0====>get port num");
			return PONVLAN_PORT_OFFSET_VP;
		}
	}
	else if(skb->pon_vlan_flag & PON_PKT_FROM_LAN)
	{
		PONVLAN_PRINT(PONVLAN_MSG_TRACE,"packet from lan and mark is %x====>get port num",skb->mark);
		num = skb_mark_convert_port_num(skb);
		if(num > 0)
		{
			PONVLAN_PRINT(PONVLAN_MSG_TRACE|skb->pon_vlan_flag,"packet from lan num=%d ====>get port num",num);
			return num;
		}

		PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"packet from lan and can't find port====>get port num");
		return -1;
	}
	else if (skb->pon_vlan_flag & PON_PKT_FROM_HYBRID_PPTP) //hybrid PPTP
	{
		num = skb_mark_convert_port_num(skb);
		if(num > 0)
		{
			PONVLAN_PRINT(PONVLAN_MSG_TRACE|skb->pon_vlan_flag,"packet from PPTP num=%d base on skb->mark",num);
			return num;
		}
		else{
			PONVLAN_PRINT(PONVLAN_MSG_TRACE|skb->pon_vlan_flag,"not find port! return -1");
			return -1;
		}
	}
	if(hy_enable == 1 && (skb->pon_vlan_flag & PON_VLAN_RX_CALL_HOOK) && !memcmp(out_dev->name, "eth0.",5)) //hybrid downstream eth
	{
		for(i = 0; i < hy_lan_count; i++)
		{
			if(strcmp(out_dev->name, hy_lan[i]) == 0)
			{
				num = devname_convert_port_num(out_dev->name);
				if(num > 0)
				{
					return num;
				}
				else
				{
					PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"packet from wan and can't find port====>get port num");
					return -1;
				}
			}
		}
		PONVLAN_PRINT(PONVLAN_MSG_TRACE,"hybrid downstream hgu eth, out_dev name is %s return veip",out_dev->name);
		return PONVLAN_PORT_OFFSET_VP;
	}
	else if(skb->pon_vlan_flag & PON_PKT_FROM_WAN || skb->pon_vlan_flag & PON_PKT_FROM_HYBRID_SFU_WAN)
	{
		PONVLAN_PRINT(PONVLAN_MSG_TRACE,"packet from wan and out_dev name is %s====>get port num",out_dev->name);

		if(TCSUPPORT_PON_IP_HOST_VAL){
			if(skb->pon_vlan_flag & PON_PKT_VOIP_RX)
				return PONVLAN_PORT_OFFSET_IPHOST;
		}
		num = devname_convert_port_num(out_dev->name);
		if(num > 0)
		{
			return num;
		}
		else
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"packet from wan and can't find port====>get port num");
			return -1;
		}
	}
	else if( (skb->pon_vlan_flag & PON_VLAN_RX_CALL_HOOK) && (pon_vlan_all_data.onu_mode == MODE_HGU))
	{
		PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"packet from wan(hgu RX)\n");
		return PONVLAN_PORT_OFFSET_VP;
	}
	else if( (skb->pon_vlan_flag & PON_VLAN_TX_CALL_HOOK) && (pon_vlan_all_data.onu_mode == MODE_HGU))
	{
		PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"packet from wan(hgu TX)\n");
		return PONVLAN_PORT_OFFSET_VP;
	}

	return -1;
}

int checkBroadcastPkt(unchar *addr)
{
	unchar i = 0;
	if(addr == NULL)
		return 0;

	for(i = 0;i < 6; i++)
    {
    	if(addr[i] != 0xff)
            return 0;
    }
	return 1;
}


static inline pon_vlan * get_vlan_opt_data(struct sk_buff *skb)
{
	int i = 0,dir = 0,port = 0,flag = 0;
	struct net_device *out_dev = NULL;
	int offset=0;
	unsigned char linkMode=0;
	
	if(skb == NULL)
	{
		printk("\r\nskb is NULL NULL ====>get vlan opt data");
		return NULL;
	}
	
	out_dev = skb->dev;

#if 0 //def TCSUPPORT_PON_IP_HOST
	// HGU Voip
	if(pon_vlan_all_data.onu_mode == MODE_HGU){
		if((skb->pon_vlan_flag & PON_PKT_VOIP_TX)
			|| (skb->pon_vlan_flag & PON_PKT_VOIP_RX))
			port = IP_HOST_PORT_ID;
		else
			port = 11;
	}
#else
	if(pon_vlan_all_data.onu_mode == MODE_HGU && 
		skb->pon_vlan_flag & PON_PKT_FROM_HYBRID_VEIP)
		port = PONVLAN_PORT_OFFSET_VP;
#endif
	else
		port = get_port_num(skb);
	
	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"Port Num is %d ====>get vlan opt data",port);
	if(port == -1)
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Get port error ====>get vlan opt data");
		return NULL;
	}

	dir = get_stream_direction(skb->pon_vlan_flag);
	for(i = 1; i < pon_vlan_all_data.total_port_count; i++)
	{
		PONVLAN_PRINT(PONVLAN_MSG_TRACE,"frame port is %d,vlan data port is %d  ====>get vlan opt data",port,pon_vlan_all_data.pon_vlan_data[i].port_index);
		if(pon_vlan_all_data.pon_vlan_data[i].port_index == port)
		{
			if(dir == UPSTREAM)
			{
				if(pon_vlan_all_data.pon_vlan_data[i].up_rule_count != 0)
				{
					PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"we have rule on port %d in upstream,====>get vlan opt data",port);
					return &pon_vlan_all_data.pon_vlan_data[i];
				}
				else
				{
					PONVLAN_PRINT(PONVLAN_MSG_TRACE,"we will check default rule on port %d in upstream,====>get vlan opt data",port);
					flag = 1;
					break;
				}
			}
			else if(dir == DOWNSTREAM)
			{
				if(pon_vlan_all_data.pon_vlan_data[i].down_rule_count != 0)
				{
					PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"we have rule on port %d in downstream,====>get vlan opt data",port);
					return &pon_vlan_all_data.pon_vlan_data[i];
				}
				else
				{
					if(pon_vlan_all_data.pon_vlan_data[i].down_stream_mode != INVERSE_MODE)
					{
						PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"wmode is transparent or stripped on port %d in downstream,====>get vlan opt data",port);
						return &pon_vlan_all_data.pon_vlan_data[i];
					}
					PONVLAN_PRINT(PONVLAN_MSG_TRACE,"we will check default rule on port %d in downstream,====>get vlan opt data",port);
					flag = 1;
					break;
				}
			}
			else
			{
				PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"lan 2 lan or other case,value is %d.return NULL ====>get vlan opt data",port);
				return NULL;
			}
		}
	}

	if(flag != 1)
	{
		PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"unknow packet's port,value is %d.return NULL ====>get vlan opt data",port);
		return NULL;
	}

	if(TCSUPPORT_BBF_247_VAL)
    {/* 247²âÊÔÓÃÀý6.4 */    
		__u8 *mac = NULL;
        mac = skb->data;
        PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"TCSUPPORT_BBF_247_VAL, data in skb is %x %x %x %x %x %x \n",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        if(checkBroadcastPkt(mac) == 1)
            return NULL;
	}

	/*get default pon vlan*/
    ECNT_API_XPON_MODE_GET(&linkMode);
	if(linkMode == 2) //2=PON_LINK_STATUS_EPON
		offset = PON_VLAN_OFFSET_DEFAULT_EPON; 
	else if(linkMode == 1) //1=PON_LINK_STATUS_GPON
		offset = PON_VLAN_OFFSET_DEFAULT_GPON;
	else
		return NULL;		 

	//Check Default Rule
	if(pon_vlan_all_data.pon_vlan_data[i].enable_default_rule == ENABLE)
	{
		if(dir == UPSTREAM)
		{
			if(pon_vlan_all_data.pon_vlan_data[offset].up_rule_count != 0)
			{
				PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"we will use default rule on port %d in upstream,====>get vlan opt data",
					pon_vlan_all_data.pon_vlan_data[offset].port_index);
				return &pon_vlan_all_data.pon_vlan_data[offset];
			}
			else
			{
				PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"No rule for port %d in upstream.return NULL ====>get vlan opt data",port);
				return NULL;
			}
		}
		else if(dir == DOWNSTREAM)
		{
			if(pon_vlan_all_data.pon_vlan_data[offset].down_rule_count != 0)
			{
				PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"we will use default rule on port %d in downstream,====>get vlan opt data",
					pon_vlan_all_data.pon_vlan_data[offset].port_index);
				return &pon_vlan_all_data.pon_vlan_data[offset];
			}
			else
			{
				PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"No rule for port %d downstream.return NULL ====>get vlan opt data",port);
				return NULL;
			}
		}
		else
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"Unknown error for port %d downstream.return NULL ====>get vlan opt data",port);
			return NULL;
		}
	}
	else
	{
		PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"No rule on port %d and don't use default rule,return NULL ====>get vlan opt data",port);
		return NULL;
	}
	return NULL;
}

static inline int store_tag_info(struct sk_buff *skb)
{
	__u16 * tmp = NULL;
	int i = 0,j = 0;
	int tag_limit = 0;

	if(skb == NULL)
	{
		printk("\r\nskb is NULL ====>store tag info");
		return -1;
	}

	/* for epon only resolve the outer tag, and treat the tag with each mode action. */
	if(pon_vlan_all_data.xpon_mode == MODE_EPON)
	{
		tag_limit = 1;
	}
	else
	{
		tag_limit = HANDLE_TAG_LIMIT;
	}
	skb->pon_tag_num = 0;
	tmp = (__u16 *)skb->data;
	tmp += MAC_ADDR_LEN_BYTE;	//2*6=12bytes, the size of d-mac and s-mac	
	for(i = 0; i < tag_limit; i++)
	{
		PONVLAN_PRINT(PONVLAN_MSG_TRACE,"TPID is %x,tci is %x",*tmp,*(tmp + 1));
		if(!is_vlan(tmp))
		{
			break;
		}
		if(skb->pon_tag_num > 0)
		{
			for(j = skb->pon_tag_num; j > 0; j--)
			{
				skb->pon_vlan_tpid[j] = skb->pon_vlan_tpid[j - 1];
				skb->pon_vlan_tci[j] = skb->pon_vlan_tci[j - 1];
			}
		}
		skb->pon_vlan_tpid[0] = htons(*tmp);
		skb->pon_vlan_tci[0] = htons(*(tmp+1));
		skb->pon_tag_num++;
		tmp += VLAN_TAG_LEN_BYTE/sizeof(__u16); //2*2 =4byte, the size of vlan tag

		//skb_pull_rcsum(skb, VLAN_HLEN);
		memmove(skb->data + VLAN_HLEN, skb->data, 12);
		skb_pull(skb, VLAN_HLEN);
		skb->mac_header += VLAN_HLEN;
		PONVLAN_PRINT(PONVLAN_MSG_TRACE,"Tag Conut is %d",skb->pon_tag_num);
	}
	
	skb->protocol = *(__u16 *)(skb->data + 12);	
	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"protocol is  %x",skb->protocol);
	
	//more than 4 tags,it will cause error.
	/*if(is_vlan(tmp))
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"frame has 5 or more tags,return -1 ====>store tag info");
		return -1;
	}*/
	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"handle tag success ====>store tag info");

	return 0;
}

static inline int ds_store_tag_info(struct sk_buff *skb,ds_vlan_info * ds_vlan_info)
{
	__u16 * tmp = NULL;
	int i = 0,j = 0;
	int tag_limit = 2;

	if(skb == NULL)
	{
		printk("\r\n****skb is NULL ====>ds store tag info");
		return -1;
	}

	if(ds_vlan_info == NULL)
	{
		printk("\r\n****ds_vlan_info is NULL ====>ds store tag info");
		return -1;
	}

	ds_vlan_info->pon_tag_num = 0;
	tmp = (__u16 *)skb->data;
	tmp += MAC_ADDR_LEN_BYTE;	//2*6=12bytes, the size of d-mac and s-mac	
	for(i = 0; i < tag_limit; i++)
	{
		PONVLAN_PRINT(PONVLAN_MSG_TRACE,"TPID is %x,tci is %x",*tmp,*(tmp + 1));
		if(!is_vlan(tmp))
		{
			break;
		}
		if(ds_vlan_info->pon_tag_num > 0)
		{
			for(j = ds_vlan_info->pon_tag_num; j > 0; j--)
			{
				ds_vlan_info->pon_vlan_tpid[j] = ds_vlan_info->pon_vlan_tpid[j - 1];
				ds_vlan_info->pon_vlan_tci[j] = ds_vlan_info->pon_vlan_tci[j - 1];
			}
		}
		ds_vlan_info->pon_vlan_tpid[0] = htons(*tmp);
		ds_vlan_info->pon_vlan_tci[0] = htons(*(tmp+1));
		ds_vlan_info->pon_tag_num++;
		tmp += VLAN_TAG_LEN_BYTE/sizeof(__u16); //2*2 =4byte, the size of vlan tag

		PONVLAN_PRINT(PONVLAN_MSG_TRACE,"Tag Conut is %d",ds_vlan_info->pon_tag_num);
	}

	ds_vlan_info->ethertype = htons(*tmp);
	
	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****handle tag success ====>ds_store tag info");

	return 0;
}


static inline int ds_match_tag_ext(ds_vlan_info * vlan_info, pon_vlan_rule * rule, int tci_index, pon_vlan * data, int strict_flag)
{
	int tpid = 0,pbit = 0,dei = 0,vid = 0;
    int pbitMatchFlag = 1;
	int tag_index = tci_index + 1;
	__u16 tag_tpid = vlan_info->pon_vlan_tpid[tci_index];
	__u16 tci = vlan_info->pon_vlan_tci[tci_index];
	__u16 in_tpid = data->input_tpid;
	__u16 out_tpid = data->output_tpid;
	int vidMatchFlag = 1;
    unsigned char linkMode =0;

	if(rule == NULL)
	{
		printk("\r\n****rule is NULL ====>pon Match Tag");
		return -1;
	}

	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****match tag index is %d ====>pon Match Tag",tag_index);

	switch(tag_index)
	{
		case 1:
			tpid = rule->filter_inner_tpid;
			pbit = rule->filter_inner_pri;
			dei = rule->filter_inner_dei;
			vid = rule->filter_inner_vid;
			break;

		case 2:
			tpid = rule->filter_outer_tpid;
			pbit = rule->filter_outer_pri;
			dei = rule->filter_outer_dei;
			vid = rule->filter_outer_vid;
			break;

		case 3:
			tpid = rule->down_filter_inner_tpid;
			pbit = rule->down_filter_inner_pri;
			dei = rule->down_filter_inner_dei;
			vid = rule->down_filter_inner_vid;
			break;

		case 4:
			tpid = rule->down_filter_outer_tpid;
			pbit = rule->down_filter_outer_pri;
			dei = rule->down_filter_outer_dei;
			vid = rule->down_filter_outer_vid;
			break;

		default:
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****paramater error ====>pon Match Tag");
			return 0;
	}
	/*if EPON mode(2); only check vid*/
    ECNT_API_XPON_MODE_GET(&linkMode);
    if(2 != linkMode){
		switch(tpid)
		{
			case FILTER_TPID_DO_NOT_CARE:
				break;

			case FILTER_TPID_8100:
				if(tag_tpid != ETH_P_8021Q)
					return 0;
				break;

			case FILTER_TPID_EQUAL_TO_INPUT_TPID:
				if(tag_tpid != in_tpid)
					return 0;
				break;

			case FILTER_TPID_EQUAL_TO_OUTPUT_TPID:
				if(tag_tpid != out_tpid)
					return 0;
				break;

			default:
				return 0;
		}
		
		PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****TPID Match Success ====>pon Match Tag");
		
		if (TCSUPPORT_CY_PON_VAL == 0) {
			if(pbit >= 0 && pbit < 8)
			{
				if(getPbit(tci) != pbit)
				{	
					PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****match tag pbit not match, pbit = %d, skb get pbit = %d \n", pbit, getPbit(tci));
					pbitMatchFlag = 0;
				}
				else
				{
					pbitMatchFlag = 1;
				}
			}
			else if(pbit == FILTER_PRI_DO_NOT_CARE){
			}
			else
				return 0;
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****Pbit Match Success ====>pon Match Tag");
		}
		
		if(dei == 0 || dei == 1)
		{
			if(getDEI(tci) != dei)
			{
				PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****match tag dei not match, dei = %d, skb get dei = %d \n", dei, getDEI(tci));
				return 0;
			}
		}
		else if(dei == FILTER_DEI_DO_NOT_CARE){
		}
		else
			return 0;
		PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****DEI Match Success ====>pon Match Tag");
	}

	if(vid >= 0 && vid <= 4095)
	{
		if(getVID(tci) != vid)
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****match tag vlan not match, vlan = %d, skb get vlan = %d \n", vid, getVID(tci));
			vidMatchFlag = 0;
		} 
		else 
		{ 
			vidMatchFlag = 1;
		}
	}
	else if(vid == FILTER_VID_DO_NOT_CARE){
		if(strict_flag){
			vidMatchFlag = 0;
		}
	}
	else
		return 0;

	if (!pbitMatchFlag || !vidMatchFlag) 
	{		
		PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****match tag pbit or vid fail\n");
		return 0;		
	} 
		
	return 1;
}

static inline int match_tag(struct sk_buff * skb, pon_vlan_rule * rule, int tci_index, pon_vlan * data, int direction)
{
	int tpid = 0,pbit = 0,dei = 0,vid = 0;
    int pbitMatchFlag = 1;
	int tag_index = tci_index + 1;
	__u16 tag_tpid = skb->pon_vlan_tpid[tci_index];
	__u16 tci = skb->pon_vlan_tci[tci_index];
	__u16 in_tpid = data->input_tpid;
	__u16 out_tpid = data->output_tpid;
	int vidMatchFlag = 1;
	int ret = RULE_MATCH_TOTAL;
    unsigned char linkMode =0;

	if(rule == NULL)
	{
		printk("\r\nrule is NULL ====>pon Match Tag");
		return -1;
	}

	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"match tag index is %d ====>pon Match Tag",tag_index);

	switch(tag_index)
	{
		case 1:
			tpid = rule->filter_inner_tpid;
			pbit = rule->filter_inner_pri;
			dei = rule->filter_inner_dei;
			vid = rule->filter_inner_vid;
			break;

		case 2:
			tpid = rule->filter_outer_tpid;
			pbit = rule->filter_outer_pri;
			dei = rule->filter_outer_dei;
			vid = rule->filter_outer_vid;
			break;

		case 3:
			tpid = rule->down_filter_inner_tpid;
			pbit = rule->down_filter_inner_pri;
			dei = rule->down_filter_inner_dei;
			vid = rule->down_filter_inner_vid;
			break;

		case 4:
			tpid = rule->down_filter_outer_tpid;
			pbit = rule->down_filter_outer_pri;
			dei = rule->down_filter_outer_dei;
			vid = rule->down_filter_outer_vid;
			break;

		default:
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"paramater error ====>pon Match Tag");
			return 0;
	}
	/*if EPON mode(2); only check vid*/
    ECNT_API_XPON_MODE_GET(&linkMode);
    if(2 != linkMode){
	switch(tpid)
	{
		case FILTER_TPID_DO_NOT_CARE:
			break;

		case FILTER_TPID_8100:
			if(tag_tpid != ETH_P_8021Q)
				return 0;
			break;

		case FILTER_TPID_EQUAL_TO_INPUT_TPID:
			if(tag_tpid != in_tpid)
				return 0;
			break;

		case FILTER_TPID_EQUAL_TO_OUTPUT_TPID:
			if(tag_tpid != out_tpid)
				return 0;
			break;

		default:
			return 0;
	}
	
	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"TPID Match Success ====>pon Match Tag");
	
	if (TCSUPPORT_CY_PON_VAL == 0) {
	if(pbit >= 0 && pbit < 8)
	{
		if(getPbit(tci) != pbit)
		{	
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"match tag pbit not match, pbit = %d, skb get pbit = %d \n", pbit, getPbit(tci));
			pbitMatchFlag = 0;
		}
		else
		{
			pbitMatchFlag = 1;
		}
	}
	else if(pbit == FILTER_PRI_DO_NOT_CARE){}
	else
		return 0;
	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"Pbit Match Success ====>pon Match Tag");
	}
	
	if(dei == 0 || dei == 1)
	{
		if(getDEI(tci) != dei)
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"match tag dei not match, dei = %d, skb get dei = %d \n", dei, getDEI(tci));
			return 0;
		}
	}
	else if(dei == FILTER_DEI_DO_NOT_CARE){}
	else
		return 0;
	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"DEI Match Success ====>pon Match Tag");
	}

	if(vid >= 0 && vid <= 4095)
	{
		if(getVID(tci) != vid)
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"match tag vlan not match, vlan = %d, skb get vlan = %d \n", vid, getVID(tci));
			vidMatchFlag = 0;
		} 
		else 
		{ 
			vidMatchFlag = 1;
		}
	}
	else if(vid == FILTER_VID_DO_NOT_CARE){}
	else
		return 0;

	if (!pbitMatchFlag && !vidMatchFlag) 
	{		
		PONVLAN_PRINT(PONVLAN_MSG_WARNING,"match tag pbit and vid fail\n");
		return 0;		
	} 
	else if (!pbitMatchFlag && vidMatchFlag) 
	{
		if ((direction == DOWNSTREAM) && (data->downstream_mode_mask & DOWNSTREAM_MODE_PRI) && (data->down_stream_mode != STRIPPED_MODE))
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"match tag downstream print match pbit fail\n");
				if(ECNT_CONTINUE == ECNT_API_BBF247_MATCH_DS_PBIT(bbf247_ignore_ds_pbit)){
					return 0;
				}
				else{
    				//do not do downstream pbit match

				}
		} 
		
		ret = RULE_MATCH_VID;
	} 
	else if (pbitMatchFlag && !vidMatchFlag) 
	{
		if ((direction == DOWNSTREAM) && (data->downstream_mode_mask & DOWNSTREAM_MODE_PRI) && !(data->downstream_mode_mask & DOWNSTREAM_MODE_VID))
		{
			ret = RULE_MATCH_PRI;
		} 
		else
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"match tag downstream or upstream print match vlan fail\n");
			return 0;
		}		
	}
	else if (pbitMatchFlag && vidMatchFlag) 
	{	
		ret = RULE_MATCH_TOTAL;
		
		if (direction == DOWNSTREAM) 
		{
			if ((data->downstream_mode_mask & DOWNSTREAM_MODE_PRI) && !(data->downstream_mode_mask & DOWNSTREAM_MODE_VID))
			{
				ret = RULE_MATCH_PRI;
			} 
			else if (!(data->downstream_mode_mask & DOWNSTREAM_MODE_PRI) && (data->downstream_mode_mask & DOWNSTREAM_MODE_VID))
			{ 
				ret = RULE_MATCH_VID;
			} 
			else if ((data->downstream_mode_mask & DOWNSTREAM_MODE_PRI) && (data->downstream_mode_mask & DOWNSTREAM_MODE_VID)
					&& (data->downstream_mode_mask != DOWNSTREAM_MODE_MATCH_ALL)) 
			{
				ret = RULE_MATCH_PRI_VID;
			}
		}		
	}

	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"VID Match Success ====>pon Match Tag");

	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"All Match Success ====>pon Match Tag direction = %d, ret = %d\n", direction, ret);

	return ret;
}

static inline int up_rule_tag0(struct sk_buff * skb, pon_vlan * data, int direction,int index)
{
	pon_vlan_rule * rule = NULL;
	
	if(skb == NULL)
	{
		printk("\r\nrule is NULL ====>match rule");
		return 0;
	}

	if(data == NULL)
	{
		printk("\r\ndata is NULL ====>match rule");
		return 0;
	}

	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"rule direction is %s ====>pon match rule",(direction == UPSTREAM)?"Up":"Down");
	if(direction == UPSTREAM)
	{
		rule = &(data->up_rule[index]);
	}
	else if(direction == DOWNSTREAM)
	{
		rule = &(data->down_rule[index]);
	}
	else
		return 0;

	if(rule->tag_num != 0)
	{
		PONVLAN_PRINT(PONVLAN_MSG_WARNING," skb tag num %d, rule's tag num is %d ====>pon match rule",skb->pon_tag_num,rule->tag_num);
		return 0;
	}

	return 1;
}

int up_rule_index = -1;

static inline int ds_match_rule_ext(ds_vlan_info * vlan_info, pon_vlan * data, int index, int strict_flag)
{
	pon_vlan_rule * rule = NULL;
	int matchFlag = 0;
	int i = 0;
	
	if(vlan_info == NULL)
	{
		printk("\r\n****ds_vlan_info is NULL ====>ds match rule");
		return -1;
	}

	if(data == NULL)
	{
		printk("\r\n****data is NULL ====>ds match rule");
		return -1;
	}
	
	rule = &(data->down_rule[index]);

	if(rule->tag_num >= 5)
	{
		PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****rule's tag num is %d ,it will cause error====>pon match rule",rule->tag_num);
		return 0;
	}
	
	if(vlan_info->pon_tag_num != rule->tag_num)
	{
		PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****frame has %d Tag,rule's tag num is %d ====>pon match rule",vlan_info->pon_tag_num,rule->tag_num);
		return 0;
	}

	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"****Begin Match the Rule ====>pon match rule");
	if(rule->filter_ethertype != 0)
	{
		if(vlan_info->ethertype != rule->filter_ethertype)
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****Ether Type not match,frame etherType is %x,rule's EtherType is %x ====>pon match rule",vlan_info->ethertype,rule->filter_ethertype);
			return 0;
		}
	}

	if(vlan_info->pon_tag_num == 0){ 
			matchFlag = 1;
	}
	else{
		for(i = vlan_info->pon_tag_num - 1; i >= 0; i--){
			if(i >= 2){
				return -1;
			}
			
			matchFlag = ds_match_tag_ext(vlan_info, rule, i, data,strict_flag);
			if(matchFlag <= 0)	{
				PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****match tag %d error return 0 ====>pon match rule",i+1);
				return 0;
			}

		}
	}

	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****match rule ====>pon match rule");
	return matchFlag;
}


static inline int match_rule(struct sk_buff * skb, pon_vlan * data, int direction,int index)
{
	pon_vlan_rule * rule = NULL;
	__u8 * tmp = NULL;
	__u16 ethertype = 0;
    int matchValue = 0;
	int matchFlag = 0;
	int i = 0;
	
	if(skb == NULL)
	{
		printk("\r\nrule is NULL ====>match rule");
		return -1;
	}

	if(data == NULL)
	{
		printk("\r\ndata is NULL ====>match rule");
		return -1;
	}

	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"rule direction is %s ====>pon match rule",(direction == UPSTREAM)?"Up":"Down");

	if (TCSUPPORT_CT_PON_SN_VAL) {
		if (up_rule_index != -1) {
			if (direction == DOWNSTREAM && (data->port_index == 0))
				if (index != up_rule_index)
					return 0;
		}
	}
	
	if(direction == UPSTREAM)
	{
		rule = &(data->up_rule[index]);
	}
	else if(direction == DOWNSTREAM)
	{
		rule = &(data->down_rule[index]);
	}
	else
		return 0;

	if((direction == UPSTREAM && rule->tag_num > 2) || (direction == DOWNSTREAM && rule->tag_num >= 5))
	{
		PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"rule's tag num is %d ,it will cause error====>pon match rule",rule->tag_num);
		return 0;
	}
	
	if(skb->pon_tag_num != rule->tag_num)
	{
		PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"frame has %d Tag,rule's tag num is %d ====>pon match rule",skb->pon_tag_num,rule->tag_num);
		return 0;
	}

	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"Begin Match the Rule ====>pon match rule");
	if(rule->filter_ethertype != 0)
	{
		tmp = skb->data;
		tmp += 12;
		ethertype = (*tmp << 8) + *(tmp + 1);
		if(ethertype != rule->filter_ethertype)
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"Ether Type not match,frame etherType is %x,rule's EtherType is %x ====>pon match rule",ethertype,rule->filter_ethertype);
			return 0;
		}
	}

	if(skb->pon_tag_num == 0){
			matchFlag = 1;
	}
	else{
		for(i = skb->pon_tag_num - 1; i >= 0; i--){
			matchValue = match_tag(skb, rule, i, data, direction);
			if(matchValue == 0)	{
				PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"match tag %d error return 0 ====>pon match rule",i+1);
				return 0;
			}

			if(i < 2 && (direction == UPSTREAM ) && (RULE_MATCH_TOTAL != matchValue)){
				PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"upstream match total error return 0 ====>pon match rule");
				return 0;
			}

			if(i == 1)
				matchFlag+= matchValue<<3;
			else if(i == 0)
				matchFlag+= matchValue;
		}
	}

	if (TCSUPPORT_CT_PON_SN_VAL) {
		/* when use veip vlan rule, record the upstream rule index */
		if (direction == UPSTREAM && (data->port_index == 0)) {
			//printk("\r\nmatch_rule(): use up_rule_index is %d", up_rule_index);
			up_rule_index = index; 
		}
	}

	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"match rule ====>pon match rule");
	return matchFlag;
}

static inline int get_match_rule(struct sk_buff * skb, pon_vlan * vlan_data ,int dir)
{
    int i = 0;
    int idx_low_pir = INVALID_VLAN_RULE_NUM;
    int idx_default = INVALID_VLAN_RULE_NUM;
    int has_default_rule = 0;
    
    for(i = vlan_data->up_rule_count -1; i >= 0; i--) //search from tail
	{
		if(HIGH_PRIORITY == vlan_data->up_rule[i].rule_priority 
            && (0 != match_rule(skb, vlan_data, dir,i)) ){
            PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"match high pri rule idx = %d ====>pon match rule",i);
            return i;
        }
        if(LOW_PRIORITY == vlan_data->up_rule[i].rule_priority 
            && (0 != match_rule(skb, vlan_data, dir,i)) 
            && INVALID_VLAN_RULE_NUM == idx_low_pir ){
            idx_low_pir = i;
        }
        if (PRIORITY_DEFAULT_ENTRY == vlan_data->up_rule[i].rule_priority 
            && (0 != match_rule(skb, vlan_data, dir,i)) 
            && INVALID_VLAN_RULE_NUM == idx_default ){
            idx_default = i;
        }
        if(PRIORITY_DEFAULT_ENTRY == vlan_data->up_rule[i].rule_priority)	
            has_default_rule =1;
    }
    if (INVALID_VLAN_RULE_NUM != idx_low_pir){
        PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"match low pri rule idx = %d ====>pon match rule",idx_low_pir);
        return idx_low_pir;
    }

    if (INVALID_VLAN_RULE_NUM != idx_default){
        PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"default rule idx = %d ====>pon match rule",idx_default);
        return idx_default;
    }

    PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"no match rule ====>pon match rule");
	    
	//if(has_default_rule || ((vlan_data->up_rule_count != 0 )&& (vlan_data->port_index != PONVLAN_PORT_OFFSET_DEFGPON))){
	if(has_default_rule ){
		PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"default not march");
        return DEFAULT_RULE_NOT_MATCH;
	}
    else
        return INVALID_VLAN_RULE_NUM;
}


static inline int get_pbit_based_on_dscp(struct sk_buff * skb, __u8 * map)
{
	int i = 0;
	char * tmp = NULL;

	if(skb == NULL)
	{
		printk("\r\nskb is NULL ====>get pbit based on dscp");
		return -1;
	}

	if(map == NULL)
	{
		printk("\r\nmap is NULL ====>get pbit based on dscp");
		return -1;
	}

	tmp = skb->data;
	tmp += 12;
	if((__u8)(*tmp) == 0x08 && (__u8)(*(tmp + 1)) == 0x00)
	{
		tmp += 2;
		if(((*tmp & 0xF0) >> 4) == PROTOCOL_IPV4)
		{
			tmp++;
			i = (*tmp & 0xFC) >> 2;
			PONVLAN_PRINT(PONVLAN_MSG_TRACE,"get success,DSCP is %d,pbit is %d ====>get pbit based on DSCP",i,map[i]);
			return map[i];
		}
		
		PONVLAN_PRINT(PONVLAN_MSG_WARNING,"ip version errror return 0 ====>get pbit based on DSCP");
		return 0;
	}
	
	if((__u8)(*tmp) == 0x86 && (__u8)(*(tmp + 1)) == 0xDD)
	{
		tmp += 2;
		if(((*tmp & 0xF0) >> 4) == PROTOCOL_IPV6)
		{
			i = (*tmp & 0x0F) + ((*(tmp + 1) & 0xC0) >> 6);
			PONVLAN_PRINT(PONVLAN_MSG_TRACE,"get success,DSCP is %d,pbit is %d ====>get pbit based on DSCP",i,map[i]);
			return map[i];
		}
		
		PONVLAN_PRINT(PONVLAN_MSG_WARNING,"ip version errror return 0 ====>get pbit based on DSCP");
		return 0;
	}
	
	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"frame protocol error return 0 ====>get pbit based on DSCP");
	return 0;
}

static inline int write_tag_info_to_skb(struct sk_buff * skb,int index,int tpid,int pbit,int dei,int vid,__u16 in_tpid,__u16 out_tpid,__u8 * map,original_tag otag)
{
	switch(tpid)
	{
		case TREAT_TPID_8100:
			skb->pon_vlan_tpid[index] = ETH_P_8021Q;
			break;

		case TREAT_TPID_OUTPUT_TPID:
			skb->pon_vlan_tpid[index] = out_tpid;
			break;

		case TREAT_TPID_COPY_FROM_INNER:
			if(otag.tag_num == 1 || otag.tag_num == 2)
			{
				skb->pon_vlan_tpid[index] = otag.inner_tpid;
			}
			else
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"TPID copy from inner error ====>Write Tag Info to Skb");
				return -1;
			}
			break;
			
		case TREAT_TPID_COPY_FROM_OUTER:
			if(otag.tag_num == 2)
			{
				skb->pon_vlan_tpid[index] = otag.outer_tpid;
			}
			else
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"TPID copy from outer error ====>Write Tag Info to Skb");
				return -1;
			}
			break;

		case TREAT_TPID_INPUT_TPID:
			skb->pon_vlan_tpid[index] = in_tpid;
			break;
			
		default:
			PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Vlan rule TPID error ====>Write Tag Info to Skb");
			return -1;
	}

	switch(pbit)
	{
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
			setPbit(skb->pon_vlan_tci[index],pbit);
			break;

		case TREAT_PRI_COPY_FROM_INNER:
			if(otag.tag_num == 1 || otag.tag_num == 2)
			{
				setPbit(skb->pon_vlan_tci[index],getPbit(otag.inner_tci));
			}
			else
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Pbit copy from inner error ====>Write Tag Info to Skb");
				return -1;
			}
			break;

		case TREAT_PRI_COPY_FROM_OUTER:
			if(otag.tag_num == 2)
			{
				setPbit(skb->pon_vlan_tci[index],getPbit(otag.outer_tci));
			}
			else
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Pbit copy from outer error ====>Write Tag Info to Skb");
				return -1;
			}
			break;

		case TREAT_PRI_BASED_ON_DSCP:
			setPbit(skb->pon_vlan_tci[index],get_pbit_based_on_dscp(skb,map));
			break;

		default:
			PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Vlan rule Pbit error ====>Write Tag Info to Skb");
			return -1;
	}

	switch (dei)
	{
		case 0:
		case 1:
			setDEI(skb->pon_vlan_tci[index],dei);
			break;

		case TREAT_DEI_COPY_FROM_INNER:
			if(otag.tag_num == 1 || otag.tag_num == 2)
			{
				setDEI(skb->pon_vlan_tci[index],getDEI(otag.inner_tci));
			}
			else
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"DEI copy from inner error ====>Write Tag Info to Skb");
				return -1;
			}
			break;

		case TREAT_DEI_COPY_FROM_OUTER:
			if(otag.tag_num == 2)
			{
				setDEI(skb->pon_vlan_tci[index],getDEI(otag.outer_tci));
			}
			else
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"DEI copy from outer error ====>Write Tag Info to Skb");
				return -1;
			}
			break;

		default:
			PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Vlan rule DEI error ====>Write Tag Info to Skb");
			return -1;
	}


	if(vid >= 0 && vid <= 4095)
	{
		setVID(skb->pon_vlan_tci[index],vid);
	}
	else if(vid == TREAT_VID_COPY_FROM_INNER)
	{
		if(otag.tag_num == 1 || otag.tag_num == 2)
		{
			setVID(skb->pon_vlan_tci[index],getVID(otag.inner_tci));
		}
		else
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"VID copy from inner error ====>Write Tag Info to Skb");
			return -1;
		}
	}
	else if(vid == TREAT_VID_COPY_FROM_OUTER)
	{
		if(otag.tag_num == 2)
		{
			setVID(skb->pon_vlan_tci[index],getVID(otag.outer_tci));
		}
		else
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"VID copy from outer error ====>Write Tag Info to Skb");
			return -1;
		}
	}
	else
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Vlan rule VID error ====>Write Tag Info to Skb");
		return -1;
	}
	return 0;
}

static inline int add_tag_to_skb(struct sk_buff * skb, pon_vlan_rule * rule, int tagIndex, __u16 in_tpid,__u16 out_tpid,__u8 * map,original_tag otag)
{
	int tpid=0,pbit=0,dei=0,vid=0,pos=0;
	
	if(skb == NULL)
	{
		printk("\r\nskb is NULL ====>Add Tag to Skb");
		return -1;
	}

	if(rule == NULL)
	{
		printk("\r\nrule is NULL ====>Add Tag to Skb");
		return -1;
	}
	
	if(map == NULL)
	{
		printk("\r\nmap is NULL ====>Add Tag to Skb");
		return -1;
	}

	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"add no.%d tag ====>Add Tag to Skb",tagIndex);
	if(tagIndex == ADD_FIRST_TAG)
	{
		tpid = rule->add_fst_tpid;
		pbit = rule->add_fst_pri;
		dei = rule->add_fst_dei;
		vid = rule->add_fst_vid;
	}
	else if(tagIndex == ADD_SECOND_TAG)
	{
		tpid = rule->add_sec_tpid;
		pbit = rule->add_sec_pri;
		dei = rule->add_sec_dei;
		vid = rule->add_sec_vid;
	}
	else
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"paramater error ====>Add Tag to Skb");
		return -1;
	}

	pos = skb->pon_tag_num;
	skb->pon_vlan_tci[pos] = 0;//clean tag info

	if(write_tag_info_to_skb(skb,pos,tpid,pbit,dei,vid,in_tpid,out_tpid,map,otag) == -1)
		return -1;

	skb->pon_tag_num ++;
	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"Add tag success ====>Add Tag to Skb");
	return 0;
}

static inline int del_tag_from_skb(struct sk_buff * skb, int num)
{
	int pos=0;
	int i=0;

	if(skb == NULL)
	{
		printk("\r\nskb is NULL ====>del tag from skb");
		return -1;
	}

	if(num > skb->pon_tag_num || num == 0)
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Del num is %d,and packet Tag num is %d,it will cause an error====> Del Tag",num,skb->pon_tag_num);
		return -1;
	}

	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"del_tag_from_skb, Del num is %d,and packet Tag num is %d",num,skb->pon_tag_num);
	for(i=0; i<num; i++){ //remove vlan  tag
		pos =  skb->pon_tag_num - num +i;
		if(pos>=0 && pos<4){
			skb->pon_vlan_tpid[pos] = 0;
			skb->pon_vlan_tci[pos] = 0;
		}
		else
			break;
	}

	skb->pon_tag_num -= num;
	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"del tag success ====>Del Tag from Skb");
	return 0;
	
}

static inline int change_tag_in_skb(struct sk_buff * skb, pon_vlan_rule * rule, int tag_position, __u16 in_tpid, __u16 out_tpid,__u8 * map,original_tag otag,int matchFlag)
{
	int tpid=0,pbit=0,dei=0,vid=0,pos=0;

	if(skb == NULL)
	{
		printk("\r\nskb is NULL ====>Change tag in skb");
		return -1;
	}

	if(rule == NULL)
	{
		printk("\r\nrule is NULL ====>Change tag in skb");
		return -1;
	}
	
	if(map == NULL)
	{
		printk("\r\nmap is NULL ====>Change tag in skb");
		return -1;
	}

	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"change no.%d tag ====>Change Tag to Skb",tag_position);
	if(tag_position == CHANGE_INNER_TAG)
	{
	    
		tpid = rule->add_fst_tpid;
		if(RULE_MATCH_PRI == (matchFlag&0x7)) 
		{			
			pbit = rule->add_fst_pri;
			vid = getVID(otag.inner_tci);
		}
        else if(RULE_MATCH_VID == (matchFlag&0x7))
        {
			pbit = getPbit(otag.inner_tci);
			vid = rule->add_fst_vid; 
        }
		else
		{
		    pbit = rule->add_fst_pri;
			vid = rule->add_fst_vid;
		}
		
		dei = rule->add_fst_dei;
	}
	else if(tag_position == CHANGE_OUTER_TAG || tag_position == ADD_AND_CHANGE_OUTER_TAG || tag_position == DEL_AND_CHANGE_INNER_TAG)
	{
		tpid = rule->add_sec_tpid;
		if((tag_position == ADD_AND_CHANGE_OUTER_TAG)&&(RULE_MATCH_VID == (matchFlag&0x7))) 
			pbit = getPbit(otag.inner_tci);
		else if(RULE_MATCH_VID == ((matchFlag>>3)&0x7))	 
		{
			if(tag_position == DEL_AND_CHANGE_INNER_TAG)
				pbit = getPbit(otag.inner_tci);
			else
				pbit = getPbit(otag.outer_tci);
		}
		else
			pbit = rule->add_sec_pri;
		dei = rule->add_sec_dei;
		vid = rule->add_sec_vid;
	}
	else
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"paramater error ====>Change tag in skb");
		return -1;
	}

	if(tag_position == ADD_AND_CHANGE_OUTER_TAG)
	{
		if(rule->tag_num == 1)
			pos = CHANGE_INNER_TAG;
		else if(rule->tag_num == 2)
			pos = CHANGE_OUTER_TAG;
		else
			return -1;
	}
	else if(tag_position == DEL_AND_CHANGE_INNER_TAG)
	{
		if(rule->tag_num == 2)
			pos = CHANGE_INNER_TAG;
		else if(rule->tag_num == 3)
			pos = CHANGE_OUTER_TAG;
		else
			return -1;
	}
	else
		pos = tag_position;

	if(write_tag_info_to_skb(skb,pos,tpid,pbit,dei,vid,in_tpid,out_tpid,map,otag) == -1)
		return -1;

	return 0;
}

static inline int do_option(struct sk_buff * skb, pon_vlan * data, int direction,int index,original_tag otag,int matchFlag)
{
	int i = 0;
	pon_vlan_rule * rule = NULL;
	
	if(skb == NULL)
	{
		printk("\r\nskb is NULL ====>do option");
		return -1;
	}

	if(data == NULL)
	{
		printk("\r\ndata is NULL ====>do option");
		return -1;
	}
	
	if(direction == UPSTREAM)
	{
		rule = &(data->up_rule[index]);
	}
	else if(direction == DOWNSTREAM)
	{
		rule = &(data->down_rule[index]);
	}
	else
		return -1;

	i = rule->treatment_method;

	switch(i/10)
	{
		case 0:
			if(i == METHOD_TRANSPARENT)
				return 0;
			else
				return -1; // default is discarded. so ret = -2 will continue search match rule.

		case METHOD_ADD_TAG:
			if(add_tag_to_skb(skb,rule,ADD_FIRST_TAG,data->input_tpid,data->output_tpid,data->dscp_map,otag) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Add first tag to skb error====> Do option");
				return -1;
			}
			
			if(i == ADD_AND_CHANGE_OUTER_TAG)
			{
				if(change_tag_in_skb(skb,rule,ADD_AND_CHANGE_OUTER_TAG,data->input_tpid,data->output_tpid,data->dscp_map,otag,matchFlag) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Change tag in skb error(add case)====> Do option");
					return -1;
				}
			}
			
			if(i%10 == ADD_SECOND_TAG)
			{
				if(add_tag_to_skb(skb,rule,ADD_SECOND_TAG,data->input_tpid,data->output_tpid,data->dscp_map,otag) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Add second tag to skb error====> Do option");
					return -1;
				}
			}
			return 0;

		case METHOD_DEL_TAG:
			if(i == DEL_AND_CHANGE_INNER_TAG)
			{
				if(del_tag_from_skb(skb, 1) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Del tag from skb error====> Do option");
					return -1;
				}
				if(change_tag_in_skb(skb,rule,DEL_AND_CHANGE_INNER_TAG,data->input_tpid,data->output_tpid,data->dscp_map,otag,matchFlag) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Change tag in skb error(del case)====> Do option");
					return -1;
				}
			}
			else
			{
				if(del_tag_from_skb(skb, i%10) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Del tag from skb error====> Do option");
					return -1;
				}
			}
			return 0;

		case METHOD_CHANGE_TAG:
			if(i%10 == 0 || i%10 == 2)
			{
				if(change_tag_in_skb(skb,rule,CHANGE_INNER_TAG,data->input_tpid,data->output_tpid,data->dscp_map,otag,matchFlag) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Change inner tag in skb error====> Do option");
					return -1;
				}
			}
			
			if(i%10 == 1 || i%10 == 2)
			{
				if(change_tag_in_skb(skb,rule,CHANGE_OUTER_TAG,data->input_tpid,data->output_tpid,data->dscp_map,otag,matchFlag) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Change outer tag in skb error====> Do option");
					return -1;
				}
			}
			return 0;
			
		default:
			PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Operation Method Error ====> Do option");
			return -1;
	}
	
}

int save_mac_vlan_info(struct sk_buff * skb,pon_vlan * vlan_data,original_tag otag, __u8 uprule_index)
{
	int i = 0;
	mac_vid_pair * mv_pair = vlan_data->mac_vid;
	
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0) 
	struct timeval tv;
#else
	struct timespec64 tv;
#endif
	
	
	if((skb->pon_vlan_flag & PON_PKT_FROM_LAN) || (skb->pon_vlan_flag & PON_VLAN_TX_CALL_HOOK))
	{
	
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0) 
		do_gettimeofday(&tv);
#else
		ktime_get_real_ts64(&tv);
#endif
		for(i = 0; i < MAX_MAC_VID; i++)
		{
			//same original info mean don't need save this packet info.
			//it will cause error with same original info but different new vlan in one port.
			if(memcmp((skb->data + 6),mv_pair->src_mac,6) == 0
				&& (mv_pair->vid_mask & RULE_ENABLE) != 0 && ((tv.tv_sec - mv_pair->last_time) < pon_vlan_all_data.mac_vlan_time))
			{
				if((mv_pair->vid_mask & ORG_INNER_VID) != 0 && mv_pair->org_inner_vid == getVID(otag.inner_tci))
				{
					if(otag.tag_num > 1)
					{
						if((mv_pair->vid_mask & ORG_OUTER_VID) != 0 && mv_pair->org_outer_vid == getVID(otag.outer_tci))
						{
							PONVLAN_PRINT(PONVLAN_MSG_TRACE,"break because of same pkt info(2 tag case) ====> save mac info");
						}
						else{
							//if(((tv.tv_sec - mv_pair->last_time) > 1) && hwnat_clean_entry_by_dst_mac_hook)
							//	hwnat_clean_entry_by_dst_mac_hook(mv_pair->src_mac);
							mv_pair->org_outer_vid = getVID(otag.outer_tci);
							mv_pair->uprule_index = uprule_index;
							PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->vlan_tag_flag,"update vid1. the upstream mac:%02x:%02x:%02x:%02x:%02x:%02x, uprule_index:%d,org_vid:%x, rs_vid:%x, i:%d\n",
								mv_pair->src_mac[0], mv_pair->src_mac[1],mv_pair->src_mac[2], mv_pair->src_mac[3],mv_pair->src_mac[4], mv_pair->src_mac[5],
								uprule_index, getVID(otag.inner_tci), getVID(skb->pon_vlan_tci[skb->pon_tag_num - 1]), i);
						}
					}
					else
					{
						PONVLAN_PRINT(PONVLAN_MSG_TRACE,"break because of same pkt info(1 tag case) ====> save mac info");
					}
				}
				else{
					mv_pair->org_inner_vid = getVID(otag.inner_tci);
					mv_pair->uprule_index = uprule_index;
					//if(((tv.tv_sec - mv_pair->last_time) > 1) && hwnat_clean_entry_by_dst_mac_hook)
					//	hwnat_clean_entry_by_dst_mac_hook(mv_pair->src_mac);
					PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->vlan_tag_flag,"update vid2. the upstream mac:%02x:%02x:%02x:%02x:%02x:%02x, uprule_index:%d,org_vid:%x, rs_vid:%x, i:%d\n",
						mv_pair->src_mac[0], mv_pair->src_mac[1],mv_pair->src_mac[2], mv_pair->src_mac[3],mv_pair->src_mac[4], mv_pair->src_mac[5],
						uprule_index, getVID(otag.inner_tci), getVID(skb->pon_vlan_tci[skb->pon_tag_num - 1]), i);
				}
				mv_pair->last_time = tv.tv_sec;
				return 0;
			}
			mv_pair++;
		}
		
		/* if not match a mac record, then add the record in . */
		mv_pair = vlan_data->mac_vid;
		for(i = 0; i < MAX_MAC_VID; i++)
		{
			if((mv_pair->vid_mask & RULE_ENABLE) == 0//empty rule
				|| ((tv.tv_sec - mv_pair->last_time) > pon_vlan_all_data.mac_vlan_time))//time out rule
			{
				memcpy(mv_pair->src_mac,skb->data + ETH_ALEN,ETH_ALEN);
				mv_pair->uprule_index = uprule_index;
				PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->vlan_tag_flag,"record the upstream mac:%02x:%02x:%02x:%02x:%02x:%02x, uprule_index:%d,org_vid:%x, rs_vid:%x, i:%d",
					mv_pair->src_mac[0], mv_pair->src_mac[1],mv_pair->src_mac[2], mv_pair->src_mac[3],mv_pair->src_mac[4], mv_pair->src_mac[5],
					uprule_index, getVID(otag.inner_tci), getVID(skb->pon_vlan_tci[skb->pon_tag_num - 1]), i);

				if(otag.tag_num == 0)
				{
					mv_pair->vid_mask = 0;
					mv_pair->org_inner_vid = 0;
				}

				if(otag.tag_num > 0)
				{
					mv_pair->vid_mask |= ORG_INNER_VID;
					mv_pair->org_inner_vid = getVID(otag.inner_tci);
				}
				if(otag.tag_num > 1)
				{
					mv_pair->vid_mask |= ORG_OUTER_VID;
					mv_pair->org_outer_vid = getVID(otag.outer_tci);
				}
				mv_pair->last_time = tv.tv_sec;
				if(skb->pon_tag_num <= 4)
				{
					mv_pair->vid_mask |= RULE_ENABLE;
					mv_pair->rs_outer_vid = getVID(skb->pon_vlan_tci[skb->pon_tag_num - 1]);
				}
				break;
			}
			mv_pair++;
		}
	}
	return 0;
}

int get_uprule_index_by_mac(struct sk_buff * skb,pon_vlan * vlan_data)
{
	int i = 0;
	mac_vid_pair * mv_pair = vlan_data->mac_vid;
	int matchIndex = -1, matchFlag = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0) 
	struct timeval tv;
	do_gettimeofday(&tv);
#else
	struct timespec64 tv;
	ktime_get_real_ts64(&tv);
#endif

	for(i = 0; i < MAX_MAC_VID; i++)
	{
		if(mv_pair->vid_mask & RULE_ENABLE)
		{
			if((tv.tv_sec - mv_pair->last_time) > pon_vlan_all_data.mac_vlan_time)//time out,clean the rule and check next
			{
				mv_pair->vid_mask = 0;
				continue;
			}
			//unicat
			if(memcmp(skb->data,mv_pair->src_mac,6) == 0){
				matchFlag = match_rule(skb, vlan_data, DOWNSTREAM,mv_pair->uprule_index);
				if(matchFlag == 0)
					continue;
				else if((matchFlag == RULE_MATCH_TOTAL) || (matchFlag == RULE_MATCH_TOTAL << 3))
					return mv_pair->uprule_index;
				else{
					if(matchIndex == -1)
						matchIndex = mv_pair->uprule_index;
				}
				
			}
		}
		mv_pair++;
	}
	if(matchIndex != -1)
		return matchIndex;
	
	return -1;
}


int set_mac_vlan(struct sk_buff * skb,pon_vlan * vlan_data,original_tag otag)
{
	int i = 0;
	mac_vid_pair * mv_pair = vlan_data->mac_vid;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0) 
	struct timeval tv;
	do_gettimeofday(&tv);
#else
	struct timespec64 tv;
	ktime_get_real_ts64(&tv);
#endif
	for(i = 0; i < MAX_MAC_VID; i++)
	{
		if(mv_pair->vid_mask & RULE_ENABLE)
		{
			if((tv.tv_sec - mv_pair->last_time) > pon_vlan_all_data.mac_vlan_time)//time out,clean the rule and check next
			{
				mv_pair->vid_mask = 0;
				continue;
			}
			
			if(memcmp(skb->data,mv_pair->src_mac,6) == 0)
			{
				if((otag.tag_num == 1 && (getVID(otag.inner_tci) == getVID(mv_pair->rs_outer_vid)))
					|| (otag.tag_num == 2 && (getVID(otag.outer_tci) == getVID(mv_pair->rs_outer_vid))))
				{
					if(skb->pon_tag_num == 1 && (mv_pair->vid_mask & ORG_INNER_VID) && !(mv_pair->vid_mask & ORG_OUTER_VID))
					{
						setVID(skb->pon_vlan_tci[0],getVID(mv_pair->org_inner_vid));
					}
					if(skb->pon_tag_num == 2 && (mv_pair->vid_mask & ORG_INNER_VID) && (mv_pair->vid_mask & ORG_OUTER_VID))
					{
						setVID(skb->pon_vlan_tci[0],getVID(mv_pair->org_inner_vid));
						setVID(skb->pon_vlan_tci[1],getVID(mv_pair->org_outer_vid));
					}
					break;
				}
			}
		}
		mv_pair++;
	}
	return 0;
}

struct sk_buff* eponVlanPriRemark(struct sk_buff *skb)
{

    unsigned char uc802prio = 0; 
    __u16 vid = 1;
    int copy_len = 0;
    struct vlan_ethhdr* vlan_hdr;
    struct ethhdr * eth_header;

    if(skb == NULL) {
        return NULL;
    }
    
    if(unlikely(skb_cloned(skb ) ) )
    {
        printk("fatal error: skb should be unshared by the caller!\n");
        return NULL;
    }

    uc802prio = skb->epon_pbit;

    if(uc802prio != EPON_PRI_REMARK_DEFAULT_VALUE)
    {
        eth_header = (struct ethhdr *) skb->data;
        
        if((ETH_P_8021Q == ntohs(eth_header->h_proto))||(ETH_P_QinQ_88a8 == ntohs(eth_header->h_proto)) \
            ||(ETH_P_QinQ_9100 == ntohs(eth_header->h_proto)))
        {
            vlan_hdr = (struct vlan_ethhdr*)skb->data;
            vid = (ntohs(vlan_hdr->h_vlan_TCI) & VLAN_VID_MASK);
        }
        else 
        {
            /*Insert a vlan tag with vid =1*/
            if ( skb_headroom(skb) < VLAN_HLEN ) 
            {
                struct sk_buff *skb2 = skb_realloc_headroom(skb, VLAN_HLEN);
                if (skb2 == NULL) 
                {
                    printk("insert tag fail\n");
                    return NULL;
                }
                dev_kfree_skb(skb);
                skb = skb2;
            }

            /*offset 4 bytes*/
            skb_push(skb, VLAN_HLEN);

            copy_len = (VLAN_ETH_ALEN<<1);
            /*move the mac address to the beginning of new header*/
            memmove(skb->data, skb->data+VLAN_HLEN, copy_len);
            skb->network_header -= VLAN_HLEN;
            skb->mac_header -= VLAN_HLEN;

            eth_header = (struct ethhdr *) skb->data;
            vlan_hdr = (struct vlan_ethhdr*)skb->data;
        }

        eth_header->h_proto = htons(ETH_P_8021Q);
        
        vlan_hdr->h_vlan_TCI = 0;
        /*bits priority and vid vlaue*/
        vlan_hdr->h_vlan_TCI |= htons(((uc802prio & 0x7) << VLAN_PRIO_SHIFT)|vid) ;
    }
    return skb;
}

int insert_tag_function(struct sk_buff **pskb)
{
    int i = 0;
	struct sk_buff *skb = NULL;

	if(pskb == NULL)
		return -1;

	skb = *pskb;
    if(skb == NULL)
        return -1;

    for(i = 0; i < skb->pon_tag_num; i++)
    {
        PONVLAN_PRINT(PONVLAN_MSG_TRACE,"In the Pon Insert Tag,vlan tag index is %d,tpid is %x,tci is %x",i,skb->pon_vlan_tpid[i],skb->pon_vlan_tci[i]);
        skb = __pon_vlan_put_tag(skb, skb->pon_vlan_tpid[i], skb->pon_vlan_tci[i]);
        if (skb == NULL)
            return -1;
    }
    
    if( skb->pon_vlan_flag & PON_CLASSIFICATION_REMARK ) 
    {
        skb = eponVlanPriRemark(skb);
        if(skb == NULL){
            return -1;
        }
		else
			*pskb = skb;
    }

#if 0
    if((skb->pon_vlan_flag & PON_VLAN_RX_CALL_HOOK) && pon_vlan_all_data.onu_mode == MODE_HGU)
    {
        if(DBG_Level > 1)
        {
            printk("skb info is");
            for(i = 0; i < 14; i++)
                printk("%02x",skb->data[i]);
        }
        skb->data += 12;
        skb->protocol = *(__u16 *)(skb->data);
        skb->data += 2;
        PONVLAN_PRINT(PONVLAN_MSG_TRACE,"In the Pon Insert Tag,HGU ds print,protocol is  %x,skb data is %02x%02x, dev_name=%s",htons(skb->protocol),skb->data[0],skb->data[1], skb->dev->name);
    }
#endif
    
    return 0;
}
int is_same_up_rule(pon_vlan_rule * rule0, pon_vlan_rule * rule1)
{
    if (rule0->add_fst_tpid != rule1->add_fst_tpid)
        return 0;
    if (rule0->add_fst_pri != rule1->add_fst_pri)
        return 0;
    if (rule0->add_fst_dei != rule1->add_fst_dei)
        return 0;
    if (rule0->add_fst_vid != rule1->add_fst_vid)
        return 0;
    if (rule0->add_sec_tpid != rule1->add_sec_tpid)
        return 0;
    if (rule0->add_sec_pri != rule1->add_sec_pri)
        return 0;
    if (rule0->add_sec_dei != rule1->add_sec_dei)
        return 0;
    if (rule0->add_sec_vid != rule1->add_sec_vid)
        return 0;
    if (rule0->rule_priority != rule1->rule_priority)
        return 0;
    if ((rule0->treatment_method == 40 &&  (rule1->treatment_method == 21 && rule1->tag_num == 0))
        || ((rule0->treatment_method == 21 && rule0->tag_num == 0) &&  rule1->treatment_method == 40 )
        || (rule0->treatment_method == 40 && rule0->treatment_method == rule1->treatment_method))    
        return 1;
    else
        return 0;
}

/* check the N:1 aggregation mode. */
int should_send_broadcast_by_multi_rules(struct sk_buff *skb)
{
    int i = 0;
    int j = 0;
    pon_vlan * vlan_data = NULL;
    if(skb == NULL)
    {
        if(DBG_Level > 0)
        	printk("\r\n skb = NULL");
        return -1;
    }
    vlan_data = get_vlan_opt_data(skb);
           
    if(vlan_data == NULL)
    {
       if(DBG_Level > 0)
            printk("\r\nno rule for this packet,discard packet");
       return -1;
    }
    else
    {
        if(DBG_Level > 0)
            printk("\r\nFind Vlan rule in port %d",vlan_data->port_index);
    }

    for(i = 0;  i < (vlan_data->up_rule_count - 1); i++)
    {
        for( j = i+1; j < vlan_data->up_rule_count ; j++ )
        {
            if(is_same_up_rule(&(vlan_data->up_rule[i]),&(vlan_data->up_rule[j])))
            {
                return 1;
        	}
    	}
    }

    return 0;
}

int send_broadcast_by_multi_rules(struct sk_buff *skb)
{
    int i = 0;
    struct sk_buff *skb_tmp = NULL;
    pon_vlan * vlan_data = NULL;
    original_tag otag = {0};
    int downMatchFlag = 0;
    int have_match = 0; 

    if(skb == NULL)
    {
        PONVLAN_PRINT(PONVLAN_MSG_ERR," skb = NULL");
        return -1;
    }

    if((skb->pon_vlan_flag & PON_VLAN_RX_CALL_HOOK) && pon_vlan_all_data.onu_mode == MODE_HGU){
        return 0;
    }
    
    vlan_data = get_vlan_opt_data(skb);

    if(vlan_data == NULL)
    {
       PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"no rule for this packet,discard packet");
       return -1;
    }
    else
    {
       PONVLAN_PRINT(PONVLAN_MSG_TRACE,"Find Vlan rule in port %d",vlan_data->port_index);
    }

    /* send a copy for each matched downstream vid. */
    for(i = vlan_data->down_rule_count-1;  i >= 0; i--)
    {
        
        /* if operation is transparent do not resend */
        if(METHOD_TRANSPARENT == vlan_data->down_rule[i].treatment_method)
        {
            continue;
        }

        skb_tmp = skb_copy(skb, GFP_ATOMIC);
        if(skb_tmp == NULL){
            PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag," skb copy error");
            return -1;
        }

#if 0
        if((skb_tmp->pon_vlan_flag & PON_VLAN_RX_CALL_HOOK) && pon_vlan_all_data.onu_mode == MODE_HGU)
            skb_tmp->data -= 14;
#endif
        
        if(store_tag_info(skb_tmp) == -1)
        {
            kfree_skb(skb_tmp);
            PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag," store tag info error");
            continue;
        }
        memset(&otag, 0, sizeof(otag));

        otag.tag_num = skb_tmp->pon_tag_num;
        if(skb_tmp->pon_tag_num > 0)
        {
           otag.inner_tpid = skb_tmp->pon_vlan_tpid[0];
           otag.inner_tci = skb_tmp->pon_vlan_tci[0];
        }
        if(skb_tmp->pon_tag_num > 1)
        {
           otag.outer_tpid= skb_tmp->pon_vlan_tpid[1];
           otag.outer_tci = skb_tmp->pon_vlan_tci[1];
        }
        downMatchFlag = match_rule(skb_tmp, vlan_data, DOWNSTREAM,i);
        if(downMatchFlag != 0){
        /* for broadcast frame, will never matched by mac, so only need to ignore the first matched rule to avoid the repeat frame. */
            if(have_match == 0)
            {
                have_match = 1;
                kfree_skb(skb_tmp);
                PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag," ignore matched rule. ");
                continue;
            }
            if(do_option(skb_tmp, vlan_data, DOWNSTREAM, i, otag, downMatchFlag) == -1)
            {
                kfree_skb(skb_tmp);
                PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag," do option error");
                continue;
            }
        }
        else{/* if not match, then drop the frame.  */
            kfree_skb(skb_tmp);
            continue;
        }
        if(insert_tag_function(&skb_tmp) != 0)
        {
            PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag," insert tag error");
            continue;
        }

        dev_queue_xmit(skb_tmp);
    }

    return 0;
}

int get_stream_direction(__u32 pon_vlan_flag)
{
	if((pon_vlan_flag & PON_PKT_FROM_HYBRID_PPTP) || (pon_vlan_flag & PON_PKT_FROM_HYBRID_VEIP))
	{
		return UPSTREAM;
	}
    else if((((pon_vlan_flag & PON_PKT_FROM_CPE) || (pon_vlan_flag & PON_PKT_FROM_LAN)) && pon_vlan_all_data.onu_mode == MODE_SFU)
        || (pon_vlan_all_data.onu_mode == MODE_HGU && (pon_vlan_flag & PON_VLAN_TX_CALL_HOOK)))
    {
        return UPSTREAM;
    }
    else if(((pon_vlan_flag & PON_PKT_FROM_WAN) && pon_vlan_all_data.onu_mode == MODE_SFU)
        || (pon_vlan_all_data.onu_mode == MODE_HGU && (pon_vlan_flag & PON_VLAN_RX_CALL_HOOK)))
    {
        return DOWNSTREAM;
    }
    else
        return UNKNOWN_STEAM;
}

int is_cpe2lan_pkt(__u32 pon_vlan_flag, struct net_device *out_dev)
{
    if(((pon_vlan_flag & PON_PKT_FROM_CPE)  && out_dev->name[0] == 'e') && pon_vlan_all_data.onu_mode == MODE_SFU)
        return 1;
    else
        return 0;
}

inline int is_lan2lan_pkt(__u32 pon_vlan_flag, struct net_device *out_dev)
{
    if(( (pon_vlan_flag & PON_PKT_FROM_LAN) && out_dev->name[0] == 'e') && pon_vlan_all_data.onu_mode == MODE_SFU)
        return 1;
	else if((pon_vlan_flag & PON_PKT_FROM_HYBRID_PPTP) 
		&& (out_dev->name[0] == 'e' || out_dev->name[0] == 'b'))
	{
		PONVLAN_PRINT(PONVLAN_MSG_WARNING," is_lan2lan_pkt, dev=%s,skip vlan operation!\n",out_dev->name);
		return 1;
	}
    else
        return 0;
}

int is_valid_tx_rx_pkt(__u32 pon_vlan_flag, struct net_device *out_dev)
{
    if(hy_enable == 1){
		if((pon_vlan_flag & PON_PKT_FROM_HYBRID_VEIP) || (pon_vlan_flag & PON_PKT_FROM_HYBRID_PPTP)) //hybrid upstream
			return 1;
		else if((pon_vlan_flag & PON_VLAN_RX_CALL_HOOK) && (out_dev->name[0] == 'e' || !strcmp(out_dev->name,"pon")))  //hybrid downstream  pon or eth
			return 1;
		else
			return 0;
	}
    else if((pon_vlan_all_data.onu_mode == MODE_HGU && ((pon_vlan_flag & PON_VLAN_RX_CALL_HOOK) || (pon_vlan_flag & PON_VLAN_TX_CALL_HOOK)))//HGU mode,check tx/rx hook  
        || ((out_dev->name[0] == 'e' || out_dev->name[0] == 'r' || out_dev->name[0] == 'u' || strcmp(out_dev->name,"pon") == 0) && pon_vlan_all_data.onu_mode == MODE_SFU) //sfu voip tx include in "pon" tx    
            || (TCSUPPORT_PON_IP_HOST_VAL && (pon_vlan_flag & PON_PKT_VOIP_RX) && pon_vlan_all_data.onu_mode == MODE_SFU) // sfu voip downstream
        )
        return 1;
    else
        return 0;
}
int pon_vlan_ani_filter(struct sk_buff *skb, __u8 ponVlanFilterDirFlag)
{
	int ret = 0;
	int ani_num = 0,i = 0;
	__u16 ani_id[GPON_GEM_MAX_ANI_NUM] = {0};
    __u8 discardFlag = GPON_PKT_VLAN_FILTER_DISCARD;

	if(gponAniMapEnableFlag == 0){
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_TRACE,"gponAniMap Disable\n");
		return -1;
	}
	GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_TRACE,"Enter\n");
	if(skb == NULL || skb->dev == NULL || skb->dev->name[0] == '\0' ){
		if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"parameter is NULL, fail\n");
		return -1;
	}

	ani_num = gpon_ani_map_get_by_gem(skb->gem_port,ani_id);
	GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_TRACE,"gem_id=%d, ani_num=%d\n",skb->gem_port,ani_num);
	if((ani_num <= 0) || (ani_num > GPON_GEM_MAX_ANI_NUM)){
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"gpon_ani_map_get_by_gem fail\n");
		return -1;
	}
	for(i=0; i<ani_num; i++){
		if(ani_id[i] != skb->v_if){
			skb->v_if = ani_id[i];
			ret = matchVlanFilterRule(skb, ponVlanFilterDirFlag,&discardFlag);
			if((ret == GPON_VLAN_FILTER_SUCCESS) && (discardFlag == GPON_PKT_VLAN_FILTER_DISCARD)){
				continue;
			}else{
				GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_TRACE,"success, gem_id=%d, ani_id=%d\n",skb->gem_port,ani_id[i]);
				return 0;
			}
		}
	}
	GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"all ani filter fail\n");
	return -1;
}
int pon_vlan_filter(struct sk_buff *skb)
{
    __u8 ponVlanFilterDirFlag = 0;
    int k = 0;
    __u8 discardFlag = 0;
    int result = 0;
    __u8 direction = -1;


    if(skb->pon_vlan_flag & PON_PKT_SEND_TO_WAN)
        direction = UPSTREAM;
    else
    {
        direction = get_stream_direction(skb->pon_vlan_flag);
        if(direction != DOWNSTREAM || memcmp(skb->dev->name,"pon",3)){
            PONVLAN_PRINT(PONVLAN_MSG_WARNING,"pkt is not down sream in pon dev");
            return 0;
        }
    }
    
	if(direction == UPSTREAM && skb->pon_tag_num == 0)
	{
		pon_check_untag(skb);
	}
/*according to the original vlan tag, execute ANI RX and UNI TX vlan filter, downstream*/
    
    if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE){
        if( direction == DOWNSTREAM )
            printk("\r\n pon_insert_tag->ANI_RX, UNI_TX, skb->pon_tag_num=%d",skb->pon_tag_num);
        else if( direction == UPSTREAM)
            printk("\r\n pon_insert_tag->UNI_RX, ANI_TX, skb->pon_tag_num=%d",skb->pon_tag_num);
        for(k = 0; k < skb->pon_tag_num; k++){
            printk("\r\n skb->pon_vlan_tci[%d]=0x%02x",k,skb->pon_vlan_tci[k]);
        }
    }
    if( direction == DOWNSTREAM )
        ponVlanFilterDirFlag = (GPON_VLAN_FILTER_HANDLE_ANI_RX_VLAN_TAG |GPON_VLAN_FILTER_HANDLE_UNI_TX_VLAN_TAG);
    else if( direction == UPSTREAM)
        ponVlanFilterDirFlag = (GPON_VLAN_FILTER_HANDLE_UNI_RX_VLAN_TAG |GPON_VLAN_FILTER_HANDLE_ANI_TX_VLAN_TAG);
    
    result = matchVlanFilterRule(skb, ponVlanFilterDirFlag,&discardFlag);
    if((result == GPON_VLAN_FILTER_SUCCESS) && (discardFlag == GPON_PKT_VLAN_FILTER_DISCARD)){
		if(pon_vlan_ani_filter(skb, ponVlanFilterDirFlag) == -1){
	        if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_TRACE || (skb->pon_vlan_flag & PON_PKT_TRACE_FLAG) ){
	            if( direction == DOWNSTREAM )
	                printk("\r\n pon_insert_tag->WAN->LAN->matchVlanFilterRule, discard, return -1 \n");
	            else if( direction == UPSTREAM)
	                printk("\r\n pon_insert_tag->LAN->WAN->matchVlanFilterRule, discard, return -1 \n");
	        }
	        return -1;
		}
    }else if(result == GPON_VLAN_FILTER_FAIL){
        if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR || (skb->pon_vlan_flag & PON_PKT_TRACE_FLAG) ){
            if( direction == DOWNSTREAM )
                printk("\r\n pon_insert_tag->WAN->LAN->matchVlanFilterRule, fail \n");
            else if( direction == UPSTREAM)
                printk("\r\n pon_insert_tag->LAN->WAN, matchVlanFilterRule, fail");
        }
    }
    return 0;
}

static int is_mcast_data_pkt(struct sk_buff* skb)
{
	char mac[3]  =  { 0x01,0x00,0x5e};
	char mac2[2] = {0x33,0x33};
	unsigned char *dest = NULL;

	if(NULL == skb || NULL == skb->dev)
	{
		return 0;
	}

	dest = eth_hdr(skb)->h_dest;
	if(NULL == dest)
	{
	    return 0;
	}

	if (!memcmp(dest,mac,3) || !memcmp(dest,mac2,2))
		return 1;

	return 0;
}

/*return value: 0(not goto), 1(goto insert_tag), -1(goto fail)*/
int handle_igmp_pkt(struct sk_buff *skb)
{
    int i = 0,port = PONVLAN_PORT_OFFSET_VP;

    if(pon_vlan_all_data.igmp_enable_flag == 1 && is_mcast_data_pkt(skb) == 1)
    {
        PONVLAN_PRINT(PONVLAN_MSG_TRACE,"Igmp add Tag function is enable");
        port = get_port_num(skb);
        for(i = 1; i < pon_vlan_all_data.total_port_count; i++)
        {
            if(pon_vlan_all_data.pon_vlan_data[i].port_index == port)
            {
                if(handle_igmp_tag(&skb,pon_vlan_all_data.pon_vlan_data[i].igmp_mode,pon_vlan_all_data.pon_vlan_data[i].igmp_tci) == -1){
                    PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Igmp handle tag fail");
                    return -1;
                }
                return 1;
            }
        }
        PONVLAN_PRINT(PONVLAN_MSG_TRACE|skb->pon_vlan_flag,"no rule for this igmp packet,do nothing here");
        return 1;
    }
    else
    {
        PONVLAN_PRINT(PONVLAN_MSG_TRACE|skb->pon_vlan_flag,"Igmp add Tag function is disable,do as normal packet");
    }
    return 0;
}
int change_pkt_by_match_rule(struct sk_buff *skb, __u8 direction, pon_vlan * vlan_data, original_tag otag)
{
    int MatchIndex = 0;
    int matchFlag = 0;

    MatchIndex = get_match_rule(skb, vlan_data ,direction);

    if(direction == DOWNSTREAM)
        matchFlag = match_rule(skb,vlan_data,direction,MatchIndex);

    if(MatchIndex > -1 && MatchIndex < vlan_data->down_rule_count)
    {
        PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"Match the Rule %d,we will do the option",MatchIndex);
        
        if(do_option(skb, vlan_data, direction, MatchIndex, otag, matchFlag) == -1)
        {
            PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Do Option Error,drop packet");
            return -1;
        }
        else
        {
            if(vlan_data->mac_bind_vlan_enable == 1 && direction == UPSTREAM){
                PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"Match the Rule %d, save the upstream rule index by mac", MatchIndex);
                save_mac_vlan_info(skb,vlan_data,otag,MatchIndex);
            }
            else if(vlan_data->mac_bind_vlan_enable == 1 && direction == DOWNSTREAM){
                PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"Match the Rule %d, set the old mac enable info", MatchIndex);
                set_mac_vlan(skb,vlan_data,otag);
            }
            return 1;
        }
    }
    else if (MatchIndex == DEFAULT_RULE_NOT_MATCH)
    {
        PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"not match the default rule,drop packet");
        return -1;
    }
    
    PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"can not find rule");
    return 0;
}
int handle_common_pkt(struct sk_buff *skb, int *downstream_broadcast_flag)
{
    int direction = -1;
    int ret = 0;
    int iMatchIndex = -1;
    __u8 *broadcast_dst_mac;
    pon_vlan * vlan_data = NULL;
    original_tag otag = {0};
    int downMatchFlag = 0;
    unsigned char linkMode=0;
    int default_portIndex=0;
    int default_offset=0;

    if(skb == NULL)
    {
        PONVLAN_PRINT(PONVLAN_MSG_ERR,"skb is NULL");
        return -1;
    }
    direction = get_stream_direction(skb->pon_vlan_flag);

    if(direction == UNKNOWN_STEAM)
    {
        PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"unknown the stream direction");
        return 0;
    }
    
    vlan_data = get_vlan_opt_data(skb);
       
    if(vlan_data == NULL)
    {
       PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"no rule for this packet,discard packet");
       return -1;
    }
    else
    {
       PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"Find Vlan rule in port %d",vlan_data->port_index);
    }


    otag.tag_num = skb->pon_tag_num;
    if(skb->pon_tag_num > 0)
    {
       otag.inner_tpid = skb->pon_vlan_tpid[0];
       otag.inner_tci = skb->pon_vlan_tci[0];
    }
    if(skb->pon_tag_num > 1)
    {
       otag.outer_tpid= skb->pon_vlan_tpid[1];
       otag.outer_tci = skb->pon_vlan_tci[1];
    }

	/*get default pon vlan*/
    ECNT_API_XPON_MODE_GET(&linkMode);
	if(linkMode == 2) {//2=PON_LINK_STATUS_EPON
		default_portIndex = PONVLAN_PORT_OFFSET_DEFAULT; 
		default_offset = PON_VLAN_OFFSET_DEFAULT_EPON;
	}
	else if(linkMode == 1){ //1=PON_LINK_STATUS_GPON
		default_portIndex = PONVLAN_PORT_OFFSET_DEFGPON;
		default_offset = PON_VLAN_OFFSET_DEFAULT_GPON;
	}
	
    /* sfu voip tx include in PON_PKT_FROM_CPE */
    if(direction == UPSTREAM)
    {
        PONVLAN_PRINT(PONVLAN_MSG_TRACE,"upstream print,tag num is %d",skb->pon_tag_num);
        
        if((pon_vlan_all_data.veip_enable_flag == DISABLE) && (pon_vlan_all_data.onu_mode == MODE_HGU))
        {
        	if((skb->pon_vlan_flag & PON_PKT_FROM_HYBRID_VEIP)  // hybrid hgu port
				|| (hy_enable == 0 && (skb->pon_vlan_flag & PON_VLAN_TX_CALL_HOOK)))  //pure hgu
        	{
	            PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"upstream print, VEIP Disable, HGU Mode, TX_CALL_HOOK");
	            return 1;
        	}
        }

        ret = change_pkt_by_match_rule(skb, direction, vlan_data, otag);
        if(ret != 0)
        {
            PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"upstream print, match the rule return value=%d",ret);
            return ret;
        }
        PONVLAN_PRINT(PONVLAN_MSG_WARNING,"No Match rule, use default ====>Insert Tag");
        if(vlan_data->port_index != default_portIndex && vlan_data->enable_default_rule)
        {
            vlan_data = &pon_vlan_all_data.pon_vlan_data[default_offset];
            ret = change_pkt_by_match_rule(skb, direction, vlan_data, otag);
            if(ret != 0)
            {
                PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"upstream print, match default rule return value=%d",ret);
                return ret;
            }
        }
        PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"upstream print,no rule match");
        return -1;
    }
    else if(direction == DOWNSTREAM)
    {
        PONVLAN_PRINT(PONVLAN_MSG_TRACE,"downstream print, tag num is %d",skb->pon_tag_num);
        
        if((pon_vlan_all_data.veip_enable_flag == DISABLE) && (pon_vlan_all_data.onu_mode == MODE_HGU)) 
        {
        	if((hy_enable == 1 && !(skb->pon_vlan_flag & PON_PKT_FROM_HYBRID_SFU_WAN))  // hybrid hgu port
				|| (hy_enable == 0 && (skb->pon_vlan_flag & PON_VLAN_RX_CALL_HOOK)))  //pure hgu
        	{
	            PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"downstream print, VEIP Disable, HGU Mode, PON_VLAN_RX_CALL_HOOK");
	            return 1;
        	}
        }

        if(hy_enable == 1 && skb->pon_tag_num == 0 && !(skb->pon_vlan_flag & PON_PKT_FROM_HYBRID_SFU_WAN))
        {
            PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"downstream print, Hybrid VEIP untag pkt pass");
            return 1;
        }
        
        if(vlan_data->down_stream_mode == TRANSPARENT_MODE)
        {
            PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"downstream print,in transparent mode");
            return 1;
        }
        
        if(vlan_data->down_stream_mode == STRIPPED_MODE)
        {
            PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"downstream print,in stripped mode");
            if(skb->pon_tag_num > 0)
                skb->pon_tag_num--;
            return 1;
        }

		if(vlan_data->down_stream_mode == DOWNSTEAM_MODE_8)
        {
            PONVLAN_PRINT(PONVLAN_MSG_WARNING,"Downstream discard all downstream packet,in DOWNSTEAM_MODE_8 mode\n"); 
            return -1;
        }

        if(vlan_data->mac_bind_vlan_enable == 1)
        {

#ifndef TCSUPPORT_BBF_247_VAL
            broadcast_dst_mac = skb->data;
            if(checkBroadcastPkt(broadcast_dst_mac) == 1){
                *downstream_broadcast_flag = 1;
            }
#endif
            //unicast
            iMatchIndex = get_uprule_index_by_mac(skb, vlan_data);
            if(iMatchIndex > -1 && iMatchIndex < vlan_data->down_rule_count)
            {
                downMatchFlag = match_rule(skb, vlan_data, direction,iMatchIndex);
                if(downMatchFlag != 0){
                    if(do_option(skb, vlan_data, direction, iMatchIndex, otag, downMatchFlag) == -1)
                    {
                        PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Do Option Error,drop packet");
                        return -1;
                    }
                    else
                        return 1;
                }
                else
                {
                    ret = change_pkt_by_match_rule(skb, direction, vlan_data, otag);
                    if(ret != 0)
                    {
                        PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"downstream print, match the rule return value=%d",ret);
                        return ret;
                    }
                }
            }
            else
            {
                ret = change_pkt_by_match_rule(skb, direction, vlan_data, otag);
                if(ret != 0)
                {
                    PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"downstream print, match the rule return value=%d",ret);
                    return ret;
                }
            }
        }
        else
        {
            ret = change_pkt_by_match_rule(skb, direction, vlan_data, otag);
            if(ret != 0)
            {
                PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"downstream print, match the rule return value=%d",ret);
                return ret;
            }
        }
        if(TCSUPPORT_CHS_VAL && vlan_data->down_rule_count > 0){
            PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"TCSUPPORT_CHS no specify rule match the packet,drop packet");
            return -1;
        }
		
        if(TCSUPPORT_BBF_247_VAL)
		{/* 247²âÊÔÓÃÀý6.4 */
	        broadcast_dst_mac = skb->data;;
	        if(checkBroadcastPkt(broadcast_dst_mac) == 1){
	            PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"TCSUPPORT_BBF_247_VAL, packet is broadcast,drop packet");
	            return -1;
	        }
        }

		if (vlan_data->downstream_unmatch_oper == DOWNSTREAM_MODE_UNMATCH_FORWARD)
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"Downstream no rule match the packet,forward packet\n");
			return 1;
		}
		else 
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Downstream no rule match the packet,drop packet\n");
        	return -1;
		}  
    }
    return 0;
}
int is_trace_skb(struct sk_buff *skb)
{
    int match_result = 1;
    int i = 0;
    __u16 *tmp = NULL;

    if(skb == NULL || trace_pkt_info.check_mark == 0)
        return 0;

    tmp = (__u16 *)(skb->data+12);

    for(i = 0; i < CHECK_MARK_NUM; i++)
    {
        switch(trace_pkt_info.check_mark & (1 << i))
        {
            case CHECK_DST_MAC:
                if(memcmp(skb->data, trace_pkt_info.dst_mac, 6) == 0)
                    match_result &= 1;
                else
                    match_result &= 0;
                break;
            case CHECK_SRC_MAC:
                if(memcmp(skb->data+6, trace_pkt_info.src_mac, 6) == 0)
                    match_result &= 1;
                else
                    match_result &= 0;
                break;
            case CHECK_OUTER_VID:
                if(is_vlan(tmp) && *(++tmp) == htons(trace_pkt_info.outer_vid))
                    match_result &= 1;
                else
                    match_result &= 0;
                break;
            case CHECK_INNER_VID:
                if(is_vlan(tmp) && *(++tmp) == htons(trace_pkt_info.inner_vid))
                    match_result &= 1;
                else
                    match_result &= 0;
                break;
            default:
                break;
        }
    }
    //printk("----THIS TRACING PACKET IS -------%s\n", match_result?"MATCHED":"NOT MATCHED");
    return match_result;
}

void dump_skb(struct sk_buff *skb)
{
	char tmp[80];
	char *p = skb->data;
	char *t = tmp;
	int i, n = 0;

	printk("ERR skb=%08lx skb->head=%08lx \n data=%08lx len=%d mark=%08x\n cb=%08lx \n", 
	    (unsigned long) skb, (unsigned long) skb->head, (unsigned long) skb->data, skb->len, skb->mark, 
	    *(unsigned long int *)(skb->cb));
    printk("skb dev %s, orig dev %s\n", skb->dev->name, skb->original_dev->name);
	for (i = 0; i < skb->data_len; i++) {  
		t += sprintf(t, "%02x ", *p++ & 0xff);
		if ((i & 0x0f) == 0x0f) {
			printk("%04x: %s\n", n, tmp);
			n += 16;
			t = tmp;
		}
	}
	if (i & 0x0f)
		printk("%04x: %s\n", n, tmp);
}

int is_to_or_from_br0(struct sk_buff *skb)
{
	struct net_device *device = NULL;
	unsigned char *dest = eth_hdr(skb)->h_dest;
	unsigned char *src = eth_hdr(skb)->h_source;
	unsigned char bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};

#if KERNEL_2_6_36
	device = dev_get_by_name(&init_net,"br0");
#else
	device = dev_get_by_name("br0");
#endif

	if(device == NULL || hy_enable == 0)
		return 0;

	if(skb->dev->name[0] == 'r' || skb->dev->name[0] == 'u' || skb->dev->name[0] == 'e'  || skb->dev->name[0] == 'b')
	{
		if((memcmp(dest, device->dev_addr, 6) == 0) && (skb->pon_vlan_flag & PON_PKT_FROM_HYBRID_VEIP)){
			PONVLAN_PRINT(PONVLAN_MSG_WARNING|PON_PKT_TRACE_FLAG,"hgu port to br0");
			return 1;
		}
		else if((memcmp(dest, bcast, 6) == 0)  &&  (skb->pon_vlan_flag & PON_PKT_FROM_HYBRID_VEIP)){
			PONVLAN_PRINT(PONVLAN_MSG_WARNING|PON_PKT_TRACE_FLAG,"hgu port to broadcast");
			return 1;
		}
		else if( (memcmp(src, device->dev_addr, 6) == 0)  &&  !(skb->pon_vlan_flag & PON_VLAN_RX_CALL_HOOK)){
			PONVLAN_PRINT(PONVLAN_MSG_WARNING|PON_PKT_TRACE_FLAG,"br0 to hgu port");
			return 1;
		}
	}

	return 0;
}

int pon_insert_tag(struct sk_buff **pskb)
{
    struct sk_buff *skb = NULL;
    struct sk_buff *skb_tmp = NULL;
    struct net_device *out_dev = NULL;
    int i;
    int ret = 0;
    int ds_broadcast_flag = 0;
    
    if(pskb == NULL)
    {
        PONVLAN_PRINT(PONVLAN_MSG_ERR|PON_PKT_TRACE_FLAG,"pskb is NULL pointer,return -1 ====> pon insert tag");
        return -1;
    }
    if(*pskb == NULL)
    {
        PONVLAN_PRINT(PONVLAN_MSG_ERR|PON_PKT_TRACE_FLAG,"pskb is NULL pointer,return -1 ====> pon insert Tag");
        return -1;
    }
    skb = *pskb;
    out_dev = skb->dev;
    PONVLAN_PRINT(PONVLAN_MSG_TRACE,"Skb flag is %x, out_dev is %s  hy_enable=%d ",skb->pon_vlan_flag,out_dev->name,hy_enable);

    skb->pon_vlan_flag |= (is_trace_skb(skb)?PON_PKT_TRACE_FLAG:0);

    if(pon_vlan_all_data.vlan_enable_flag == 0) {//disable vlan function
        if (pon_vlan_all_data.onu_mode == MODE_HGU){
            PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag," vlan disable and onu type is HGU");
            return 0; /* modify by sun.zhu */
        }else{
            PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"vlan disable and onu type is SFU ");
            goto Insert_Tag;
        }
    }

    
    if(skb->pon_vlan_flag & PON_LEAVE_PKT_DEAL)
    {
        return 0;
    }

	if(is_to_or_from_br0(skb)){
		return 0;
	}

    if(skb->pon_vlan_flag & PON_PKT_SEND_TO_WAN)//gpon sent to wan,do vlan filter here
    {
        PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"LAN->WAN vlan filter goto");
        goto VLAN_Filter;
    }
    
    if(skb->pon_vlan_flag & PON_PKT_INSERT_FLAG)
    {
        PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"frame tag has been handle,just return 0");
        return 0;
    }
    

	if((DBG_Level & PONVLAN_DROP_DUMP_UP) && 
		(skb->pon_vlan_flag & (PON_VLAN_TX_CALL_HOOK | PON_PKT_FROM_CPE | PON_PKT_FROM_LAN | PON_PKT_VOIP_TX | PON_PKT_FROM_HYBRID_VEIP | PON_PKT_FROM_HYBRID_PPTP))){
		printk("[%s][%d] flag:%x,mark:%x\n",__FUNCTION__, __LINE__, skb->pon_vlan_flag, skb->mark);
		dump_skb(skb);
	}
	
	if((DBG_Level & PONVLAN_DROP_DUMP_DOWN) && 
		(skb->pon_vlan_flag & (PON_VLAN_RX_CALL_HOOK | PON_PKT_FROM_WAN | PON_PKT_VOIP_RX))){
		printk("[%s][%d] flag:%x,mark:%x\n",__FUNCTION__, __LINE__, skb->pon_vlan_flag, skb->mark);
		dump_skb(skb);
	}

    if(is_valid_tx_rx_pkt(skb->pon_vlan_flag, out_dev)){
        PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"valid pkt");
		if(hy_enable == 1  && !memcmp(out_dev->name,"pon",3) && skb->pon_vlan_flag & PON_VLAN_RX_CALL_HOOK)
		{
			//do not set INSERT_FLAG, and do not execute downstream uni(pptp/veip) vlan tag operation
		}
		else
        	skb->pon_vlan_flag |= PON_PKT_INSERT_FLAG;
    }
    else{
        PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"invalid tx rx pkt ");
        return 0;
    }

    if(is_cpe2lan_pkt(skb->pon_vlan_flag, out_dev))// cpe2lan direction.do nothing
    {
        PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"pkt lan2lan goto insert tag");
        goto Insert_Tag;
    }

    /* ipoa, no mac header, don't should insert vlan tag,it shouldn't be here anyway*/
    if (skb->data == skb_network_header(skb)) { 
        PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"ipoa pkt, not handle here");
        return 0;
    }

    skb = skb_unshare(*pskb, GFP_ATOMIC);
    *pskb = skb;
    if (skb == NULL)
    {
        PONVLAN_PRINT(PONVLAN_MSG_ERR,"skb copy error ====>Insert Tag");
        return -1;
    }

    skb_tmp = skb_copy(skb, GFP_ATOMIC);

	if (TCSUPPORT_RA_HWNAT_VAL && ra_sw_nat_hook_xfer  && (skb_tmp != NULL)){
		ra_sw_nat_hook_xfer(skb, skb_tmp);
    }

    if(((skb->pon_vlan_flag & PON_VLAN_RX_CALL_HOOK) && pon_vlan_all_data.onu_mode == MODE_HGU && !memcmp(out_dev->name,"pon",3))  //in pon dev,downstream
		|| (hy_enable==1 && !memcmp(out_dev->name,"eth",3) && !(skb->pon_vlan_flag & PON_VLAN_RX_CALL_HOOK))) //hybrid upstream, not pon dev
	
	{	
		skb->data -= 14;

		/* hgu downstream multicast vlan handle*/
		if(xpon_hgu_down_multicast_vlan_tci_hook != NULL)
		{		
			unsigned char * macDa =  skb_mac_header(skb);
			struct vlan_ethhdr *vhethdr = (struct vlan_ethhdr *)(skb->data);
			unsigned short etherType = ntohs(vhethdr->h_vlan_proto);
			if((NULL != macDa) && (0x01 == macDa[0]) && (0x00 == macDa[1]) && (0x5e == macDa[2])&&(0x8100 == etherType))
			{
				ret = xpon_hgu_down_multicast_vlan_tci_hook(skb);
				if(1 == ret){
					/*In G.988 9.3.27, if Downstream IGMP and multicast  TCI first byte is non-zero,
						extend VLAN tagging operation ME is ignored, so return 0 directly.*/
					skb->data += 14;
					return 0;
					//goto Insert_Tag;
				}
			}
		}
    }
	
    //in some bridge case,frame will not run the vlan_recv.store vlan info here.
    if(store_tag_info(skb) == -1)
    {
        PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Store tag info error when bridge mode.return -1. ====> Pon insert tag");
        goto Fail;
    }
    if(TCSUPPORT_PON_VLAN_FILTER_VAL && (ret = pon_vlan_filter(skb)) == -1){
        PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"pon vlan filter fail");
        goto Fail;
    }

    if(hy_enable ==1  && !memcmp(out_dev->name,"pon",3) && skb->pon_vlan_flag & PON_VLAN_RX_CALL_HOOK) //hybrid downstream in pon dev
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"hybrid downstream in pon dev only do filter pon_vlan_flag=%x\n",skb->pon_vlan_flag);
		goto Insert_Tag;
    }

    ret = handle_igmp_pkt(skb);

    if(ret == -1){
        PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"handle igmp pkt fail");
        goto Fail;
    }else if(ret == 1){
        PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"handle igmp pkt success");
        goto Insert_Tag;
    }

    if(is_lan2lan_pkt(skb->pon_vlan_flag, out_dev))//lan2lan should do upstream vlan operations according the source port.
    {
        skb->dev = skb->original_dev;
    }

    ret = handle_common_pkt(skb, &ds_broadcast_flag);

    if(ret == -1){
        PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"handle common pkt fail");
        goto Fail;
    }else if(ret == 1){
        if(is_lan2lan_pkt(skb->pon_vlan_flag, out_dev))//lan2lan should do downstream vlan operation according the dest port.
        {
            skb->pon_vlan_flag = (skb->pon_vlan_flag & (~PON_PKT_FROM_LAN));
            skb->pon_vlan_flag |= PON_PKT_FROM_WAN;
            skb->dev = out_dev;
            PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"pkt lan2lan double treat.");
            ret = handle_common_pkt(skb, &ds_broadcast_flag);

            if(ret == -1){
                PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"handle common lan2lan pkt fail");
                goto Fail;
            }else if(ret == 1){
                PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"handle common lan2lan pkt success");
                goto Insert_Tag;
            }

        }
        else{
            PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"handle common pkt success");
            goto Insert_Tag;
        }
    }

VLAN_Filter:
    if( TCSUPPORT_PON_VLAN_FILTER_VAL && (ret = pon_vlan_filter(skb)) == -1){
        PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"pon vlan filter fail");
        goto Fail;
    }
    if(skb->pon_vlan_flag & PON_PKT_SEND_TO_WAN){
        PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"pon vlan filter success LAN->WAN");
        if(skb_tmp != NULL){
            kfree_skb(skb_tmp);
    		skb_tmp = NULL;
        }
        return 0;
    }


Insert_Tag:
    if(ds_broadcast_flag == 1 && should_send_broadcast_by_multi_rules(skb_tmp))
        send_broadcast_by_multi_rules(skb_tmp);
    if(skb_tmp != NULL){
        kfree_skb(skb_tmp);
		skb_tmp = NULL;
    }

    for(i = 0; i < skb->pon_tag_num; i++)
    {
        PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"In the Pon Insert Tag,vlan tag index is %d,tpid is %x,tci is %x",i,skb->pon_vlan_tpid[i],skb->pon_vlan_tci[i]);
        skb = __pon_vlan_put_tag(skb, skb->pon_vlan_tpid[i], skb->pon_vlan_tci[i]);
        if (skb)
            skb->dev = out_dev;
        else{
            PONVLAN_PRINT(PONVLAN_MSG_ERR,"skb is NULL,we will discard packet");
            return -1;
        }
    }
    
    if( skb->pon_vlan_flag & PON_CLASSIFICATION_REMARK ) 
    {
        skb = eponVlanPriRemark(skb);
        if(skb == NULL){
            PONVLAN_PRINT(PONVLAN_MSG_ERR,"skb is NULL,we will discard packet");
            return -1;
        }else{ 
            *pskb = skb;/*pskb point to skb which was modified*/
        }
    }
    
    if(((skb->pon_vlan_flag & PON_VLAN_RX_CALL_HOOK) && pon_vlan_all_data.onu_mode == MODE_HGU && !memcmp(out_dev->name,"pon",3)) //hgu, downstream
		|| (hy_enable==1 && !memcmp(out_dev->name,"eth",3) && !(skb->pon_vlan_flag & PON_VLAN_RX_CALL_HOOK))) // hybrid, not pon dev
	
    {
        if(DBG_Level > 1)
        {
            printk("skb info is");
            for(i = 0; i < 14; i++)
                printk("%02x",skb->data[i]);
        }
        skb->data += 12;
        skb->protocol = *(__u16 *)(skb->data);
        PONVLAN_PRINT(PONVLAN_MSG_WARNING|skb->pon_vlan_flag,"In the Pon Insert Tag,HGU ds print,protocol is  %x,skb data is %02x",skb->protocol,skb->data[0]);
        skb->data += 2;
    }
    
	if((DBG_Level & PONVLAN_DROP_DUMP_UP) && 
		(skb->pon_vlan_flag & (PON_VLAN_TX_CALL_HOOK | PON_PKT_FROM_CPE | PON_PKT_FROM_LAN | PON_PKT_VOIP_TX))){
		printk("[%s][%d] flag:%x,mark:%x\n",__FUNCTION__, __LINE__, skb->pon_vlan_flag, skb->mark);
		dump_skb(skb);
	}
	
	if((DBG_Level & PONVLAN_DROP_DUMP_DOWN) && 
		(skb->pon_vlan_flag & (PON_VLAN_RX_CALL_HOOK | PON_PKT_FROM_WAN | PON_PKT_VOIP_RX))){
		printk("[%s][%d] flag:%x,mark:%x\n",__FUNCTION__, __LINE__, skb->pon_vlan_flag, skb->mark);
		dump_skb(skb);
	}
    return 0;

Fail:
#if defined(TCSUPPORT_RA_HWNAT) && defined(TCSUPPORT_RA_HWNAT_ENHANCE_HOOK)
    if(DBG_Level & PONVLAN_DROP_DUMP_UP || DBG_Level & PONVLAN_DROP_DUMP_DOWN){
        dump_skb(skb);
    }
    if(ra_sw_nat_hook_drop_packet){
        ra_sw_nat_hook_drop_packet(skb);
    }
#endif
    if(skb_tmp != NULL)
        kfree_skb(skb_tmp);
    PONVLAN_PRINT(PONVLAN_MSG_ERR|skb->pon_vlan_flag,"Insert Tag Error,we will discard packet");
    return -1;
}


int pon_vlan_get_mode(void)
{
	return pon_vlan_all_data.onu_mode;
}

/*
	check the skb data, if skb->pon_tag_num is 0.
*/
int pon_check_untag(struct sk_buff *skb)
{
	struct vlan_ethhdr *vhethdr = NULL;
	struct vlan_hdr *vhdr = NULL;
	unsigned short vheth_tpid = 0;
	unsigned short vh_tpid = 0;
	int j=0;

	if(gponVlanFilterDbgFlag >=  GPON_VLAN_FILTER_DEBUG_LEVEL_ERROR)
		dump_skb(skb);

	if(skb == NULL)
	{
		printk("\r\n[%s][%d]skb is NULL pointer,return -1.", __FUNCTION__, __LINE__);
		return -1;
	}

	vhethdr = (struct vlan_ethhdr *)(skb->data);
	vheth_tpid = ntohs(vhethdr->h_vlan_proto);
	if((vheth_tpid== 0x8100) ||(vheth_tpid == 0x88a8)||(vheth_tpid == 0x9100))
	{
		skb->pon_vlan_tci[j] = ntohs(vhethdr->h_vlan_TCI);
		skb->pon_vlan_tpid[j] = vheth_tpid;
		skb->pon_tag_num++;
		j++;
		vhdr = (struct vlan_hdr *)(skb->data + 14);
		vh_tpid = ntohs(vhdr->h_vlan_encapsulated_proto);
		while(((vh_tpid == 0x8100)||(vh_tpid == 0x88a8)||(vh_tpid == 0x9100))&&(j < 4)){
			skb->pon_vlan_tci[j] = ntohs(*(u16 *)(vhdr+1));
			skb->pon_vlan_tpid[j] = vh_tpid;
			skb->pon_tag_num++;
			j++;
			vhdr++;
		}
		/* Take off the VLAN header (4 bytes currently) */
	}

	return 0;
}


/*
	Store the tag to skb when we recv a packet from lan or wan.
	We will add the tag when xmit the packet.
*/
int pon_store_vlan_tag(struct sk_buff *skb, struct net_device *orig_dev)
{
	struct vlan_hdr *vhdr = NULL;
	u16 proto;
	unsigned char * tmp = NULL;
	int j = 0;

	if(pon_vlan_all_data.onu_mode == MODE_HGU)
		return 1;
	
	if(skb == NULL)
	{
		printk("\r\nskb is NULL pointer,return -1 ====> pon store tag");
		return -1;
	}
	if(orig_dev == NULL)
	{
		printk("\r\norig_dev is NULL pointer,return -1 ====> pon store Tag");
		return -1;
	}
	
	vhdr = (struct vlan_hdr *)(skb->data);
	tmp = skb->data - 2;

	if((orig_dev != NULL) && ((orig_dev->name[0] == 'b') || (orig_dev->name[0] == 'n')))
	{
		proto = vhdr->h_vlan_encapsulated_proto;
		skb->protocol = proto;

		if(skb->pon_tag_num > 0)
		{
			for(j = skb->pon_tag_num; j > 0; j--)
			{
				skb->pon_vlan_tpid[j] = skb->pon_vlan_tpid[j - 1];
				skb->pon_vlan_tci[j] = skb->pon_vlan_tci[j - 1];
			}
		}
		skb->pon_vlan_tci[0] = vhdr->h_vlan_TCI;
		skb->pon_vlan_tpid[0] = *(u16 *)tmp;
		skb->pon_tag_num++;
		/* Take off the VLAN header (4 bytes currently) */
		skb_pull_rcsum(skb, VLAN_HLEN);
		skb->dev = orig_dev;
	}
	else
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"orig_dev %s error====>pon store vlan tag",orig_dev->name);
		return -1;
	}
	return 0;
}

static inline int convert_dev_name_to_index(char * name)
{
	int i = 0;
	
	if(name == NULL)
	{
		printk("\r\nname is NULL ====>convert dev name");
		return -1;
	}

	if(strncmp(name,"nas",3) == 0 && name[5] == '_')
	{
		//dev name is like nasx_y,and the index of this entry is 1x*8+y
		i = (name[3] - '0')*10 + (name[4] - '0');
		i = (i - 13) * 8 + (name[6] - '0');
		if(i < 0 || i > 63)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"dev name error====>convert dev name");
			PONVLAN_PRINT(PONVLAN_MSG_TRACE,"dev name is %s,index is %d ====>convert dev name",name,i);
			return -1;
		}
		PONVLAN_PRINT(PONVLAN_MSG_TRACE,"dev name is %s,index is %d ====>convert dev name",name,i);
		return i;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"dev name error error====>convert dev name");
	return -1;
}

int pon_check_vlan_tag(struct net_device *dev, struct sk_buff *skb)
{
	int i = 0;
	char * name = dev->name;
	__u16 * tmp = NULL;
	int vid = 0;

	if(pon_vlan_all_data.if_vlan_bind_enable_flag == DISABLE)
		return 1;
	
	if(skb == NULL)
	{
		printk("\r\nskb is NULL ====>pon check vlan tag");
		return -1;
	}
	
	if(dev == NULL)
	{
		printk("\r\ndev is NULL ====>pon check vlan tag");
		return -1;
	}
	if(strcmp(name,"pon") == 0)
		return 1;
	
	i = convert_dev_name_to_index(name);
	if(i > -1 && i < 64)
	{
		tmp = (__u16 *)skb->data;
		tmp += 6;
		if(!is_vlan(tmp))
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"untag frame,discard anyway====>pon check vlan tag");
			return 0;
		}
		tmp++;
		vid = getVID(*tmp);

		if(pon_vlan_all_data.pon_if_vlan_pair_data[i] == vid || pon_vlan_all_data.pon_if_vlan_pair_data[i] == IF_VLAN_TRANSPARENT)
			return 1;
	}
	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"check vlan tag error====>pon check vlan tag");
	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"dev name is %s,i is %d,paramater vid is %d",name,i,vid);
	return 0;
}

int pon_check_user_group(struct sk_buff *skb)
{
	int i = 0,port = 0,flag = 0;
	__u32 group1 = 0,group2 = 0;
	struct net_device *out_dev = NULL;

	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"do user group check,out dev is %s",out_dev->name);
	
	if(pon_vlan_all_data.user_group_enable_flag == DISABLE || pon_vlan_all_data.onu_mode == MODE_HGU)//HGU only have 1 UNI port.
		return 1;

	if(skb == NULL)
		return -1;

	out_dev = skb->dev;
	
	if(!(skb->pon_vlan_flag & PON_PKT_FROM_LAN) || (skb->pon_vlan_flag & PON_USER_GROUP_FLAG))//only check lan port.
		return 1;

	if(out_dev->name[0] == 'e' || out_dev->name[0] == 'r' || out_dev->name[0] == 'u')
		skb->pon_vlan_flag |= PON_USER_GROUP_FLAG;
	else
		return 1;
		

	port = get_port_num(skb);
	for(i = 1; i < pon_vlan_all_data.total_port_count; i++)
	{
		if(pon_vlan_all_data.pon_vlan_data[i].port_index == port)
		{
			group1 = pon_vlan_all_data.pon_vlan_data[i].user_group;
			flag = 1;
		}
	}

	if(flag != 1)
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"check org lan port error====>pon check user group");
		goto Fail;
	}

	switch(out_dev->name[0])
	{
		case 'e':
			if(pon_vlan_all_data.lan_port_count == 1){	//single port case.return lan port 1
				port = 11;
			}else if(out_dev->name[4] == '.'){
					port = (10 + (out_dev->name[5] - '0'));
			}else{
					goto Fail;
			}
			break;

		case 'r':
			#ifdef TCSUPPORT_WLAN_AC  //debug by fred.li
			if ('i' == out_dev->name[2] )
				{
					port = (25 + (out_dev->name[3] - '0'));
					break;
				}else						
			#endif
			port = (21 + (out_dev->name[2] - '0'));
			break;


		case 'u':
			port = (30 + (out_dev->name[3] - '0'));
			break;

		default:
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"can't find send port====>pon check user group");
			goto Fail;
	}

	flag = 0;
	for(i = 1; i < pon_vlan_all_data.total_port_count; i++)
	{
		if(pon_vlan_all_data.pon_vlan_data[i].port_index == port)
		{
			group2 = pon_vlan_all_data.pon_vlan_data[i].user_group;
			flag = 1;
		}
	}

	if(flag != 1)
	{
		if(DBG_Level > 0)
			printk("\r\ncheck send lan port error====>pon check user group");
		goto Fail;
	}

	if(group1 == 0xffffffff && group2 == 0xffffffff)
		return 1;
	else
	{
		if(group1 == 0xffffffff || group2 == 0xffffffff)
			goto Fail;

		if(group1 == group2)
			return 1;
		else
			goto Fail;
	}
	
Fail:
	kfree_skb(skb);
	//skb = NULL;
	return -1;
}


static void pcp_decode(__u16 * tci, __u16 mode)
{
	__u8 pbit = 0,dei = 0;

	if(mode == 0)
		return;
	
	pbit = getPbit(*tci);
	switch(mode)
	{
		case 3:
			if(pbit == 0)
				dei = 1;
			if(pbit == 1)
				pbit--;
			break;
		case 2:
			if(pbit == 2)
				dei = 1;
			if(pbit == 3)
				pbit--;
			break;
		case 1:
			if(pbit == 4)
				dei = 1;
			if(pbit == 5)
				pbit--;
			break;
		default:
			break;
	}

	setPbit(*tci,pbit);
	setDEI(*tci,dei);
	return;
}


int pon_pcp_decode(struct sk_buff **pskb)
{
	struct sk_buff *skb = NULL;
	struct net_device *out_dev = NULL;
	
	if(pskb == NULL)
	{
		printk("\r\npskb is NULL pointer,return -1 ====> pon PCP decode");
		return -1;
	}
	if(*pskb == NULL)
	{
		printk("\r\npskb is NULL pointer,return -1 ====> pon PCP decode");
		return -1;
	}
	skb = *pskb;
	out_dev = skb->dev;
	if(strcmp(out_dev->name,"pon") != 0)	
	{
		PONVLAN_PRINT(PONVLAN_MSG_TRACE,"out dev is not wan,do nothing here");
		return 0;
	}
	
	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"before decode,tci is %x====>Insert Tag",(*((__u16*)(skb->data + 14))));

	if(skb->pon_tag_num != 0)
	{
		pcp_decode(&(skb->pon_vlan_tci[skb->pon_tag_num - 1]),pon_vlan_all_data.pcp_mode);
		pcp_decode((__u16*)(skb->data + 14),pon_vlan_all_data.pcp_mode);
	}
	
	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"after decode,tci is %x====>Insert Tag",(*((__u16*)(skb->data + 14))));
	return 0;
}

static int pon_vlan_switch_option(pon_vlan_ioctl * data,void * arg)
{
	if(data->option_flag == OPT_GET)
	{
		data->vlan_enable = pon_vlan_all_data.vlan_enable_flag;
		if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
		{
			printk("\r\ncopy to user error ====> pon vlan Switch opt");
			return -1;
		}
		return 0;
	}
	else if(data->option_flag == OPT_SET)
	{
		pon_vlan_all_data.vlan_enable_flag = data->vlan_enable;
		return 0;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan Switch opt");
	return -1;
}

static int pon_vlan_veip_switch_option(pon_vlan_ioctl * data,void * arg)
{
	if(data->option_flag == OPT_GET)
	{
		data->veip_vlan_enable = pon_vlan_all_data.veip_enable_flag;
		if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
		{
			printk("\r\ncopy to user error ====> pon vlan Switch opt");
			return -1;
		}
		return 0;
	}
	else if(data->option_flag == OPT_SET)
	{
		pon_vlan_all_data.veip_enable_flag = data->veip_vlan_enable;
		return 0;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan veip Switch opt");
	return -1;
}

static int pon_vlan_downstream_mode_option(pon_vlan_ioctl * data,void * arg)
{
	pon_vlan * tmp = NULL;

	if(data->option_flag == OPT_GET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			data->downstream_mode = tmp->down_stream_mode;
			if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan downstream mode opt");
				return -1;
			}
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Get Downstream mode errror,can't find port ====> pon vlan downstream mode opt");
		return -1;
	}
	else if(data->option_flag == OPT_SET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			tmp->down_stream_mode = data->downstream_mode;
			pon_vlan_set_downstream_mode_mask(tmp);
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set Downstream mode error,can't find port ====> pon vlan downstream mode opt");
		return -1;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan downstream mode opt");
	return -1;
}


static int pon_vlan_downstream_forward_option(pon_vlan_ioctl * data,void * arg)
{
	pon_vlan * tmp = NULL;

	if ((NULL == data) || (NULL == arg))
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"pon vlan downstream forward opt data or arg is NULL\n");
		return -1;
	}

	if(data->option_flag == OPT_GET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			data->downstream_unmatch_oper = tmp->downstream_unmatch_oper;
			if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan downstream forward opt\n");
				return -1;
			}
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Get Downstream forward error,can't find port ====> pon vlan downstream forward opt\n");
		return -1;
	}
	else if(data->option_flag == OPT_SET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			tmp->downstream_unmatch_oper = data->downstream_unmatch_oper;
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set Downstream forward error,can't find port ====> pon vlan downstream forward opt\n");
		return -1;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan downstream forward opt\n");
	return -1;
}


static int pon_vlan_tpid_option(pon_vlan_ioctl * data,void * arg)
{
	pon_vlan * tmp = NULL;

	if(data->option_flag == OPT_GET)
	{
		if(data->tpid_type == 0)
		{
			//donothing here
		}
		else if(data->tpid_type == 1)
		{
			tmp = get_pon_vlan_by_port(data->port);
			if(tmp != NULL)
			{
				data->input_tpid = tmp->input_tpid;
				data->output_tpid = tmp->output_tpid;
				if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan tpid opt");
					return -1;
				}
				return 0;
			}
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"get TPID error,can't find port ====> pon vlan tpid opt");
			return -1;
		}
		else
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"get TPID error,can't find port ====> pon vlan tpid opt");
			return -1;
		}
	}
	else if(data->option_flag == OPT_SET)
	{
		if(data->tpid_type == 0)
		{
			if(pon_check_pack(data->special_tpid))
			{
				return add_tpid(data->special_tpid);
			}
			else
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set TPID 0x%x error,TPID has been used ====> pon vlan tpid opt",data->special_tpid);
				return -1;
			}
		}
		else if(data->tpid_type == 1)
		{
			tmp = get_pon_vlan_by_port(data->port);
			if(tmp != NULL)
			{
				if(data->input_tpid != 0){ //if tpid is 0 ,not set to pon vlan rule
				tmp->input_tpid = data->input_tpid;
				}
				if(data->output_tpid != 0){
				tmp->output_tpid = data->output_tpid;
				}
				return 0;
			}
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set TPID error,can't find port ====> pon vlan tpid opt");
			return -1;
		}
		else
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set TPID error,tpid type error ====> pon vlan tpid opt");
			return -1;
		}
	}
	else if(data->option_flag == OPT_DEL)
	{
		if(data->tpid_type == 0)
		{
			if(remove_tpid(data->special_tpid) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"remove tpid error ====> pon vlan tpid opt");
				return -1;
			}
			return 0;
		}
		else
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set TPID errror,tpid type error ====> pon vlan tpid opt");
			return -1;
		}
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan tpid opt");
	return -1;
}

static int pon_vlan_default_rule_option(pon_vlan_ioctl * data,void * arg)
{
	pon_vlan * tmp = NULL;

	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"option_flag=%d, port = %d",data->option_flag,data->port);
	if(data->option_flag == OPT_GET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			data->default_vlan_rule_flag = tmp->enable_default_rule;
			if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan default rule opt");
				return -1;
			}
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"get default rule errror,can't find port ====> pon vlan default rule opt");
		return -1;
	}
	else if(data->option_flag == OPT_SET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			tmp->enable_default_rule = data->default_vlan_rule_flag;
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set default rule errror,can't find port ====> pon vlan default rule opt");
		return -1;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan default rule opt");
	return -1;
}

static inline int calc_down_sub_filter(pon_vlan_rule * rule, pon_vlan_sub_rule * subrule, int index)
{
	int tpid = 0,pbit = 0,dei = 0,vid = 0;
	
	if(index == FILTER_FIRST_TAG)
	{
		tpid = rule->add_fst_tpid;
		pbit = rule->add_fst_pri;
		dei = rule->add_fst_dei;
		vid = rule->add_fst_vid;
	}
	else if(index == FILTER_SECONE_TAG)
	{
		tpid = rule->add_sec_tpid;
		pbit = rule->add_sec_pri;
		dei = rule->add_sec_dei;
		vid = rule->add_sec_vid;
	}
	else
		return -1;

	switch(tpid)
	{
		case TREAT_TPID_8100:
			subrule->tpid = FILTER_TPID_8100;
			break;

		case TREAT_TPID_OUTPUT_TPID:
			subrule->tpid = FILTER_TPID_EQUAL_TO_OUTPUT_TPID;
			break;

		case TREAT_TPID_COPY_FROM_INNER:
			if(rule->tag_num < 1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc tpid copy inner error ====> calc down sub filter opt");
				return -1;
			}
			subrule->tpid = rule->filter_inner_tpid;
			break;

		case TREAT_TPID_COPY_FROM_OUTER:
			if(rule->tag_num < 2)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc tpid copy outer error ====> calc down sub filter opt");
				return -1;
			}
			subrule->tpid = rule->filter_outer_tpid;
			break;
			
		default:
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc tpid error ====> calc down sub filter opt");
			return -1;
	}

	switch(pbit)
	{
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
			if(TCSUPPORT_C1_MS_VAL)
				subrule->pri = rule->filter_inner_pri;
			else
				subrule->pri = pbit;
			break;

		case TREAT_PRI_COPY_FROM_INNER:
			if(rule->tag_num < 1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc pbit copy inner error ====> calc down sub filter opt");
				return -1;
			}
			subrule->pri = rule->filter_inner_pri;
			break;

		case TREAT_PRI_COPY_FROM_OUTER:
			if(rule->tag_num < 2)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc pbit copy outer error ====> calc down sub filter opt");
				return -1;
			}
			subrule->pri = rule->filter_outer_pri;
			break;

		case TREAT_PRI_BASED_ON_DSCP:
			subrule->pri = FILTER_PRI_DO_NOT_CARE;
			break;

		default:
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc pbit error ====> calc down sub filter opt");
			return -1;
			break;
	}

	if(dei ==0 || dei == 1)
	{
		subrule->dei = dei;
	}
	else if(dei == TREAT_DEI_COPY_FROM_INNER)
	{
		if(rule->tag_num < 1)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc dei copy inner error ====> calc down sub filter opt");
			return -1;
		}
		subrule->dei = rule->filter_inner_dei;
	}
	else if(dei  == TREAT_DEI_COPY_FROM_OUTER)
	{
		if(rule->tag_num < 2)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc dei copy outer error ====> calc down sub filter opt");
			return -1;
		}
		subrule->dei = rule->filter_outer_dei;
	}
	else
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc DEI error ====> calc down sub filter opt");
		return -1;
	}
    
#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_CDS)
        subrule->tpid = FILTER_TPID_DO_NOT_CARE;
        subrule->dei = FILTER_DEI_DO_NOT_CARE;
#endif/*TCSUPPORT_COMPILE*/

	if(vid >=0 && vid <= 4095)
	{
		subrule->vid = vid;
	}
	else if(vid == TREAT_VID_COPY_FROM_INNER)
	{
		if(rule->tag_num < 1)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc vid copy inner error ====> calc down sub filter opt");
			return -1;
		}
		subrule->vid = rule->filter_inner_vid;
	}
	else if(vid  == TREAT_VID_COPY_FROM_OUTER)
	{
		if(rule->tag_num < 2)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc vid copy outer error ====> calc down sub filter opt");
			return -1;
		}
		subrule->vid = rule->filter_outer_vid;
	}
	else
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc vid error ====> calc down sub filter opt");
		return -1;
	}
	
	return 0;
}

static inline int calc_down_sub_treat(pon_vlan_rule * rule, pon_vlan_sub_rule * subrule, int index)
{
	int tpid = 0,pbit = 0,dei = 0,vid = 0;
	
	if(index == ADD_FIRST_TAG)
	{
		tpid = rule->filter_inner_tpid;
		pbit = rule->filter_inner_pri;
		dei = rule->filter_inner_dei;
		vid = rule->filter_inner_vid;
	}
	else if(index == ADD_SECOND_TAG)
	{
		tpid = rule->filter_outer_tpid;
		pbit = rule->filter_outer_pri;
		dei = rule->filter_outer_dei;
		vid = rule->filter_outer_vid;
	}
	else
		return -1;

	//handle TPID
	if(rule->treatment_method/10 == METHOD_CHANGE_TAG && index == ADD_FIRST_TAG && 
		(rule->filter_inner_tpid == FILTER_TPID_EQUAL_TO_INPUT_TPID || rule->filter_inner_tpid == FILTER_TPID_EQUAL_TO_OUTPUT_TPID))
	{
		if(rule->add_fst_tpid == TREAT_TPID_COPY_FROM_OUTER && rule->tag_num < 2)
			return -1;

		if(rule->filter_inner_tpid == FILTER_TPID_EQUAL_TO_INPUT_TPID)
			subrule->tpid = TREAT_TPID_INPUT_TPID;
		else if(rule->filter_inner_tpid == FILTER_TPID_EQUAL_TO_OUTPUT_TPID)
			subrule->tpid = TREAT_TPID_OUTPUT_TPID;
	}
	else if(rule->treatment_method/10 == METHOD_CHANGE_TAG && index == ADD_SECOND_TAG && 
		(rule->filter_outer_tpid == FILTER_TPID_EQUAL_TO_INPUT_TPID || rule->filter_outer_tpid == FILTER_TPID_EQUAL_TO_OUTPUT_TPID))
	{
		if(rule->tag_num < 2)
			return -1;

		if(rule->filter_outer_tpid == FILTER_TPID_EQUAL_TO_INPUT_TPID)
			subrule->tpid = TREAT_TPID_INPUT_TPID;
		else if(rule->filter_outer_tpid == FILTER_TPID_EQUAL_TO_OUTPUT_TPID)
			subrule->tpid = TREAT_TPID_OUTPUT_TPID;
	}
	else
	{
		switch(tpid)
		{
			case FILTER_TPID_DO_NOT_CARE:
			case FILTER_TPID_8100:
				subrule->tpid = TREAT_TPID_8100;
				break;

			case FILTER_TPID_EQUAL_TO_INPUT_TPID:
				subrule->tpid = TREAT_TPID_INPUT_TPID;
				break;

			default:
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc tpid error ====> calc down sub treat opt");
				return -1;
		}
	}

	//handle Pbit
	if(rule->treatment_method/10 == METHOD_CHANGE_TAG && index == ADD_FIRST_TAG && 
		rule->filter_inner_pri == FILTER_PRI_DO_NOT_CARE)
	{
		if(rule->add_fst_pri == TREAT_PRI_COPY_FROM_OUTER && rule->tag_num < 2)
			return -1;

		subrule->pri = TREAT_PRI_COPY_FROM_INNER;
	}
	else if(rule->treatment_method/10 == METHOD_CHANGE_TAG && index == ADD_SECOND_TAG &&
		rule->filter_outer_pri == FILTER_PRI_DO_NOT_CARE)
	{
		if(rule->tag_num < 2)
			return -1;

		subrule->pri = TREAT_PRI_COPY_FROM_OUTER;
	}
	else
	{
		if(pbit >=0 && pbit < 8)
		{
			subrule->pri = pbit;
		}
		else if(pbit == FILTER_PRI_DO_NOT_CARE)
		{
			subrule->pri = 0;
		}
		else
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc pbit error ====> calc down sub treat opt");
			return -1;
		}
	}

	//handle DEI
	if(rule->treatment_method/10 == METHOD_CHANGE_TAG && index == ADD_FIRST_TAG && 
		(rule->filter_inner_dei == TREAT_DEI_COPY_FROM_INNER || rule->filter_inner_dei == TREAT_DEI_COPY_FROM_OUTER))
	{
		if(rule->filter_inner_dei == TREAT_DEI_COPY_FROM_OUTER && rule->tag_num < 2)
			return -1;

		subrule->dei = rule->filter_inner_dei;
	}
	else if(rule->treatment_method/10 == METHOD_CHANGE_TAG && index == ADD_SECOND_TAG && 
		(rule->filter_outer_dei == TREAT_DEI_COPY_FROM_INNER || rule->filter_outer_dei == TREAT_DEI_COPY_FROM_OUTER))
	{
		if(rule->tag_num < 2)
			return -1;

		subrule->dei = rule->filter_outer_dei;
	}
	else
	{
		if(dei ==0 || dei == 1)
		{
			subrule->dei = dei;
		}
		else if(dei == FILTER_DEI_DO_NOT_CARE)
		{
			subrule->dei = FILTER_DEI_DO_NOT_CARE;
		}
		else
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc DEI error ====> calc down sub treat opt");
			return -1;
		}
	}

	//handle VID
	if(rule->treatment_method/10 == METHOD_CHANGE_TAG && index == ADD_FIRST_TAG && 
		(rule->add_fst_vid == TREAT_VID_COPY_FROM_INNER || rule->add_fst_vid == TREAT_VID_COPY_FROM_OUTER))
	{
		if(rule->add_fst_vid == TREAT_VID_COPY_FROM_OUTER && rule->tag_num < 2)
			return -1;

		subrule->vid = rule->add_fst_vid;
	}
	else if(rule->treatment_method/10 == METHOD_CHANGE_TAG && index == ADD_SECOND_TAG && 
		(rule->add_sec_vid == TREAT_VID_COPY_FROM_INNER || rule->add_sec_vid == TREAT_VID_COPY_FROM_OUTER))
	{
		if(rule->tag_num < 2)
			return -1;

		subrule->vid = rule->add_sec_vid;
	}
	else
	{
		if(vid >=0 && vid <= 4095)
		{
			subrule->vid = vid;
		}
		else if(vid == FILTER_VID_DO_NOT_CARE)
		{
			if((rule->tag_num >= 2)&&(index == ADD_SECOND_TAG)){				
				subrule->vid = TREAT_VID_COPY_FROM_OUTER;			
			}else{				
				subrule->vid = TREAT_VID_COPY_FROM_INNER;			
			}
		}
		else
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc vid error ====> calc down sub treat opt");
			return -1;
		}
	}
	
	return 0;
}

static inline int pon_vlan_generate_down_rule(pon_vlan_rule * uprule, pon_vlan_rule * downrule)
{
	int i = 0;
	pon_vlan_sub_rule subrule = {0};
	
	if(downrule == NULL)
	{
		printk("\r\ndown rule pointer is NULL return ====> pon vlan generate down rule opt");
		return -1;
	}
	if(uprule == NULL)
	{
		printk("\r\nup rule pointer is NULL return ====> pon vlan generate down rule opt");
		return -1;
	}

	i = uprule->treatment_method;
	switch(i/10)
	{
		case 0:
			if(i == METHOD_TRANSPARENT)
			{
				memcpy(downrule,uprule,sizeof(pon_vlan_rule));
				return 0;
			}
			else if(i == METHOD_BLOCK)
			{
				downrule->tag_num = 5;//ignore this rule
				return 0;
			}
			else
				return -1;

		case METHOD_ADD_TAG:
			memcpy(downrule,uprule,sizeof(pon_vlan_rule));
			if(i == ADD_AND_CHANGE_OUTER_TAG)
			{
				downrule->tag_num = uprule->tag_num + 1;
				downrule->treatment_method = DEL_AND_CHANGE_INNER_TAG;
			}
			else
			{
				downrule->tag_num = uprule->tag_num + i%10;
				downrule->treatment_method = 30 + i%10;
			}
			if(calc_down_sub_filter(uprule,&subrule,FILTER_FIRST_TAG) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc first sub rule error return ====> pon vlan generate down rule opt");
				return -1;
			}
			switch(uprule->tag_num)
			{
				case 0:
					memcpy(&(downrule->filter_inner_tpid),&(subrule),sizeof(subrule));
					break;

				case 1:
					memcpy(&(downrule->filter_outer_tpid),&(subrule),sizeof(subrule));
					break;

				case 2:
					memcpy(&(downrule->down_filter_inner_tpid),&(subrule),sizeof(subrule));
					break;

				default:
					PONVLAN_PRINT(PONVLAN_MSG_ERR,"Uprule tagNum error return ====> pon vlan generate down rule opt");
					return -1;
			}
			if(i%10 == ADD_SECOND_TAG || i == ADD_AND_CHANGE_OUTER_TAG)
			{
				if(calc_down_sub_filter(uprule,&subrule,FILTER_SECONE_TAG) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc second sub rule error return ====> pon vlan generate down rule opt");
					return -1;
				}
				if(i == ADD_AND_CHANGE_OUTER_TAG)
				{
					switch(uprule->tag_num)
					{
						case 1:
							memcpy(&(downrule->filter_inner_tpid),&(subrule),sizeof(subrule));
							break;

						case 2:
							memcpy(&(downrule->filter_outer_tpid),&(subrule),sizeof(subrule));
							break;

						default:
							PONVLAN_PRINT(PONVLAN_MSG_ERR,"Uprule tagNum error return ====> pon vlan generate down rule opt");
							return -1;
					}
				}
				else
				{
					switch(uprule->tag_num)
					{
						case 0:
							memcpy(&(downrule->filter_outer_tpid),&(subrule),sizeof(subrule));
							break;

						case 1:
							memcpy(&(downrule->down_filter_inner_tpid),&(subrule),sizeof(subrule));
							break;

						case 2:
							memcpy(&(downrule->down_filter_outer_tpid),&(subrule),sizeof(subrule));
							break;

						default:
							PONVLAN_PRINT(PONVLAN_MSG_ERR,"Uprule tagNum error return ====> pon vlan generate down rule opt");
							return -1;
					}
				}
			}
			if(i == ADD_AND_CHANGE_OUTER_TAG)
			{
				switch(uprule->tag_num)
				{
					case 1:
						if(calc_down_sub_treat(uprule,&subrule,ADD_FIRST_TAG) == -1)
						{
							PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc first sub treat error(add and change) ====> pon vlan generate down rule opt");
							return -1;
						}
						break;

					case 2:
						if(calc_down_sub_treat(uprule,&subrule,ADD_SECOND_TAG) == -1)
						{
							PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc second sub treat error(add and change) ====> pon vlan generate down rule opt");
							return -1;
						}
						break;

					default:
						PONVLAN_PRINT(PONVLAN_MSG_ERR,"Uprule tagNum error return ====> pon vlan generate down rule opt");
						return -1;
				}
				memcpy(&(downrule->add_sec_tpid),&(subrule),sizeof(subrule));
			}
			break;

		case METHOD_DEL_TAG:
			memcpy(downrule,uprule,sizeof(pon_vlan_rule));
			if(i == DEL_AND_CHANGE_INNER_TAG)
			{
				downrule->tag_num = uprule->tag_num - 1;
				downrule->treatment_method = ADD_AND_CHANGE_OUTER_TAG;
			}
			else
			{
				downrule->tag_num = uprule->tag_num - i%10;
				downrule->treatment_method = 20 + i%10;
			}

			if(i == DEL_AND_CHANGE_INNER_TAG)
			{
				if(calc_down_sub_filter(uprule,&subrule,FILTER_SECONE_TAG) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc sub rule error return(del change) ====> pon vlan generate down rule opt");
					return -1;
				}
				memcpy(&(downrule->filter_inner_tpid),&(subrule),sizeof(subrule));
			}
			
			if((uprule->tag_num > 0 && uprule->tag_num == i%10) || (uprule->tag_num == 2 && i == DEL_AND_CHANGE_INNER_TAG))
			{
				if(calc_down_sub_treat(uprule,&subrule,ADD_FIRST_TAG) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc first sub treat error(case 3) ====> pon vlan generate down rule opt");
					return -1;
				}
				if(i == DEL_AND_CHANGE_INNER_TAG)
					memcpy(&(downrule->add_sec_tpid),&(subrule),sizeof(subrule));
				else
					memcpy(&(downrule->add_fst_tpid),&(subrule),sizeof(subrule));
			}
			
			
			if(uprule->tag_num > 1)
			{
				if(calc_down_sub_treat(uprule,&subrule,ADD_SECOND_TAG) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc second sub treat error ====> pon vlan generate down rule opt");
					return -1;
				}
				if(i%10 == 1)
					memcpy(&(downrule->add_fst_tpid),&(subrule),sizeof(subrule));
				else if(i%10 == 2)
					memcpy(&(downrule->add_sec_tpid),&(subrule),sizeof(subrule));
				else if(i%10 == 3)
					memcpy(&(downrule->add_fst_tpid),&(subrule),sizeof(subrule));
				else
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc sec sub treat error(case 3) ====> pon vlan generate down rule opt");
					return -1;
				}
			}
			break;

		case METHOD_CHANGE_TAG:
			memcpy(downrule,uprule,sizeof(pon_vlan_rule));
			if(uprule->tag_num == 1 || (uprule->tag_num == 2 && (i%10 == 2 || i%10 == 0)))
			{
				if(calc_down_sub_filter(uprule,&subrule,FILTER_FIRST_TAG) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc first sub rule error (case 4) ====> pon vlan generate down rule opt");
					return -1;
				}
				memcpy(&(downrule->filter_inner_tpid),&(subrule),sizeof(subrule));

				if(calc_down_sub_treat(uprule,&subrule,ADD_FIRST_TAG) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc first sub treat error (case 4)====> pon vlan generate down rule opt");
					return -1;
				}
				memcpy(&(downrule->add_fst_tpid),&(subrule),sizeof(subrule));
			}

			if(uprule->tag_num == 2 && (i%10 == 2 || i%10 == 1))
			{
				if(calc_down_sub_filter(uprule,&subrule,FILTER_SECONE_TAG) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc first sub rule error (case 4) ====> pon vlan generate down rule opt");
					return -1;
				}
				memcpy(&(downrule->filter_outer_tpid),&(subrule),sizeof(subrule));

				if(calc_down_sub_treat(uprule,&subrule,ADD_SECOND_TAG) == -1)
				{
					PONVLAN_PRINT(PONVLAN_MSG_ERR,"calc first sub treat error (case 4)====> pon vlan generate down rule opt");
					return -1;
				}
				memcpy(&(downrule->add_sec_tpid),&(subrule),sizeof(subrule));
			}
			break;
			
		default:
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"Generate down rule error ====> pon vlan generate down rule opt");
			return -1;
	}
    
#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_CDS)
        downrule->filter_ethertype = 0;
#endif/*TCSUPPORT_COMPILE*/

	return 0;
}

static int pon_vlan_same_rule_check(pon_vlan_rule * r1,pon_vlan_rule * r2)
{
	if(r1->filter_ethertype != r2->filter_ethertype || r1->tag_num != r2->tag_num)
		return 0;

	if(r1->tag_num == 0)
		return 1;
	
	if(r1->tag_num > 0)
	{
		if(r1->filter_inner_tpid != r2->filter_inner_tpid || r1->filter_inner_pri != r2->filter_inner_pri
			|| r1->filter_inner_dei != r2->filter_inner_dei || r1->filter_inner_vid != r2->filter_inner_vid)
			return 0;
	}

	if(r1->tag_num > 1)
	{
		if(r1->filter_outer_tpid != r2->filter_outer_tpid || r1->filter_outer_pri != r2->filter_outer_pri
			|| r1->filter_outer_dei != r2->filter_outer_dei || r1->filter_outer_vid != r2->filter_outer_vid)
			return 0;
	}
	return 1;
	
}

static int pon_vlan_get_rule_complexity(pon_vlan_rule * rule)
{
	uint8_t complexity = 0;
	
	if(rule->filter_outer_tpid != FILTER_TPID_DO_NOT_CARE)
 		complexity++;
	if(rule->filter_outer_pri != FILTER_PRI_DO_NOT_CARE)
 		complexity++;
	if(rule->filter_outer_dei != FILTER_DEI_DO_NOT_CARE)
 		complexity++;
	if(rule->filter_outer_vid != FILTER_VID_DO_NOT_CARE)
 		complexity++;
	if(rule->filter_inner_tpid != FILTER_TPID_DO_NOT_CARE)
 		complexity++;
	if(rule->filter_inner_pri != FILTER_PRI_DO_NOT_CARE)
 		complexity++;
	if(rule->filter_inner_dei != FILTER_DEI_DO_NOT_CARE)
 		complexity++;
	if(rule->filter_inner_vid != FILTER_VID_DO_NOT_CARE)
 		complexity++;
	if(rule->filter_ethertype != FILTER_ETP_DO_NOT_CARE)
		complexity++;
	
	rule->rule_complexity = complexity;
	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"current rule has complexity %d",complexity);
	return 0;
}

static int compare(const void * member1,const  void *member2)
{
	return ((pon_vlan_rule *) member1)->rule_complexity -((pon_vlan_rule *) member2)->rule_complexity;

}

static int pon_vlan_resort_rule_option(pon_vlan * port_data,int direction)
{
	int count = 0;
	pon_vlan_rule * table = NULL;
	
	if(direction == UPSTREAM){
		count = port_data->up_rule_count;
		table = port_data->up_rule;
	}else{
		count = port_data->down_rule_count;
		table = port_data->down_rule;
	}

	sort(table,count,sizeof(pon_vlan_rule),compare,NULL);
	return 0;
}
static void pon_vlan_resort_rule_expires(TIMER_FUN_PAAM data)
{
	pon_vlan * vlan_data = NULL;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	vlan_data = (pon_vlan *)data;
#else
	vlan_data = from_timer(vlan_data, data, resort_timer);
#endif 
	if(NULL == vlan_data){
		return;
	}
	pon_vlan_resort_rule_option(vlan_data,UPSTREAM);
	pon_vlan_resort_rule_option(vlan_data,DOWNSTREAM);
}

static int pon_vlan_rule_option(pon_vlan_ioctl * data,void * arg)
{
	int j = 0,k = 0,m = 0;
	pon_vlan * tmp = NULL;
	pon_vlan_rule downrule = {0};

	if(data->option_flag == OPT_GET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			if(data->rule_index >= tmp->up_rule_count && tmp->up_rule_count != 0)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"index out of up rule count ====> pon vlan rule opt");
				return -1;
			}
			memcpy(&data->rule,&tmp->up_rule[data->rule_index],sizeof(pon_vlan_rule));
			data->rule_index++;

			if(data->rule_index == tmp->up_rule_count)
				data->rule_index = 0xff;

			if(tmp->up_rule_count == 0)
				data->rule_index = 0;

			if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan rule opt");
				return -1;
			}
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Get vlan rule error,can't find port====> pon vlan rule opt");
		return -1;
	}
	else if(data->option_flag == OPT_SET)
	{
		tmp = get_pon_vlan_by_port(data->port);
                if (TCSUPPORT_CT_PON_SN_VAL) {
			if (data->port == 40) {
				data->rule.treatment_method = METHOD_TRANSPARENT;
				//printk("\r\nset default vlan treadmeng_method to METHOD_TRANSPARENT");
			}
		}
		if(tmp != NULL)
		{
			if(tmp->up_rule_count >= VLAN_RULE_LIMIT)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"Rule count has reach the limit on port %d ====> pon vlan rule opt",data->port);
				return -1;
			}

			for(j = 0; j < tmp->up_rule_count; j++)
			{
				//if(memcmp(&data->rule,(tmp->up_rule) + j,sizeof(pon_vlan_rule)) == 0)
				//	return 0;
				if(pon_vlan_same_rule_check(&data->rule,((tmp->up_rule) + j)) == 1)
					return 0;
			}
			
			if(pon_vlan_generate_down_rule(&data->rule,&downrule) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"Generate down rule error on port %d ====> pon vlan rule opt",data->port);
				return -1;
			}
			pon_vlan_get_rule_complexity(&data->rule);
			downrule.rule_complexity = data->rule.rule_complexity;
			memset(&tmp->mac_vid, 0, sizeof(mac_vid_pair)*MAX_MAC_VID);	
		    if( TCSUPPORT_CT_SFU_SX_VAL 
				&& (tmp->up_rule_count!=0) && (downrule.treatment_method==0))
			{
				memcpy(&tmp->up_rule[tmp->up_rule_count],&tmp->up_rule[0],sizeof(pon_vlan_rule));
				memcpy(&tmp->up_rule[0],&data->rule,sizeof(pon_vlan_rule));			

				memcpy(&tmp->down_rule[tmp->down_rule_count],&tmp->down_rule[0],sizeof(pon_vlan_rule));
				memcpy(&tmp->down_rule[0],&downrule,sizeof(pon_vlan_rule));
			}
            else{
			    memcpy(&tmp->up_rule[tmp->up_rule_count],&data->rule,sizeof(pon_vlan_rule));
			    memcpy(&tmp->down_rule[tmp->down_rule_count],&downrule,sizeof(pon_vlan_rule));
            }
			tmp->up_rule_count ++;
			tmp->down_rule_count ++;
			if( TCSUPPORT_CT_SFU_SX_VAL 
				&& (tmp->up_rule_count!=0) && (downrule.treatment_method==0))
			{
				//do nothing
			}else{
				if(pon_vlan_all_data.resort_enable_flag){
					PONVLAN_START_TIMER(tmp->resort_timer,PONVLAN_RESORT_TIMER);
				}
			}
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"Add rule success on port %d,up rule count is %d====> pon vlan rule opt\r\n",data->port,tmp->up_rule_count);
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Add vlan rule error,can't find port====> pon vlan rule opt");
		return -1;
	}
	else if(data->option_flag == OPT_DEL)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			if(data->rule_index >= tmp->up_rule_count)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"Del index is larger than rule num on port %d ====> pon vlan rule opt",data->port);
				return -1;
			}
			memset(&tmp->mac_vid, 0, sizeof(mac_vid_pair)*MAX_MAC_VID);
			if(data->rule_index < tmp->up_rule_count - 1)
			{
				memcpy(&tmp->up_rule[data->rule_index],&tmp->up_rule[data->rule_index + 1],sizeof(pon_vlan_rule)*(tmp->up_rule_count - data->rule_index - 1));
				memcpy(&tmp->down_rule[data->rule_index],&tmp->down_rule[data->rule_index + 1],sizeof(pon_vlan_rule)*(tmp->down_rule_count - data->rule_index - 1));
			}
			tmp->up_rule_count --;
			tmp->down_rule_count--;
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"Del rule success on port %d,up rule count is %d, ====> pon vlan rule opt\r\n",data->port,tmp->up_rule_count);
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Del vlan rule error ====> pon vlan rule opt");
		return -1;
	}
	else if(data->option_flag == OPT_CLEAN)
	{
		if(data->port == PONVLAN_PORT_HWNAT_CLEAN)
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"Clean HWNAT success ====> pon vlan rule opt");
#if defined(TCSUPPORT_RA_HWNAT) && defined(TCSUPPORT_RA_HWNAT_ENHANCE_HOOK)
			if(ra_sw_nat_hook_clean_table)
				ra_sw_nat_hook_clean_table();
#endif
			return 0;
		}
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
		    memset(&tmp->mac_vid, 0, sizeof(mac_vid_pair)*MAX_MAC_VID);
			memset(tmp->up_rule, 0, sizeof(pon_vlan_rule) * tmp->rule_limit);
			tmp->up_rule_count = 0;
			memset(tmp->down_rule, 0, sizeof(pon_vlan_rule) * tmp->rule_limit);
			tmp->down_rule_count = 0;
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"Clean rule success on port %d ====> pon vlan rule opt",data->port);
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Clean vlan rule error ====> pon vlan rule opt");
		return -1;
	}
	else if(data->option_flag == OPT_CLEAN_ALL)
	{
		for(j = 0; j < pon_vlan_all_data.total_port_count; j++)
		{
			tmp = &(pon_vlan_all_data.pon_vlan_data[j]);
			memset(&tmp->mac_vid, 0, sizeof(mac_vid_pair)*MAX_MAC_VID);
			memset(tmp->up_rule, 0, sizeof(pon_vlan_rule) * tmp->rule_limit);
			tmp->up_rule_count = 0;
			memset(tmp->down_rule, 0, sizeof(pon_vlan_rule) * tmp->rule_limit);
			tmp->down_rule_count = 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_WARNING,"Clean rule success on all port ====> pon vlan rule opt");
		return 0;
	}
	else if(data->option_flag == OPT_SHOW)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			printk("\r\nWe will show some globel info below");
			printk("\r\nVLan status is %d,Igmp status is %d,resort status is %d",pon_vlan_all_data.vlan_enable_flag,pon_vlan_all_data.igmp_enable_flag,\
																				pon_vlan_all_data.resort_enable_flag);
			
			printk("\r\nInterface bind vlan is %s,VID is:",(pon_vlan_all_data.if_vlan_bind_enable_flag == ENABLE)?"Enable":"Disable");
			for(j = 0; j < 8; j++)
			{
				printk("\r\n");
				for(k = 0; k < 8; k++)
				{
					printk("%d_%d:%d  ",13 + j,k,pon_vlan_all_data.pon_if_vlan_pair_data[j * 8 + k]);
				}
			}

			printk("\r\nPCP mode is %d",pon_vlan_all_data.pcp_mode);
			printk("\r\nUser group is %s",(pon_vlan_all_data.user_group_enable_flag == ENABLE)?"Enable":"Disable");
			printk("\r\nMax TPID num is %d",pon_vlan_all_data.total_special_tpid);
			m = pon_vlan_all_data.total_special_tpid;
			m = (m%4 == 0)?(m/4):(m/4 + 1);
			for(j = 0; j < m; j++)
			{
				printk("\r\n");
				for(k = 0; k < 4; k++)
				{
					if(j * 4 + k >= pon_vlan_all_data.total_special_tpid)
						break;
					printk("%d: %s  0x%x  ",(j * 4 + k),((j * 4 + k) < pon_vlan_all_data.tpid_counter)?"Enable":"Disable",
						pon_vlan_all_data.pon_vlan_type[j * 4 + k].type);
				}
			}
			
			printk("\r\nWe will show all rule on port %d",data->port);
			printk("\r\nInput TPID is %x,Output TPID is %x",tmp->input_tpid,tmp->output_tpid);
			printk("\r\nDownstream mode is %d",tmp->down_stream_mode);
			printk("\r\nDownstream mode	mask is 0x%x\n",tmp->downstream_mode_mask);
			printk("\r\nDownstream mode unmatch_oper is %s\n",(tmp->downstream_unmatch_oper == DOWNSTREAM_MODE_UNMATCH_FORWARD)?"Foward":"Discard");
			printk("\r\nDefault Rule is %s",(tmp->enable_default_rule == ENABLE)?"Enable":"Disable");
			printk("\r\nUser group is %x",tmp->user_group);
			printk("\r\nDSCP Map is:");
			for(j = 0; j < 8; j++)
			{
				printk("\r\n");
				for(k = 0; k < 8; k++)
				{
					printk("%d ",tmp->dscp_map[j * 8 + k]);
				}
			}

			printk("\r\nMac bind Vlan is %s,Time out is %d\n",(tmp->mac_bind_vlan_enable == ENABLE)?"Enable":"Disable",pon_vlan_all_data.mac_vlan_time);
			for(j = 0; j < MAX_MAC_VID; j++)
			{
				printk("source mac is [%02x:%02x:%02x:%02x:%02x:%02x], uprule index [%d],vid mask is [%x]\r\n",
				tmp->mac_vid[j].src_mac[0],tmp->mac_vid[j].src_mac[1],tmp->mac_vid[j].src_mac[2],tmp->mac_vid[j].src_mac[3],tmp->mac_vid[j].src_mac[4],tmp->mac_vid[j].src_mac[5],
				tmp->mac_vid[j].uprule_index+1, tmp->mac_vid[j].vid_mask);
				printk("Org_Outer vid is [0x%x],Org_Outer pbit is [0x%x]\r\n",tmp->mac_vid[j].org_outer_vid,tmp->mac_vid[j].org_outer_pbit);
				printk("Org_Inner vid is [0x%x],Org_Inner pbit is [0x%x]\r\n",tmp->mac_vid[j].org_inner_vid,tmp->mac_vid[j].org_inner_pbit);
				printk("RS_Outer vid is [0x%x],Last time is [%ld]\r\n",tmp->mac_vid[j].rs_outer_vid,tmp->mac_vid[j].last_time);
			}
			
			printk("Igmp mode is %d,Igmp tci is %x\n",tmp->igmp_mode,tmp->igmp_tci);
			printk("Upstream direction\n");
			printk("Idx TagN foinfo         fiinfo        Etp  Method   tfinfo     tsinfo      pri complex\n");
			for(j = 0; j < tmp->up_rule_count; j++)
			{
				printk("%d   %d    %d %d %d %d     %d %d %d %d     %x  %d      %d %d %d %d     %d %d %d %d     %s %d\n",j+1,tmp->up_rule[j].tag_num,
					tmp->up_rule[j].filter_outer_tpid,tmp->up_rule[j].filter_outer_pri,tmp->up_rule[j].filter_outer_dei,tmp->up_rule[j].filter_outer_vid,
					tmp->up_rule[j].filter_inner_tpid,tmp->up_rule[j].filter_inner_pri,tmp->up_rule[j].filter_inner_dei,tmp->up_rule[j].filter_inner_vid,
					tmp->up_rule[j].filter_ethertype,tmp->up_rule[j].treatment_method,
					tmp->up_rule[j].add_fst_tpid,tmp->up_rule[j].add_fst_pri,tmp->up_rule[j].add_fst_dei,tmp->up_rule[j].add_fst_vid,
					tmp->up_rule[j].add_sec_tpid,tmp->up_rule[j].add_sec_pri,tmp->up_rule[j].add_sec_dei,tmp->up_rule[j].add_sec_vid,
					(HIGH_PRIORITY==tmp->up_rule[j].rule_priority)?"High":((LOW_PRIORITY==tmp->up_rule[j].rule_priority)?"Low":"Default"),
					tmp->up_rule[j].rule_complexity);
			}
			printk("Downstream direction\n");
			for(j = 0; j < tmp->down_rule_count; j++)
			{
				printk("Idx TagN foinfo         fiinfo        Etp  Method   tfinfo     tsinfo      pri complex\n");
				printk("%d   %d    %d %d %d %d     %d %d %d %d     %x  %d      %d %d %d %d     %d %d %d %d     %s %d\n",j+1,tmp->down_rule[j].tag_num,
					tmp->down_rule[j].filter_outer_tpid,tmp->down_rule[j].filter_outer_pri,tmp->down_rule[j].filter_outer_dei,tmp->down_rule[j].filter_outer_vid,
					tmp->down_rule[j].filter_inner_tpid,tmp->down_rule[j].filter_inner_pri,tmp->down_rule[j].filter_inner_dei,tmp->down_rule[j].filter_inner_vid,
					tmp->down_rule[j].filter_ethertype,tmp->down_rule[j].treatment_method,
					tmp->down_rule[j].add_fst_tpid,tmp->down_rule[j].add_fst_pri,tmp->down_rule[j].add_fst_dei,tmp->down_rule[j].add_fst_vid,
					tmp->down_rule[j].add_sec_tpid,tmp->down_rule[j].add_sec_pri,tmp->down_rule[j].add_sec_dei,tmp->down_rule[j].add_sec_vid,
					(HIGH_PRIORITY==tmp->up_rule[j].rule_priority)?"High":((LOW_PRIORITY==tmp->up_rule[j].rule_priority)?"Low":"Default"),
					tmp->down_rule[j].rule_complexity);
				printk("down_foinfo         down_fiinfo\n");
				printk("%d %d %d %d     %d %d %d %d\n",tmp->down_rule[j].down_filter_outer_tpid,tmp->down_rule[j].down_filter_outer_pri,tmp->down_rule[j].down_filter_outer_dei,
					tmp->down_rule[j].down_filter_outer_vid,tmp->down_rule[j].down_filter_inner_tpid,tmp->down_rule[j].down_filter_inner_pri,tmp->down_rule[j].down_filter_inner_dei,tmp->down_rule[j].down_filter_inner_vid);
			}
			printk("\r\n");
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Show vlan rule error ====> pon vlan rule opt");
		return -1;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan rule opt");
	return -1;
}

/*  
	EPON vlan setting begin.
	Translate vlan rule from ioctl structure into ponvlan structure following CTC V3.0.
*/
static int epon_vlan_rule_translate(pon_vlan_epon_ioctl * epon_rule, pon_vlan_rule* tar_rule)
{
	uint new_tag = 0;

	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"Enter");
	if((NULL == epon_rule) || (NULL == tar_rule))
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Param error!");
		return -1;
	}
	memset(tar_rule, 0, sizeof(pon_vlan_rule));
	if(epon_rule->num == 0)
	{
		tar_rule->tag_num = 0;
		/* only support transparent/drop/add one tag for untag frame. */
		switch(epon_rule->method)
		{
		case EPON_VLAN_TR_PASS:
			tar_rule->treatment_method = METHOD_TRANSPARENT;
			break;
		case EPON_VLAN_TR_DROP:
			tar_rule->treatment_method = METHOD_BLOCK;
			break;
		case EPON_VLAN_TR_ADD_ONE:
			tar_rule->treatment_method = 21;
			new_tag = epon_rule->new_tags[0];

			tar_rule->add_fst_vid = (new_tag & 0xFFF);
			tar_rule->add_fst_dei = ((new_tag >> 12) & 0x1);
			tar_rule->add_fst_pri = ((new_tag >> 13) & 0x7);
			tar_rule->add_fst_tpid = 0; //default:0x8100
			
			break;
		default:
			PONVLAN_PRINT(PONVLAN_MSG_WARNING, "Unknown treatment for untag frame!");
			return -1;
			break;
		}
	}
	else if(epon_rule->num == 1)
	{
		/* only support transparent/drop/strip/translate for tag frame. */
		tar_rule->tag_num = 1;
		
		if(epon_rule->mask & EPON_VLAN_MASK_TPID)
		{
			tar_rule->filter_inner_tpid = ((epon_rule->old_tags[0] >> 16) & 0xFFFF);
		}
		else{
			tar_rule->filter_inner_tpid = 0;
		}
		
		if(epon_rule->mask & EPON_VLAN_MASK_PRI)
		{
			tar_rule->filter_inner_pri = ((epon_rule->old_tags[0] >> 13) & 0x7);
		}
		else{
			tar_rule->filter_inner_pri = 8;
		}
		
		if(epon_rule->mask & EPON_VLAN_MASK_CFI)
		{
			tar_rule->filter_inner_dei = ((epon_rule->old_tags[0] >> 12) & 0x1);
		}
		else{
			tar_rule->filter_inner_dei = 2;
		}
		
		if(epon_rule->mask & EPON_VLAN_MASK_VID)
		{
			tar_rule->filter_inner_vid = ((epon_rule->old_tags[0]) & 0xFFF);
		}
		else{
			tar_rule->filter_inner_vid = 4096;
		}
		
		switch(epon_rule->method)
		{
		case EPON_VLAN_TR_PASS:
			tar_rule->treatment_method = METHOD_TRANSPARENT;
			break;
		case EPON_VLAN_TR_DROP:
			tar_rule->treatment_method = METHOD_BLOCK;
			break;
		case EPON_VLAN_TR_STRIP_ONE:
			tar_rule->treatment_method = 31;			
			break;
		case EPON_VLAN_TR_TRANS:
			tar_rule->treatment_method = 40;
			new_tag = epon_rule->new_tags[0];
			tar_rule->add_fst_vid = (new_tag & 0xFFF);
			tar_rule->add_fst_dei = ((new_tag >> 12) & 0x1);
			tar_rule->add_fst_pri = ((new_tag >> 13) & 0x7);
			tar_rule->add_fst_tpid = 0; //default:0x8100
			break;
		default:
			PONVLAN_PRINT(PONVLAN_MSG_WARNING, "Unknown treatment for one tag frame!");
			return -1;
			break;
		}
	}
	/*
	else if(epon_rule->num == 2)
	{
		tar_rule->tag_num = 2;
		switch(epon_rule->method)
		{
		case EPON_VLAN_TR_PASS:
			tar_rule->treatment_method = METHOD_TRANSPARENT;
			break;
		case EPON_VLAN_TR_DROP:
			tar_rule->treatment_method = METHOD_BLOCK;
			break;
		default:
			PONVLAN_PRINT(PONVLAN_MSG_WARNING, "Unknown treatment for two tag frame!");
			return -1;
			break;
		}
	}
	*/
	else{
		PONVLAN_PRINT(PONVLAN_MSG_WARNING, "Not supportted tagnum:%d!", epon_rule->num);
		return -1;
	}
	return 0;
}

static int epon_vlan_rule_add(pon_vlan * port_rule, pon_vlan_epon_ioctl * epon_rule)
{
	int j = 0;
	pon_vlan_rule tmp_rule;
	
	if(epon_vlan_rule_translate(epon_rule, &tmp_rule) != 0)
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"pon vlan translate fail.");
		return -1;
	}
	if(epon_rule->dir == EPON_VLAN_DIR_UP)
	{
		if(port_rule->up_rule_count >= VLAN_RULE_LIMIT)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"Up rule count has reach the limit on port %d.",epon_rule->port);
			return -1;
		}
		for(j = 0; j < port_rule->up_rule_count; j++)
		{
			if(pon_vlan_same_rule_check(&tmp_rule,((port_rule->up_rule) + j)) == 1)
				return 0;
		}
		memcpy(&port_rule->up_rule[port_rule->up_rule_count], &tmp_rule, sizeof(pon_vlan_rule));
		port_rule->up_rule_count ++;
	}
	else if(epon_rule->dir == EPON_VLAN_DIR_DOWN)
	{
		if(port_rule->down_rule_count >= VLAN_RULE_LIMIT)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"Down rule count has reach the limit on port %d.",epon_rule->port);
			return -1;
		}
		/*for(j = 0; j < tmp->down_rule_count; j++)
		{
			if(pon_vlan_same_rule_check(&tmp_rule,((tmp->down_rule) + j)) == 1)
				return 0;
		}*/
		memcpy(&port_rule->down_rule[port_rule->down_rule_count], &tmp_rule, sizeof(pon_vlan_rule));
		port_rule->down_rule_count ++;
	}
	else{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"epon rule direction error! port:%d",epon_rule->port);
		return -1;
	}
	memset(&port_rule->mac_vid[0], 0, sizeof(mac_vid_pair)*MAX_MAC_VID); 
	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"Add rule success on port %d, up rule count is %d, down rule count is %d\r\n",
		epon_rule->port, port_rule->up_rule_count, port_rule->down_rule_count);
	return 0;

}
static int epon_vlan_rule_del(pon_vlan * port_rule, int dir, uint32_t index)
{	
	if(dir == EPON_VLAN_DIR_UP)
	{
		if(index >= port_rule->up_rule_count)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"Del Up rule index is larger than rule num on port %d.", port_rule->port_index);
			return -1;
		}
		memset(&port_rule->mac_vid[0], 0, sizeof(mac_vid_pair)*MAX_MAC_VID);
		if(index < port_rule->up_rule_count - 1)
		{
			memcpy(&port_rule->up_rule[index], &port_rule->up_rule[index + 1],
				sizeof(pon_vlan_rule)*(port_rule->up_rule_count - index - 1));
		}
		port_rule->up_rule_count --;
	}
	else if(dir == EPON_VLAN_DIR_DOWN)
	{
		if(index >= port_rule->down_rule_count)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"Del Down Rule index is larger than rule num on port %d.", port_rule->port_index);
			return -1;
		}
		memset(&port_rule->mac_vid[0], 0, sizeof(mac_vid_pair)*MAX_MAC_VID);
		if(index < port_rule->down_rule_count - 1)
		{
			memcpy(&port_rule->down_rule[index],&port_rule->down_rule[index + 1],
				sizeof(pon_vlan_rule)*(port_rule->down_rule_count - index - 1));
		}
		port_rule->down_rule_count--;
	}
	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"Del rule success on port %d,up rule count is %d,down rule count is %d\r\n",
		port_rule->port_index, port_rule->up_rule_count, port_rule->down_rule_count);
	return 0;

}

static int epon_vlan_rule_clr_port(pon_vlan * port_rule)
{
    memset(&port_rule->mac_vid[0], 0, sizeof(mac_vid_pair)*MAX_MAC_VID);
	memset(port_rule->up_rule, 0, sizeof(pon_vlan_rule) * port_rule->rule_limit);
	port_rule->up_rule_count = 0;
	memset(port_rule->down_rule, 0, sizeof(pon_vlan_rule) * port_rule->rule_limit);
	port_rule->down_rule_count = 0;
	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"Clean rule success on port %d.",port_rule->port_index);
	return 0;
}

static int epon_vlan_rule_clr_all(void)
{
	int j = 0;
	pon_vlan * tmp;
	
	for(j = 0; j < pon_vlan_all_data.total_port_count; j++)
	{
		tmp = &(pon_vlan_all_data.pon_vlan_data[j]);
		memset(&tmp->mac_vid[0], 0, sizeof(mac_vid_pair)*MAX_MAC_VID);
		memset(tmp->up_rule, 0, sizeof(pon_vlan_rule) * tmp->rule_limit);
		tmp->up_rule_count = 0;
		memset(tmp->down_rule, 0, sizeof(pon_vlan_rule) * tmp->rule_limit);
		tmp->down_rule_count = 0;
	}
	return 0;
}



/*	
	For epon vlan rule setting.
*/
static int epon_vlan_rule_option(pon_vlan_epon_ioctl * epon_rule)
{
	pon_vlan * tmp = NULL;
	
	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"Enter");
	if(epon_rule->op == EPON_VLAN_OP_ADD)
	{
		tmp = get_pon_vlan_by_port(epon_rule->port);
		if(tmp != NULL)
		{
			return epon_vlan_rule_add(tmp, epon_rule);
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Add vlan rule error,can't find port====> pon vlan rule opt");
		return -1;
	}
	else if(epon_rule->op == EPON_VLAN_OP_DEL)
	{
		tmp = get_pon_vlan_by_port(epon_rule->port);
		if(tmp != NULL)
		{
			return epon_vlan_rule_del(tmp, epon_rule->dir, epon_rule->index);
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Del epon vlan rule error");
		return -1;
	}
	else if(epon_rule->op == EPON_VLAN_OP_CLR_PORT)
	{
		tmp = get_pon_vlan_by_port(epon_rule->port);
		if(tmp != NULL)
		{
			return epon_vlan_rule_clr_port(tmp);
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Clean vlan rule error.");
		return -1;
	}
	else if(epon_rule->op == EPON_VLAN_OP_CLR_ALL)
	{
		epon_vlan_rule_clr_all();
		PONVLAN_PRINT(PONVLAN_MSG_WARNING,"Clean rule success on all port.");
		return 0;
	}
	else if(epon_rule->op == EPON_VLAN_OP_CLR_HWNAT)
	{
		/* clear hw_nat table. */
		if(TCSUPPORT_RA_HWNAT_VAL && TCSUPPORT_RA_HWNAT_ENHANCE_HOOK_VAL)
		{
			if(ra_sw_nat_hook_clean_table)
				ra_sw_nat_hook_clean_table();
		}

		PONVLAN_PRINT(PONVLAN_MSG_WARNING,"Clean HWNAT success.");
		return 0;
	}
	else{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Unknown epon vlan setting!");
		return -1;
	}

}
/*  
	EPON vlan setting end.
*/

static int pon_vlan_dscp_map_option(pon_vlan_ioctl * data,void * arg)
{
	int i = 0,j = 0,k = 0;
	pon_vlan * tmp = NULL;

	if(data->option_flag == OPT_GET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			for(j = 0; j < 6; j++)
			{
				data->dscp_map[j] = 0;
			}
			for(j = 0; j < 64; j++)
			{
				k += 3;
				if(i == 0 || i == 3)
				{
					if(k > 32)
					{
						data->dscp_map[i] += tmp->dscp_map[j] >> 1;
						i ++;
						data->dscp_map[i] += (tmp->dscp_map[j] & 1) << 31;
						k = 1;
						continue;
					}
					data->dscp_map[i] += tmp->dscp_map[j] << (32 - k);
				}
				else if(i == 1 || i == 4)
				{
					if(k > 32)
					{
						data->dscp_map[i] += tmp->dscp_map[j] >> 2;
						i ++;
						data->dscp_map[i] += (tmp->dscp_map[j] & 3) << 30;
						k = 2;
						continue;
					}
					data->dscp_map[i] += tmp->dscp_map[j] << (32 - k);
				}
				else if(i == 2 || i == 5)
				{
					if(k >= 32)
					{
						data->dscp_map[i] += tmp->dscp_map[j];
						i ++;
						k = 0;
						continue;
					}
					data->dscp_map[i] += tmp->dscp_map[j] << (32 - k);
				}
			}
			if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan DSCP map opt");
				return -EFAULT;
			}
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set DSCP map errror,can't find port ====> pon vlan DSCP map opt");
		return -1;
	}
	if(data->option_flag == OPT_SET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			for(j = 0; j < 64; j++)
			{
				if(j < 10)
				{
					k = data->dscp_map[0];
					tmp->dscp_map[j] = (k & (7 << (3 * (9 - j) + 2))) >> (3 * (9 - j) + 2);
					
				}
				else if(j == 10)
				{
					k = data->dscp_map[0];
					tmp->dscp_map[j] = (k & 3) << 1;
					k = data->dscp_map[1];
					tmp->dscp_map[j] += (((k & (1 << 31)) >> 31) & 1);
				}
				else if(j < 21)
				{
					k = data->dscp_map[1];
					tmp->dscp_map[j] = (k & (7 << (3 * (9 - (j - 11)) + 1))) >> (3 * (9 - (j - 11)) + 1);
				}
				else if(j == 21)
				{
					k = data->dscp_map[1];
					tmp->dscp_map[j] = (k & 0x01) << 2;
					k = data->dscp_map[2];
					tmp->dscp_map[j] += (((k & (3 << 30)) >> 30) & 3);
				}
				else if(j < 32)
				{
					k = data->dscp_map[2];
					tmp->dscp_map[j] = (k & (7 << (3 * (9 - (j - 22))))) >> (3 * (9 - (j - 22)));
				}
				else if(j < 42)
				{
					k = data->dscp_map[3];
					tmp->dscp_map[j] = (k & (7 << (3 * (9 - (j - 32)) + 2))) >> (3 * (9 - (j - 32)) + 2);
				}
				else if(j == 42)
				{
					k = data->dscp_map[3];
					tmp->dscp_map[j] = (k & 3) << 1;
					k = data->dscp_map[4];
					tmp->dscp_map[j] += (((k & (1 << 31)) >> 31) & 1);
				}
				else if(j < 53)
				{
					k = data->dscp_map[4];
					tmp->dscp_map[j] = (k & (7 << (3 * (9 - (j - 43)) + 1))) >> (3 * (9 - (j - 43)) + 1);
				}
				else if(j == 53)
				{
					k = data->dscp_map[4];
					tmp->dscp_map[j] = (k & 0x01) << 2;
					k = data->dscp_map[5];
					tmp->dscp_map[j] += (((k & (3 << 30)) >> 30) & 3);
				}
				else if(j < 64)
				{
					k = data->dscp_map[5];
					tmp->dscp_map[j] = (k & (7 << (3 * (9 - (j - 54))))) >> (3 * (9 - (j - 54)));
				}
				tmp->dscp_map[j] &= 7;//clean first 5 bits
			}
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set DSCP map errror,can't find port ====> pon vlan DSCP map opt");
		return -1;
	}
	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"option error ====> pon vlan DSCP map opt");
	return -1;
}

static int pon_vlan_igmp_switch_option(pon_vlan_ioctl * data,void * arg)
{
	if(data->option_flag == OPT_GET)
	{
		data->igmp_enable = pon_vlan_all_data.igmp_enable_flag;
		if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan igmp switch opt");
			return -1;
		}
		return 0;
	}
	else if(data->option_flag == OPT_SET)
	{
		pon_vlan_all_data.igmp_enable_flag = data->igmp_enable;
		return 0;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan Igmp Switch opt");
	return -1;
}

static int pon_vlan_igmp_mode_option(pon_vlan_ioctl * data,void * arg)
{
	pon_vlan * tmp = NULL;

	if(data->option_flag == OPT_GET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			data->igmp_mode = tmp->igmp_mode;
			if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan igmp mode opt");
				return -EFAULT;
			}
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set igmp mode error,can't find port ====> pon vlan igmp mode opt");
		return -1;
	}
	else if(data->option_flag == OPT_SET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			tmp->igmp_mode = data->igmp_mode;
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set igmp mode error,can't find port ====> pon vlan igmp mode opt");
		return -1;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan igmp mode opt");
	return -1;
}

static int pon_vlan_igmp_tci_option(pon_vlan_ioctl * data,void * arg)
{
	pon_vlan * tmp = NULL;

	if(data->option_flag == OPT_GET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			data->igmp_tci = tmp->igmp_tci;
			if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan igmp tci opt");
				return -1;
			}
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set igmp tci error,can't find port ====>pon vlan igmp tci opt");
		return -1;
	}
	else if(data->option_flag == OPT_SET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			tmp->igmp_tci = data->igmp_tci;
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set igmp tci error,can't find port ====>pon vlan igmp tci opt");
		return -1;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan igmp tci opt");
	return -1;
}


static int pon_vlan_if_vlan_switch_option(pon_vlan_ioctl * data,void * arg)
{
	if(data->option_flag == OPT_GET)
	{
		data->if_vlan_switch = pon_vlan_all_data.if_vlan_bind_enable_flag;
		if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan if vlan Switch opt");
			return -1;
		}
		return 0;
	}
	else if(data->option_flag == OPT_SET)
	{
		pon_vlan_all_data.if_vlan_bind_enable_flag = data->if_vlan_switch;
			return 0;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan if vlan opt");
	return -1;
}



static int pon_vlan_if_vlan_rule_option(pon_vlan_ioctl * data,void * arg)
{
	int i = 0;
	if(data->option_flag == OPT_GET)
	{
		i = convert_dev_name_to_index(data->dev_name);
		if(i > -1 && i < 64)
		{
			data->dev_vid = pon_vlan_all_data.pon_if_vlan_pair_data[i];
			if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan if vlan opt");
				return -1;
			}
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"get interface vid error,can't find port ====>pon vlan if vlan opt");
		return -1;
	}
	else if(data->option_flag == OPT_SET)
	{
		i = convert_dev_name_to_index(data->dev_name);
		if(i > -1 && i < 64)
		{
			pon_vlan_all_data.pon_if_vlan_pair_data[i] = data->dev_vid;
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"set interface vid error ====> pon vlan if vlan opt");
		return -1;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan if vlan opt");
	return -1;
}

static int pon_vlan_pcp_mode_option(pon_vlan_ioctl * data,void * arg)
{
	if(data->option_flag == OPT_GET)
	{
		data->pcp_mode = pon_vlan_all_data.pcp_mode;
		if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan PCP opt");
			return -1;
		}
		return 0;
	}
	else if(data->option_flag == OPT_SET)
	{
		pon_vlan_all_data.pcp_mode = data->pcp_mode;
		return 0;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan PCP opt");
	return -1;
}

static int pon_vlan_mac_bind_vid_switch_option(pon_vlan_ioctl * data,void * arg)
{
	pon_vlan * tmp = NULL;

	if(data->option_flag == OPT_GET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			data->mac_bind_vid_enable = tmp->mac_bind_vlan_enable;
			if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan Mac bind Vid opt");
				return -1;
			}
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"get default rule errror,can't find port ====> pon vlan Mac bind Vid opt");
		return -1;
	}
	else if(data->option_flag == OPT_SET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			tmp->mac_bind_vlan_enable = data->mac_bind_vid_enable;
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set default rule errror,can't find port ====> pon vlan Mac bind Vid opt");
		return -1;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan Mac bind Vid opt");
	return -1;
}

static int pon_vlan_mac_bind_vid_time_option(pon_vlan_ioctl * data,void * arg)
{
	if(data->option_flag == OPT_GET)
	{
		data->mac_vlan_time = pon_vlan_all_data.mac_vlan_time;
		if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan mac vlan time opt");
			return -1;
		}
		return 0;
	}
	else if(data->option_flag == OPT_SET)
	{
		pon_vlan_all_data.mac_vlan_time = data->mac_vlan_time;
		return 0;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan mac vlan time opt");
	return -1;
}


static int pon_vlan_user_group_switch_option(pon_vlan_ioctl * data,void * arg)
{
	int i = 0;
	pon_vlan * tmp = NULL;
	
	if(data->option_flag == OPT_GET)
	{
		data->user_group_enable = pon_vlan_all_data.user_group_enable_flag;
		if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan user group switch opt");
			return -1;
		}
		return 0;
	}
	else if(data->option_flag == OPT_SET)
	{
		pon_vlan_all_data.user_group_enable_flag = data->user_group_enable;
		if(pon_vlan_all_data.user_group_enable_flag == DISABLE)
		{
			for(i = 0; i < pon_vlan_all_data.total_port_count; i++)
			{
				tmp = &pon_vlan_all_data.pon_vlan_data[i];
				tmp->user_group = 0xffffffff;
			}
			for(i = 0; i < 4; i++)
			{
				macMT7530SetPortBrgInd(macMT7530LanPortMap2Switch(i),0xff);
			}
		}
		return 0;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan user group Switch opt");
	return -1;
}

static int pon_vlan_user_group_rule_option(pon_vlan_ioctl * data,void * arg)
{
	pon_vlan * tmp = NULL;
	int i = 0,port = 0;
	__u32 group1 = 0,group2 = 0;
	__u8 mask = 0;

	if(data->option_flag == OPT_GET)
	{
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			data->user_group = tmp->user_group;
			if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> pon vlan user group opt");
				return -1;
			}
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"get default rule errror,can't find port ====> pon vlan user group opt");
		return -1;
	}
	else if(data->option_flag == OPT_SET)
	{
		if(pon_vlan_all_data.onu_mode == MODE_HGU)
		{
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"can not set user group in HGU mode");
			return 0;
		}
		tmp = get_pon_vlan_by_port(data->port);
		if(tmp != NULL)
		{
			tmp->user_group = data->user_group;
			group1 = tmp->user_group;
			port = tmp->port_index;
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"port is %d====>set user group",port);
			if(port > 10 && port < 15)//11~14,switch port.
			{
				for(i = 11; i< 11+pon_vlan_all_data.lan_port_count;i++)
				//for(i = 11; i < 15; i++)
				{
					if(!TCSUPPORT_MULTI_SWITCH_EXT_VAL)
					{
						if((i >= 15) || (data->port >= 15))
							continue;
					}
					if(i == port)
						continue;
					tmp = get_pon_vlan_by_port(i);
					if(NULL == tmp)
						continue;
					group2 = tmp->user_group;
					PONVLAN_PRINT(PONVLAN_MSG_TRACE,"group1 is %x,group2 is %x====>set user group",group1,group2);
					if(group1 == 0xffffffff && group2 == 0xffffffff)
					{
						macMT7530GetPortBrgInd(macMT7530LanPortMap2Switch(i - 11),&mask);
						mask |= 1 << (data->port - 11);
						macMT7530SetPortBrgInd(macMT7530LanPortMap2Switch(i - 11),mask);
						macMT7530GetPortBrgInd(macMT7530LanPortMap2Switch(data->port - 11),&mask);
						mask |= 1 << (i - 11);
						macMT7530SetPortBrgInd(macMT7530LanPortMap2Switch(data->port - 11),mask);
					}
					else
					{
						if((group1 == 0xffffffff || group2 == 0xffffffff) || (group1 != group2))
						{
							macMT7530GetPortBrgInd(macMT7530LanPortMap2Switch(i - 11),&mask);
							mask &= ~(1 << (data->port - 11));
							PONVLAN_PRINT(PONVLAN_MSG_TRACE,"set port mask,port is %d,mask is %x====>set user group",(i - 11),mask);
							macMT7530SetPortBrgInd(macMT7530LanPortMap2Switch(i - 11),mask);
							macMT7530GetPortBrgInd(macMT7530LanPortMap2Switch(data->port - 11),&mask);
							mask &= ~(1 << (i - 11));
							PONVLAN_PRINT(PONVLAN_MSG_TRACE,"set port mask,port is %d,mask is %x====>set user group",(data->port - 11),mask);
							macMT7530SetPortBrgInd(macMT7530LanPortMap2Switch(data->port - 11),mask);
						}
						else
						{
							macMT7530GetPortBrgInd(macMT7530LanPortMap2Switch(i - 11),&mask);
							mask |= 1 << (data->port - 11);
							macMT7530SetPortBrgInd(macMT7530LanPortMap2Switch(i - 11),mask);
							macMT7530GetPortBrgInd(macMT7530LanPortMap2Switch(data->port - 11),&mask);
							mask |= 1 << (i - 11);
							macMT7530SetPortBrgInd(macMT7530LanPortMap2Switch(data->port - 11),mask);
						}
					}
				}
			}
			return 0;
		}
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"Set default rule errror,can't find port ====> pon vlan user group opt");
		return -1;
	}
	else if(data->option_flag == OPT_CLEAN_ALL)
	{
		for(i = 0; i < pon_vlan_all_data.total_port_count; i++)
		{
			tmp = &pon_vlan_all_data.pon_vlan_data[i];
			tmp->user_group = 0xffffffff;
		}
		for(i = 0; i < 4; i++)
		{
			macMT7530SetPortBrgInd(macMT7530LanPortMap2Switch(i),0xff);
		}
		return 0;
	}
	PONVLAN_PRINT(PONVLAN_MSG_ERR,"option error ====> pon vlan user group opt");
	return -1;
}
static void pon_vlan_trace_pkt_info(pon_vlan_ioctl * data,void * arg)
{
    memset(&trace_pkt_info, 0, sizeof(pon_vlan_trace_drop));

    memcpy(&trace_pkt_info, &data->trace_pkt_info, sizeof(pon_vlan_trace_drop));

    printk("check mark:%d\n", trace_pkt_info.check_mark);
    printk("dst_mac:[%02x:%02x:%02x:%02x:%02x:%02x], src_mac:[%02x:%02x:%02x:%02x:%02x:%02x]\n",
            trace_pkt_info.dst_mac[0],trace_pkt_info.dst_mac[1],trace_pkt_info.dst_mac[2],trace_pkt_info.dst_mac[3],trace_pkt_info.dst_mac[4],trace_pkt_info.dst_mac[5],
            trace_pkt_info.src_mac[0],trace_pkt_info.src_mac[1],trace_pkt_info.src_mac[2],trace_pkt_info.src_mac[3],trace_pkt_info.src_mac[4],trace_pkt_info.src_mac[5]);
    printk("outer VID = %d, inner VID = %d\n", trace_pkt_info.outer_vid, trace_pkt_info.inner_vid);

    if(trace_pkt_info.check_mark != 0)
        DBG_Level |= PON_PKT_TRACE_FLAG;
    else
        DBG_Level &= (~PON_PKT_TRACE_FLAG);
}

int create_port_info(pon_vlan *vlan_data)
{
    vlan_data->up_rule_count = 0;
	vlan_data->down_rule_count = 0;
	vlan_data->down_stream_mode = INVERSE_MODE;
	vlan_data->enable_default_rule = ENABLE;
	vlan_data->input_tpid = ETH_P_8021Q;
	vlan_data->output_tpid = ETH_P_8021Q;
	vlan_data->rule_limit = VLAN_RULE_LIMIT;
	vlan_data->igmp_mode = 0;
	vlan_data->igmp_tci = 0;
	vlan_data->port_index = 0;
	memset(vlan_data->mac_vid,0,sizeof(mac_vid_pair)*MAX_MAC_VID);
	vlan_data->mac_bind_vlan_enable = ENABLE;
	vlan_data->user_group = 0xffffffff;//not in a group

	pon_vlan_set_downstream_mode_mask(vlan_data);
	
	memset(vlan_data->dscp_map,0,64);
	vlan_data->up_rule = kmalloc(sizeof(pon_vlan_rule) * vlan_data->rule_limit,GFP_ATOMIC);
	if(vlan_data->up_rule == NULL)
	{
		printk("malloc Pon_Up_Vlan_Rule error,out of memory\n");
		return -1;
	}
	memset(vlan_data->up_rule, 0, sizeof(pon_vlan_rule) * vlan_data->rule_limit);

	vlan_data->down_rule = kmalloc(sizeof(pon_vlan_rule) * vlan_data->rule_limit,GFP_ATOMIC);
	if(vlan_data->down_rule == NULL)
	{
		printk("malloc Pon_Down_Vlan_Rule error,out of memory\n");
		return -1;
	}
	memset(vlan_data->down_rule, 0, sizeof(pon_vlan_rule) * vlan_data->rule_limit);

	return 0;
}
int init_default_port(pon_vlan ** data, int port_start)
{
    int j;
    int ret = 0;
    pon_vlan * vlan_data = *data;
    
    if(vlan_data == NULL)
        return -1;

    ret = create_port_info(vlan_data);
    if(ret == -1)
    {
        printk("ponvlan init create port[%d] info fail\n", port_start);
        return -1;
    }
    vlan_data->port_index = port_start;
	vlan_data->up_rule_count = 3;
	vlan_data->down_rule_count = 3;
	PONVLAN_CREATE_TIMER(&vlan_data->resort_timer,pon_vlan_resort_rule_expires,(unsigned long)vlan_data,PONVLAN_RESORT_TIMER);
	for(j = 0; j < 3; j++)
	{
		//set default rule transparent
		vlan_data->up_rule[j].filter_outer_tpid = 0;
	    vlan_data->up_rule[j].filter_outer_dei = 2;
	    vlan_data->up_rule[j].filter_outer_pri = 8;
	    vlan_data->up_rule[j].filter_outer_vid = 4096;

	    vlan_data->up_rule[j].filter_inner_tpid = 0;
	    vlan_data->up_rule[j].filter_inner_dei = 2;
	    vlan_data->up_rule[j].filter_inner_pri = 8;
	    vlan_data->up_rule[j].filter_inner_vid = 4096;

	    vlan_data->up_rule[j].tag_num = j;
	    vlan_data->up_rule[j].filter_ethertype = 0;
	    vlan_data->up_rule[j].treatment_method = 0;

		vlan_data->down_rule[j].filter_outer_tpid = 0;
	    vlan_data->down_rule[j].filter_outer_dei = 2;
	    vlan_data->down_rule[j].filter_outer_pri = 8;
	    vlan_data->down_rule[j].filter_outer_vid = 4096;

	    vlan_data->down_rule[j].filter_inner_tpid = 0;
	    vlan_data->down_rule[j].filter_inner_dei = 2;
	    vlan_data->down_rule[j].filter_inner_pri = 8;
	    vlan_data->down_rule[j].filter_inner_vid = 4096;

	    vlan_data->down_rule[j].tag_num = j;
	    vlan_data->down_rule[j].filter_ethertype = 0;
	    vlan_data->down_rule[j].treatment_method = 0;
	}
	*data = vlan_data + 1;
	return 0;
}
int init_portInfo_by_portIndex(pon_vlan ** data, int port_start, int port_limit)
{
    int ret = 0;
    int i = 0;
    pon_vlan * vlan_data = *data;
    
    if(vlan_data == NULL)
        return -1;
    for(i = 0; i < port_limit; i++)
    {
        ret = create_port_info(vlan_data);
        if(ret == -1)
        {
            printk("ponvlan init create port[%d] info fail\n", port_start+i);
            return -1;
        }
	PONVLAN_CREATE_TIMER(&vlan_data->resort_timer,pon_vlan_resort_rule_expires,(unsigned long)vlan_data,PONVLAN_RESORT_TIMER);
        vlan_data->port_index = port_start+i;
        vlan_data++;
    }
    *data = vlan_data;
    return 0;
}

static int vlan_data_init(pon_vlan_all * data)
{
	pon_vlan * vlan_data = NULL;

	if(data == NULL)
	{
		printk("\r\ndata is NULL pointer,return -1 ====> vlan data init");
		return -1;
	}
	vlan_data = data->pon_vlan_data;

	init_default_port(&vlan_data, PONVLAN_PORT_OFFSET_DEFAULT); /* no default rules for epon sfu. */
	init_default_port(&vlan_data, PONVLAN_PORT_OFFSET_DEFGPON);/* only one port for deault rule, because only check the first port for default in 'handle_common_pkt'. */
	init_portInfo_by_portIndex(&vlan_data, PONVLAN_PORT_OFFSET_VP, data->virtual_port_count);
	init_portInfo_by_portIndex(&vlan_data, PONVLAN_PORT_START_ETH, data->lan_port_count);
	init_portInfo_by_portIndex(&vlan_data, PONVLAN_PORT_OFFSET_WLAN, data->wlan_port_count);

	if(TCSUPPORT_MULTI_USER_ITF_VAL || TCSUPPORT_MULTI_SWITCH_EXT_VAL)
	    init_portInfo_by_portIndex(&vlan_data, PONVLAN_PORT_OFFSET_WLANAC, data->wlan_ac_port_count);

	init_portInfo_by_portIndex(&vlan_data, PONVLAN_PORT_OFFSET_USB, data->usb_port_count);

	if(TCSUPPORT_PON_IP_HOST_VAL)
	    init_portInfo_by_portIndex(&vlan_data, PONVLAN_PORT_OFFSET_IPHOST, data->ipHost_port_count);
	
	return 0;
}
static int vlan_data_clean(pon_vlan_all * data)
{
	pon_vlan * vlan_data = NULL;
	int i = 0;
	
	if(data == NULL)
	{
		printk("\r\ndata is NULL pointer,return -1 ====> vlan data clean");
		return -1;
	}
	
	vlan_data = data->pon_vlan_data;
	
	for(i = 0; i < (data->total_port_count); i++)
	{
		del_timer(&vlan_data->resort_timer);
		kfree(vlan_data->up_rule);
		kfree(vlan_data->down_rule);
		vlan_data++;
	}

	kfree(data->pon_vlan_data);
	data->pon_vlan_data = NULL;

	return 0;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,35)
static struct file_operations ponvlan_fops = {
	.owner =		THIS_MODULE,
	.write =		NULL,
	.read =			NULL,
	.unlocked_ioctl =	ponvlan_ioctl,
#ifdef TCSUPPORT_CPU_ARMV8_64
	.compat_ioctl	= 	ponvlan_ioctl,
#endif
	.open =			ponvlan_open,
	.release =		NULL,
};	
#else
static struct file_operations ponvlan_fops = {
	.owner =		THIS_MODULE,
	.write =		NULL,
	.read =		NULL,
	.ioctl =		ponvlan_ioctl,
	.open =		ponvlan_open,
	.release =	NULL,
};
#endif

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,35)
long ponvlan_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
#else
int ponvlan_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg)
#endif
{
	pon_vlan_ioctl data;
	pon_vlan_epon_ioctl epon_rule;
	int ret = 0;
#ifdef TCSUPPORT_PON_VLAN_FILTER
	gponVlanFilterIoctl_t temp = {0};
	__u8 vlanFilterDbgLev = 0;
	__u8 multiEnableFlag = 0;
	gponVlanFilterIoctl_t dupFilterRule;
#endif

	if(filp == NULL)
	{
		printk("\r\nfilp is NULL return ====> pon vlan ioctl");
		return -1;
	}


#ifdef TCSUPPORT_CPU_ARMV8_64
		cmd = cmd & IOCTL_CMD;
#endif


	if(cmd == (PONVLAN_IOC_EPON_RULE_OPT))
	{
		memset(&epon_rule, 0, sizeof(epon_rule));
		if (copy_from_user(&epon_rule, (pon_vlan_epon_ioctl*)arg, sizeof(epon_rule)))
		{
			return -EFAULT;
		}
	}
	/*else if(cmd >= PONVLAN_IOC_ADD_VLAN_FILTER_ENTRY && cmd <= PONVLAN_IOC_VLAN_FILTER_DBG_LEVEL)
	{
		;//todo args copy.
	}*/
	else{
		memset(&data, 0, sizeof(data));
		if (copy_from_user(&data, (pon_vlan_ioctl*)arg, sizeof(data)))
		{
			return -EFAULT;
		}
	}
	switch(cmd)
	{
		case PONVLAN_IOC_SWITCH_OPT:
			ret = pon_vlan_switch_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_VEIP_SWITCH_OPT:
			ret = pon_vlan_veip_switch_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_DOWNSTREAM_MODE_OPT:
			ret = pon_vlan_downstream_mode_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_DOWNSTREAM_FORWARD_OPT:
			ret = pon_vlan_downstream_forward_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_TPID_OPT:
			ret = pon_vlan_tpid_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_DEFAULT_RULE_FLAG_OPT:
			ret = pon_vlan_default_rule_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_RULE_OPT:
			ret = pon_vlan_rule_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_DSCP_MAP_OPT:
			ret = pon_vlan_dscp_map_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_IGMP_VLAN_SWITCH_OPT:
			ret = pon_vlan_igmp_switch_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_IGMP_VLAN_MODE_OPT:
			ret = pon_vlan_igmp_mode_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_IGMP_VLAN_TCI_OPT:
			ret = pon_vlan_igmp_tci_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_IF_VLAN_SWITCH_OPT:
			ret = pon_vlan_if_vlan_switch_option(&data,(void*)arg);
			break;
			
		case PONVLAN_IOC_IF_VLAN_RULE_OPT:
			ret = pon_vlan_if_vlan_rule_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_PCP_MODE_OPT:
			ret = pon_vlan_pcp_mode_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_MAC_BIND_VID_SWITCH_OPT:
			ret = pon_vlan_mac_bind_vid_switch_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_MAC_BIND_VID_TIME_OPT:
			ret = pon_vlan_mac_bind_vid_time_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_USER_GROUP_SWITCH_OPT:
			ret = pon_vlan_user_group_switch_option(&data,(void*)arg);
			break;

		case PONVLAN_IOC_USER_GROUP_RULE_OPT:
			ret = pon_vlan_user_group_rule_option(&data,(void*)arg);
			break;
			
		case PONVLAN_IOC_DBG_LEVEL_OPT:
			if(data.dbg_level == 0x80)
			{
				if(pon_vlan_all_data.onu_mode == MODE_HGU)
					pon_vlan_all_data.onu_mode = MODE_SFU;
				else
					pon_vlan_all_data.onu_mode = MODE_HGU;
				printk("\r\nnow mode is %s",(pon_vlan_all_data.onu_mode == MODE_HGU)?"HGU":"SFU");
			}
			else if(data.dbg_level == 0) 
			    DBG_Level = 0;
			else
				DBG_Level |= data.dbg_level;
			printk("\r\nset dbg level success,now value is %x",DBG_Level);
			break;
		case PONVLAN_IOC_ENABLE_TRACE_DROP:
			pon_vlan_trace_pkt_info(&data, (void*)arg);
			break;
		case PONVLAN_IOC_EPON_RULE_OPT:
			epon_vlan_rule_option(&epon_rule);
			break;
		case PONVLAN_IOC_XPON_MODE_OPT:
			if (copy_from_user(&pon_vlan_all_data.xpon_mode, (pon_vlan_epon_ioctl*)arg, sizeof(__u16)))
			{
				return -EFAULT;
			}
			break;

#ifdef TCSUPPORT_PON_VLAN_FILTER
		/*vlan filter*/
		case PONVLAN_IOC_ADD_VLAN_FILTER_ENTRY:
			{
				COPY_FROM_USER(&temp,(gponVlanFilterIoctl_t *)arg,sizeof(gponVlanFilterIoctl_t),ret);
				memcpy(&dupFilterRule,&temp,sizeof(gponVlanFilterIoctl_t));
				ret  = addGponVlanFilterRuleInKernel(&temp);
				if(dupFilterRule.portType == 1){//ani port : add vlan filter rule to multicast gemport
				    int mulitcast_ani = 0;
				    ECNT_API_XPON_MULITCAST_ANI_GET(&mulitcast_ani);
					if(mulitcast_ani >=0 && mulitcast_ani < GPON_MAX_ANI_INTERFACE){
						dupFilterRule.port = mulitcast_ani;
						addGponVlanFilterRuleInKernel(&dupFilterRule);
					}
				}
			}
			break;
		case PONVLAN_IOC_GET_VLAN_FILTER_ENTRY:
			{
				COPY_FROM_USER(&temp,(gponVlanFilterIoctl_t *)arg,sizeof(gponVlanFilterIoctl_t),ret);
				ret  = getGponVlanFilterRuleInKernel(&temp);
				COPY_TO_USER((gponVlanFilterIoctl_t *)arg,&temp,sizeof(gponVlanFilterIoctl_t),ret);
			 }
			 break;
		case PONVLAN_IOC_DEL_VLAN_FILTER_ENTRY:
			{
				COPY_FROM_USER(&temp,(gponVlanFilterIoctl_t *)arg,sizeof(gponVlanFilterIoctl_t),ret);
				if(temp.cleanFlag == 1)
					cleanGponVlanFilterRuleInKernel();
				else{
					delGponVlanFilterRuleInKernel(temp.port, temp.portType);
					if(temp.portType == 1){ //ani port: delete vlan filter rule to multicast gemport
					    int mulitcast_ani = 0;
				        ECNT_API_XPON_MULITCAST_ANI_GET(&mulitcast_ani);
						if(mulitcast_ani >=0 && mulitcast_ani < GPON_MAX_ANI_INTERFACE){
							delGponVlanFilterRuleInKernel(mulitcast_ani, 1);
						}
					}
				}
				ret = 0;
			}
			break;
		case PONVLAN_IOC_DISP_VLAN_FILTER_ENTRY:
			displayAllGponVlanFilterRuleInKernel();
			ret = 0;
			break;
		case PONVLAN_IOC_VLAN_FILTER_DBG_LEVEL:
			{
				COPY_FROM_USER(&vlanFilterDbgLev,(__u8 *)arg,sizeof(__u8),ret);
				ret  = setGponVlanFilterDbgLeverInKernel(vlanFilterDbgLev);
			}			
			break;
        case PONVLAN_IOC_VLAN_MULT_FILTER_ENABLE:
			{			
				COPY_FROM_USER(&multiEnableFlag,(__u8 *)arg,sizeof(__u8),ret);
				printk("Old pon vlan multicast filter enable is %d\r\n",pon_vlan_all_data.multi_filter_enable);
				pon_vlan_all_data.multi_filter_enable = multiEnableFlag;
				printk("Now set pon vlan multicast filter enable to %d\r\n",pon_vlan_all_data.multi_filter_enable);
			}
			break;
		case PONVLAN_IOC_UNI_FILTER_SWITCH_OPT:
			ret = pon_vlan_uni_filter_switch_option(&data,(void*)arg);
			break;
#endif			
		case PONVLAN_IOC_ADD_GPON_ANI_MAP_OPT:
		case PONVLAN_IOC_GET_GPON_ANI_MAP_OPT:
		case PONVLAN_IOC_DEL_GPON_ANI_MAP_OPT:
		case PONVLAN_IOC_CLEAN_GPON_ANI_MAP_OPT:
		case PONVLAN_IOC_DISP_GPON_ANI_MAP_OPT:
		case PONVLAN_IOC_GPON_ANI_MAP_DBG_LEVEL:
		case PONVLAN_IOC_ENABLE_GPON_ANI_MAP:
			ret = gpon_ani_opt_dispatch(cmd,(void*)arg);
			break;
		case PONVLAN_IOC_SET_HYBRID_ENABLE:
		case PONVLAN_IOC_SET_HYBRID_SFU_BR_WAN:
		case PONVLAN_IOC_SET_HYBRID_SFU_LAN_ADD:
		case PONVLAN_IOC_SET_HYBRID_SFU_LAN_DEL:
		case PONVLAN_IOC_SET_HYBRID_DISP:
		case PONVLAN_IOC_SET_HYBRID_RESET:
		case PONVLAN_IOC_GET_HYBRID_PORT_MASK:
			ret = pon_vlan_hybrid_mode_ioctl(cmd,&data,(void*)arg);
			break;
		case PONVLAN_IOC_SET_DS_BCAST_1_TO_N_OPT:
			if(data.ds_bcast_1toN_enable == 0)
				pon_vlan_all_data.ds_bcast_1toN_flag = DISABLE;
			else if(data.ds_bcast_1toN_enable == 1)
				pon_vlan_all_data.ds_bcast_1toN_flag = ENABLE;
			printk("ds_bcast_1toN_flag is %d (0:Disable, 1:Enable)\n",pon_vlan_all_data.ds_bcast_1toN_flag);
			break;
		case PONVLAN_IOS_RESORT_FLAG :
			if(data.resort_enable == ENABLE)
				pon_vlan_all_data.resort_enable_flag = ENABLE;
			else if(data.resort_enable == DISABLE)
				pon_vlan_all_data.resort_enable_flag = DISABLE;
			printk("current resorr enable flag is %s\n",(ENABLE == pon_vlan_all_data.resort_enable_flag) ? "enable" :"disable");
			break;
		default:
			ret = -1;
			printk("\r\ncmd error  ========> Pon Vlan ioctl");
			break;
	}
	return ret;
}

int ponvlan_open(struct inode *inode, struct file *filp)
{
	return 0;
}

int pon_vlan_create_timer(struct timer_list *timer, ponvlanTimerCallback callback, unsigned long param,unsigned long expire)
{
	if((timer == NULL) ||( callback == NULL) ){
		return -1;
	}
	
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0) 
	setup_timer(timer, callback, param);
#else
	timer_setup(timer, callback, 0);
#endif
	timer->expires= expire;

	return 0;
}

int pon_vlan_check_tpid(__u16 skb_tpid, __u16 filter_tpid)
{

	switch(filter_tpid)
	{
		case FILTER_TPID_DO_NOT_CARE:
            	return 1;
			break;

		case FILTER_TPID_8100:
			if(skb_tpid == ETH_P_8021Q)
				return 1;
			break;
            
		default:
			return 0;
	}	

    return 0;
}


int pon_vlan_get_down_vlan_opt(int port,unsigned int src_vlan,int *mode, int *vlan, int *pri)
{
	int i = 0;
	pon_vlan * vlan_data = NULL;
    __u8  rule_port = 0;
    pon_vlan_rule * rule = NULL;
    __u16  tpid = (src_vlan >> 16) & 0xFFFF;
    __u8   pbit = (src_vlan & 0x0000E000) >> 13;
    __u16  vlan_id = (src_vlan & 0x00000FFF);
    __u16  tci = (src_vlan & 0x0000FFFF);
 	int matchFlag=0;
	
    PONVLAN_PRINT(PONVLAN_MSG_TRACE,"port %d SRC_VLAN %x,tpid %x tci %x  pbit %d vlan_id %d",port,src_vlan,tpid,tci,pbit,vlan_id);
	for(i = 1; i < pon_vlan_all_data.total_port_count; i++)
	{
		rule_port = pon_vlan_all_data.pon_vlan_data[i].port_index;
        PONVLAN_PRINT(PONVLAN_MSG_TRACE,"frame port is %d,vlan rule port is %d, lan port is %d",port,rule_port,(rule_port % 10));
		if(rule_port== port)
        {
            PONVLAN_PRINT(PONVLAN_MSG_TRACE,"matched port, get down rule ");
			vlan_data = &pon_vlan_all_data.pon_vlan_data[i];
            break;
        }
    }

	if(vlan_data == NULL)
    {
        PONVLAN_PRINT(PONVLAN_MSG_ERR,"there is no rule in this port return");
        return -1;
    }

    for(i = 0; i < vlan_data->down_rule_count; i++)
    {
    	rule = &vlan_data->down_rule[i];
    	if(rule->tag_num == 1)
        {
            PONVLAN_PRINT(PONVLAN_MSG_TRACE,"filter inner info %d %d %d",rule->filter_inner_tpid,rule->filter_inner_pri,rule->filter_inner_vid);
            if(vlan_id == rule->filter_inner_vid)
			{
            	if(matchFlag==0 || rule->treatment_method ==31)
            	{
            		//untag rule is prioritest, so overwrite it
            		matchFlag = 1;
	            	*vlan = rule->add_fst_vid;
					*pri = rule->add_fst_pri;
	                *mode = rule->treatment_method;
	                PONVLAN_PRINT(PONVLAN_MSG_TRACE,"pon vlan down match rule, method is %d vlan is %d, pri=%d",*mode,*vlan,*pri);
	            }
            }
        }
    }
    PONVLAN_PRINT(PONVLAN_MSG_WARNING,"matchFlag=%d\n",matchFlag);
	if(matchFlag == 1)
		return VLAN_SUCCESS;
	else
		return VLAN_FAILURE;
}

int pon_vlan_get_down_rule_info(VLAN_Down_opt_t * down_info)
{
	int mode = 0;
	int vlan = 0;
	int pri = 0;
    int ret = 0;
    
	ret = pon_vlan_get_down_vlan_opt(down_info->port,down_info->src_vlan,&mode,&vlan,&pri);
    if(ret == VLAN_SUCCESS)
    {
    	down_info->mode = mode;
        down_info->vlan = vlan;
    }

    return ret;
}

int pon_vlan_get_down_tci_info(VLAN_Down_opt_t * down_info)
{
	int mode = 0;
	int vlan = 0;
	int pri = 0;
	int ret = VLAN_SUCCESS;

	if(down_info == NULL)
		return VLAN_FAILURE;
		
	ret = pon_vlan_get_down_vlan_opt(down_info->port,down_info->src_vlan,&mode,&vlan,&pri);
    if(ret == VLAN_SUCCESS)
    {
    	down_info->mode = mode;
        down_info->vlan = vlan;
		down_info->pri = pri;
    }

    return ret;
}

int pon_vlan_get_down_uni(struct sk_buff *skb){
	pon_vlan * vlan_data = NULL;
	ds_vlan_info vlan_info;
	int i,j = 0;
	int tp_uni = -1;
	

	if(skb == NULL)
    {
        PONVLAN_PRINT(PONVLAN_MSG_ERR,"skb is NULL");
        return -1;
    }

	if(ds_store_tag_info(skb, &vlan_info) == -1){
		return -1;
	}
	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****pon_tag_num = %d",vlan_info.pon_tag_num);
	for(i = 0; i < vlan_info.pon_tag_num; i++){
		PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****tci = %d, tpid = %d",vlan_info.pon_vlan_tci[i],vlan_info.pon_vlan_tpid[i]);
	}
	PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****ethertype = %d",vlan_info.ethertype);
	
    for(i = 1; i <= MAX_ECNT_ETHER_PORT_NUM; i++)
	{
		vlan_data = get_pon_vlan_by_port(i+PONVLAN_PORT_OFFSET_ETH);
		
		if(vlan_data != NULL){
			PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****uni = %d ponvlan_port = %d",i,vlan_data->port_index);
			if(vlan_data->down_stream_mode == INVERSE_MODE){
				for(j = 0; j < vlan_data->down_rule_count; j++){
					PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****uni = %d, rule_idx = %d",i,j);
					
					if(ds_match_rule_ext(&vlan_info,vlan_data,j,1) > 0){
						PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****ds strict match uni = %d, rule_idx = %d",i,j);
						return i;
					}
					if(tp_uni == -1 && ds_match_rule_ext(&vlan_info,vlan_data,j,0) > 0){
						PONVLAN_PRINT(PONVLAN_MSG_WARNING,"****ds unstrict match uni = %d, rule_idx = %d",i,j);
						tp_uni = i;
					}
				}
			}
		}
	}
	
	return tp_uni;
}


static void pon_vlan_get_api_dispatch(xpon_vlan_api_data_t * api_data)
{
	int uniport = -1;
	api_data->ret = VLAN_SUCCESS;
    
    switch(api_data->cmd_id)
    {
        case PON_GET_VLAN_DOWN_OPT:
            api_data->ret = pon_vlan_get_down_rule_info(api_data->vlan_down_opt);
            break;
        case GET_DOWN_TCI_FOR_MCAST_OPT:
            api_data->ret = pon_vlan_get_down_tci_info(api_data->vlan_down_opt);
            break;	
		case PON_GET_VLAN_DOWN_UNI:
			uniport = pon_vlan_get_down_uni(api_data->skb);
			
			if(uniport == -1){
				api_data->uni= -1;
                api_data->ret = VLAN_FAILURE;
			}else{
				api_data->uni= uniport;
                api_data->ret = VLAN_SUCCESS;
			}
            break;
        default:
            dump_stack();
            printk("unknown command id!\n");
            api_data->ret = VLAN_NO_API;
            break;
    }
}

static void pon_vlan_set_api_dispatch(xpon_vlan_api_data_t * api_data)
{
    api_data->ret = VLAN_SUCCESS;
    
    switch(api_data->cmd_id)
    {		
        case MATCH_VLAN_FILTER_RULE_OP:
            api_data->ret = matchVlanFilterRuleOp(api_data->Op.port, api_data->Op.portType, api_data->Op.vlan_type, api_data->Op.vlan_tag, api_data->Op.dir);
            break;

        default:
            dump_stack();
            printk("unknown command id!\n");
            api_data->ret = VLAN_NO_API;
            break;
    }
}

int pon_vlan_api_dispatch(struct ecnt_data *in_data)
{
    xpon_vlan_api_data_t * api_data = NULL;

	if(in_data == NULL)
		return ECNT_HOOK_ERROR;
	
	api_data = (xpon_vlan_api_data_t *)in_data;
    switch(api_data->api_type) 
    {
        case XPON_VLAN_API_TYPE_GET:
            pon_vlan_get_api_dispatch(api_data);
            break;

        case XPON_VLAN_API_TYPE_SET:
            pon_vlan_set_api_dispatch(api_data);
            break;

        default:
            dump_stack();
            printk("unknown api_data->api_type: %d\n", api_data->api_type);
            api_data->ret = VLAN_NO_API;
            break;
    }
    
    return ECNT_RETURN;
}

struct ecnt_hook_ops vlan_api_dispatch_hook_ops = {
    .name = "pon_vlan_api_dispatch",
    .hookfn = pon_vlan_api_dispatch,
    .is_execute = 1,
    .maintype = ECNT_XPON_VLAN,
    .subtype = ECNT_XPON_VLAN_API,
    .priority   = 1,
};

static void init_pon_vlan_macro_compatible(void)
{
	pon_vlan_dev_offset         = dev_offset_macro_compatible();
	pon_vlan_wlan_dev_offset    = wlan_dev_offset_macro_compatible();
	pon_vlan_wlan_ac_dev_offset = wlan_ac_dev_offset_macro_compatible();
	pon_vlan_usb_dev_offset     = usb_dev_offset_macro_compatible();
	
	return ;
}

static int hybrid_port_mask_read_proc(char *page, char **start, off_t off,int count, int *eof, void *data)
{
		printk("hybrid_port_mask: %x\n",hy_port_mask);
		return 0;
}

static int hybrid_update_port_type(unsigned int old_mask,unsigned int new_mask)
{
	int i,j,ret = 0;
	unsigned int tmp = 0;
	pon_vlan_ioctl data = {0};
	pon_vlan *tmp_vlan_rule = NULL;
	
	for(i=0; i<HYBRID_LAN_COUNT_MAX; i++)
	{
		tmp = (old_mask ^ new_mask) & (0x01 << i);
		if(tmp != 0)
		{
			snprintf(data.hy_sfu_lan,PON_VLAN_ITF_NAME_SIZE,"eth0.%d",i+1);
			if(old_mask & (0x01 << i))  //from sfu port to hgu port
			{
				printk("change %s from sfu to hgu\n",data.hy_sfu_lan);
				ret = pon_vlan_hybrid_mode_ioctl(PONVLAN_IOC_SET_HYBRID_SFU_LAN_DEL,&data, NULL);
			}
			else ////from hgu port to sfu port
			{
				tmp_vlan_rule = get_pon_vlan_by_port(VLAN_LAN_PORT_ID_BASE + i);
				if(tmp_vlan_rule == NULL)
					continue;
				
				if(tmp_vlan_rule->up_rule_count == 0)
					continue;

				for(j=0; j<tmp_vlan_rule->up_rule_count; j++)
				{
					if((tmp_vlan_rule->up_rule[j].treatment_method > 20 && tmp_vlan_rule->up_rule[j].treatment_method < 43)
						|| (tmp_vlan_rule->up_rule[j].treatment_method == 0))  //has valid vlan rule
					{
						printk("change %s from hgu to sfu\n",data.hy_sfu_lan);
						ret = pon_vlan_hybrid_mode_ioctl(PONVLAN_IOC_SET_HYBRID_SFU_LAN_ADD,&data,NULL);
						break;
					}
				}

			}
		}
	}
	
	return ret;
}

static int hybrid_port_mask_write_proc(struct file *file, const char *buffer, unsigned long count, void *data)
{
	char val_string[8]={0};
	unsigned int tmp=0;
	unsigned int old_val=0;
	unsigned int action=0;

	if (count > sizeof(val_string) - 1)
		return -EINVAL ;
	
	if (copy_from_user(val_string, buffer, count))
		return -EFAULT ;	

	old_val = hy_port_mask;
	if(val_string[0] == '0')
	{
		hy_port_mask = tmp;
		printk("set hy_port_mask: all disable success!\n");
	}
	else 
	{
		tmp = atoi(val_string); 
		if(tmp > 0 && tmp < 16)
		{
			hy_port_mask = tmp;
			printk("set hy_port_mask: %d  success!\n",hy_port_mask);
		}
		else
			printk("the value is beyond the bitmask of 4 lan ports!\n");
	}

	if((old_val == 0 && hy_port_mask != 0)
		|| (old_val != 0 && hy_port_mask == 0))
	{
		action = HYBRID_ACTION_CHANGE_MODE;
	}
	else if(old_val != hy_port_mask){
		action = HYBRID_ACTION_CHANGE_PORT;
	}
	else //no change
	{
		action = 0xFF; //unused
	}

	switch(action)
	{
		case HYBRID_ACTION_CHANGE_MODE:
			printk("change mode\n");
			hybrid_update_port_type(old_val,hy_port_mask);
			//system("echo gpon startup 1 > /proc/gpon/debug"); //re-online
			break;
		case HYBRID_ACTION_CHANGE_PORT:
			hybrid_update_port_type(old_val,hy_port_mask);
			break;
		default :
			break;
	}

	return count;
}

/* in hybrid mode, include hgu port(veip) and sfu port(pptp),
this function is to  check if the packet from or to sfu port */
int pon_check_hybrid_sfu_lan(struct sk_buff **pskb)
{	
	struct sk_buff *skb = NULL;
	struct net_device *out_dev = NULL;
	int  i;
	char tmp[8]={0};

	if(hy_enable != 1)
	{
		return 0;
	}

	if(pskb == NULL)
	{
		printk("[%s][%d]pskb is NULL pointer,return 0\n", __FUNCTION__, __LINE__);
		return 0;
	}
	if(*pskb == NULL)
	{
		printk("[%s][%d]*pskb is NULL pointer,return 0\n", __FUNCTION__, __LINE__);
		return 0;
	}

	skb = *pskb;
	out_dev = skb->dev;
	if(out_dev == NULL)
	{
		printk("\r\n[%s][%d] skb->dev is NULL ", __FUNCTION__, __LINE__);
		return 0;
	}

	memcpy(tmp,out_dev->name,6);
	PONVLAN_PRINT(PONVLAN_MSG_WARNING," dev=%s ",tmp);

	for(i = 0; i < hy_lan_count; i++)
	{
		if(strcmp(out_dev->name, hy_lan[i]) == 0)
		{
			PONVLAN_PRINT(PONVLAN_MSG_TRACE,"return 1");
			return 1;
		}
	}

	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"return 0");
	return 0;
}

/* in hybrid mode, there is  a  bridge wan for sfu port pkt forwarder, otherwise, sfu pkt will be discard
this function is to  check if the packet fwd by this bridge wan*/
int pon_check_hybrid_sfu_fwd_wan(struct sk_buff **pskb)
{
	struct sk_buff *skb = NULL;
	struct net_device *out_dev = NULL;
	char tmp[8]={0};

	if(hy_enable != 1)
	{
		return 0;
	}
	
	if(pskb == NULL)
	{
		printk("[%s][%d]pskb is NULL pointer,return 0\n", __FUNCTION__, __LINE__);
		return 0;
	}
	if(*pskb == NULL)
	{
		printk("[%s][%d]*pskb is NULL pointer,return 0\n", __FUNCTION__, __LINE__);
		return 0;
	}

	skb = *pskb;
	out_dev = skb->dev;
	if(out_dev == NULL)
	{
		printk("\r\n[%s][%d] skb->dev is NULL ", __FUNCTION__, __LINE__);
		return 0;
	}
	
	memcpy(tmp,out_dev->name,7);
	PONVLAN_PRINT(PONVLAN_MSG_WARNING," dev=%s ",tmp);

	if(strcmp(out_dev->name, hy_br_wan) == 0)
	{
		PONVLAN_PRINT(PONVLAN_MSG_TRACE,"return 1");
		return 1;
	}

	PONVLAN_PRINT(PONVLAN_MSG_TRACE,"return 0");
	return 0;
}

static void notify_hrybrid_to_other_modes(char *portName, int portType)
{
	printk("%s: %s %d\n",__FUNCTION__,portName,portType);
	//notify xpon_igmp mode
	XPON_IGMP_API_SET_ONE_PORT_TYPE_OPT(portName,portType);

	//notify kernel multicast modues
	ECNT_HOOK_MC_API_SET_PORT_TYPE(portName,portType);

	return;
}
int pon_vlan_hybrid_mode_ioctl(int cmd, pon_vlan_ioctl *data, void* arg)
{
	int ret=0;
	int is_found = 0;
	int i;

	if(data == NULL)
		return -1;
	
	if(!TCSUPPORT_PON_SFU_HGU_HYBRID_VAL || pon_vlan_all_data.onu_mode != MODE_HGU){
		printk("not support this command\n");
		return 0;
	}
	
	switch(cmd)
	{
		case PONVLAN_IOC_SET_HYBRID_ENABLE:
			if(data->hy_enable == 0 || data->hy_enable == 1)
			{
				hy_enable = data->hy_enable;
				ret = 0;
			}
			else{
				ret = -1;
			}

			if(hy_enable == 0){
				for(i = 0; i < hy_lan_count; i++){
					notify_hrybrid_to_other_modes(data->hy_sfu_lan,XPON_HYBRID_MODE_PORT_TYPE_VEIP);
				}
			}
			printk("hybrid enable :%d\n", hy_enable);
			break;
		case PONVLAN_IOC_SET_HYBRID_SFU_BR_WAN:
			strcpy(hy_br_wan, data->hy_sfu_br_wan);
			XPON_IGMP_API_HYBRID_BRIDGE_WAN(hy_br_wan);
			ret = 0;
			printk("hybrid wan :%s\n", hy_br_wan);
			break;
		case PONVLAN_IOC_SET_HYBRID_SFU_LAN_ADD:
			is_found = 0;
			if(hy_lan_count >= 0 && hy_lan_count < HYBRID_LAN_COUNT_MAX)
			{
				for(i = 0; i < hy_lan_count; i++){
					if(!strcmp(hy_lan[i], data->hy_sfu_lan))
					{
						is_found = 1;
						break;
					}
				}
				if(is_found == 0)
				{
					strcpy(hy_lan[hy_lan_count++], data->hy_sfu_lan);
					notify_hrybrid_to_other_modes(data->hy_sfu_lan,XPON_HYBRID_MODE_PORT_TYPE_PPTP);
				}
				ret = 0;
				printk("hybrid lan added success!\n");
			}
			else{
				ret = -1;
			}
			break;
		case PONVLAN_IOC_SET_HYBRID_SFU_LAN_DEL:
			is_found = 0;
			for(i = 0; i < hy_lan_count; i++){
				if(is_found == 1)
				{
					strcpy(hy_lan[i-1], hy_lan[i]);
				}
				else if(!strcmp(hy_lan[i], data->hy_sfu_lan))
				{
					is_found = 1;
				}
			}
			if(is_found == 1){
				memset(hy_lan[hy_lan_count],0,PON_VLAN_ITF_NAME_SIZE);
				hy_lan_count--;
				notify_hrybrid_to_other_modes(data->hy_sfu_lan,XPON_HYBRID_MODE_PORT_TYPE_VEIP);
				ret = 0;
				printk("hybrid lan deleted success!\n");
			}
			else{
				ret = -1;
			}
			break;
		case PONVLAN_IOC_SET_HYBRID_DISP:
			printk("\r\nhybrid enable:%d, lan count:%d, wan:%s\n",hy_enable, hy_lan_count, hy_br_wan);
			for(i = 0; i < hy_lan_count; i++)
			{
				printk("hybrid lan[%d]:%s\n", i, hy_lan[i]);
			}
			break;
		case PONVLAN_IOC_SET_HYBRID_RESET:
			hy_enable = 0;
			hy_lan_count = 0;
			memset(hy_br_wan, 0, sizeof(hy_br_wan));
			memset(hy_lan, 0, sizeof(hy_lan));
			for(i = 0; i < hy_lan_count; i++){
				notify_hrybrid_to_other_modes(data->hy_sfu_lan,XPON_HYBRID_MODE_PORT_TYPE_VEIP);
			}
			break;
		case PONVLAN_IOC_GET_HYBRID_PORT_MASK: 
			if(arg == NULL)
				return -1;
			
			data->hy_port_mask = hy_port_mask;
			if (copy_to_user((void __user *)arg, data, sizeof(pon_vlan_ioctl)) == -1)
			{
				PONVLAN_PRINT(PONVLAN_MSG_ERR,"copy to user error ====> get Hybrid port mask error\r\n");
				return -1;
			}
			printk("hy_port_mask = 0x%x\n",data->hy_port_mask);
			break;
	}

	return ret;
}
/*********************************************************************
Function:       
Input parameter :  index, mean the num of rule which matched, 1....N
Ouput parameter : 0,     fail
				  1,    handle success
*********************************************************************/
int pon_vlan_is_ds_1_to_N(struct sk_buff **pskb, int *count)
{
	pon_vlan * vlan_data = NULL;
	struct sk_buff *skb = NULL;
	int i=0,cnt=0;
	int ret=0;

	if(pskb == NULL || count == NULL)
	{
		printk("\r\npskb is NULL pointer,return -1 ====> pon insert tag");
		return 0;
	}
	if(*pskb == NULL)
	{
		printk("\r\npskb is NULL pointer,return -1 ====> pon insert Tag");
		return 0;
	}
	
	skb = *pskb;
	if(pon_vlan_all_data.vlan_enable_flag == DISABLE   //ponvlan disable
		||	pon_vlan_all_data.onu_mode == MODE_SFU	//sfu
		||	pon_vlan_all_data.veip_enable_flag == DISABLE //veip disable
		||	pon_vlan_all_data.ds_bcast_1toN_flag == DISABLE) //ds bcast 1toN disable
	{
		return 0;
	}

	skb->data -= 14;
	if(store_tag_info(skb) == -1)
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"store_tag_info failed!");
		ret =0;
		goto restore_skb;
	}
	if(skb->pon_tag_num > 0)
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"pon_tag_num 0, tci=%x",skb->pon_vlan_tci[0]);
	}
	if(skb->pon_tag_num > 1)
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"pon_tag_num 1, tci=%x",skb->pon_vlan_tci[1]);
	}

	vlan_data = get_pon_vlan_by_port(PONVLAN_PORT_OFFSET_VP);
	if(vlan_data == NULL){
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"get_pon_vlan_by_port failed!");
		ret =0;
		goto restore_skb;
	}

	for(i = vlan_data->down_rule_count -1; i >= 0; i--) //search from tail
	{
		if(match_rule(skb, vlan_data, DOWNSTREAM,i) == 1)
		{
			cnt ++;
		}
	}

	if(cnt> 1){
		*count = cnt;
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"return ture, count=%d",*count);
		ret =1;
	}
	else{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"return false");
		ret = 0;
	}

restore_skb:
	for( i = 0; i < skb->pon_tag_num; i++ )
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR, "vlan tag index is %d, tpid is %x, tci is %x", i, skb->pon_vlan_tpid[i], skb->pon_vlan_tci[i]);
		skb = __pon_vlan_put_tag(skb, skb->pon_vlan_tpid[i], skb->pon_vlan_tci[i]);
		if( skb == NULL )
		{
			ret = 0;
			return ret;
		}
	}
	
	skb->data += 14;
	return ret;
	
}


/*********************************************************************
Function:       
Input parameter :  index, mean the num of rule which matched, 1....N
Ouput parameter : 0,     fail
				  1,    handle success
*********************************************************************/
int pon_vlan_ds_1_to_N_handler(struct sk_buff **pskb, int index)
{
	pon_vlan * vlan_data = NULL;
	struct sk_buff *skb = NULL;
	int i=0,j=0;
	original_tag otag = {0};
	int ret=0;

	if(pskb == NULL)
	{
		printk("\r\npskb is NULL pointer,return -1 ====> pon insert tag");
		return 0;
	}
	if(*pskb == NULL)
	{
		printk("\r\npskb is NULL pointer,return -1 ====> pon insert Tag");
		return 0;
	}

	skb = *pskb;
	if(pon_vlan_all_data.vlan_enable_flag == 0	 //ponvlan disable
		||	pon_vlan_all_data.onu_mode == MODE_SFU	//sfu
		||	pon_vlan_all_data.veip_enable_flag == DISABLE //veip disable
		||	pon_vlan_all_data.ds_bcast_1toN_flag == DISABLE)  //ds bcast 1toN disable 
	{
		return 0;
	}

	skb->data -= 14;
	if(store_tag_info(skb) == -1)
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"store_tag_info failed!");
		ret =0;
		goto handle_end;
	}

	otag.tag_num = skb->pon_tag_num;
	if(skb->pon_tag_num > 0)
	{
		otag.inner_tpid = skb->pon_vlan_tpid[0];
		otag.inner_tci = skb->pon_vlan_tci[0];
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"pon_tag_num 0, tci=%x",otag.inner_tci);
	}
	if(skb->pon_tag_num > 1)
	{
		otag.outer_tpid= skb->pon_vlan_tpid[1];
		otag.outer_tci = skb->pon_vlan_tci[1];
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"pon_tag_num 1, tci=%x",otag.outer_tci);
	}

	PONVLAN_PRINT(PONVLAN_MSG_ERR,"enter index=%d",index); 

	vlan_data = get_pon_vlan_by_port(PONVLAN_PORT_OFFSET_VP);
	if(vlan_data == NULL){
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"get_pon_vlan_by_port failed!");
		ret =0;
		goto handle_end;
	}

	for(i = vlan_data->down_rule_count -1; i >= 0; i--) //search from tail
	{
		if(match_rule(skb, vlan_data, DOWNSTREAM,i) == 1)
		{
			j++;
			if(j != index)
				continue;
		}
		else
			continue;

		PONVLAN_PRINT(PONVLAN_MSG_ERR,"match rule index %d",i); 	
		if(do_option(skb, vlan_data, DOWNSTREAM, i, otag,0) == -1)
		{
			PONVLAN_PRINT(PONVLAN_MSG_ERR,"Do Option Error,drop packet ====>Insert Tag");
		}

		break;
	}
	
	for(i = 0; i < skb->pon_tag_num; i++)
	{
		PONVLAN_PRINT(PONVLAN_MSG_ERR,"vlan tag index is %d,tpid is %x,tci is %x",i,skb->pon_vlan_tpid[i],skb->pon_vlan_tci[i]);
		skb = __pon_vlan_put_tag(skb, skb->pon_vlan_tpid[i], skb->pon_vlan_tci[i]);
		if (skb == NULL)
		{
			ret =0;
			goto handle_end;
		}
	}
	skb->pon_vlan_flag |= PON_PKT_INSERT_FLAG;

handle_end:
	if(skb != NULL)
		skb->data += 14;

	PONVLAN_PRINT(PONVLAN_MSG_ERR,"return 1");
	return 1;
}

static int  pon_vlan_init(void)
{
	int i = 0,status = 0;
	struct proc_dir_entry *hybrid_port_mask_proc = NULL;
		
	printk("%s\n", __FUNCTION__);
	init_pon_vlan_macro_compatible();
	
#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_CY_E8_SFU)
	pon_vlan_all_data.vlan_enable_flag = DISABLE;
#else/*TCSUPPORT_COMPILE*/
	pon_vlan_all_data.vlan_enable_flag = ENABLE;
#endif/*TCSUPPORT_COMPILE*/
	pon_vlan_all_data.resort_enable_flag = ENABLE;
    if (TCSUPPORT_CT_PON_SN_VAL|| TCSUPPORT_VNPTT_VAL||TCSUPPORT_CDS_VAL || TCSUPPORT_ALPHION_PON_VAL) {
		pon_vlan_all_data.veip_enable_flag = ENABLE; 
	}
	else {
	    pon_vlan_all_data.veip_enable_flag = DISABLE; 
	} 
	
	pon_vlan_all_data.igmp_enable_flag = DISABLE;
	pon_vlan_all_data.if_vlan_bind_enable_flag = DISABLE;
	pon_vlan_all_data.user_group_enable_flag = ENABLE;
    pon_vlan_all_data.multi_filter_enable = PON_VLAN_MULT_FILTER_DEFAULT;
	pon_vlan_all_data.uni_filter_enable_flag = ENABLE;
	pon_vlan_all_data.ds_bcast_1toN_flag = DISABLE;

    ECNT_API_XPON_ONU_TYPE_GET(&i);
	if(i == 1)
		pon_vlan_all_data.onu_mode = MODE_SFU;
	else
		pon_vlan_all_data.onu_mode = MODE_HGU;
	
	if(TCSUPPORT_CUC_VAL){
		if(pon_vlan_all_data.onu_mode == MODE_HGU)	
			pon_vlan_all_data.vlan_enable_flag = DISABLE;
	}
	
	pon_vlan_all_data.virtual_port_count = 1;
	
	
	printk("\r\nMulti Lan port");
	if(TCSUPPORT_MULTI_SWITCH_EXT_VAL){
		pon_vlan_all_data.lan_port_count = 8;
	}else{
		pon_vlan_all_data.lan_port_count = MAX_ECNT_ETHER_PORT_NUM;
	}

	
	if(TCSUPPORT_MULTI_USER_ITF_VAL || TCSUPPORT_MULTI_SWITCH_EXT_VAL)
	    pon_vlan_all_data.wlan_ac_port_count = MAX_ECNT_WALNAC_PORT_NUM;  	  	
	else
    	pon_vlan_all_data.wlan_ac_port_count = 0;

    pon_vlan_all_data.wlan_port_count = MAX_ECNT_WALN_PORT_NUM;	
	pon_vlan_all_data.usb_port_count  = MAX_ECNT_USB_PORT_NUM; 
	
    if(TCSUPPORT_PON_IP_HOST_VAL)
		pon_vlan_all_data.ipHost_port_count = 1;
    else{
        pon_vlan_all_data.ipHost_port_count = 0;
	}
	
	
	pon_vlan_all_data.total_port_count = pon_vlan_all_data.virtual_port_count + pon_vlan_all_data.lan_port_count \
	                                        + pon_vlan_all_data.wlan_port_count + pon_vlan_all_data.usb_port_count \
	                                        + pon_vlan_all_data.wlan_ac_port_count + pon_vlan_all_data.ipHost_port_count;

	pon_vlan_all_data.total_port_count += 2; // 2 default rule port.
	
	
	pon_vlan_all_data.total_special_tpid = 16;
	pon_vlan_all_data.tpid_counter = 0;

	pon_vlan_all_data.pcp_mode = 0;

	pon_vlan_all_data.mac_vlan_time = 300;

	pon_vlan_all_data.pon_vlan_type = kmalloc(sizeof(struct packet_type) * (pon_vlan_all_data.total_special_tpid),GFP_ATOMIC);
	if(pon_vlan_all_data.pon_vlan_type == NULL)
	{
		printk("malloc PON_VLan_Type error,out of memory\n");
		return -1;
	}
	memset(pon_vlan_all_data.pon_vlan_type,0,sizeof(struct packet_type) * pon_vlan_all_data.total_special_tpid);
	for(i = 0; i < pon_vlan_all_data.total_special_tpid; i++)
	{
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)
		pon_vlan_all_data.pon_vlan_type[i].func = NULL;
#else
		pon_vlan_all_data.pon_vlan_type[i].func = vlan_skb_recv;
#endif		
	}
	
	pon_vlan_all_data.pon_vlan_data = kmalloc(sizeof(pon_vlan) * (pon_vlan_all_data.total_port_count),GFP_ATOMIC);
	if(pon_vlan_all_data.pon_vlan_data == NULL)
	{
		printk("malloc PON_VLan_Data error,out of memory\n");
		return -1;
	}

	if(vlan_data_init(&pon_vlan_all_data) == -1)
	{
		printk("vlan_data_init error ====> GPON_Vlan\n");
		return -1;
	}
	/*init vlan list*/
	if( TCSUPPORT_PON_VLAN_FILTER_VAL && (initVlanList() == -1) ){
		printk("Pon_vlan_init-->initVlanList, fail\n");
		return -1;
	}
	
	for(i = 0; i < MAX_PON_IF; i++)
	{
		pon_vlan_all_data.pon_if_vlan_pair_data[i] = IF_VLAN_BLOCK;
	}
	gpon_init_ani_map();
	
	status = register_chrdev(PONVLAN_MAJOR, "ponvlan", &ponvlan_fops);
	if (status < 0)
		return status;
	
	rcu_assign_pointer(pon_insert_tag_hook, pon_insert_tag);
	rcu_assign_pointer(pon_vlan_get_mode_hook, pon_vlan_get_mode);
	rcu_assign_pointer(pon_store_tag_hook, pon_store_vlan_tag);
	rcu_assign_pointer(pon_check_vlan_hook, pon_check_vlan_tag);
	rcu_assign_pointer(pon_check_tpid_hook, is_vlan);
	rcu_assign_pointer(pon_check_user_group_hook, pon_check_user_group);
	rcu_assign_pointer(pon_PCP_decode_hook, pon_pcp_decode);
	rcu_assign_pointer(pon_vlan_is_ds_1_to_N_hook, pon_vlan_is_ds_1_to_N);
	rcu_assign_pointer(pon_vlan_ds_1_to_N_handler_hook, pon_vlan_ds_1_to_N_handler);

	if(TCSUPPORT_TLS_VAL)
		rcu_assign_pointer(ra_sw_nat_hook_tls_vtag_handle_hook, pon_insert_tag);

	//hybridMode
	if(TCSUPPORT_PON_SFU_HGU_HYBRID_VAL){
		rcu_assign_pointer(pon_hybrid_sfu_lan_check_hook, pon_check_hybrid_sfu_lan);
		rcu_assign_pointer(pon_hybrid_sfu_wan_check_hook, pon_check_hybrid_sfu_fwd_wan);
		hybrid_port_mask_proc = create_proc_entry("tc3162/hybrid_port_mask",0444,NULL);
		hybrid_port_mask_proc->read_proc = hybrid_port_mask_read_proc;
		hybrid_port_mask_proc->write_proc = hybrid_port_mask_write_proc;
	}

	if (ECNT_REGISTER_SUCCESS != ecnt_register_hook(&vlan_api_dispatch_hook_ops) ){
        panic("Register hook function failed! %s:%d", __FUNCTION__, __LINE__);
    }
	return 0;
}

static void  pon_vlan_exit(void)
{
	int i = 0;

	for(i = 0; i < pon_vlan_all_data.tpid_counter; i++)
	{
		dev_remove_pack(&pon_vlan_all_data.pon_vlan_type[i]);
	}
	
	if(vlan_data_clean(&pon_vlan_all_data) == -1)
	{
		printk("vlan_data_clean error ====> GPON_Vlan\n");
	}

	kfree(pon_vlan_all_data.pon_vlan_type);
	/*clean vlan filter rule*/
	if( TCSUPPORT_PON_VLAN_FILTER_VAL )
	    cleanGponVlanFilterRuleInKernel();

	unregister_chrdev(PONVLAN_MAJOR, "ponvlan");
	rcu_assign_pointer(pon_insert_tag_hook, NULL);
	rcu_assign_pointer(pon_vlan_get_mode_hook, NULL);
	rcu_assign_pointer(pon_store_tag_hook, NULL);
	rcu_assign_pointer(pon_check_vlan_hook, NULL);
	rcu_assign_pointer(pon_check_tpid_hook, NULL);
	rcu_assign_pointer(pon_check_user_group_hook, NULL);
	rcu_assign_pointer(pon_PCP_decode_hook, NULL);
	rcu_assign_pointer(pon_vlan_is_ds_1_to_N_hook, NULL);
	rcu_assign_pointer(pon_vlan_ds_1_to_N_handler_hook, NULL);

	if(TCSUPPORT_TLS_VAL)
		rcu_assign_pointer(ra_sw_nat_hook_tls_vtag_handle_hook, NULL);
	
	//hybridMode
	if(TCSUPPORT_PON_SFU_HGU_HYBRID_VAL){
		rcu_assign_pointer(pon_hybrid_sfu_lan_check_hook, NULL);
		rcu_assign_pointer(pon_hybrid_sfu_wan_check_hook, NULL);
		remove_proc_entry("xpon/hybrid_port_mask",0);
	}

	ecnt_unregister_hook(&vlan_api_dispatch_hook_ops);
	return;
}

module_init(pon_vlan_init);
module_exit(pon_vlan_exit);
