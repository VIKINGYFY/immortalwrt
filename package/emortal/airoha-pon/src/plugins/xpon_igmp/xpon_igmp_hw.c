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
	xpon_igmp_hw.c
	
	Abstract:It is for XPON multicast interface with HWNAT and Switch hardware

	Revision History:
	Who			When			What
	--------	----------		----------------------------------------------
	Name				Date			Modification logs
	lidong.hu		2012/7/28	Create
*/


#include <linux/capability.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/times.h>
#include <linux/list.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/if_vlan.h>


#include <ecnt_hook/ecnt_hook_pon_vlan.h>
#include <modules/eth_global_def.h>
#include <ecnt_hook/ecnt_hook_ether.h>
#include <ecnt_hook/ecnt_hook_pon_vlan.h>
#include <asm/tc3162/tc3162.h>
#include "xpon_igmp_core.h"


#ifdef TCSUPPORT_RA_HWNAT
extern int (*hwnat_is_alive_pkt_hook)(struct sk_buff* skb);
extern int (*hwnat_skb_to_foe_hook)(struct sk_buff* skb);
extern int (*hwnat_set_special_tag_hook)(int index, int tag);
extern int (*hwnat_delete_foe_entry_hook)(int index); 
extern int (*hwnat_delete_foe_entry_hook_unlock)(int index); 
extern int (*hwnat_is_multicast_entry_hook)(int index ,unsigned char* grp_addr,unsigned char* src_addr,int type);
extern int (*ra_sw_nat_hook_free) (struct sk_buff * skb);
extern int (*hwnat_is_drop_entry_hook)(int index ,unsigned char* grp_addr,unsigned char* src_addr,int type);
extern int (*hwnat_set_multicast_vlan_hook)(int index, int vid, int vpm);
extern int (*hwnat_multicast_set_info_for_sfu_hook)(int index, int tag);

#else
static int (*hwnat_is_alive_pkt_hook)(struct sk_buff* skb) = NULL;
static int (*hwnat_skb_to_foe_hook)(struct sk_buff* skb) = NULL;
static int (*hwnat_set_special_tag_hook)(int index, int tag) = NULL;
static int (*hwnat_delete_foe_entry_hook)(int index) = NULL; 
static int (*hwnat_delete_foe_entry_hook_unlock)(int index) = NULL; 
static int (*hwnat_is_multicast_entry_hook)(int index ,unsigned char* grp_addr,unsigned char* src_addr,int type) = NULL;
static int (*ra_sw_nat_hook_free) (struct sk_buff * skb) = NULL;
static int (*hwnat_is_drop_entry_hook)(int index ,unsigned char* grp_addr,unsigned char* src_addr,int type) = NULL;
static int (*hwnat_set_multicast_vlan_hook)(int index, int vid, int vpm) = NULL;
static int (*hwnat_multicast_set_info_for_sfu_hook)(int index, int tag) = NULL;

#endif

#ifdef TCSUPPORT_RA_HWNAT_ENHANCE_HOOK
extern int (*ra_sw_nat_hook_drop_packet) (struct sk_buff * skb);
extern int (*ra_sw_nat_cds_all_ratelimit_hook) (struct sk_buff* skb);
#else
static int (*ra_sw_nat_hook_drop_packet) (struct sk_buff * skb) = NULL;
static int (*ra_sw_nat_cds_all_ratelimit_hook) (struct sk_buff * skb) = NULL;
#endif

extern unsigned int macMT7530LanPortMap2Switch(unsigned int portId);
extern unsigned int macMT7530LanPortMap2SwitchForMulti(unsigned int portId);
#ifdef TCSUPPORT_CPU_ARMV8
extern u32 GET_HIR(void);
#endif
#define ETH_P_SPECIAL_TAG_1 0x0100
#define ETH_P_SPECIAL_TAG_2 0x0200
#define ETH_P_SPECIAL_TAG_3 0x0300
#define ETH_P_SPECIAL_TAG_MASK 0x0300
#define IS_SPECIAL_TAG(skb) ((vlan_eth_hdr(skb)->h_vlan_proto & 0xfc78) == 0)

/******************************************************************/
static DEFINE_SPINLOCK(hw_nat_multicast_lock);
static DEFINE_SPINLOCK(hw_nat_drop_multicast_lock);
static int mac_to_str(unsigned char* mac_str,unsigned char* mac);

static int update_port_mask_static_acl(int gemport,int vid, unsigned char* grp_addr, unsigned char* src_ip);
/******************************************************************/

static struct list_head* xpon_get_hwnat_list(void)
{
	xPON_IGMPConf_t* igmp_conf =   xpon_get_igmp_conf();
	return &igmp_conf->hwnat_igmp;
}

int xpon_check_hwfwd_list_threshold(void)
{
    int ret = 0;
    struct list_head* hwnat_list = NULL;
    struct list_head* tmpEntry = NULL;

    spin_lock_bh(&hw_nat_multicast_lock);
    hwnat_list = xpon_get_hwnat_list(); 
    list_for_each(tmpEntry, hwnat_list){
        ret++;
        if(ret > XPON_HWNAT_FWD_THRESHOLD)
            break;
    }
    spin_unlock_bh(&hw_nat_multicast_lock); 
    
    return ret;
}


static struct list_head* xpon_get_hwnat_drop(void)
{
	xPON_IGMPConf_t* igmp_conf =   xpon_get_igmp_conf();
	return &igmp_conf->hwnat_drop;
}

static int xpon_hwnat_delete_entry(xPON_IGMP_HWNATEntry_t* entry)
{
	if (entry == NULL)
    {
        return 0;
    }

//	del_timer(&entry->age_timer);
	list_del(&entry->list);
	xpon_free(entry);

	return 0;
}

static int xpon_hwnat_flow_index(struct sk_buff* skb)
{
	int index = -1; 
	
	if (hwnat_skb_to_foe_hook && skb)
		index = hwnat_skb_to_foe_hook(skb);

	return index;
}
#if 0
static int xpon_hwnat_is_valid_flow(struct sk_buff* skb)
{
	if (hwnat_is_alive_pkt_hook)
		return hwnat_is_alive_pkt_hook(skb);

	return 0;
}
#endif

void pon_vlan_get_down_vlan_action(int port, unsigned int src_vlan, int* mode, int* vid)
{
	int pon_vlan_mode = 0;
	int pon_vlan_vid = 0;
	
	ECNT_API_XPON_GET_DOWN_OPT(port,src_vlan,&pon_vlan_mode,&pon_vlan_vid);
	*mode = pon_vlan_mode;
	*vid = pon_vlan_vid;
	return;
}

static int xpon_hwnat_get_vlan_action_by_port(int port, unsigned int src_vlan, int* mode, int* vid)
{
	xPON_PortConf_t* port_conf =  xpon_port_conf_by_id(port);
	u16 src_vid = src_vlan&0xfff;

	if(NULL == port_conf)
		return 0;
	
	if(0 == port_conf->down_vlan_mode)	/* do vlan operation in pon vlan */
	{
		pon_vlan_get_down_vlan_action(port, src_vlan, mode, vid);
	}
	else
	{
		*mode = port_conf->down_vlan_mode;
		*vid = xpon_get_trans_vid(port,src_vid);
	}

	return 0;
}

static ECNT_SWITCH_VLAN_MODE xpon_hwnat_get_switch_vlan_mode(short int down_vlan_mode)
{
	switch (down_vlan_mode)
	{
		case 0:   //transparent
		return PORT_VLAN_TAG;
			break;
			
		case 1:   //strip
		return PORT_VLAN_UNTAG;
			break;
			
		case 2:   //add 
		return PORT_VLAN_STACK;
			break;
			
		case 3:   //replace VID + P +DEL
		/* not support, replace VID only */
		
		case 4:   //replace VID
		return PORT_VLAN_SWAP;
			break;

		default:
			break;
	}

	return PORT_VLAN_TAG;
}

static void xpon_hwnat_1toN_vlan_Handle(xPON_IGMP_HWNATEntry_t* entry ,int mask)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int i,port=0,switch_port=0,multi_vlan_flag=0;
	int tmp_mode=-1,tmp_vid=-1;
	u16 src_vid = entry->src_vlan&0xfff;
	xPON_IGMP_Multi_Vlan_t multi_vlan;
	ECNT_SWITCH_VLAN_MODE vlan_mode;

	if(NULL == igmp_conf)
		return;
	MULTICAST_NOTICE_INFO("Enter \n");
	memset(&multi_vlan, 0x0, sizeof(xPON_IGMP_Multi_Vlan_t));
	
	for (i=1;i<igmp_conf->uni_num && i<MAX_UNI_PORT_NUM;i++)
	{
		port = i;
		
		if ((igmp_conf->uni_port[i].port_flag & XPON_BRIDGE_PORT)==0)
			continue;

		switch_port = macMT7530LanPortMap2Switch(port-1);

        if(0 == ((1<<(port-1)) & entry->sw_mask))
			continue;

		xpon_hwnat_get_vlan_action_by_port(port,entry->src_vlan,&tmp_mode,&tmp_vid);
		MULTICAST_NOTICE_INFO("port %d mode %d vid %d.\n", port, tmp_mode, tmp_vid);
		multi_vlan.port_num++;
		multi_vlan.vlan_action[multi_vlan.port_num-1].switch_port = switch_port;
		multi_vlan.vlan_action[multi_vlan.port_num-1].mode = tmp_mode;
		multi_vlan.vlan_action[multi_vlan.port_num-1].vid= tmp_vid;
	}

	if(multi_vlan.port_num <= 1)
		return;

	/* compare with first port vlan action */
	tmp_mode = multi_vlan.vlan_action[0].mode;
	tmp_vid = multi_vlan.vlan_action[0].vid;
	for(i=1;i<multi_vlan.port_num && i<MAX_UNI_PORT_NUM;i++)
	{
		if(multi_vlan.vlan_action[i].mode != tmp_mode)
		{
			multi_vlan_flag = 1;
			break;
		}
		if(multi_vlan.vlan_action[i].vid != tmp_vid)
		{
			multi_vlan_flag = 1;
			break;
		}
	}

	if(0 == multi_vlan_flag)
		return;

	for(i=1;i<multi_vlan.port_num && i<MAX_UNI_PORT_NUM;i++) 
	{
		vlan_mode = xpon_hwnat_get_switch_vlan_mode(multi_vlan.vlan_action[i].mode);
		tmp_vid = multi_vlan.vlan_action[i].vid;
		switch_port = multi_vlan.vlan_action[i].switch_port;
		
		if((tmp_vid<=0) || (tmp_vid > 4095))
			continue;
		
		ETHER_API_PER_VLAN_ACTION(switch_port, src_vid, tmp_vid, vlan_mode, 1);
		MULTICAST_NOTICE_INFO("PER_VLAN_ACTION switch_port:%d o_vid:%d n_vid:%d vlan_mode:%d enable\n",
			switch_port, src_vid, tmp_vid, vlan_mode);
	}

	if(hwnat_set_multicast_vlan_hook){
		MULTICAST_NOTICE_INFO("hwnat_set_multicast_vlan_hook vid:%d\n", src_vid);
		hwnat_set_multicast_vlan_hook(entry->hwnat_index, src_vid, 1);
	}

	return;
}

static int xpon_hwnat_update_flow(xPON_IGMP_HWNATEntry_t* entry ,int mask)
{
	int type = 0;
	int ret = 0;
	
	if (entry->hwnat_type == XPON_MASK_MLDV1)
		type = 1;

	if (hwnat_is_multicast_entry_hook)
		ret = hwnat_is_multicast_entry_hook(entry->hwnat_index,entry->grp_addr,entry->src_addr,type);
    MULTICAST_NOTICE_INFO("hwnat_is_multicast_entry_hook return = %d, type = %d\n", ret, type);

	if (ret < 1)
	{
		xpon_hwnat_delete_entry(entry);
		return 0;
	}
	
	//if (hwnat_set_special_tag_hook)
	//	hwnat_set_special_tag_hook(entry->hwnat_index,mask);
	if (hwnat_multicast_set_info_for_sfu_hook)
		hwnat_multicast_set_info_for_sfu_hook(entry->hwnat_index,mask);	
	
	entry->hwnat_mask = mask;

	xpon_hwnat_1toN_vlan_Handle(entry, mask);
        
	return 0;
}

static int xpon_hwnat_close_foe_unlock(xPON_IGMP_HWNATEntry_t* entry)
{
	int type = 0;
	int ret = 0;

	if (entry->hwnat_type == XPON_MASK_MLDV1)
		type = 1;

	if (hwnat_is_multicast_entry_hook)
		ret = hwnat_is_multicast_entry_hook(entry->hwnat_index,entry->grp_addr,entry->src_addr,type);
	
	
	if (ret > 0 && hwnat_delete_foe_entry_hook_unlock)
		hwnat_delete_foe_entry_hook_unlock(entry->hwnat_index);
	
	return 0;
}

static int xpon_hwnat_close_foe(xPON_IGMP_HWNATEntry_t* entry)
{
	int type = 0;
	int ret = 0;

	if (entry->hwnat_type == XPON_MASK_MLDV1)
		type = 1;

	if (hwnat_is_multicast_entry_hook)
		ret = hwnat_is_multicast_entry_hook(entry->hwnat_index,entry->grp_addr,entry->src_addr,type);
	
	
	if (ret > 0 && hwnat_delete_foe_entry_hook)
		hwnat_delete_foe_entry_hook(entry->hwnat_index);
	
	return 0;
}

static int xpon_hwnat_port_mask(int type,int vid,unsigned char*  grp_addr, unsigned char* src_ip, unsigned int* sw_mask)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	unsigned char grp_mac[8];
	int i,port,mask,logic_lan_idx = 0;
	char dev_name[8] = {0};
	
	mask = 0;
	*sw_mask = 0;
	memset(grp_mac,0,8);
	
	if (type == XPON_MASK_IGMPV2)
	{
		xpon_ip4_to_mac(grp_addr,grp_mac);
	}
	else if(type == XPON_MASK_MLDV1)
	{
		xpon_ip6_to_mac(grp_addr,grp_mac);
	}
	else
		return 0;	
	
	for (i=1; i<MAX_UNI_PORT_NUM; i++)
	{
		port = i;

		if ((igmp_conf->uni_port[i].port_flag & XPON_BRIDGE_PORT)==0)
			continue;
        MULTICAST_NOTICE_INFO("port id= %d, igmp flag = %x, type = %d, vid = %d\n",port, igmp_conf->flag, type, vid);
		if (((type == XPON_MASK_IGMPV2) && (igmp_conf->flag & XPON_IGMP_SNOOPING_DISABLED))		/* set special tag for flood, if snooping disabled. */
			|| ((type == XPON_MASK_MLDV1) && (igmp_conf->flag & XPON_MLD_SNOOPING_DISABLED))	/* set special tag for flood, if MLD snooping disabled. */
			|| (xpon_should_forward_flow(port,vid,grp_mac,grp_addr,src_ip)>0))			/* set special tag for flood, if snooping enabled and the in the forward table. */
		{
			(*sw_mask) |= 1 << (port-1);
			ENCT_HOOK_XPON_ETH_MAP_PORT_TO_DEV_NAME(port, dev_name);
			
			logic_lan_idx = getLogicLANIndexByName(dev_name);
			mask |= 1 << logic_lan_idx;
		}
	}
	
	return mask;
}


static int xpon_hwnat_delete_flow_unlock(xPON_IGMP_HWNATEntry_t* entry)
{
	xPON_IGMP_HWNATEntry_t local_entry;
    MULTICAST_NOTICE_INFO("xpon_hwnat_delete_flow begin entry %p.\n",entry);
	if (entry == NULL)
		return 0;

    memset(&local_entry, 0, sizeof(xPON_IGMP_HWNATEntry_t));
    memcpy(&local_entry,entry,sizeof(xPON_IGMP_HWNATEntry_t));

	xpon_hwnat_delete_entry(entry);
	xpon_hwnat_close_foe_unlock(&local_entry);
	return 0;
}


static int xpon_hwnat_delete_flow(xPON_IGMP_HWNATEntry_t* entry)
{
    xPON_IGMP_HWNATEntry_t local_entry;
    MULTICAST_NOTICE_INFO("xpon_hwnat_delete_flow begin entry %p.\n",entry);
    if (entry == NULL)
        return 0;
    
    memset(&local_entry, 0, sizeof(xPON_IGMP_HWNATEntry_t));
    memcpy(&local_entry,entry,sizeof(xPON_IGMP_HWNATEntry_t));
	
    if (0 != entry->sw_mask)
    {
        entry->sw_mask = 0;
        //xpon_hwnat_update_external_switch(entry, 0);
    }

    xpon_hwnat_delete_entry(entry);
    xpon_hwnat_close_foe(&local_entry);

    return 0;
}

int xpon_hwnat_del_spec_flows(xPON_FwdEntry_t* entry)
{
    xPON_IGMP_HWNATEntry_t*   tmp = NULL;
	xPON_IGMP_HWNATEntry_t* hw_entry = NULL;
	struct list_head*  hwnat_flow = NULL;
    
    MULTICAST_NOTICE_INFO("xpon_hwnat_del_spec_flows vid %d.\n",entry->vid);
    
    spin_lock_bh(&hw_nat_multicast_lock);
    
    hwnat_flow = xpon_get_hwnat_list();
    list_for_each_entry_safe(hw_entry,tmp,hwnat_flow,list)
    {
    	if(hw_entry->hwnat_vid == entry->vid
        && (0 == memcmp(hw_entry->grp_addr,entry->grp_addr,16))
		&& (0 == memcmp(hw_entry->src_addr,entry->src_addr,16)))
		{
        	xpon_hwnat_delete_flow(hw_entry);
			break;
        }
    }
    
    spin_unlock_bh(&hw_nat_multicast_lock);
    
	return 0;
}

int xpon_hwnat_clear_flows(void)
{
    xPON_IGMP_HWNATEntry_t*   tmp = NULL;
	xPON_IGMP_HWNATEntry_t* entry = NULL;
	struct list_head*  hwnat_flow = NULL;
    
    MULTICAST_NOTICE_INFO("clear hw_nat flow.\n");
    
    spin_lock_bh(&hw_nat_multicast_lock);
    
    hwnat_flow = xpon_get_hwnat_list();
    list_for_each_entry_safe(entry,tmp,hwnat_flow,list)
    {
        xpon_hwnat_delete_flow(entry);
    }
    
    spin_unlock_bh(&hw_nat_multicast_lock);
    
	return 0;
}

void add_flow_timer_func(TIMER_FUN_PAAM arg)
{
    int type = 0;
    int ret = 0;
    struct list_head* hwnat_list  = NULL;
    xPON_IGMP_HWNATEntry_t* tmpEntry = NULL;
    xPON_IGMP_HWNATEntry_t* next = NULL;

    spin_lock_bh(&hw_nat_multicast_lock);
    hwnat_list = xpon_get_hwnat_list();    
    list_for_each_entry_safe(tmpEntry, next, hwnat_list, list)
    {
        if(XPON_MASK_MLDV1 == tmpEntry->hwnat_type){
            type = 1;
        }
        if(hwnat_is_multicast_entry_hook)
        {
            ret = hwnat_is_multicast_entry_hook(tmpEntry->hwnat_index,tmpEntry->grp_addr,tmpEntry->src_addr,type);
            if(ret!=1){
                xpon_hwnat_delete_flow(tmpEntry);
            }
        }else{
            printk("[%s %d]ERR: hwnat_is_multicast_entry_hook is NULL.\n", __FUNCTION__, __LINE__);
        }
    }
    spin_unlock_bh(&hw_nat_multicast_lock);
    /*timer 30s*/
    mod_timer(&igmp_conf.addFlwTm, round_jiffies(jiffies) + XPON_HWNAT_AGE_TIME);
}

#if 0
void xpon_hwnat_timer_timeout(unsigned long arg)
{
	int ret  = 0;
	int type = 0;
    
    xPON_IGMP_HWNATEntry_t    hw_entry;
    xPON_IGMP_HWNATEntry_t* entry = NULL;
	struct list_head* hwnat_flow  = NULL;
    xPON_IGMP_HWNATEntry_t*   tmp = NULL;
    xPON_IGMP_HWNATEntry_t* hwnat_entry = NULL;
    int found_entry = 0;
    
    memset(&hw_entry, 0 , sizeof(xPON_IGMP_HWNATEntry_t));

	spin_lock(&hw_nat_multicast_lock);

    entry = (xPON_IGMP_HWNATEntry_t* )arg;
    if(NULL == entry)
    {
        goto out_unlock;
    }

    /*ensure entry exist*/
 	hwnat_flow = xpon_get_hwnat_list();
 	list_for_each_entry_safe(hwnat_entry,tmp,hwnat_flow,list)
    {
        if(hwnat_entry == entry)
        {
            found_entry = 1;
            break;
        }
    }
    if(found_entry != 1)
    {
        goto out_unlock;
    }
    
	if (XPON_MASK_MLDV1 == entry->hwnat_type)
    {
        type = 1;
    }   

	if (hwnat_is_multicast_entry_hook)
		ret = hwnat_is_multicast_entry_hook(entry->hwnat_index,entry->grp_addr,entry->src_addr,type);

	if (ret == 1)
	{
        MULTICAST_CRITIC_INFO("update xpon_igmp hw_nat ageing time.\n");
		mod_timer(&entry->age_timer,round_jiffies(jiffies) + XPON_HWNAT_AGE_TIME);
		goto out_unlock;
	}
    
    hw_entry.hwnat_index = entry->hwnat_index;
    hw_entry.hwnat_vid   = entry->hwnat_vid;
    hw_entry.hwnat_type  = entry->hwnat_type;
    if(XPON_MASK_IGMPV2 == entry->hwnat_type)
    {
        memcpy(hw_entry.grp_addr, entry->grp_addr, 4);
        memcpy(hw_entry.src_addr, entry->src_addr, 4);
    }
    else if(XPON_MASK_MLDV1 == entry->hwnat_type)
    {
        memcpy(hw_entry.grp_addr, entry->grp_addr, 16);
        memcpy(hw_entry.src_addr, entry->src_addr, 16);
    }
    else
    {
        goto out_unlock;
    }
	
    /*ensure the same entry*/
	if (entry)
    {
        hwnat_flow = xpon_get_hwnat_list();
        list_for_each_entry_safe(hwnat_entry,tmp,hwnat_flow,list)
        {
            if((hw_entry.hwnat_index   == hwnat_entry->hwnat_index) \
               && (hw_entry.hwnat_vid  == hwnat_entry->hwnat_vid)  \
               && (hw_entry.hwnat_type == hwnat_entry->hwnat_type))
            {
                if((XPON_MASK_IGMPV2 == entry->hwnat_type) \
                    && !memcmp(hw_entry.grp_addr, hwnat_entry->grp_addr,4) \
                    && !memcmp(hw_entry.src_addr, hwnat_entry->src_addr,4))
                {
                    MULTICAST_CRITIC_INFO("del xpon_igmp hw_nat entry.\n");
                    xpon_hwnat_delete_flow(hwnat_entry);
					break;
                }
                else if((XPON_MASK_MLDV1 == entry->hwnat_type) \
                    && !memcmp(hw_entry.grp_addr, hwnat_entry->grp_addr,16) \
                    && !memcmp(hw_entry.src_addr, hwnat_entry->src_addr,16))
                {
                    MULTICAST_CRITIC_INFO("del xpon_igmp hw_nat entry.\n");
                    xpon_hwnat_delete_flow(hwnat_entry);
					break;
                }
                else
                {
                    MULTICAST_NOTICE_INFO("we can do nothing.\n");
                }
            }
        }
    }   
		
out_unlock:

	spin_unlock(&hw_nat_multicast_lock);
    
	return;
}
#endif

static xPON_IGMP_HWNATEntry_t* xpon_hwnat_add_flow(struct sk_buff* skb,int proto,int vid,int gemport,unsigned char* grp_addr, unsigned char* src_addr,unsigned char* mac_da)
{
	int index = -1;
    
   	xPON_IGMP_HWNATEntry_t*   ptr = NULL;
    xPON_IGMP_HWNATEntry_t* entry = NULL;
    xPON_IGMP_HWNATEntry_t* hwnat_entry = NULL;
	struct list_head* hwnat_flow  = NULL;
	int irqs_ret = 0;
    
    entry = (xPON_IGMP_HWNATEntry_t* )xpon_alloc(sizeof(xPON_IGMP_HWNATEntry_t));
    if (NULL == entry)
    {
        MULTICAST_ERROR_INFO("malloc memory is error.\n");
        return NULL;
    }
    
    MULTICAST_DEBUG_INFO("**hw_nat add flow**.\n");
	
    irqs_ret = irqs_disabled();
	if(0 != irqs_ret)
	    spin_lock(&hw_nat_multicast_lock);
    else
	    spin_lock_bh(&hw_nat_multicast_lock);

    index = xpon_hwnat_flow_index(skb);

    MULTICAST_DEBUG_INFO("index=%d\n",index);

	if (0 > index)
    {
        xpon_free(entry);
        entry = NULL;
        goto out_unlock;
    }

    /*before add entry, del same entry*/
    hwnat_flow = xpon_get_hwnat_list();
    list_for_each_entry_safe(hwnat_entry, ptr, hwnat_flow, list)    
    {
        if (hwnat_entry->hwnat_index == index)
        {
            xpon_hwnat_delete_entry(hwnat_entry);
			break;
        }      
    }

	entry->hwnat_type = proto;
	entry->src_vlan = (skb->vlan_tags[0]<<16) | skb->vlan_tags[1];
	entry->hwnat_vid= vid;
	entry->hwnat_index = index;
	entry->hwnat_mask = 0;
	entry->sw_mask = 0;
    
    entry->gem_port_id = gemport;
    
	memcpy(entry->grp_addr,grp_addr,16);
	memcpy(entry->src_addr,src_addr,16);
    memcpy(entry->mac_da,mac_da,6);
	list_add_tail(&entry->list,hwnat_flow);
//	setup_timer(&entry->age_timer, xpon_hwnat_timer_timeout, (unsigned long)entry);	
//	mod_timer(&entry->age_timer, round_jiffies(jiffies) + XPON_HWNAT_AGE_TIME);
    
    MULTICAST_DEBUG_INFO("hwnat_index=%d,grp_ip=%d.%d.%d.%d\n",entry->hwnat_index,entry->grp_addr[0],
    entry->grp_addr[1],entry->grp_addr[2],entry->grp_addr[3]);

out_unlock:
   	if(0 != irqs_ret)
	    spin_unlock(&hw_nat_multicast_lock);
    else
	    spin_unlock_bh(&hw_nat_multicast_lock);    
	
	return entry;
}


int xpon_hwnat_learn_flow(struct sk_buff* skb)
{
	unsigned char dest_addr[16],src_addr[16];
	short int vid = -1,proto = 0;
	int eth_type = 0;
	struct iphdr*  ih;
	struct ipv6hdr* i6h;
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	unsigned char* buff = skb_mac_header(skb)+18;
    int gem_port_id = 0;
	unsigned char mac_da[6];
	xPON_IGMP_HWNATEntry_t* entry = NULL;
	int special_tag_num = 0;
	unsigned short int* prot;

	MULTICAST_DEBUG_INFO("xpon_hwnat_learn_flow.\n");

	if (igmp_conf->flag & XPON_HWNAT_DISABLED){
		return 0;
	}
	if(xpon_is_multicast_addr(vlan_eth_hdr(skb)->h_dest) == 0){
		return 0;
	}
	memcpy(mac_da,vlan_eth_hdr(skb)->h_dest,6);
	if(isEN7580 || isEN7581 || isAN7583)
	{	
		/* If there is no tag. There is no special tag for internel lan switch in EN7580.*/
		if((ntohs(vlan_eth_hdr(skb)->h_vlan_proto) == ETH_P_IP)
			||(ntohs(vlan_eth_hdr(skb)->h_vlan_proto) == ETH_P_IPV6))
		{
			/* untag packet */
			eth_type = ntohs(vlan_eth_hdr(skb)->h_vlan_proto);
			buff = skb_mac_header(skb)+14;
		}
		/* Maybe it's "vlan tag" or "special tag for ext" or "special tag for ext" + "vlan tag".
		    "special tag for ext" is inserted when skb  processed by eth tx before sent to external switch.
		*/
		else if((ntohs(vlan_eth_hdr(skb)->h_vlan_proto) == ETH_P_8021Q)
			|| (ntohs(vlan_eth_hdr(skb)->h_vlan_proto) == ETH_P_QinQ_88a8) 
			|| (ntohs(vlan_eth_hdr(skb)->h_vlan_proto) == ETH_P_QinQ_9100))
		{
			prot = (unsigned short*)(skb_mac_header(skb)+16);
			MULTICAST_DEBUG_INFO("vlan:0x%x.\n", ntohs(*prot));
			/* If there are 2 tags, then skip the "special tag for ext". */
			if((ntohs(*prot) == ETH_P_8021Q))
			{
				prot++;
				vid = ntohs(*prot);
				prot++;
				eth_type = ntohs(*prot);
				buff = skb_mac_header(skb)+22;
			}
			/* If there is 1 tag, then get the vid. */
			else
			{
				vid = ntohs(vlan_eth_hdr(skb)->h_vlan_TCI) & VLAN_VID_MASK;
				eth_type = ntohs(vlan_eth_hdr(skb)->h_vlan_encapsulated_proto);
			}
		}
		MULTICAST_NOTICE_INFO("learn hw flow, ethtype = %d, vlan_proto = %x,special_tag_num= %d,  vid = %d\n", eth_type, ntohs(vlan_eth_hdr(skb)->h_vlan_proto),special_tag_num, vid);
	}
	else{

		if((ntohs(vlan_eth_hdr(skb)->h_vlan_proto) != ETH_P_8021Q) /* do not have 802.1q tag */
#ifdef TCSUPPORT_PON_VLAN
			&& (ntohs(vlan_eth_hdr(skb)->h_vlan_proto) != ETH_P_QinQ_88a8) 
			&& (ntohs(vlan_eth_hdr(skb)->h_vlan_proto) != ETH_P_QinQ_9100)
#endif
			&& ('e' != skb->dev->name[0]) /* and do not have special tag */
		){
			/* untag packet */
			eth_type = ntohs(vlan_eth_hdr(skb)->h_vlan_proto);
			buff = skb_mac_header(skb)+14;
		}
		else
		{

			/* If there are 2 tags, then skip the "special tag for ext". */
			prot = (unsigned short*)(skb_mac_header(skb)+16);
			MULTICAST_DEBUG_INFO("vlan:0x%x.\n", ntohs(*prot));
			if(ntohs(*prot) == ETH_P_8021Q)
			{
				prot++;
				vid = ntohs(*prot);
				prot++;
				eth_type = ntohs(*prot);
				buff = skb_mac_header(skb)+22;
			}
			else
			{			
				eth_type = ntohs(vlan_eth_hdr(skb)->h_vlan_encapsulated_proto);
				if ('e' == skb->dev->name[0]) /* have special tag */
				{
					special_tag_num = (ntohs(vlan_eth_hdr(skb)->h_vlan_proto) & 0x0300) >> 8;
					if(special_tag_num > 0)
					vid = ntohs(vlan_eth_hdr(skb)->h_vlan_TCI) & VLAN_VID_MASK;
				}
				else/* have 802.1q tag */
				{
					vid = ntohs(vlan_eth_hdr(skb)->h_vlan_TCI) & VLAN_VID_MASK;
				}
			}
		}
	}
	eth_type &= 0xffff;

	memset(dest_addr,0,16);
	memset(src_addr,0,16);
	if (eth_type==ETH_P_IP)
	{
		proto = XPON_MASK_IGMPV2;
		ih = (struct iphdr*)buff;
		memcpy(dest_addr,&ih->daddr,4);
		memcpy(src_addr,&ih->saddr,4);
	}
	else if(eth_type==ETH_P_IPV6)
	{
		proto = XPON_MASK_MLDV1;
		i6h = (struct ipv6hdr*)buff;
		memcpy(dest_addr, i6h->daddr.s6_addr,16);
		memcpy(src_addr, i6h->saddr.s6_addr,16);	
	}
	else
	{
		MULTICAST_ERROR_INFO("type = %x\n",eth_type);
        return 0;
	}
    
    /*epon 2, gpon=1*/
    if(1 == igmp_conf->xpon_mode)
    {
        gem_port_id = skb->gem_port;
    }
	MULTICAST_ERROR_INFO("xpon_hwnat_add_flow = proto:%x, vid:%d\n",proto, vid);
	entry = xpon_hwnat_add_flow(skb,proto,vid,gem_port_id, dest_addr,src_addr,mac_da);
	xpon_hwnat_update_flow_by_hw(entry);
	return 0;
}

int src_ip_is_null(unsigned char *addr)
{
	unsigned char null_addr[16]={0};
	if(0 == memcmp(addr,null_addr,sizeof(null_addr)))
	{
		MULTICAST_WARN_INFO("SRC IP IS NULL\n");
		return 1;
	}
	return 0;
}

int xpon_hwnat_del_hw_flow(xPON_FwdEntry_t*  fwd_entry)
{
    xPON_IGMP_HWNATEntry_t* entry = NULL;
    xPON_IGMP_HWNATEntry_t* tmp = NULL;
    struct list_head* hwnat_flow = NULL;
    MULTICAST_NOTICE_INFO("Enter del hw flow, type = %d\n", fwd_entry->type);

    spin_lock_bh(&hw_nat_multicast_lock);
    
    hwnat_flow = xpon_get_hwnat_list();
    MULTICAST_NOTICE_INFO("hw_flow = %p\n", hwnat_flow);
    list_for_each_entry_safe(entry,tmp,hwnat_flow,list)
    {
        MULTICAST_NOTICE_INFO("Enter del hw flow, hwnat_index = %d\n", entry->hwnat_index);
        switch (fwd_entry->type)
    	{
    		case MULTCASTCTL_MAC_DA:
    		case MULTCASTCTL_MAC_DA_VLAN:
    			if (0 == memcmp(entry->mac_da,fwd_entry->grp_addr,6)){
                    xpon_hwnat_delete_flow(entry);
					goto out_unlock;
                 }
    			break;
    		case MULTCASTCTL_IPV4_SA_MAC_DA:
                if ((0 == memcmp(entry->mac_da,fwd_entry->grp_addr,6)) && 
    			    (0 == memcmp(entry->src_addr,fwd_entry->src_addr,4) || src_ip_is_null(fwd_entry->src_addr))){
                    xpon_hwnat_delete_flow(entry);
					goto out_unlock;
                }
    			break;	
    		case MULTCASTCTL_IPV4_DA_VLAN:
                if ( 0 == memcmp(entry->grp_addr,fwd_entry->grp_addr,6)){
                    xpon_hwnat_delete_flow(entry);
					goto out_unlock;
                }
                break;
    		case MULTCASTCTL_IPV6_DA_VLAN:
                if ( 0 == memcmp(entry->grp_addr,fwd_entry->grp_addr,16)){
                    xpon_hwnat_delete_flow(entry);
					goto out_unlock;
                }
    			break;
    		case MULTCASTCTL_IPV6_SA_MAC_DA:
                if ((0 == memcmp(entry->mac_da,fwd_entry->grp_addr,6)) &&
    			    (0 == memcmp(entry->src_addr,fwd_entry->src_addr,16) || src_ip_is_null(fwd_entry->src_addr))){
                    xpon_hwnat_delete_flow(entry);
					goto out_unlock;
                }
    			break;
    		case MULTCASTCTL_IPV4_DA:
    		    MULTICAST_NOTICE_INFO("hw entry grp_addr= %02x %02x %02x %02x, fwd_entry grp_addr= %02x %02x %02x %02x\n", 
    		                        entry->grp_addr[0], entry->grp_addr[1], entry->grp_addr[2], entry->grp_addr[3],
    		                        fwd_entry->grp_addr[0], fwd_entry->grp_addr[1], fwd_entry->grp_addr[2], fwd_entry->grp_addr[3]);
				MULTICAST_NOTICE_INFO("hw entry src_addr= %02x %02x %02x %02x, fwd_entry src_addr= %02x %02x %02x %02x\n",
									entry->src_addr[0], entry->src_addr[1], entry->src_addr[2], entry->src_addr[3],
									fwd_entry->src_addr[0], fwd_entry->src_addr[1], fwd_entry->src_addr[2], fwd_entry->src_addr[3]);
				if (0 == memcmp(entry->grp_addr,fwd_entry->grp_addr,4) &&
						(0 == memcmp(entry->src_addr,fwd_entry->src_addr,4) || src_ip_is_null(fwd_entry->src_addr))){
                    xpon_hwnat_delete_flow(entry);
					goto out_unlock;
                }
    			break;
    		case MULTCASTCTL_IPV6_DA:
                if ((0 == memcmp(entry->grp_addr,fwd_entry->grp_addr,16)) &&
    			    (0 == memcmp(entry->src_addr,fwd_entry->src_addr,16) || src_ip_is_null(fwd_entry->src_addr))){
                    xpon_hwnat_delete_flow(entry);
					goto out_unlock;
                }
    			break;
    		default:
    			break;
    	}
    }
	
out_unlock:
    spin_unlock_bh(&hw_nat_multicast_lock);

    return 0;
}

int xpon_hwnat_update_external_switch(xPON_IGMP_HWNATEntry_t* entry, unsigned int sw_mask)
{
	unsigned int grp_addr = 0, src_addr = 0;
	int i;
	MULTICAST_NOTICE_INFO("sw_mask:%x\n", sw_mask);
	if(entry->hwnat_type == XPON_MASK_MLDV1 
		|| entry->hwnat_type == XPON_MASK_MLDV1)
	{
		/* byte12~byte15, for MLD. */
		for(i = 12; i < 16; i++)
		{
			grp_addr |= ((unsigned int)entry->grp_addr[i]) & 0xFF;
			src_addr |= ((unsigned int)entry->src_addr[i]) & 0xFF;
			if(i < 15)
			{
				grp_addr = (grp_addr << 8);
				src_addr = (src_addr << 8);
			}
		}
	}
	else if(entry->hwnat_type == XPON_MASK_IGMPV1
		|| entry->hwnat_type == XPON_MASK_IGMPV2
		|| entry->hwnat_type == XPON_MASK_IGMPV3)
	{
		/* byte0~byte4, for IGMP. */
		for(i = 0; i < 4; i++)
		{
			grp_addr |= ((unsigned int)entry->grp_addr[i]) & 0xFF;
			src_addr |= ((unsigned int)entry->src_addr[i]) & 0xFF;
			if(i < 3)
			{
				grp_addr = (grp_addr << 8);
				src_addr = (src_addr << 8);
			}
		}

	}
	else{
		return 0;
	}
	MULTICAST_NOTICE_INFO("grp_addr:%x, src_addr:%x, mask:%x\n", grp_addr, src_addr, sw_mask);
	ETHER_API_ADD_ARL_IPTBL_MULTI(1, grp_addr, src_addr, sw_mask);
	return 0;

}

int xpon_hwnat_update_flow_by_hw(xPON_IGMP_HWNATEntry_t* entry)
{
	int mask = 0;
	unsigned int sw_mask = 0;
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
    int static_acl_mask = 0; 
    int irqs_ret = 0;

	if (igmp_conf->flag & XPON_HWNAT_DISABLED)
		return 0;
    irqs_ret = irqs_disabled();
    if(0 != irqs_ret)
	    spin_lock(&hw_nat_multicast_lock);
    else
	    spin_lock_bh(&hw_nat_multicast_lock);

	if(NULL == entry){
		goto out_unlock;
	}else{
		/*epon 2, gpon=1*/
		if(1 == igmp_conf->xpon_mode)
		{
		    static_acl_mask = update_port_mask_static_acl(entry->gem_port_id,entry->hwnat_vid,entry->grp_addr,entry->src_addr);
		}

        mask = xpon_hwnat_port_mask(entry->hwnat_type,(entry->src_vlan&0xfff),entry->grp_addr,entry->src_addr, &sw_mask);
		mask |= static_acl_mask;

		MULTICAST_ERROR_INFO("xpon_hwnat_port_mask = hwnat_type:%x, vid:%d, mask:%x, hw_mask:%x\n",entry->hwnat_type, entry->src_vlan, mask, entry->hwnat_mask);    
		/* update externel switch forward rule. */
		if (sw_mask != entry->sw_mask)
		{
			if((((entry->sw_mask&0xE0)==0)&&((sw_mask&0xE0)==0xE0))||(((entry->sw_mask&0xE0)==0xE0)&&((sw_mask&0xE0)==0))){
				MULTICAST_DEBUG_INFO("[%s %d]Don't change external switch\n", __FUNCTION__, __LINE__);
			}else{
				entry->sw_mask = sw_mask;
				//xpon_hwnat_update_external_switch(entry, sw_mask);
			}
		}

		if (mask != entry->hwnat_mask)
		{
		    if (mask == 0)
		    {
		        xpon_hwnat_delete_flow_unlock(entry);
		    }         
		    else
		    {
		        xpon_hwnat_update_flow(entry,mask);
		    }         
		}
    }
out_unlock:
    if(0 != irqs_ret)
	    spin_unlock(&hw_nat_multicast_lock);
    else
	    spin_unlock_bh(&hw_nat_multicast_lock);

	return 0;
}

int xpon_hwnat_update_flow_by_fwd(xPON_FwdEntry_t* entry)
{
	int mask = 0;
	unsigned int sw_mask = 0;
	xPON_IGMP_HWNATEntry_t* hwnat_entry = NULL, *tmp = NULL;
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	struct list_head* hwnat_flow = NULL; 
    int static_acl_mask = 0; 

	if (igmp_conf->flag & XPON_HWNAT_DISABLED)
		return 0; 

	spin_lock_bh(&hw_nat_multicast_lock);

    hwnat_flow = xpon_get_hwnat_list();
    list_for_each_entry_safe(hwnat_entry,tmp,hwnat_flow,list)
    {
		if(((XPON_MASK_IGMPV2 == hwnat_entry->hwnat_type)&&(0 == memcmp(entry->grp_addr, hwnat_entry->grp_addr,4))) \
			|| ((XPON_MASK_MLDV1 == hwnat_entry->hwnat_type)&&(0 == memcmp(entry->grp_addr, hwnat_entry->grp_addr,16)))) 
		{
			/*epon 2, gpon=1*/
	        if(1 == igmp_conf->xpon_mode)
	        {
	            static_acl_mask = update_port_mask_static_acl(hwnat_entry->gem_port_id,hwnat_entry->hwnat_vid,hwnat_entry->grp_addr,hwnat_entry->src_addr);
	        }
			
	        mask = xpon_hwnat_port_mask(hwnat_entry->hwnat_type,(hwnat_entry->src_vlan&0xfff),hwnat_entry->grp_addr,hwnat_entry->src_addr, &sw_mask);
	        mask |= static_acl_mask;

	        /* update externel switch forward rule. */
	        if (sw_mask != hwnat_entry->sw_mask)
	        {
	            hwnat_entry->sw_mask = sw_mask;
	            //xpon_hwnat_update_external_switch(hwnat_entry, sw_mask);
	        }
			
	        if (mask != hwnat_entry->hwnat_mask)
	        {
	            if (mask == 0)
	            {
	                xpon_hwnat_delete_flow(hwnat_entry);
	            }         
	            else
	            {
	                xpon_hwnat_update_flow(hwnat_entry,mask);
	            }         
	        }
			break;
		}
		else
		{
			MULTICAST_NOTICE_INFO("we can do nothing.\n");
		}		
    }
	
	spin_unlock_bh(&hw_nat_multicast_lock);

	return 0;
}

/******************************************************************************/
static int xpon_hwnat_delete_drop(xPON_IGMP_HWNATEntry_t* entry)
{
	int type = 0;
	int ret = 0;

	if (entry->hwnat_type == XPON_MASK_MLDV1)
		type = 1;

	if (hwnat_is_drop_entry_hook)
		ret = hwnat_is_drop_entry_hook(entry->hwnat_index,entry->grp_addr,entry->src_addr,type);
	
	if (hwnat_delete_foe_entry_hook)
		hwnat_delete_foe_entry_hook(entry->hwnat_index);
	
	xpon_hwnat_delete_entry(entry);

	return 0;
}

int xpon_hwnat_clear_all_drop(void)
{
	xPON_IGMP_HWNATEntry_t* entry = NULL;
    xPON_IGMP_HWNATEntry_t*  tmp  = NULL;
	struct list_head*   drop_list = NULL;
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();

	if(igmp_conf->flag & XPON_HWNAT_DROP_DISABLED)
		return 0;

	MULTICAST_NOTICE_INFO("xpon_hwnat_clear_all_drop.\n");

    spin_lock_bh(&hw_nat_drop_multicast_lock);

    drop_list = xpon_get_hwnat_drop();
	list_for_each_entry_safe(entry, tmp, drop_list, list)
	{
		xpon_hwnat_delete_drop(entry);
	}
    
	spin_unlock_bh(&hw_nat_drop_multicast_lock);
    
	return 0;
}

void add_drop_timer_func(TIMER_FUN_PAAM arg)
{
    int type = 0;
    int ret = 0;
    struct list_head* hwnat_drop_list = NULL;
    xPON_IGMP_HWNATEntry_t* next = NULL;
    xPON_IGMP_HWNATEntry_t* tmpEntry = NULL;

    spin_lock_bh(&hw_nat_drop_multicast_lock);
    hwnat_drop_list = xpon_get_hwnat_drop();
    list_for_each_entry_safe(tmpEntry, next, hwnat_drop_list, list)
    {
        if(XPON_MASK_MLDV1 == tmpEntry->hwnat_type){
            type = 1;
        }
        if(hwnat_is_drop_entry_hook)
        {
            ret = hwnat_is_drop_entry_hook(tmpEntry->hwnat_index,tmpEntry->grp_addr,tmpEntry->src_addr,type);
            if(1!=ret){
                xpon_hwnat_delete_drop(tmpEntry);
            }
        }else{
            printk("[%s %d]ERR: hwnat_is_drop_entry_hook is NULL.\n", __FUNCTION__, __LINE__);
        }
    }
    spin_unlock_bh(&hw_nat_drop_multicast_lock);
    /*timer 30s*/
    mod_timer(&igmp_conf.addDrpTm, round_jiffies(jiffies) + XPON_HWNAT_AGE_TIME);
}

int xpon_hwnat_clear_drop_by_grpip(unsigned char is_ipv6,unsigned char* grp_addr)
{
	xPON_IGMP_HWNATEntry_t* entry = NULL;
	xPON_IGMP_HWNATEntry_t*  tmp  = NULL;
	struct list_head*   drop_list = NULL;
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();

	if(igmp_conf->flag & XPON_HWNAT_DROP_DISABLED)
		return 0;

	if(!grp_addr)
		return 0;

	MULTICAST_NOTICE_INFO("xpon_hwnat_clear_drop_by_grpip.\n");

	spin_lock_bh(&hw_nat_drop_multicast_lock);

	drop_list = xpon_get_hwnat_drop();
	list_for_each_entry_safe(entry, tmp, drop_list, list)
	{
		if(is_ipv6)
		{
			if((entry->hwnat_type != XPON_MASK_MLDV1)&&(entry->hwnat_type != XPON_MASK_MLDV2))
				continue;
			
			if (!memcmp(entry->grp_addr,grp_addr,16))
			{
				xpon_hwnat_delete_drop(entry);
				break;
			}
		}
		else
		{
			if((entry->hwnat_type != XPON_MASK_IGMPV1)&&(entry->hwnat_type != XPON_MASK_IGMPV2)&&(entry->hwnat_type != XPON_MASK_IGMPV3))
				continue;
			
			if (!memcmp(entry->grp_addr,grp_addr,4))
			{
				xpon_hwnat_delete_drop(entry);
				break;
			}
		}

	}

	spin_unlock_bh(&hw_nat_drop_multicast_lock);

	return 0;
}

#if 0
void xpon_hwnat_drop_timeout(unsigned long arg)
{
	int ret = 0;
	int type = 0;
    xPON_IGMP_HWNATEntry_t             hw_entry;
    xPON_IGMP_HWNATEntry_t*            entry = NULL;
    xPON_IGMP_HWNATEntry_t*              tmp = NULL;
    struct list_head*        hwnat_drop_flow = NULL;
    xPON_IGMP_HWNATEntry_t* hwnat_drop_entry = NULL;
    int found_entry = 0;
    
    memset(&hw_entry, 0 , sizeof(xPON_IGMP_HWNATEntry_t));

    spin_lock(&hw_nat_drop_multicast_lock);
    entry = (xPON_IGMP_HWNATEntry_t* )arg;
    if(NULL == entry)
    {
        goto out_unlock;
    }

    /*ensure entry exist*/
	hwnat_drop_flow = xpon_get_hwnat_drop();
	list_for_each_entry_safe(hwnat_drop_entry,tmp,hwnat_drop_flow,list)
    {
        if(hwnat_drop_entry == entry)
        {
            found_entry = 1;
            break;
        }
    }
	if(1 != found_entry)
    {
        goto out_unlock;
    }
   
    
    
	if (XPON_MASK_MLDV1 == entry->hwnat_type)
    {
        type = 1;
    }   

	if (hwnat_is_drop_entry_hook)
		ret = hwnat_is_drop_entry_hook(entry->hwnat_index,entry->grp_addr,entry->src_addr,type);

	if (ret > 0)
	{
		mod_timer(&entry->age_timer,round_jiffies(jiffies)+XPON_HWNAT_AGE_TIME);
		goto out_unlock;
	}
    
    /*ensure the same entry*/
    hw_entry.hwnat_index = entry->hwnat_index;
    hw_entry.hwnat_vid   = entry->hwnat_vid;
    hw_entry.hwnat_type  = entry->hwnat_type;
    if(XPON_MASK_IGMPV2 == entry->hwnat_type)
    {
        memcpy(hw_entry.grp_addr, entry->grp_addr, 4);
        memcpy(hw_entry.src_addr, entry->src_addr, 4);
    }
    else if(XPON_MASK_MLDV1 == entry->hwnat_type)
    {
        memcpy(hw_entry.grp_addr, entry->grp_addr, 16);
        memcpy(hw_entry.src_addr, entry->src_addr, 16);
    }
    else
    {
        goto out_unlock;
    }

	if (entry)
    {
        hwnat_drop_flow = xpon_get_hwnat_drop();
        list_for_each_entry_safe(hwnat_drop_entry,tmp,hwnat_drop_flow,list)
        {
            if((hw_entry.hwnat_index   == hwnat_drop_entry->hwnat_index) \
               && (hw_entry.hwnat_vid  == hwnat_drop_entry->hwnat_vid)  \
               && (hw_entry.hwnat_type == hwnat_drop_entry->hwnat_type))
            {
                if((XPON_MASK_IGMPV2 == entry->hwnat_type) \
                    && !memcmp(hw_entry.grp_addr, hwnat_drop_entry->grp_addr,4) \
                    && !memcmp(hw_entry.src_addr, hwnat_drop_entry->src_addr,4))
                {
                    xpon_hwnat_delete_drop(hwnat_drop_entry);
					break;
                }
                else if((XPON_MASK_MLDV1 == entry->hwnat_type) \
                    && !memcmp(hw_entry.grp_addr, hwnat_drop_entry->grp_addr,16) \
                    && !memcmp(hw_entry.src_addr, hwnat_drop_entry->src_addr,16))
                {
                    xpon_hwnat_delete_drop(hwnat_drop_entry);
					break;
                }
                else
                {
                    MULTICAST_NOTICE_INFO("we can do nothing.\n");
                }
            }
        }
    }   

out_unlock:
    
    spin_unlock(&hw_nat_drop_multicast_lock);
    
	return;
}
#endif

int xpon_hwnat_add_drop(struct sk_buff* skb)
{
	xPON_IGMP_HWNATEntry_t* entry = NULL;
    xPON_IGMP_HWNATEntry_t* hwnat_drop_entry = NULL;
    xPON_IGMP_HWNATEntry_t* ptr = NULL;
	struct list_head* hwnat_drop = NULL;
	unsigned char dest_addr[16],src_addr[16];
	int vid = -1;
	short int proto = 0;
	int eth_type = 0;
    int index = -1;
	
	if (NULL == skb)
    {
        return 0;
    }
    
	MULTICAST_NOTICE_INFO("xpon_hwnat_add_drop .\n" );
	memset(dest_addr,0,16);
	memset(src_addr,0,16);
	vid = xpon_get_outmost_vid(skb);
	eth_type = xpon_get_packet_type(skb);
	eth_type &= 0xffff;
	
	xpon_get_src_addr(src_addr, skb);
	xpon_get_dest_addr(dest_addr, skb);
	
	if (eth_type==ETH_P_IP)
		proto = XPON_MASK_IGMPV2;	
	else if (eth_type==ETH_P_IPV6)
		proto = XPON_MASK_MLDV1;
	else
		xpon_igmp_debug(XPON_IGMP_DEBUG_HW,"\n ========>xpon_hwnat_add_drop(): tpye = %x",eth_type);

    entry = (xPON_IGMP_HWNATEntry_t* )xpon_alloc(sizeof(xPON_IGMP_HWNATEntry_t));
    if (NULL == entry)
    {
        return 0;
    }
    
    spin_lock_bh(&hw_nat_drop_multicast_lock);
	hwnat_drop = xpon_get_hwnat_drop();
    if(NULL == hwnat_drop)
    {
        xpon_free(entry);
        goto out_unlock;
    }
    
	index = xpon_hwnat_flow_index(skb);
    if(0 >= index)
    {
        xpon_free(entry);
        goto out_unlock;
    }

    /*before add entry, del same entry, ensure index unique*/
    list_for_each_entry_safe(hwnat_drop_entry, ptr, hwnat_drop, list)    
    {
        if (hwnat_drop_entry->hwnat_index == index)
        {
            xpon_hwnat_delete_entry(hwnat_drop_entry);
			break;
        }      
    }

	entry->hwnat_type = proto;
	entry->hwnat_vid= vid;
	entry->hwnat_index = index;
	entry->hwnat_mask = 0;
	entry->sw_mask = 0;
	memcpy(entry->grp_addr,dest_addr,16);
	memcpy(entry->src_addr,src_addr,16);
	list_add_tail(&entry->list,hwnat_drop);
//	setup_timer(&entry->age_timer, xpon_hwnat_drop_timeout, (unsigned long)entry);	
//	mod_timer(&entry->age_timer,round_jiffies(jiffies)+XPON_HWNAT_AGE_TIME);
    
out_unlock:
    spin_unlock_bh(&hw_nat_drop_multicast_lock);

	return 0;
}

int xpon_hwnat_drop_multicast(struct sk_buff* skb)
{
	int index = xpon_hwnat_flow_index(skb);
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int ret = 0;
	
	if(igmp_conf->flag & XPON_HWNAT_DROP_DISABLED)
		 goto skb_drop;
		
	if (index <= 0)
		goto skb_drop;
	
	if (ra_sw_nat_cds_all_ratelimit_hook)
	{
		ret = ra_sw_nat_cds_all_ratelimit_hook(skb);
		if(!ret){
			if (ra_sw_nat_hook_drop_packet)
			ret = ra_sw_nat_hook_drop_packet(skb);
		}			
	}
	else{
		if (ra_sw_nat_hook_drop_packet)
			ret = ra_sw_nat_hook_drop_packet(skb);
	}
	
	if (ret > 0)
	{
        xpon_hwnat_add_drop(skb);
	}
skb_drop:	
	kfree_skb(skb);

	return 0;
}


int xpon_hwnat_wan_mvlan_change()
{
	xpon_hwnat_clear_all_drop();
	
	xpon_hwnat_clear_flows();

	igmp_hwnat_clear_flows();
	
	return 0;	
}
int xpon_hwnat_flow_read(char *buf, char **start, off_t off, int count,int *eof, void *data)
{
	int len = 0;
	xPON_IGMP_HWNATEntry_t* entry = NULL;
	struct list_head* hwnat_flow = xpon_get_hwnat_list();

	len += sprintf(buf,"index type mask  src_vlan   vid   grp_addr       src_addr \n");
	list_for_each_entry_rcu(entry,hwnat_flow,list)
	{
		len += sprintf(buf+len,"%d  %d     %d   %d      0x%08x  %d   [%02x-%02x-%02x-%02x] [%02x-%02x-%02x-%02x] \n",entry->hwnat_index,entry->hwnat_type,entry->hwnat_mask,
							entry->sw_mask,entry->src_vlan,entry->hwnat_vid,entry->grp_addr[0],entry->grp_addr[1],entry->grp_addr[2],entry->grp_addr[3],
							entry->src_addr[0],entry->src_addr[1],entry->src_addr[2],entry->src_addr[3]);
		
	}
	
	*start = buf + off;
	if (len < off + count)
		*eof = 1;
	len -= off;
	if (len > count)
		len = count ;
	if (len <0)
		len = 0;
		
	return len;

}

int xpon_hwnat_drop_read(char *buf, char **start, off_t off, int count,int *eof, void *data)
{
	int len = 0;
	xPON_IGMP_HWNATEntry_t* entry = NULL;
	struct list_head* hwnat_flow = xpon_get_hwnat_drop();

	len += sprintf(buf,"index  type  vid  grp_addr   src_addr \n");
	list_for_each_entry_rcu(entry,hwnat_flow,list)
	{
		len += sprintf(buf+len,"%d  %d  %d  [%02x-%02x-%02x-%02x] [%02x-%02x-%02x-%02x] \n",entry->hwnat_index,entry->hwnat_type,
							entry->hwnat_vid,entry->grp_addr[0],entry->grp_addr[1],entry->grp_addr[2],entry->grp_addr[3],
							entry->src_addr[0],entry->src_addr[1],entry->src_addr[2],entry->src_addr[3]);
		
	}
	
	*start = buf + off;
	if (len < off + count)
		*eof = 1;
	len -= off;
	if (len > count)
		len = count ;
	if (len <0)
		len = 0;
		
	return len;

}

void xpon_droptbl_del_by_port_vid(int port, int vid)
{
    struct list_head*           hwnat_drop_flow = NULL;
    xPON_IGMP_HWNATEntry_t*     hwnat_drop_entry = NULL;
	xPON_IGMP_HWNATEntry_t*     tmp = NULL;

    spin_lock_bh(&hw_nat_drop_multicast_lock);
    hwnat_drop_flow = xpon_get_hwnat_drop();
    if(hwnat_drop_flow == NULL){
        spin_unlock_bh(&hw_nat_drop_multicast_lock);
        return;
    }
    
	list_for_each_entry_safe(hwnat_drop_entry, tmp, hwnat_drop_flow, list)
	{
	    if (hwnat_drop_entry->hwnat_vid == vid){
			xpon_hwnat_delete_drop(hwnat_drop_entry);
		}
	}
	spin_unlock_bh(&hw_nat_drop_multicast_lock);
}

int xpon_hwnat_delate_drop_by_fwd(xPON_FwdEntry_t* entry)
{
    struct list_head*        hwnat_drop_flow = NULL;
    xPON_IGMP_HWNATEntry_t* hwnat_drop_entry = NULL;
	xPON_IGMP_HWNATEntry_t*              tmp = NULL;

	spin_lock_bh(&hw_nat_drop_multicast_lock);
	if (entry)
	{
		xpon_igmp_debug(XPON_IGMP_DEBUG_HW,"to delete entry: [%02x-%02x-%02x-%02x] [%02x-%02x-%02x-%02x] \n",
							entry->grp_addr[0],entry->grp_addr[1],entry->grp_addr[2],entry->grp_addr[3],
							entry->src_addr[0],entry->src_addr[1],entry->src_addr[2],entry->src_addr[3]);
		
		hwnat_drop_flow = xpon_get_hwnat_drop();
		list_for_each_entry_safe(hwnat_drop_entry,tmp,hwnat_drop_flow,list)
		{
			xpon_igmp_debug(XPON_IGMP_DEBUG_HW,"current drop entry: %d  %d     %d   %d      0x%08x  %d   [%02x-%02x-%02x-%02x] [%02x-%02x-%02x-%02x] \n",
							hwnat_drop_entry->hwnat_index,hwnat_drop_entry->hwnat_type,hwnat_drop_entry->hwnat_mask,
							hwnat_drop_entry->sw_mask,hwnat_drop_entry->src_vlan,hwnat_drop_entry->hwnat_vid,
							hwnat_drop_entry->grp_addr[0],hwnat_drop_entry->grp_addr[1],hwnat_drop_entry->grp_addr[2],hwnat_drop_entry->grp_addr[3],
							hwnat_drop_entry->src_addr[0],hwnat_drop_entry->src_addr[1],hwnat_drop_entry->src_addr[2],hwnat_drop_entry->src_addr[3]);

			if(XPON_MASK_IGMPV2 == hwnat_drop_entry->hwnat_type)
			{
				if(entry->flag & XPON_MASK_IGMPV3)
				{
					if(0 == memcmp(entry->grp_addr, hwnat_drop_entry->grp_addr,4) && (0 == memcmp(entry->src_addr, hwnat_drop_entry->src_addr,4) || !xpon_is_non_zero(entry->src_addr,4)))
					{
						xpon_hwnat_delete_drop(hwnat_drop_entry);
						break;
					}
					else
						MULTICAST_NOTICE_INFO("entry not found.\n");
				}
				else
				{
					if(0 == memcmp(entry->grp_addr, hwnat_drop_entry->grp_addr,4))
					{
						xpon_hwnat_delete_drop(hwnat_drop_entry);
					}
					else
						MULTICAST_NOTICE_INFO("entry not found.\n");
				}
			}
			else if(XPON_MASK_MLDV1 == hwnat_drop_entry->hwnat_type) 
			{
				if(entry->flag & XPON_MASK_MLDV2){
					if(0 == memcmp(entry->grp_addr, hwnat_drop_entry->grp_addr,16) && (0 == memcmp(entry->src_addr, hwnat_drop_entry->src_addr,16) || !xpon_is_non_zero(entry->src_addr,16)))
					{
						xpon_hwnat_delete_drop(hwnat_drop_entry);
						break;
					}
					else{
						MULTICAST_NOTICE_INFO("entry not found.\n");
					}
				}else{
					if(0 == memcmp(entry->grp_addr, hwnat_drop_entry->grp_addr,16))
					{
						xpon_hwnat_delete_drop(hwnat_drop_entry);
					}else{
						MULTICAST_NOTICE_INFO("entry not found.\n");
					}
				
				}
			}
			else
			{
				MULTICAST_NOTICE_INFO("we can do nothing.\n");
			}
		}
	}else{
		MULTICAST_ERROR_INFO("fwd table has not found.\n");
		spin_unlock_bh(&hw_nat_drop_multicast_lock);
		return -1;
	} 
	spin_unlock_bh(&hw_nat_drop_multicast_lock);
	return 0;
}

/*************************debug info********************************************/
static int mac_to_str(unsigned char* mac_str,unsigned char* mac)
{
	if (mac_str==NULL || mac == NULL)
		return 0;	
	sprintf(mac_str,"%02x:%02x:%02x:%02x:%02x:%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
	return 0;
}

int debug_show_xpon_hwnat_list(void)
{
    xPON_IGMP_HWNATEntry_t*   tmp = NULL;
    xPON_IGMP_HWNATEntry_t* entry = NULL;
    struct list_head*  hwnat_flow = NULL;
    unsigned char grp_str[64] = {0};

    printk("***************HW FWD list*****************\n");
    
    spin_lock_bh(&hw_nat_multicast_lock);
    hwnat_flow = xpon_get_hwnat_list();
    list_for_each_entry_safe(entry,tmp,hwnat_flow,list)
    {
        memset(&grp_str, 0, sizeof(grp_str));
        printk("entry->hwnat_type = %d\n",   entry->hwnat_type);
        printk("entry->hwnat_index = %d\n",  entry->hwnat_index);
        printk("entry->hwnat_vid = %d\n",    entry->hwnat_vid);
        printk("entry->hwnat_mask = %d\n",   entry->hwnat_mask);
        printk("entry->sw_mask = %d\n",   entry->sw_mask);
        if(XPON_MASK_IGMPV2 == entry->hwnat_type)
        {   
			mac_to_str(grp_str,entry->grp_addr);
            printk("entry->grp_addr = %s\n", grp_str);
        }
        else if(XPON_MASK_MLDV1 == entry->hwnat_type)
        {
			//inet_ntop(AF_INET, entry->grp_addr, grp_str, 64);
            //printk("entry->grp_addr = %s\n", grp_str);
        }
    }
    spin_unlock_bh(&hw_nat_multicast_lock);
    
    printk("********************************\n");
    
    return 0;
    
}

int debug_show_xpon_hwnat_drop_list(void)
{
    xPON_IGMP_HWNATEntry_t*   tmp = NULL;
    xPON_IGMP_HWNATEntry_t* entry = NULL;
    struct list_head*  hwnat_flow = NULL;
    unsigned char grp_str[64] = {0};

    printk("\n****************HW DROP list****************\n");
    
    spin_lock_bh(&hw_nat_multicast_lock);
    hwnat_flow = xpon_get_hwnat_drop();
    list_for_each_entry_safe(entry,tmp,hwnat_flow,list)
    {
        memset(&grp_str, 0, sizeof(grp_str));
        printk("entry->hwnat_type = %d\n",   entry->hwnat_type);
        printk("entry->hwnat_index = %d\n",  entry->hwnat_index);
        printk("entry->hwnat_vid = %d\n",    entry->hwnat_vid);
        printk("entry->hwnat_mask = %d\n",   entry->hwnat_mask);
        printk("entry->sw_mask = %d\n",   entry->sw_mask);
        if(XPON_MASK_IGMPV2 == entry->hwnat_type)
        {   
			mac_to_str(grp_str,entry->grp_addr);
            printk("entry->grp_addr = %s\n", grp_str);
        }
        else if(XPON_MASK_MLDV1 == entry->hwnat_type)
        {
			//inet_ntop(AF_INET, entry->grp_addr, grp_str, 64);
            //printk("entry->grp_addr = %s\n", grp_str);
        }
    }
    spin_unlock_bh(&hw_nat_multicast_lock);
    
    printk("********************************\n");
    
    return 0;
    
}

void xpon_hwnat_show_hwnat_list(void){
	debug_show_xpon_hwnat_list();
	debug_show_xpon_hwnat_drop_list();
}

/*************************debug info end ******************************************/


//////////////////////////////////////////////////////////////////////////////////////////////////
#define u16 unsigned short int 
#define u8  unsigned char

extern int macMT7530VLan_Entry_SetETagMode(u16 vid,u16 vlan);
extern int macMT7530VLan_Entry_SetETag(u16 vid,u8 port,u8 etag);
extern int macMT7530VLan_Entry_GetETag(u16 vid,u8 port,u8* etag);
extern int macMT7530VLan_Entry_SetSTag(u16 vid,u8 port,u8 stag);
extern int macMT7530VLan_Entry_GetSTag(u16 vid,u8 port,u8* stag);
extern int macMT7530VLan_Port_AddSTag(u8 port,u8 stag,u16 vid);
extern int macMT7530VLan_Entry_Enable(u16 vid, u8 sw);

int xpon_switch_enable_entry(int vid,int sw)
{
	macMT7530VLan_Entry_Enable(vid,sw);
	return 0;
}

int xpon_switch_set_mode(int vid,int mode)
{
	xpon_igmp_debug(XPON_IGMP_DEBUG_HW,"\n xpon_switch_set_mode(): vid = %d, mode = %d",vid,mode);

	macMT7530VLan_Entry_SetETagMode(vid,mode);
	
	return 0;
}
int xpon_switch_set_egtag(int vid,int port,int etag)
{
	unsigned char val;
	macMT7530VLan_Entry_GetETag(vid,macMT7530LanPortMap2Switch(port),&val);

	xpon_igmp_debug(XPON_IGMP_DEBUG_HW,"\n xpon_switch_set_egtag(): vid = %d, port = %d, etag = %d  val = %d",vid,port,etag,val);
	
	macMT7530VLan_Entry_SetETag(vid,macMT7530LanPortMap2Switch(port),etag);
	return 0;
}

int xpon_switch_add_vid(int port,int idx,int vid)
{
	
	xpon_igmp_debug(XPON_IGMP_DEBUG_HW,"\n xpon_switch_add_vid(): port = %d, idx = %d, vid = %d",port,idx,vid);

	macMT7530VLan_Port_AddSTag(macMT7530LanPortMap2Switch(port),idx,vid);
	return 0;
}

int xpon_switch_set_stag(int vid,int port,int stag)
{
	unsigned char val;
	macMT7530VLan_Entry_GetSTag(vid,macMT7530LanPortMap2Switch(port),&val);	

	xpon_igmp_debug(XPON_IGMP_DEBUG_HW,"\n xpon_switch_set_stag(): vid = %d, port = %d, stag = %d val = %d",vid,port,stag,val);

	macMT7530VLan_Entry_SetSTag(vid,macMT7530LanPortMap2Switch(port),stag);
	return 0;
}

int xpon_switch_update_entry(int vid,int port,int mode ,int newvid)
{
#if 0
	int idx = xpon_port_vlan_find(port,vid);
	int etag;
	
	if (mode == 0)
	{
		xpon_switch_set_mode(vid,1);
		etag = 2;
	}
	else if (mode == 1)
	{
		xpon_switch_set_mode(vid,1);
		etag = 0;
	}
	else if (mode == 2 )
	{
		etag  = 1;
		xpon_switch_set_mode(vid,1);
		xpon_switch_add_vid(port-1,idx,newvid);
		xpon_switch_set_stag(vid,port-1,idx);
	}
	else
	{
		return -1;
	}
	xpon_switch_set_egtag(vid,port-1,etag);
	xpon_switch_enable_entry(vid,1);

#endif
	return 0;
}

int xpon_switch_update_port(int port)
{
	xPON_PortVLan_t* pvlan = xpon_port_vlan_by_id(port);
	xPON_PortConf_t* port_conf = xpon_port_conf_by_id(port);
	int i,mode,vid,newvid;

	if(NULL == pvlan || NULL == port_conf){
		return -1;
	}
	
	mode = port_conf->tagstrip;
	
	for(i=0;i<XPON_PORT_VLAN_CNT;i++)
	{
		vid =pvlan->vlan_id[i];
		newvid = pvlan->vlan_trans[i];
		if (vid > 0)
		{
			xpon_switch_update_entry(vid,port,mode,newvid);
		}
	}
	return 0;

}


static int update_port_mask_static_acl(int gemport,int vid, unsigned char* grp_addr, unsigned char* src_ip)
{
    xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
    int  i  = 0; 
    int port = 0;
    int match_static_flag = NOT_MATCH_STATIC_ACL;
    int mask = 0;
	int logic_lan_idx = 0;
	char dev_name[8] = {0};
    
    for(i=1;i<igmp_conf->uni_num;i++)
    {
        if ((igmp_conf->uni_port[i].port_flag & XPON_BRIDGE_PORT)==0)
            continue;
        
        port = i;

        match_static_flag  = static_acl_ctr(port, vid, gemport, grp_addr, src_ip, 1);
        //printk("match_static_flag = %d, line= %d.\n",match_static_flag,__LINE__);
        if(MATCH_STATIC_ACL == match_static_flag)
        {
        	ENCT_HOOK_XPON_ETH_MAP_PORT_TO_DEV_NAME(port, dev_name);
			logic_lan_idx = getLogicLANIndexByName(dev_name);
			mask |= 1 << logic_lan_idx;
        }
    }

    return  mask;

}

/*
int xpon_switch_clear_mulvlan(int port)
{
	xPON_PortVLan_t* pvlan = xpon_port_vlan_by_id(port);
	int i,vid;
	
	for(i=0;i<XPON_PORT_VLAN_CNT;i++)
	{
		vid = pvlan->vlan_id[i];
		if (vid > 0)
		{
			xpon_switch_set_mode(vid,0);
		}
	}
	return 0;
}
*/
