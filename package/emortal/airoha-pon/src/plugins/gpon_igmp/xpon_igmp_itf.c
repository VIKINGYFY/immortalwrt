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
	xpon_igmp_itf.c
	
	Abstract:It is for XPON multicast interface with OMCI or OAM management

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
#include <linux/netdevice.h>
#include <linux/proc_fs.h>
#include <linux/time.h>
#include <linux/if_vlan.h>
#include <linux/module.h>
#include <linux/version.h>

#include "xpon_igmp_core.h"
#include "linux/libcompileoption.h"
#include "ecnt_hook/ecnt_hook_net.h"
//#include <xpon_global/private/xpon_igmp.h>
#include "ecnt_hook/ecnt_hook_xpon_igmp.h"
#include "ecnt_hook/ecnt_hook_multicast.h"

//#include "xpon_port_info.h"

#define XPON_IGMP_MAX_ROBUST  3000
#define XPON_IGMP_MAX_TIME_INTERVAL  10000
#define XPON_IGMP_MAX_RESP_TIME 100000
#define MAX_UNI_NUM 4

static xPON_PortConf_t port_conf[4];
static xPON_PortVLan_t port_vlan[4];
static xPON_PortConf_t veip_port_conf[4];
static xPON_PortVLan_t veip_port_vlan[4];
static xPON_PortStatus_t port_status[4];
static xPON_PortStatus_t veip_port_status[4];

int xpon_igmp_hybrid_g = 0;

/*typedef struct xPON_User_Subscribe_Temp_s
{
    short int index;
	unsigned char srcip[16];
	unsigned char program_ip[16];
}xPON_User_Subscribe_Temp_t;
*/
// Map uni port to (bridge) interface, 4 port
static xPON_PortEntry_t uni_port_4p[] = 
{
	{XPONIGMP_PORT_ETH_START+0,"eth0.1",4,&port_conf[0],&port_vlan[0],&port_status[0]},
	{XPONIGMP_PORT_ETH_START+1,"eth0.2",4,&port_conf[1],&port_vlan[1],&port_status[1]},
	{XPONIGMP_PORT_ETH_START+2,"eth0.3",4,&port_conf[2],&port_vlan[2],&port_status[2]},
	{XPONIGMP_PORT_ETH_START+3,"eth0.4",4,&port_conf[3],&port_vlan[3],&port_status[3]}
};


static xPON_PortEntry_t veip_port_1p[] = 
{
	{XPONIGMP_PORT_VEIP_ID,"veip1",4,&veip_port_conf[0],&veip_port_vlan[0],&veip_port_status[0]}
};

static xPON_PortEntry_t* uni_port = uni_port_4p; 
static xPON_PortEntry_t* veip_port = veip_port_1p;


#ifdef TCSUPPORT_PON_IP_HOST
#define MAX_ANI_PORT_NUM 1
static xPON_AniEntry_t ani_port[MAX_ANI_PORT_NUM];
#else
static xPON_AniEntry_t ani_port[] = 
{
    	{1,"pon",7}	
};
#endif

#define XPON_MULVLAN_NUMBER 16
#define HIT_BIND_MUL_CPU		0x18

static uint8 multicast_to_cpu = 0;
static xPON_MVLanEntry_t mul_vlan[XPON_MULVLAN_NUMBER];

static xPON_FwdTable_t fwd_tbl;

static xPON_IGMPConf_t igmp_conf;

xPON_IGMPConf_t* xpon_get_igmp_conf(void)
{
	return &igmp_conf;
}

static struct list_head multicast_up_vlan_list;
static spinlock_t up_vlan_lock;
static mc_ctrl_packet_rx mc_ctrl_packet_rx_cb_func = NULL;
#define COPY_TO_USER(user,kernel,size) \
                    	if(0 != copy_to_user(user,kernel,size)){\
                    		printk("func:%s line:%d copy_to_user failed\n",__func__,__LINE__);\
                    		return -1;\
                    	}\
                                        
#define COPY_FROM_USER(kernel,user,size) \
                        if(0 != copy_from_user(kernel,user,size)){\
                            printk("func:%s line:%d copy_from_user failed\n",__func__,__LINE__);\
                            return -1;\
                        }\

/*---------------------------------Global IGMP config function-------------------------------------*/
 void* xpon_alloc(int size)
{
	void* ptr = NULL;
	if (size>0)
	{
		ptr = kmalloc(size, GFP_ATOMIC);
		memset(ptr,0,size);
	}
	return ptr;
} 

 void xpon_free(void* ptr)
{
	if (ptr)
    {
        kfree(ptr);
        ptr = NULL;
    } 
    
	return;
}

static void xpon_free_rcu(struct rcu_head *head) 
{
	struct xPON_FwdEntry_s *entry
		= container_of(head, struct xPON_FwdEntry_s, rcu);
	kfree(entry);
	return ;
	
}

static inline unsigned long xpon_get_system_time_sec(void) 
{
	unsigned long sec;
	sec  = (jiffies) / HZ;
	return sec;
}

xPON_PortEntry_t* xpon_port_entry_by_id(int port)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int i=0;
	for(i=0; i<igmp_conf->uni_num; i++)
	{
		if (igmp_conf->uni_port[i].port_id==port)
		{
			return &igmp_conf->uni_port[i];
		}
	}
	for(i = 0; i< igmp_conf->veip_num; i++){
		if (igmp_conf->veip_port[i].port_id==port)
		{	
			return &igmp_conf->veip_port[i];
		}
	}
	return NULL;
}

xPON_PortEntry_t* xpon_port_entry_by_name(char* name)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int i=0;
    
    if(NULL == name)
    {
        return NULL;
    }
    
	for(i=0; i<igmp_conf->uni_num; i++)
	{
		if (!strcmp(igmp_conf->uni_port[i].port_name,name))
		{
			return &igmp_conf->uni_port[i];
		}
	}
	for(i = 0; i< igmp_conf->veip_num; i++){
		if (!strcmp(igmp_conf->veip_port[i].port_name,name))
		{
			return &igmp_conf->veip_port[i];
		}	
	}
	return NULL;
}

 xPON_PortConf_t* xpon_port_conf_by_name(char* name)
{
	xPON_PortEntry_t* entry =  xpon_port_entry_by_name(name);
	if (!entry)
		return NULL;
	return entry->port_conf;
}

 xPON_PortConf_t* xpon_port_conf_by_id(int port)
{
	xPON_PortEntry_t* entry =  xpon_port_entry_by_id(port);
	if (!entry)
		return NULL;
	return entry->port_conf;
}

xPON_PortStatus_t* xpon_port_status_by_id(int port)
{
	xPON_PortEntry_t* entry =  xpon_port_entry_by_id(port);
	if (!entry)
		return NULL;
	return entry->port_status;
}

xPON_PortVLan_t* xpon_port_vlan_by_name(char* name)
{
	xPON_PortEntry_t* entry =  xpon_port_entry_by_name(name);
	if (!entry)
		return NULL;
	return entry->port_vlan;
}

 xPON_PortVLan_t* xpon_port_vlan_by_id(int port)
{
	xPON_PortEntry_t* entry =  xpon_port_entry_by_id(port);
	if (!entry)
		return NULL;
	return entry->port_vlan;
}

 int xpon_port_index_by_id(int port)
{
	xPON_IGMPConf_t* igmp = xpon_get_igmp_conf();
	int idx;

	for(idx =0;idx < igmp->uni_num;idx++)
	{
		if (igmp->uni_port[idx].port_id == port)
			return idx;
	}
	return -1;
}

int xpon_port_index_by_name(char* name)
{
	xPON_IGMPConf_t* igmp = xpon_get_igmp_conf();
	int idx;
    
    if(NULL == name)
    {
        return -1;
    }
    
	for(idx =0;idx < igmp->uni_num;idx++)
	{
		if (!strcmp(igmp->uni_port[idx].port_name,name))
			return idx;
	}

	for(idx =0;idx < igmp->veip_num;idx++)
	{
		if (!strcmp(igmp->veip_port[idx].port_name,name))
			return idx;
	}
		
	return -1;
}

int xpon_is_uni_port(char* name)
{
    if(NULL == name)
    {
        return 0;
    }
    
	return xpon_port_entry_by_name(name)==NULL ? 0:1;
}

xPON_AniEntry_t* xpon_get_ani_entry(char* name)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int i=0;
	for(i=0; i<igmp_conf->ani_num; i++)
	{
		if (!strcmp(igmp_conf->ani_port[i].ani_name,name) || !strcmp(igmp_conf->ani_port[i].ani_name,PON_ITF))
		{
			return &igmp_conf->ani_port[i];
		}
	}
	return NULL;
}

xPON_AniEntry_t* xpon_ani_entry_by_id(int id)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int i=0;
	for(i=0; i<igmp_conf->ani_num; i++)
	{
		if (igmp_conf->ani_port[i].ani_id == id)
		{
			return &igmp_conf->ani_port[i];
		}
	}
	return NULL;
}


int xpon_is_ani_port(char* name)
{
    if(NULL == name)
    {
        return 0;
    }

	if(!strncmp(PON_ITF,name,3))
	{
		MULTICAST_CRITIC_INFO("from pon, valid\n");
		return 1;
	}
	
	return xpon_get_ani_entry(name)== NULL ? 0: 1;
}

int xpon_get_up_vlan_operation_point(int port, e_vlan_operation_point_t *point)
{
    if(TCSUPPORT_XPON_IGMP_CHT_VAL)
    {
        *point = vlan_operation_in_vlan_module;
        return 0;
    }
    else
    {
        xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
        if (!conf)
            return -1;
        
        if(MULTCAST_UPTAG_TRANSPARENT == conf->up_vlan_mode)
        {
            *point = vlan_operation_in_vlan_module;
        }
        else
        {
            *point = vlan_operation_in_multicast_module;
        }
        
        return 0;   
    }
}

int xpon_get_down_vlan_operation_point(int port, e_vlan_operation_point_t *point)
{
    if(TCSUPPORT_XPON_IGMP_CHT_VAL)
    {
        *point = vlan_operation_in_vlan_module;
        return 0;
    }
    else
    {
        xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
        if (!conf)
            return -1;
        
        if(0 == conf->down_vlan_mode)
        {
            *point = vlan_operation_in_vlan_module;
        }
        else
        {
            *point = vlan_operation_in_multicast_module;
        }
        
        return 0;   
    }
}
/*-------------------------General xPON ONU config api implementation--------------------------*/  

static int xpon_igmp_get_ver(int port)
{
	int ret = -1;
	int multicast_proto_mask;
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	multicast_proto_mask = conf->proto_mask;
	switch(multicast_proto_mask&0xff)
	{
		case 2:
		case 3:		
			ret = 2;
 			break;
		case 6:
		case 7:	
			ret = 3;
			break;
		case 0x16:
		case 0x17:		
			ret = 16;
			break;
		case 0x36:
		case 0x37:
			ret = 17;
			break;
		default:
			break;
	}
	return ret;
}

static int xpon_igmp_set_ver(int port,int ver)
{
	int ret = 0;
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
    
    MULTICAST_NOTICE_INFO("ver = %d, g_care_ver_dynlist_stalist_op = %d .\n", ver, g_care_ver_dynlist_stalist_op);
    if(true == g_care_ver_dynlist_stalist_op)
    {
        switch(ver)
        {
            case 2:
                conf->proto_mask = 3;
            break;
            case 3:
                conf->proto_mask = 7;
            break;
            case 16:
                conf->proto_mask = 0x17;
            break;
            case 17:
                conf->proto_mask = 0x37;
            break;
            default:
                ret = -1;
            break;
        }
    }
	return ret;
}

static int xpon_igmp_get_func(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	if (conf->work_mode>=0 && conf->work_mode <= 2 )
		return conf->work_mode;
	return -1;
}

static int xpon_igmp_set_func(int port,int mode)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	if (mode>=0 && mode <= 2 )
	{
		conf->work_mode = mode;
		return 0;
	}
	return -1;
}

static int xpon_igmp_get_fastleave(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	return conf->fastleave;
}

static int xpon_igmp_set_fastleave(int port,int mode)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	if (conf->fastleave==0 || conf->fastleave==1)
	{
		conf->fastleave = mode;
		return 0;
	}
	return -1;
}

static int xpon_igmp_get_up_tci(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	return conf->up_vlan_tci;
}

static int xpon_igmp_set_up_tci(int port,int tci)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	conf->up_vlan_tci = tci;
	return 0;
}

static int xpon_igmp_get_up_tagctrl(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	return conf->up_vlan_mode;
}

static int xpon_igmp_set_up_tagctrl(int port,int mode)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	if (mode >=0 && mode <= 3)
	{
		conf->up_vlan_mode = mode;
		return 0;
	}
	return -1;
}

static int xpon_igmp_get_down_tci(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	return conf->down_vlan_tci;
}

static int xpon_igmp_set_down_tci(int port,int tci)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	
	conf->down_vlan_tci = tci;  
	return 0;
}

static int xpon_igmp_get_down_tagctrl(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	return conf->down_vlan_mode;
}


static int xpon_igmp_set_down_tagctrl(int port,int mode)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	if (mode >=0 && mode <= 4)
	{
		conf->down_vlan_mode = mode;    
		return 0;
	}
	return -1;
}

static int xpon_igmp_get_tagstrip(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;
	return conf->tagstrip;
}


static int xpon_igmp_set_tagstrip(int port,int mode)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL  || mode > 2 || mode < 0)
		return -1;
	if(mode == 1 || mode == 0){
		xpon_igmp_set_down_tagctrl(port, mode);
	}
	else if(mode == 2){ 
		xpon_igmp_set_down_tagctrl(port, 4);
	}

	conf->tagstrip = mode;	
	return 0;
}


static int xpon_igmp_get_maxrate(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	return conf->maxrate;
}	

static int xpon_igmp_set_maxrate(int port,int rate)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	conf->maxrate = rate;
	return 0;
}	

static int xpon_igmp_get_robust(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
	return conf->robust;
}	

static int xpon_igmp_set_robust(int port,int robust)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (!conf)
		return -1;
    /* if mod_timer input a too larege arg, timer may timeout immediately*/
    if(robust > XPON_IGMP_MAX_ROBUST || robust == 0){
        return 0;
    }
	conf->robust = robust;
	return 0;
}	

static int xpon_igmp_get_unauthor(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;
	return conf->unauthor;
}	

static int xpon_igmp_set_unauthor(int port,int unauthor)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;
	conf->unauthor = unauthor;
	return 0;
}	

static int xpon_igmp_get_query_ip(int port,unsigned long val)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;
	COPY_TO_USER((void* __user)val,conf->queryip,16);
	return 0;
}	

static int xpon_igmp_set_query_ip(int port,unsigned long val)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;
	COPY_FROM_USER(conf->queryip,(void* __user)val,16);
	
	return 0;
}	

static int xpon_igmp_get_query_interval(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;
	return conf->queryinterval;
}	

static int xpon_igmp_set_query_interval(int port,int interval)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;
    /* if mod_timer input a too larege arg, timer may timeout immediately*/
    if(interval > XPON_IGMP_MAX_TIME_INTERVAL || interval == 0){
        return 0;
    }
	conf->queryinterval = interval;
	return 0;
}	


static int xpon_igmp_get_last_interval(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;
	return conf->lastinterval;
}	

static int xpon_igmp_set_last_interval(int port,int interval)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;
	
	if(interval != 0)
		conf->lastinterval = interval;
	
	return 0;
}	

static int xpon_igmp_get_query_maxresp(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;
	return conf->maxresp;
}	

static int xpon_igmp_set_query_maxresp(int port,int maxresp)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;
    /* if mod_timer input a too larege arg, timer may timeout immediately*/
    if(maxresp > XPON_IGMP_MAX_RESP_TIME || maxresp == 0){
        return 0;
    }
	conf->maxresp = maxresp;
	return 0;
}

int xpon_igmp_get_max_playgroup(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;
	return conf->maxgroup;
}	

static int xpon_igmp_set_max_playgroup(int port,int maxgroup)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;
	conf->maxgroup = maxgroup;
	return 0;
}

static int xpon_igmp_get_max_bandwidth(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;

	return conf->maxbw;
}	

static int xpon_igmp_set_max_bandwidth(int port,int maxbw)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;

	conf->maxbw = maxbw;
	return 0;
}

static int xpon_igmp_get_bandwidth_enforcement(int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;

	return conf->bw_enforce;
}	

static int xpon_igmp_set_bandwidth_enforcement(int port,unsigned char bwe)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	if (conf == NULL)
		return -1;

	conf->bw_enforce = bwe;
	return 0;
}

static int xpon_igmp_get_pmc_curr_mcast_bw(int port)
{
	xPON_PortStatus_t * status = xpon_port_status_by_id(port);
	if (status == NULL)
		return -1;

	return status->pmc.current_mcast_bw;
}

static int xpon_igmp_get_pmc_join_msg(int port)
{
	xPON_PortStatus_t * status = xpon_port_status_by_id(port);
	if (status == NULL)
		return -1;

	return status->pmc.join_msg;
}

static int xpon_igmp_get_pmc_bw_exceeded(int port)
{
	xPON_PortStatus_t * status = xpon_port_status_by_id(port);
	if (status == NULL)
		return -1;

	return status->pmc.bw_exceeded;
}

static int xpon_igmp_get_user_subscribe_cnt(int port)
{
    int cnt = 0;
	xPON_PortStatus_t * status = xpon_port_status_by_id(port);
	if (status != NULL)
	    cnt = status->user_subscribe_cnt;

    return cnt;	
}

static int xpon_igmp_get_user_subscribe_by_index(int port, int index, char * src_ip, char * program_ip)
{
	xPON_PortStatus_t * status = xpon_port_status_by_id(port);
    struct list_head * us_list = &(status->user_subscribe_list);
    xPON_User_Subscribe_Entry_t * entry = NULL;

	if (status == NULL)
		return -1;
		
	if (list_empty(us_list))
		return -1;

    entry = xpon_user_subscribe_find(port, index, NULL, NULL);

    if (entry != NULL)
    {
        memcpy(src_ip, entry->srcip, 16);
        memcpy(program_ip, entry->program_ip, 16);
    }
    
	return 0;
}

static int xpon_igmp_get_port_flag(int port)
{
	xPON_PortEntry_t* entry = xpon_port_entry_by_id(port);
	if (entry == NULL)
		return -1;
	return entry->port_flag;
}

static int xpon_igmp_set_port_flag(int port,int flag)
{
	xPON_PortEntry_t* port_entry;
	xPON_AniEntry_t* ani_entry;
	xPON_IGMPConf_t* igmp_conf =  xpon_get_igmp_conf();
	
	if (port <= igmp_conf->uni_num || port == XPONIGMP_PORT_VEIP_ID)
	{
		port_entry = xpon_port_entry_by_id(port);
		if (port_entry == NULL)
			return -1;
		port_entry->port_flag = flag;
	} 
	else
	{
		ani_entry = xpon_ani_entry_by_id(port - igmp_conf->uni_num);
		if (ani_entry == NULL)
			return -1;
		ani_entry->ani_flag = flag;		
		
	}

	printk("xpon_igmp_set_port_flag success, port=%d,flag=0x%x\n",port,flag);
	return 0;
}


static int xpon_igmp_get_igmp_flag(void)
{
	xPON_IGMPConf_t* igmp_conf =  xpon_get_igmp_conf();

	return igmp_conf->flag;
}

static int xpon_igmp_set_igmp_flag(int flag)
{
    xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
    int old_flag = igmp_conf->flag;
    
    igmp_conf->flag = flag;
    if(((old_flag & XPON_MLD_SNOOPING_DISABLED) != (flag & XPON_MLD_SNOOPING_DISABLED))
        || ((old_flag & XPON_IGMP_SNOOPING_DISABLED) != (flag & XPON_IGMP_SNOOPING_DISABLED))){
        xpon_fwdtbl_clear();
    }

    if((old_flag & XPON_IGMP_DISABLED) != (flag & XPON_IGMP_DISABLED)){
        xpon_fwdtbl_clear();
    }

    return 0;
}

static int xpon_igmp_set_debug(int val)
{

	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();

	igmp_conf->dbglevel = val;
    
    g_MULTICAST_DEBUG_LEVEL = val;
    
	return 0;
}

static int xpon_igmp_set_care_ver_dy_stalist(int val)
{
    if(val)
    {
        g_care_ver_dynlist_stalist_op = true;
    }
    else
    {
        g_care_ver_dynlist_stalist_op = false;
    }
    
	return 0;
}

#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_IGMP_SET_GROUP)
int xpon_igmp_get_groupNum(void)
{
	return igmp_conf.group_num;
}

int xpon_igmp_set_groupNum(int num)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();

	igmp_conf->group_num = num;
	return 0;
}
#endif/*TCSUPPORT_COMPILE*/

int xpon_igmp_get_veip_acl(void)
{
	return igmp_conf.veip_acl;
}

int xpon_igmp_set_veip_acl(int val)
{
	if(val != 0 && val !=1)
	{
		return -1;
	}

	igmp_conf.veip_acl = val;
	printk("xpon_igmp_set_veip_acl veip_acl = %d\n",igmp_conf.veip_acl);

	return 0;
}

int xpon_igmp_get_empty_dynlist_pass(void)
{
	return igmp_conf.empty_dynlist_pass;
}

int xpon_igmp_set_empty_dynlist_pass(int val)
{
	if(val != 0 && val !=1)
	{
		return -1;
	}

	igmp_conf.empty_dynlist_pass = val;
	printk("xpon_igmp_set_empty_dynlist_pass veip_acl = %d\n",igmp_conf.empty_dynlist_pass);

	return 0;
}

static int xpon_igmp_set_dsbwctrl(int val)
{

	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();

	igmp_conf->dsbwctrl = val;
    
    g_DS_MCAST_BW_RATE_LIMIT_ENABLE = val;
    
	return 0;
}

static int xpon_igmp_clear_multicast_hw_entry(int val)
{
	xpon_fwdtbl_clear();
    MULTICAST_NOTICE_INFO("clear hw entry.\n");
	return 0;
}

static int xpon_igmp_clear_multicast_hw_drop_entry(unsigned long val)
{
	MULTICAST_NOTICE_INFO("clear hw drop entry.\n");
	return 0;
}
/*--------------------------Whitelis management api implementation---------------------------- */
xPON_WhiteList_Entry_t* xpon_whitelist_find(int opt,int port,int index)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	xPON_WhiteList_Entry_t* entry = NULL;
	struct list_head*  white_list;

	if (!conf)
		return NULL;
	
	if (opt == STATIC_WHITE_LIST)
		white_list = &(conf->sta_list);
	else
		white_list = &(conf->dyn_list);

	
	if (list_empty(white_list))
		return NULL;
	
	list_for_each_entry(entry,white_list,list)
	{
		if (entry->index == index)
		{
			return entry;
		}
	}

	return NULL;
}


int xpon_whitelist_del(int opt,int port,int index)
{
	xPON_WhiteList_Entry_t* entry = NULL;
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	entry = xpon_whitelist_find(opt,port,index);
	
	if (entry == NULL  || conf == NULL)
		return 0;

	if (entry->preview_info.rep_interval_timer.function != NULL)
		del_timer(&entry->preview_info.rep_interval_timer);
	list_del(&entry->list);
	xpon_free(entry);
	
	if (opt == STATIC_WHITE_LIST)	
		conf->sta_cnt--;
	else
		conf->dyn_cnt--;
     
	return 0;
}

int xpon_whitelist_clear(int opt,int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	xPON_WhiteList_Entry_t* entry, *tmp;
	struct list_head* white_list;
	
	if (!conf)
		return 0;
	
	if (opt == STATIC_WHITE_LIST)
		white_list = &(conf->sta_list);
	else
		white_list = &(conf->dyn_list);

	
	list_for_each_entry_safe(entry,tmp,white_list,list)
	{
		if (entry->preview_info.rep_interval_timer.function != NULL)
			del_timer(&entry->preview_info.rep_interval_timer);
		list_del(&entry->list);
		xpon_free(entry);
	}

	if (opt == STATIC_WHITE_LIST)	
		conf->sta_cnt = 0;
	else
		conf->dyn_cnt = 0;
       
	return 0;
}

typedef struct xPON_Whitelist_Temp_s
{
	int idx;
	int index;
	int type;
	int gem;
	int vid;
	int band;
	unsigned char  srcip[16];
	unsigned char  startip[16];
	unsigned char  endip[16];		
	unsigned short int pre_len;
	unsigned short int pre_rep_time;
	unsigned short int pre_rep_cnt;
	unsigned short int pre_rst_time;
}xPON_Whitelist_Temp_t;


static void xpon_igmp_copy_white_list_from_temp(xPON_WhiteList_Entry_t* entry, xPON_Whitelist_Temp_t * data)
{
    if(NULL== entry || NULL == data){
        return;
    }
    
	entry->index = data->index;
	entry->type = data->type;
	entry->gemid  = data->gem;
	entry->vlanid = data->vid;
	entry->bandwidth = data->band;
	memcpy(entry->srcip,data->srcip,16);
	memcpy(entry->grpstart,data->startip,16);
	memcpy(entry->grpend, data->endip,16);
	entry->preview_info.pre_len = data->pre_len;
	entry->preview_info.pre_rep_time = data->pre_rep_time;
	entry->preview_info.pre_rep_cnt = data->pre_rep_cnt;
	entry->preview_info.pre_rep_cnt_left= data->pre_rep_cnt;
	entry->preview_info.pre_rst_time = data->pre_rst_time;
}

static int xpon_igmp_copy_white_list_to_temp(xPON_Whitelist_Temp_t * data, xPON_WhiteList_Entry_t* entry)
{
    if(NULL== entry || NULL == data){
        return -1;
    }
    
	data->index = entry->index;
	data->type = entry->type; 
	data->gem = entry->gemid;
	data->vid = entry->vlanid;
	data->band = entry->bandwidth;
	memcpy(data->srcip,entry->srcip,16);
	memcpy(data->startip,entry->grpstart,16);
	memcpy(data->endip,entry->grpend,16);	
	data->pre_len = entry->preview_info.pre_len;
	data->pre_rep_time = entry->preview_info.pre_rep_time;
	data->pre_rep_cnt = entry->preview_info.pre_rep_cnt;
	data->pre_rst_time = entry->preview_info.pre_rst_time;
	return 0;
}


static int xpon_igmp_add_whitelist(int opt,int port,xPON_Whitelist_Temp_t * data)
{
	xPON_WhiteList_Entry_t* entry;
	xPON_PortConf_t* conf = NULL;
	struct list_head*  white_list;

	xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n xpon_igmp_add_whitelist():list = %d",opt);
	
    if(!data){
        return -1;
    }
	
	entry = xpon_whitelist_find(opt,port,data->index);
	if (entry){
		xpon_igmp_copy_white_list_from_temp(entry, data);
		return 0;
	}

    
	conf = xpon_port_conf_by_id(port);
	
	if (!conf){
		return -1;
	}
    
	entry = (xPON_WhiteList_Entry_t*) xpon_alloc(sizeof(xPON_WhiteList_Entry_t));
	if (entry==NULL){
		return -1;
	}
		
    xpon_igmp_copy_white_list_from_temp(entry, data);
	entry->flag = 1; 
	
	if (opt == STATIC_WHITE_LIST){
		white_list = &(conf->sta_list);
		conf->sta_cnt++;
	}else{
		white_list = &(conf->dyn_list);
		conf->dyn_cnt++;
	}

	list_add_tail(&entry->list,white_list);
	
	return 0;

}

static int xpon_igmp_del_whitelist(int opt,int port,int index)
{
	return  xpon_whitelist_del(opt,port,index);
}

static int xpon_igmp_clear_whitelist(int opt,int port)
{
	return xpon_whitelist_clear(opt,port);
}

static int xpon_igmp_get_whitelist_cnt(int opt,int port)
{
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	int cnt;

	if (!conf)
		return 0;
	
	if (opt == STATIC_WHITE_LIST)
		cnt =  conf->sta_cnt;
	else
		cnt = conf->dyn_cnt;	
	
	xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n xpon_igmp_get_whitelist_cnt():port=%d,opt=%d,cnt=%d",port,opt,cnt);
	
	return cnt;
}

static int xpon_igmp_get_whitelist(int opt,int port,xPON_Whitelist_Temp_t * data)
{

	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	xPON_WhiteList_Entry_t* entry = NULL;
	struct list_head* white_ist;
    int idx = 0;

	if (!conf || !data)
		return -1;

	if (opt == STATIC_WHITE_LIST)
		white_ist = &(conf->sta_list);
	else
		white_ist = &(conf->dyn_list);
	
    idx = data->idx;
    
	list_for_each_entry(entry,white_ist,list)
	{
		if (0 == idx)
			break;
		idx--;
	}
    return(xpon_igmp_copy_white_list_to_temp(data, entry));
}

/*-------------------------multicast VLan management api implementation--------------------------------*/

int xpon_port_vlan_find(int port,int vid)
{
	int i;
	xPON_PortVLan_t* pvlan = xpon_port_vlan_by_id(port);
	
	if (!pvlan)
		return -1;
	
	for(i=0;i<XPON_PORT_VLAN_CNT;i++)
	{
		if (pvlan->vlan_id[i] == vid)
			return i;
	}
	return -1;
}

int xpon_port_vlan_mask(int vid)
{
	int i = 0;
	xPON_IGMPConf_t* igmp = xpon_get_igmp_conf();
	
	for(i=0;i< igmp->uni_num;i++)
	{
		if (xpon_port_vlan_find(igmp->uni_port[i].port_id,vid) >= 0)
			return 1;
	}
	return 0;
}


int xpon_port_vlan_add(int port,int vid, int newvid)
{
	xPON_PortVLan_t* pvlan = xpon_port_vlan_by_id(port);
	int idx =  xpon_port_vlan_find(port,0);
	
	if (idx <0 || !pvlan)
		return -1;
	
	pvlan->vlan_id[idx] = vid;
	pvlan->vlan_trans[idx] = newvid;
	pvlan->vlan_num++;

	return 0; 
}


int xpon_port_vlan_del(int port,int vid)
{
	int idx = xpon_port_vlan_find(port,vid);
	xPON_PortVLan_t* pvlan = xpon_port_vlan_by_id(port);
	
	if (idx <0 || !pvlan) 
		return -1;

	pvlan->vlan_id[idx] = 0;
	pvlan->vlan_trans[idx] = 0;
	pvlan->vlan_num--;

	return 0;
}

 
int xpon_port_vlan_clear(int port)
{
	xPON_PortVLan_t* pvlan = xpon_port_vlan_by_id(port);
	int i,vid;	
	
	if (!pvlan)
		return -1;	
	
	for(i=0;i<XPON_PORT_VLAN_CNT;i++)
	{
		vid = pvlan->vlan_id[i];
		if (vid > 0)
		{
			xpon_port_vlan_del(port,vid);
		}
	}

	return 0;
}


static int xpon_igmp_add_portvlan(int port,int vid)
{
	int idx = xpon_port_vlan_find(port,vid);
	xPON_PortVLan_t* pvlan = xpon_port_vlan_by_id(port);
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);

	if (!pvlan  || !conf)
		return -1;
	
	if (idx <0)
	{
		if (xpon_port_vlan_add(port,vid,vid)<0)
			return -1;
	}
	else
	{
		pvlan->vlan_id[idx] = vid;
		pvlan->vlan_trans[idx] = vid;
	}

	return 0;
}

static int xpon_igmp_get_portvlan_cnt(int port)
{
	xPON_PortVLan_t* pvlan = xpon_port_vlan_by_id(port);
	
	if (!pvlan)
		return -1;
	
	return pvlan->vlan_num;
}

static int xpon_igmp_clear_portvlan(int port)
{
	xpon_port_vlan_clear(port);
	return 0;
}

static int xpon_igmp_set_portvlan_flag(int port,int flag)
{
	xPON_PortVLan_t* pvlan = xpon_port_vlan_by_id(port);
	
	if (pvlan == NULL)
		return -1;
	pvlan->vlan_flag = flag;

	return 0;
}

static int xpon_igmp_get_portvlan_flag(int port)
{
	xPON_PortVLan_t* pvlan = xpon_port_vlan_by_id(port);
	
	if (pvlan == NULL)
		return -1;

	return pvlan->vlan_flag;
}


static int xpon_igmp_set_switchvid(int port,int vid,int newvid)
{
	int idx = xpon_port_vlan_find(port,vid);
	xPON_PortVLan_t* pvlan = xpon_port_vlan_by_id(port);
	xPON_PortConf_t* conf = xpon_port_conf_by_id(port);
	
	if (!pvlan  || !conf)
		return -1;
	
	if (idx <0)
	{
		if (xpon_port_vlan_add(port,vid,newvid)<0)
			return -1;
	}
	else
	{
		pvlan->vlan_id[idx] = vid;
		pvlan->vlan_trans[idx] = newvid;
	}

	return 0;
}

//Func: Get VLan id and Switch VLan id from the given port and vlan entry index 
static int xpon_igmp_get_portvlan(int port,int idx,int* vid,int* newvid)
{
	xPON_PortVLan_t* pvlan = xpon_port_vlan_by_id(port);
	int i;
	
	if (pvlan == NULL)
		return -1;
	
	if (idx <0 || idx > pvlan->vlan_num || !vid || !newvid) 
		return -1;
	
	for(i=0;i<XPON_PORT_VLAN_CNT;i++)
	{
		if(pvlan->vlan_id[i] != 0)
		{
			if (idx==0)
				break;
			else
				idx--;
		}
	}
	
	if (i==XPON_PORT_VLAN_CNT)
		return -1;
	
	*vid = pvlan->vlan_id[i];
	*newvid = pvlan->vlan_trans[i];
	return 0;
}


/*------------------------------------------------------------------------------------*/
xPON_MVLanEntry_t* xpon_mvlan_find(int mvid)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	xPON_MVLanEntry_t* mvlan;
	int i;
		
	for(i=0;i<XPON_MVLAN_CNT;i++)
	{
		mvlan = &(igmp_conf->mul_vlan[i]);
		if (mvlan->mvlan_id == mvid)
			return mvlan;
	}
	return NULL;
}


int xpon_mvlan_add(int mvid)
{
	xPON_MVLanEntry_t* mvlan = xpon_mvlan_find(mvid);
	
	if(NULL == mvlan)
	{
		mvlan = xpon_mvlan_find(0);
		if (NULL == mvlan)
			return -1;
		mvlan->mvlan_id = mvid;
	}

	return 0;
}


int xpon_mvlan_del(int mvid)
{
	xPON_MVLanEntry_t* mvlan = xpon_mvlan_find(mvid);
	
	if (mvlan == NULL)
		return -1;
	
	mvlan->mvlan_id = 0;
	mvlan->mvlan_flag = 0;

	return 0;
}


int xpon_mvlan_clear(void)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int i;
		
	for(i=0;i<XPON_MVLAN_CNT;i++)
	{
		igmp_conf->mul_vlan[i].mvlan_id = 0;
		igmp_conf->mul_vlan[i].mvlan_flag = 0;
	}

	return 0;
}



static int xpon_igmp_add_mulvlan(int mvid)
{
	return  xpon_mvlan_add(mvid);
}


static int xpon_igmp_del_mulvlan(int mvid)
{
	return xpon_mvlan_del(mvid);
}

static int xpon_igmp_clear_mulvlan(void)
{
	return xpon_mvlan_clear();
}


static int xpon_igmp_get_mulvlan_cnt(void)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int i,cnt = 0;

	for(i=0;i<XPON_MVLAN_CNT;i++)
	{
		if (igmp_conf->mul_vlan[i].mvlan_id > 0)
			cnt++;
	}
	
	return cnt;
}

static int xpon_igmp_get_mulvlan_id(int idx)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int i;

	for(i=0;i<XPON_MVLAN_CNT;i++)
	{
		if (igmp_conf->mul_vlan[i].mvlan_id > 0)
		{
			if (idx >0)
				idx--;
			else
				break;
		}
	}
	if (i<XPON_MVLAN_CNT)
	{	
		return igmp_conf->mul_vlan[i].mvlan_id;
	}
	return -1;
}


static int xpon_igmp_set_mulvlan_flag(int mvid,int flag)
{
	xPON_MVLanEntry_t* mvlan = xpon_mvlan_find(mvid);
	
	if (mvlan == NULL)
		return -1;
	
	mvlan->mvlan_flag = flag;
	
	return 0;
}

static int xpon_igmp_get_mulvlan_flag(int mvid)
{
	xPON_MVLanEntry_t* mvlan = xpon_mvlan_find(mvid);

	
	if (mvlan == NULL)
		return -1;

	return mvlan->mvlan_flag;
}



/*---------------------IGMP forwarding table management api implementation---------------------------*/

int xpon_is_non_zero(unsigned char* addr,int len)
{
	int i;
	for(i=0;i<len;i++)
	{
		if (addr[i])
			return 1;
	}
	return 0;
}

struct list_head*  xpon_get_forward_list(void)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	return &(igmp_conf->fwd_tbl->fwd_list);
}

static int parse_and_store_vlan_action(int port, int vid, unsigned char* grp_addr,int isIpv6, ECNT_VLAN_INFO *vlanAction)
{
	int pon_vlan_mode = 0;
	unsigned short pon_vlan_tci = 0;
	e_vlan_operation_point_t vlan_point = 0;
	int iret=0;
	int down_vlan_mode=0;

	if(grp_addr == NULL || vlanAction == NULL)
	{
		MULTICAST_ERROR_INFO("port=%d, vid=%d, grp_add or vlanAction pointer is NULL\n",port,vid);
		return -1;
	}

    iret = xpon_get_down_vlan_operation_point(port, &vlan_point);
	MULTICAST_TRACE_INFO("vlan_point= %d (0 is in vlan, 1 is in xpon_igmp)\n",vlan_point);
    if(0 == iret && vlan_operation_in_multicast_module == vlan_point)
    {
    	//down vlan opration in xpon_igmp
		down_vlan_mode = xpon_igmp_get_down_tagctrl(port);
		pon_vlan_tci = xpon_igmp_get_down_tci(port);
		MULTICAST_TRACE_INFO("vlanOpt in xpon_igmp, port=%d,down_vlan_mode=%d,tci=%x\n",port,down_vlan_mode,pon_vlan_tci);
		switch(down_vlan_mode)
		{
			case 1:
				vlanAction->vlan_mode = ECNT_VLAN_STRIP;
				break;
			case 2:
				vlanAction->vlan_mode = ECNT_VLAN_ADD_AND_REMARK;
				memcpy(&vlanAction->outer_tci,&pon_vlan_tci,sizeof(pon_vlan_tci));
				break;
			case 3:
				vlanAction->vlan_mode = ECNT_VLAN_REPLACE_AND_REMARK;
				memcpy(&vlanAction->outer_tci,&pon_vlan_tci,sizeof(pon_vlan_tci));
				break;
			case 4:
				vlanAction->vlan_mode = ECNT_VLAN_REPLACE;
				pon_vlan_tci &= 0x0fff;
				memcpy(&vlanAction->outer_tci,&pon_vlan_tci,sizeof(pon_vlan_tci));
				break;
			case 5: //case 5,6,7 not support
			case 6:
			case 7:
			default: 
				vlanAction->vlan_mode = ECNT_VLAN_TRANSPARENT;
				break;
		}
	}
	else
    {
		//down vlan opration in ponvlan
		ECNT_API_GET_TCI_DOWN_OPT(port,vid,&pon_vlan_mode,&pon_vlan_tci);
		switch(pon_vlan_mode)
		{
			case 0:
				vlanAction->vlan_mode = ECNT_VLAN_TRANSPARENT;
				break;
			case 21:
				//dei not used,the bit is used to mark pbit transparent
				if(pon_vlan_tci & 0x1000)
					vlanAction->vlan_mode = ECNT_VLAN_ADD;
				else
					vlanAction->vlan_mode = ECNT_VLAN_ADD_AND_REMARK;
				memcpy(&vlanAction->outer_tci,&pon_vlan_tci,sizeof(pon_vlan_tci));
				break;
			case 31:
				vlanAction->vlan_mode = ECNT_VLAN_STRIP;
				break;
			case 40:
				//dei not used,the bit is used to mark pbit transparent
				if(pon_vlan_tci & 0x1000)
					vlanAction->vlan_mode = ECNT_VLAN_REPLACE;
				else
					vlanAction->vlan_mode = ECNT_VLAN_REPLACE_AND_REMARK;
				memcpy(&vlanAction->outer_tci,&pon_vlan_tci,sizeof(pon_vlan_tci));
				break;
			default: 
				vlanAction->vlan_mode = ECNT_VLAN_TRANSPARENT;
				break;
		}
    }

	MULTICAST_TRACE_INFO("port=%d,vid=%d,grp_addr=%u.%u.%u.%d,vlan_mode=%d,outer_tci=%x\n",port,vid,
		grp_addr[0],grp_addr[1],grp_addr[2],grp_addr[3],vlanAction->vlan_mode,vlanAction->outer_tci);
	return 0;
}
int mtk_multicast_fwdtbl_opration(int port, int vid, unsigned char* grp_addr,unsigned char* src_ip,int isIpv6, int op_type)
{
	ECNT_MC_PORT_INFO mcPortInfo={0};
	ECNT_MC_PORT_INFO_GET mcPortInfoGet = {0};
	ECNT_MC_PORT_INFO mcPortInfo_copy={0};
	xPON_PortEntry_t *portEntry = xpon_port_entry_by_id(port);
	ECNT_VLAN_INFO vlanAction={0};	
	xPON_IGMPConf_t* igmp_conf =xpon_get_igmp_conf();
	char grpMac[6]={0x01,0x00,0x5e,0x00,0x00,0x00};
	int i,ret = 0;

	if(grp_addr == NULL || portEntry == NULL)
		return -1;


	MULTICAST_TRACE_INFO(" enter, port=%d,optType=%d\n",port,op_type);	
	mcPortInfo.port_id = port -1; //mcast port id is from 0, xpon igm por id is from 1
	mcPortInfo.vlan_id = vid;
	mcPortInfo.group_addr.is_ipv6 = isIpv6;
	if(isIpv6){
		memcpy(&mcPortInfo.group_addr.IP.ipv4_address,grp_addr,16);
		grpMac[0]=0x33;
		grpMac[1]=0x33;
		grpMac[2]=grp_addr[12];
		grpMac[3]=grp_addr[13];
		grpMac[4]=grp_addr[14];
		grpMac[5]=grp_addr[15];
	}
	else{
		memcpy(&mcPortInfo.group_addr.IP.ipv4_address,grp_addr,4);
		grpMac[3]=grp_addr[1] & 0x7f;  //the 25th bit is equal to zero
		grpMac[4]=grp_addr[2];
		grpMac[5]=grp_addr[3];
	}
	memcpy(&mcPortInfo.group_mac,grpMac,6);
	
	if(port == XPONIGMP_PORT_VEIP_ID && op_type == MC_API_OP_TYPE_DELETE)
	{
		//for veip, add/update rule by mtk_multicast, buf for veip preview function, need trriger delete rule
		for(i=0;i<igmp_conf->uni_num;i++)
		{
			memcpy(&mcPortInfo_copy,&mcPortInfo,sizeof(mcPortInfo));
			portEntry = &igmp_conf->uni_port[i];
			if(portEntry->port_flag &  XPON_INVALID_PORT){			
				ret = ECNT_HOOK_MC_API_DEL_ENTRY(&mcPortInfo_copy);
				MULTICAST_TRACE_INFO("i=%d, hgu portId=%d, delete entry ret=%d\n",i,portEntry->port_id,ret);
			}
		}
	}
	else if(port != XPONIGMP_PORT_VEIP_ID){
		switch(op_type)
		{
			case MC_API_OP_TYPE_ADD:
				parse_and_store_vlan_action(port,vid,grp_addr,isIpv6,&vlanAction);
				memcpy(&portEntry->port_conf->vlan_action,&vlanAction,sizeof(vlanAction));
				memcpy(&mcPortInfo.vlan_action,&vlanAction,sizeof(vlanAction));
				ret = ECNT_HOOK_MC_API_ADD_ENTRY(&mcPortInfo);
				break;
			case MC_API_OP_TYPE_UPDATE:
				parse_and_store_vlan_action(port,vid,grp_addr,isIpv6,&vlanAction);
				if(memcmp(&portEntry->port_conf->vlan_action,&vlanAction,sizeof(vlanAction)))
				{
					MULTICAST_NOTICE_INFO("vlan action changed!\n");
					memcpy(&portEntry->port_conf->vlan_action,&vlanAction,sizeof(vlanAction));	
					memcpy(&mcPortInfo.vlan_action,&vlanAction,sizeof(vlanAction));
					ret = ECNT_HOOK_MC_API_UPDATE_ENTRY(&mcPortInfo);
				}
				break;
			case MC_API_OP_TYPE_DELETE:
				ret = ECNT_HOOK_MC_API_DEL_ENTRY(&mcPortInfo);	
				break;
			case MC_API_OP_TYPE_GET:
				ret =  ECNT_HOOK_MC_API_GET_ENTRY(&mcPortInfoGet);
				if(ret == ECNT_RETURN)
					ret = 1;
				else 
					ret = 0;
				break;
			default: 
				break;
		}
	}
	else{
		MULTICAST_DEBUG_INFO("veip port, skip add/update hw_nat in xpon_igmp\n");
		ret = 1;
	}
	
	MULTICAST_TRACE_INFO("port=%d, vid=%d, grp_add=%u.%u.%u.%u,op_type=%d, ret=%d\n",port,vid,grp_addr[0],
		grp_addr[1],grp_addr[2],grp_addr[3],op_type,ret);
	return ret;
}

int xpon_fwdtbl_operate_entry(int op,xPON_FwdEntry_t*  entry)
{

	xPON_IGMPConf_t* igmp = xpon_get_igmp_conf();	
	struct list_head*  fwd_list = xpon_get_forward_list(); 
	int isIpv6=0;

	MULTICAST_NOTICE_INFO("xpon_fwdtbl_operate_entry enter,op=%d\n",op);

	if (NULL == entry || (op != 1 && op !=2))
		return 0;

	if((entry->flag & 0xff) ==XPON_MASK_MLDV1 || (entry->flag & 0xff)==XPON_MASK_MLDV2)
		isIpv6 = 1;
	
	if (op == FWD_OPT_TYPE_ADD)
	{
	
	#if 0
		if ((entry->type==6 || entry->type==7) && xpon_pass_access_control(entry)==0)
			return 0;
    #endif
    
		list_add_tail_rcu(&entry->list,fwd_list);
		igmp->fwd_tbl->fwd_num++;
		MULTICAST_ERROR_INFO("===ADD  port=%d vid=%d grp_addr=%u.%u.%u.%u==\n",
			entry->port, entry->vid, entry->grp_addr[0],entry->grp_addr[1],entry->grp_addr[2],entry->grp_addr[3]);
		mtk_multicast_fwdtbl_opration(entry->port,entry->vid,entry->grp_addr,entry->src_addr,isIpv6,MC_API_OP_TYPE_ADD);
		MULTICAST_NOTICE_INFO("fwd_num=%d\n",igmp->fwd_tbl->fwd_num);
	}
	else if (op == FWD_OPT_TYPE_DEL){
		if (entry->preview_timer.function != NULL)
			del_timer(&entry->preview_timer);

		if (entry->leave_ageing_timer.function != NULL)
			del_timer(&entry->leave_ageing_timer);

		MULTICAST_ERROR_INFO("===DELETE  port=%d vid=%d grp_addr=%u.%u.%u.%u==\n",
			entry->port, entry->vid, entry->grp_addr[0],entry->grp_addr[1],entry->grp_addr[2],entry->grp_addr[3]);
		mtk_multicast_fwdtbl_opration(entry->port,entry->vid,entry->grp_addr,entry->src_addr,isIpv6,MC_API_OP_TYPE_DELETE);
        list_del_rcu(&entry->list);		
        call_rcu(&(entry->rcu), xpon_free_rcu);
        igmp->fwd_tbl->fwd_num--;
	}
	else if (op == FWD_OPT_TYPE_UPDATE){
		mtk_multicast_fwdtbl_opration(entry->port,entry->vid,entry->grp_addr,entry->src_addr,isIpv6,MC_API_OP_TYPE_UPDATE);
	}
	
	MULTICAST_NOTICE_INFO("xpon_fwdtbl_operate_entry end\n");

	return 1;
}

xPON_FwdEntry_t* xpon_fwdtbl_find_ext(int port, int vid,unsigned char* grp_mac,unsigned char* grp_addr,unsigned char* src_ip,int proto)
{
	struct list_head*	fwd_list = xpon_get_forward_list(); 
	xPON_FwdEntry_t* entry = NULL;
	
	if (list_empty(fwd_list))
		return NULL;

    rcu_read_lock();
	list_for_each_entry_rcu(entry,fwd_list,list)
	{
		if (entry->port != port || (entry->flag && (entry->flag & 0xff) != proto) )
			continue;

		switch (entry->type)
		{
#ifdef  TCSUPPORT_XPON_IGMP_CTC		
			case MULTCASTCTL_MAC_DA:
				if (!memcmp(entry->grp_addr,grp_mac,6))
					goto entry_out;
				break;
			case MULTCASTCTL_MAC_DA_VLAN:
				if (entry->vid==vid && !memcmp(entry->grp_addr,grp_mac,6))
					goto entry_out;
				break;
			case MULTCASTCTL_IPV4_SA_MAC_DA:
				if (!memcmp(entry->grp_addr,grp_mac,6) && !memcmp(entry->src_addr,src_ip,4))
					goto entry_out;
				break;
			case MULTCASTCTL_IPV4_DA_VLAN:
				if (entry->vid == vid && !memcmp(entry->grp_addr,grp_addr,4))
					goto entry_out;
				break;
			case MULTCASTCTL_IPV6_DA_VLAN:
				if (entry->vid == vid && !memcmp(entry->grp_addr,grp_addr,16))
					goto entry_out;
				break;				
			case MULTCASTCTL_IPV6_SA_MAC_DA:
				if (!memcmp(entry->grp_addr,grp_mac,6) && !memcmp(entry->src_addr,src_ip,16))
					goto entry_out;
				break;
#endif				
			case MULTCASTCTL_IPV4_DA:
				if (!memcmp(entry->grp_addr,grp_addr,4))
				{
					goto entry_out;		
				}	
				break;
			case MULTCASTCTL_IPV6_DA:
				if (!memcmp(entry->grp_addr,grp_addr,16))
				{
					goto entry_out;	
				}
				break;
			default:
				break;	
		}
	}

	entry = NULL;

entry_out:
    rcu_read_unlock();
    return entry;
}

void xpon_fwdtbl_del_by_port_vid(int port, int vid)
{
	struct list_head*	fwd_list = xpon_get_forward_list(); 
	xPON_IGMPConf_t* igmp = xpon_get_igmp_conf();
	xPON_FwdEntry_t* entry = NULL, *entry_tmp = NULL;

    if(fwd_list == NULL || igmp == NULL){
        return;
    }
	
	if (list_empty(fwd_list))
		return;
    
	list_for_each_entry_safe(entry, entry_tmp, fwd_list,list)
	{
		if (entry->port == port && entry->vid == vid )
		{
		    xpon_fwdtbl_operate_entry(FWD_OPT_TYPE_DEL, entry);
			break;
		}

	}
    
}

static int xpon_igmp_del_portvlan(int port,int vid)
{
	if (xpon_port_vlan_del(port,vid)<0)
		return -1;

	xpon_fwdtbl_del_by_port_vid(port, vid);
	
	return 0;
}


//Func: check if the forwarding entry has existed ,given the input 
xPON_FwdEntry_t* xpon_fwdtbl_find(int type, int port, int vid,unsigned char* grp_addr,unsigned char* src_ip,int proto)
{
	struct list_head*  fwd_list = xpon_get_forward_list(); 
	xPON_FwdEntry_t* entry = NULL;


	if (list_empty(fwd_list))
		return NULL;

    rcu_read_lock();
	list_for_each_entry_rcu(entry,fwd_list,list)
	{
		if (entry->port != port || type != entry->type || (entry->flag &0xff) != proto)
			continue;

		switch (entry->type)
		{
#ifdef  TCSUPPORT_XPON_IGMP_CTC	
			case MULTCASTCTL_MAC_DA:
				if (!memcmp(entry->grp_addr,grp_addr,6))
					goto entry_out;
				break;
			case MULTCASTCTL_MAC_DA_VLAN:
				if (entry->vid==vid && !memcmp(entry->grp_addr,grp_addr,6))
					goto entry_out;
				break;
			case MULTCASTCTL_IPV4_SA_MAC_DA:
				if (!memcmp(entry->grp_addr,grp_addr,6) && !memcmp(entry->src_addr,src_ip,4))
					goto entry_out;
				break;
			case MULTCASTCTL_IPV4_DA_VLAN:
				if (entry->vid==vid && !memcmp(entry->grp_addr,grp_addr,4))
					goto entry_out;
				break;
			case MULTCASTCTL_IPV6_DA_VLAN:
				if (entry->vid==vid && !memcmp(entry->grp_addr,grp_addr,16))
					goto entry_out;
				break;
			case MULTCASTCTL_IPV6_SA_MAC_DA:
				if (!memcmp(entry->grp_addr,grp_addr,6) && !memcmp(entry->src_addr,src_ip,16))
					goto entry_out;
				break;
#endif				
			case MULTCASTCTL_IPV4_DA:
				if (!memcmp(entry->grp_addr,grp_addr,4))
				{
					if (proto == XPON_MASK_IGMPV2)
						goto entry_out;
                	if(0 == xpon_is_non_zero(src_ip,4))
                    	goto entry_out;
					if (!memcmp(entry->src_addr,src_ip,4))
						goto entry_out;		
				}	
				break;
			case MULTCASTCTL_IPV6_DA:
				if (!memcmp(entry->grp_addr,grp_addr,16))
				{
					if (proto == XPON_MASK_MLDV1)
						goto entry_out;
					if (!memcmp(entry->src_addr,src_ip,16))
						goto entry_out;		
				}
				break;
			default:
				break;	
		}
	}
	
	entry = NULL;

entry_out:
    rcu_read_unlock();

    return entry;
}


xPON_FwdEntry_t* xpon_fwdtbl_add(int type, int port, int vid,unsigned char* grp_addr,unsigned char* src_addr,int flag,unsigned char* client_ip)
{
	xPON_FwdEntry_t* entry;

	entry = (xPON_FwdEntry_t*)xpon_alloc(sizeof(xPON_FwdEntry_t));

	if(entry == NULL)
		return NULL;

	if (grp_addr == NULL || client_ip == NULL || src_addr == NULL) {
		xpon_free(entry);
		return NULL;
	}
	
	entry->port = port;
	entry->type = type;
	entry->vid = vid;
	entry->flag = flag;
	MULTICAST_NOTICE_INFO("add fwdtbl port=%d type=%d, vid=%d, flag=%x,grp_addr=%u.%u.%u.%u\n", 
	entry->port, entry->type, entry->vid, entry->flag,grp_addr[0],grp_addr[1],grp_addr[2],grp_addr[3]);
	switch (type)
	{
#ifdef  TCSUPPORT_XPON_IGMP_CTC
		case MULTCASTCTL_MAC_DA:
		case MULTCASTCTL_MAC_DA_VLAN:
			memcpy(entry->grp_addr,grp_addr,6);
			memcpy(entry->client_ip,client_ip,6);
			break;
		case MULTCASTCTL_IPV4_SA_MAC_DA:
			memcpy(entry->grp_addr,grp_addr,6);
			memcpy(entry->client_ip,client_ip,6);
			memcpy(entry->src_addr,src_addr,4);
			break;	
		case MULTCASTCTL_IPV4_DA_VLAN:
			memcpy(entry->grp_addr,grp_addr,4);
			memcpy(entry->client_ip,client_ip,4);
			break;
		case MULTCASTCTL_IPV6_DA_VLAN:
			memcpy(entry->grp_addr,grp_addr,16);
			memcpy(entry->client_ip,client_ip,16);
			break;
		case MULTCASTCTL_IPV6_SA_MAC_DA:
			memcpy(entry->grp_addr,grp_addr,6);
			memcpy(entry->client_ip,client_ip,6);
			memcpy(entry->src_addr,src_addr,16);
			break;
#endif			
		case MULTCASTCTL_IPV4_DA:
			memcpy(entry->grp_addr,grp_addr,4);
			memcpy(entry->client_ip,client_ip,4);
			memcpy(entry->src_addr,src_addr,4);			
			break;
		case MULTCASTCTL_IPV6_DA:
			memcpy(entry->grp_addr,grp_addr,16);
			memcpy(entry->client_ip,client_ip,16);
			memcpy(entry->src_addr,src_addr,16);	
			break;
		default:
			xpon_free(entry);
			return NULL;
	}


	if(xpon_fwdtbl_operate_entry(FWD_OPT_TYPE_ADD,entry)==0)
    {
    	xpon_free(entry);
		return NULL;
    }

	entry->join_time = xpon_get_system_time_sec();
	MULTICAST_NOTICE_INFO("join time %lu\n",entry->join_time);

	return entry;
}


int xpon_fwdtbl_del(int type, int port, int vid,unsigned char* grp_addr,unsigned char* src_ip)
{

	xPON_FwdEntry_t* entry;

	entry = xpon_fwdtbl_find(type,port,vid,grp_addr,src_ip,0);
	if (entry == NULL)
		return 0;

	xpon_fwdtbl_operate_entry(FWD_OPT_TYPE_DEL,entry);
	return 0;	
}


int xpon_fwdtbl_clear(void)
{
	xPON_IGMPConf_t* igmp = xpon_get_igmp_conf();	
	struct list_head*  fwd_list = xpon_get_forward_list(); 
	xPON_FwdEntry_t* entry,*tmp;


	list_for_each_entry_safe(entry,tmp,fwd_list,list)
	{
		xpon_fwdtbl_operate_entry(FWD_OPT_TYPE_DEL,entry);
	}
	igmp->fwd_tbl->fwd_num = 0;

	ECNT_HOOK_MC_API_CLEAR_ENTRY_ALL();

	return 0;
}

int xpon_fwdtbl_static_clear(void)
{
	struct list_head*  fwd_list = xpon_get_forward_list(); 
	xPON_FwdEntry_t* entry,*tmp;


	list_for_each_entry_safe(entry,tmp,fwd_list,list)
	{
		if(entry->ruleType == FWD_RULE_TYPE_STATIC)
			xpon_fwdtbl_operate_entry(FWD_OPT_TYPE_DEL,entry);
	}

	return 0;
}

int xpon_fwdtbl_update(xPON_FwdEntry_t* entry)
{
	if(entry == NULL){	
		MULTICAST_CRITIC_INFO(" entry is NULL\n");
		return -1;
	}

	MULTICAST_CRITIC_INFO("Enter,portId=%d,grp_id=%d.%d.%d.%d\n",entry->port,entry->grp_addr[0],entry->grp_addr[1],
		entry->grp_addr[2],entry->grp_addr[3]);
	
	return xpon_fwdtbl_operate_entry(FWD_OPT_TYPE_UPDATE,entry);
}

#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_IGMP_SET_GROUP)
int xpon_fwdtbl_cnt()
{
	struct list_head*  fwd_list = xpon_get_forward_list(); 
	xPON_FwdEntry_t* entry = NULL;
	char group_address[18] = {0};
	int groupCnt = 0;
	if (list_empty(fwd_list))
		return NULL;

	rcu_read_lock();
	list_for_each_entry_rcu(entry,fwd_list,list)
	{
		sprintf(group_address, "%d.%d.%d.%d", entry->grp_addr[0], entry->grp_addr[1], entry->grp_addr[2], entry->grp_addr[3]);
		if(memcmp(group_address, "239.255.255.250",strlen("239.255.255.250")) && memcmp(group_address,"224.0.0.251",strlen("224.0.0.251")))
			groupCnt++;	
	}
	rcu_read_unlock;
	return groupCnt;
}
#endif/*TCSUPPORT_COMPILE*/

static int xpon_igmp_add_fwdentry(int type, int port, int vid,unsigned char* grp_addr,unsigned char* src_ip,unsigned char* client_ip)
{
	xPON_FwdEntry_t* entry;

	xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n xpon_igmp_add_fwdentry(): %d %d %d",type,port,vid);

	entry = xpon_fwdtbl_find(type,port,vid,grp_addr,src_ip,0);
	if(entry != NULL)
	{
		entry->type = type;
		entry->port = port;
		entry->vid = vid;
		memcpy(entry->grp_addr,grp_addr,16);
		memcpy(entry->src_addr,src_ip,16);
		return 0;
	}

    entry = xpon_fwdtbl_add(type,port,vid,grp_addr,src_ip,0,client_ip);
	entry->ruleType = FWD_RULE_TYPE_STATIC;
	if (entry == NULL)
		return -1;
	
	return 0;
}

static int xpon_igmp_del_fwdentry(int type,int port,int vid,unsigned char* grp_addr,unsigned char* src_ip)
{
	xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n xpon_igmp_del_fwdentry(): %d %d %d",type,port,vid);

	return xpon_fwdtbl_del(type,port,vid,grp_addr,src_ip);
}

static int xpon_igmp_clear_fwdtbl(int type)
{
	if(type == 1){
		printk("xpon igmp clear all static fwd entry\n");
		return xpon_fwdtbl_static_clear();
	}

	printk("xpon igmp clear all fwd entry\n");
	return xpon_fwdtbl_clear();		
}

static int xpon_igmp_get_fwdentry_cnt(void)
{
	xPON_IGMPConf_t* igmp = xpon_get_igmp_conf();
	
	return igmp->fwd_tbl->fwd_num;
}

static int xpon_igmp_get_fwdentry(int idx,int *type,int* port,int* vid,int* ruleType,unsigned char* grp_addr,unsigned char* src_ip,int* flag,unsigned char* client_ip,unsigned long* join_time)
{
	xPON_IGMPConf_t* igmp = xpon_get_igmp_conf();
	struct list_head*  fwd_list = xpon_get_forward_list();
	xPON_FwdEntry_t* entry;

	if (idx <0 || idx >= igmp->fwd_tbl->fwd_num)
		return -1;

	if(list_empty(fwd_list))
		return -1;
	
	rcu_read_lock();
	list_for_each_entry_rcu(entry,fwd_list,list)
	{
		if (idx == 0)
			break;
		idx--;
	}
	rcu_read_unlock();

	*type = entry->type;
	*port = entry->port;
	*vid = entry->vid;
	*ruleType = entry->ruleType;
	*flag = entry->flag;
	memcpy(client_ip,entry->client_ip,16);
	*join_time = entry->join_time;
	
	if(entry->type==MULTCASTCTL_IPV4_DA)
	{	
		memcpy(grp_addr,entry->grp_addr,4);
		memcpy(src_ip,entry->src_addr,4);	
	}
	else if (entry->type==MULTCASTCTL_IPV6_DA)
	{
		memcpy(grp_addr,entry->grp_addr,16);
		memcpy(src_ip,entry->src_addr,16);
	}
#ifdef  TCSUPPORT_XPON_IGMP_CTC	
	else if (entry->type==MULTCASTCTL_MAC_DA || entry->type==MULTCASTCTL_MAC_DA_VLAN)
	{
		memcpy(grp_addr,entry->grp_addr,6);	
	}
	else if (entry->type==MULTCASTCTL_IPV4_SA_MAC_DA)
	{
		memcpy(grp_addr,entry->grp_addr,6);
		memcpy(src_ip,entry->src_addr,4);
	}
	else if (entry->type==MULTCASTCTL_IPV4_DA_VLAN)
	{
		memcpy(grp_addr,entry->grp_addr,4);
	}
	else if (entry->type==MULTCASTCTL_IPV6_DA_VLAN)
	{
		memcpy(grp_addr,entry->grp_addr,16);
	}
	else if (entry->type==MULTCASTCTL_IPV6_SA_MAC_DA)
	{
		memcpy(grp_addr,entry->grp_addr,6);
		memcpy(src_ip,entry->src_addr,16);		
	}
#endif	
	else
	{
		return -1;
	}
	
	return 0;	
}

int xpon_igmp_get_fwdmode(void)
{
	xPON_IGMPConf_t* igmp = xpon_get_igmp_conf();
	
	return igmp->fwd_tbl->fwd_mode;
}

static int xpon_igmp_set_fwdmode(int mode)
{
	xPON_IGMPConf_t* igmp = xpon_get_igmp_conf();
	
	
	if (igmp->fwd_tbl->fwd_mode != mode)
	{
		xpon_fwdtbl_clear();
		xpon_port_conf_clear();
		xpon_port_status_clear();
	}
	igmp->fwd_tbl->fwd_mode = mode;
	return 0;
}

/*----------------------------------------------------------------------------*/
#define XPON_IGMP_GET_VER					1001
#define XPON_IGMP_SET_VER					1002
#define XPON_IGMP_GET_FUNC				1003
#define XPON_IGMP_SET_FUNC				1004
#define XPON_IGMP_GET_FASTLEAVE			1005
#define XPON_IGMP_SET_FASTLEAVE			1006
#define XPON_IGMP_GET_UPTCI				1007
#define XPON_IGMP_SET_UPTCI				1008
#define XPON_IGMP_GET_UPTAGCTL			1009
#define XPON_IGMP_SET_UPTAGCTL			1010
#define XPON_IGMP_GET_DOWNTCI			1011
#define XPON_IGMP_SET_DOWNTCI			1012
#define XPON_IGMP_GET_DOWNTAGCTL		1013
#define XPON_IGMP_SET_DOWNTAGCTL		1014
#define XPON_IGMP_GET_MAXRATE			1015
#define XPON_IGMP_SET_MAXRATE			1016
#define XPON_IGMP_GET_ROBUST				1017
#define XPON_IGMP_SET_ROBUST				1018
#define XPON_IGMP_GET_UNAUTHOR			1019
#define XPON_IGMP_SET_UNAUTHOR			1020
#define XPON_IGMP_GET_QUERYIP			1021
#define XPON_IGMP_SET_QUERYIP			1022
#define XPON_IGMP_GET_QUERYINTERVAL	1023
#define XPON_IGMP_SET_QUERYINTERVAL	1024
#define XPON_IGMP_GET_LASTINTERVAL	1025
#define XPON_IGMP_SET_LASTINTERVAL	1026
#define XPON_IGMP_GET_QUERYMAXRESP	1027
#define XPON_IGMP_SET_QUERYMAXRESP	1028
#define XPON_IGMP_GET_MAXPLAYGROUP	1029
#define XPON_IGMP_SET_MAXPLAYGROUP	1030
#define XPON_IGMP_GET_TAGSTRIP			1031
#define XPON_IGMP_SET_TAGSTRIP			1032
#define XPON_IGMP_GET_PORTFLAG			1033
#define XPON_IGMP_SET_PORTFLAG			1034

#define XPON_IGMP_GET_MAXBANDWIDTH	    1035
#define XPON_IGMP_SET_MAXBANDWIDTH	    1036
#define XPON_IGMP_GET_BWENFORCEMENT	    1037
#define XPON_IGMP_SET_BWENFORCEMENT	    1038
#define XPON_IGMP_GET_COUNTER_CURRENT_MCAST_BW  1039
#define XPON_IGMP_GET_COUNTER_JOIN_MSG	        1040
#define XPON_IGMP_GET_COUNTER_BW_EXCEEDED       1041
#define XPON_IGMP_GET_USER_SUBSCRIBE_CNT        1042
#define XPON_IGMP_GET_USER_SUBSCRIBE_BY_INDEX   1043

#define XPON_IGMP_GET_VEIP_ACL           1044
#define XPON_IGMP_SET_VEIP_ACL           1045
#define XPON_IGMP_GET_EMPTRY_DYNLIST_PASS   1046
#define XPON_IGMP_SET_EMPTRY_DYNLIST_PASS   1047

#define XPON_IGMP_ADD_DYNWHITELIST			2001
#define XPON_IGMP_DEL_DYNWHITELIST			2002
#define XPON_IGMP_CLEAR_DYNWHITELIST			2003
#define XPON_IGMP_GET_DYNWHITELISTCNT		2004
#define XPON_IGMP_GET_DYNWHITELIST			2005
#define XPON_IGMP_ADD_STAWHITELIST			2006
#define XPON_IGMP_DEL_STAWHITELIST			2007
#define XPON_IGMP_CLEAR_STAWHITELIST			2008
#define XPON_IGMP_GET_STAWHITELISTCNT		2009
#define XPON_IGMP_GET_STAWHITELIST			2010


#define XPON_IGMP_ADD_PORTVLAN				3001
#define XPON_IGMP_DEL_PORTVLAN				3002
#define XPON_IGMP_CLEAR_PORTVLAN			3003
#define XPON_IGMP_GET_PORTVLANCNT			3004
#define XPON_IGMP_GET_PORTVLANID			3005
#define XPON_IGMP_SET_VLANSWITCHVID		3006
#define XPON_IGMP_SET_PORTVLANFLAG		3007
#define XPON_IGMP_GET_PORTVLANFLAG		3008


#define XPON_IGMP_ADD_FWDENTRY		4001
#define XPON_IGMP_DEL_FWDENTRY		4002
#define XPON_IGMP_GET_FWDENTRYCNT	4003
#define XPON_IGMP_GET_FWDENTRY		4004
#define XPON_IGMP_CLEAR_FWDENTRY	4005
#define XPON_IGMP_SET_FWDMODE		4006
#define XPON_IGMP_GET_FWDMODE		4007

#define XPON_IGMP_ADD_MULVLAN		5001
#define XPON_IGMP_DEL_MULVLAN		5002
#define XPON_IGMP_CLEAR_MULVLAN		5003
#define XPON_IGMP_GET_MULVLANCNT	5004
#define XPON_IGMP_GET_MULVLANID		5005
#define XPON_IGMP_SET_MULVLANFLAG	5006
#define XPON_IGMP_GET_MULVLANFLAG	5007

#define XPON_IGMP_GET_IGMPFLAG		6001
#define XPON_IGMP_SET_IGMPFLAG		6002
#define XPON_IGMP_GET_XPONMODE		6003
#define XPON_IGMP_SET_XPONMODE		6004
#define XPON_IGMP_GET_ONUTYPE		6005
#define XPON_IGMP_SET_DEBUG			6006

#define XPON_IGMP_CLEAR_HW_ENTRY       6007
#define XPON_IGMP_CLEAR_HW_DROP_ENTRY  6008
#define XPON_IGMP_SET_DS_BW_CTRL       6009
#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_IGMP_SET_GROUP)
#define XPON_IGMP_GET_GROUPNUM		6010
#define XPON_IGMP_SET_GROUPNUM		6011
#endif/*TCSUPPORT_COMPILE*/

#define XPON_IGMP_SET_CARE_DY_ST_VER   6012

static int xpon_igmp_get_mode(void)
{
	return igmp_conf.xpon_mode;
}

static int xpon_igmp_get_type(void)
{
	return igmp_conf.onu_type;
}


static int xpon_igmp_get_ioctl(int cmd,int argv1,long argv2)
{
	int port = argv1;
	int val = 0;
	switch (cmd)
	{
		case XPON_IGMP_GET_VER:
			val = xpon_igmp_get_ver(port);
			break;
		case XPON_IGMP_GET_FUNC:
			val = xpon_igmp_get_func(port);
			break;
		case XPON_IGMP_GET_FASTLEAVE:
			val = xpon_igmp_get_fastleave(port);
			break;
		case XPON_IGMP_GET_UPTCI:
			val = xpon_igmp_get_up_tci(port);
			break;	
		case XPON_IGMP_GET_UPTAGCTL:
			val = xpon_igmp_get_up_tagctrl(port);
			break;
		case XPON_IGMP_GET_DOWNTCI:
			val = xpon_igmp_get_down_tci(port);
			break;
		case XPON_IGMP_GET_DOWNTAGCTL:
			val = xpon_igmp_get_down_tagctrl(port);
			break;		
		case XPON_IGMP_GET_TAGSTRIP:
			val = xpon_igmp_get_tagstrip(port);
			break;
		case XPON_IGMP_GET_MAXRATE:
			val = xpon_igmp_get_maxrate(port);
			break;
		case XPON_IGMP_GET_ROBUST:
			val = xpon_igmp_get_robust(port);
			break;
		case XPON_IGMP_GET_UNAUTHOR:
			val = xpon_igmp_get_unauthor(port);
			break;
		case XPON_IGMP_GET_QUERYINTERVAL:
			val = xpon_igmp_get_query_interval(port);
			break;
		case XPON_IGMP_GET_LASTINTERVAL:
			val = xpon_igmp_get_last_interval(port);
			break;
		case XPON_IGMP_GET_QUERYMAXRESP:
			val = xpon_igmp_get_query_maxresp(port);
			break;
		case XPON_IGMP_GET_MAXPLAYGROUP:
			val = xpon_igmp_get_max_playgroup(port);
			break;
		case XPON_IGMP_GET_MAXBANDWIDTH:
			val = xpon_igmp_get_max_bandwidth(port);
			break;	
		case XPON_IGMP_GET_BWENFORCEMENT:
			val = xpon_igmp_get_bandwidth_enforcement(port);
			break;	        
			
		case XPON_IGMP_GET_COUNTER_CURRENT_MCAST_BW:
			val = xpon_igmp_get_pmc_curr_mcast_bw(port);
			break;	
		case XPON_IGMP_GET_COUNTER_JOIN_MSG:
			val = xpon_igmp_get_pmc_join_msg(port);
			break;	
		case XPON_IGMP_GET_COUNTER_BW_EXCEEDED:
			val = xpon_igmp_get_pmc_bw_exceeded(port);
			break;	
		case XPON_IGMP_GET_PORTFLAG:
			val = xpon_igmp_get_port_flag(port);
			break;
		case XPON_IGMP_GET_IGMPFLAG:
			val = xpon_igmp_get_igmp_flag();
			break;
		case XPON_IGMP_GET_XPONMODE:
			val = xpon_igmp_get_mode();
			break;
		case XPON_IGMP_GET_ONUTYPE:
			val = xpon_igmp_get_type();
			break;
#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_IGMP_SET_GROUP)
		case XPON_IGMP_GET_GROUPNUM:
			val = xpon_igmp_get_groupNum();
			break;
#endif/*TCSUPPORT_COMPILE*/
		case XPON_IGMP_GET_VEIP_ACL:
			val = xpon_igmp_get_veip_acl();
			break;
		case XPON_IGMP_GET_EMPTRY_DYNLIST_PASS:
			val = xpon_igmp_get_empty_dynlist_pass();
			break;
		default:
			return -1;
	}

	COPY_TO_USER((void* __user)argv2,&val,sizeof(int));
	return 0;
}

int xpon_igmp_set_mode(int mode);

static int xpon_igmp_set_ioctl(unsigned long cmd,unsigned long argv1,unsigned long argv2)
{
	int port = argv1;
	unsigned long val = argv2;
	int ret = -1; 
	
	xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n xpon_igmp_set_ioctl:port = %d,val = %lu",port,val);
	
	switch (cmd)
	{
		case XPON_IGMP_SET_VER:
			ret = xpon_igmp_set_ver(port,val);
			break;
		case XPON_IGMP_SET_FUNC:
			ret = xpon_igmp_set_func(port,val);
			break;
		case XPON_IGMP_SET_FASTLEAVE:
			ret = xpon_igmp_set_fastleave(port,val);
			break;
		case XPON_IGMP_SET_UPTCI:
			ret = xpon_igmp_set_up_tci(port,val);
			break;
		case XPON_IGMP_SET_UPTAGCTL:
			ret = xpon_igmp_set_up_tagctrl(port,val);
			break;
		case XPON_IGMP_SET_DOWNTCI:
			ret = xpon_igmp_set_down_tci(port,val);
			break;
		case XPON_IGMP_SET_TAGSTRIP:
			ret = xpon_igmp_set_tagstrip(port,val);
			break;		
		case XPON_IGMP_SET_DOWNTAGCTL:
			ret = xpon_igmp_set_down_tagctrl(port,val);
			break;
		case XPON_IGMP_SET_MAXRATE:
			ret = xpon_igmp_set_maxrate(port,val);
			break;
		case XPON_IGMP_SET_ROBUST:
			ret = xpon_igmp_set_robust(port,val);
			break;
		case XPON_IGMP_SET_UNAUTHOR:
			ret = xpon_igmp_set_unauthor(port,val);
			break;
		case XPON_IGMP_SET_QUERYIP:
			ret = xpon_igmp_set_query_ip(port,val);
			break;
		case XPON_IGMP_SET_QUERYINTERVAL:
			ret = xpon_igmp_set_query_interval(port,val);
			break;
		case XPON_IGMP_SET_LASTINTERVAL:
			ret = xpon_igmp_set_last_interval(port,val);
			break;
		case XPON_IGMP_SET_QUERYMAXRESP:
			ret = xpon_igmp_set_query_maxresp(port,val);
			break;
		case XPON_IGMP_SET_MAXPLAYGROUP:
			ret = xpon_igmp_set_max_playgroup(port,val);
			break;
		case XPON_IGMP_SET_MAXBANDWIDTH:
			ret = xpon_igmp_set_max_bandwidth(port,val);
			break;			
		case XPON_IGMP_SET_BWENFORCEMENT:
			ret = xpon_igmp_set_bandwidth_enforcement(port,val);
			break;					
		case XPON_IGMP_SET_DS_BW_CTRL:
			ret = xpon_igmp_set_dsbwctrl(val);
			break;	    
		case XPON_IGMP_SET_PORTFLAG:	
			ret =xpon_igmp_set_port_flag(port,val);
			break;
		case XPON_IGMP_SET_IGMPFLAG:
			ret = xpon_igmp_set_igmp_flag(val);
			break;
		case XPON_IGMP_GET_QUERYIP:
			ret = xpon_igmp_get_query_ip(port,val);
			break;
		case XPON_IGMP_SET_XPONMODE:
			ret = xpon_igmp_set_mode(val);
			break;
		case  XPON_IGMP_SET_DEBUG:
			ret = xpon_igmp_set_debug(val);
			break;
        case  XPON_IGMP_CLEAR_HW_ENTRY:
            ret = xpon_igmp_clear_multicast_hw_entry(val);
        break;
        case  XPON_IGMP_CLEAR_HW_DROP_ENTRY:
            ret = xpon_igmp_clear_multicast_hw_drop_entry(val);
        break;
#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_IGMP_SET_GROUP)
 		case  XPON_IGMP_SET_GROUPNUM:
			ret = xpon_igmp_set_groupNum(val);
        	break;
#endif/*TCSUPPORT_COMPILE*/
        case  XPON_IGMP_SET_CARE_DY_ST_VER:
            ret = xpon_igmp_set_care_ver_dy_stalist(val);
        	break;
		case XPON_IGMP_SET_VEIP_ACL:
			ret = xpon_igmp_set_veip_acl(val);
			break;
		case XPON_IGMP_SET_EMPTRY_DYNLIST_PASS:
			ret = xpon_igmp_set_empty_dynlist_pass(val);
			break;
		default:
			return -1;
	}
	

	return ret;
}

static int xpon_igmp_whitelist_ioctl(unsigned long cmd,unsigned long argv1,unsigned long argv2)
{
    xPON_Whitelist_Temp_t temp={0};
	int ret = 0;
    int opt = DYNAMIC_WHITE_LIST;
	
	switch (cmd)
	{
		case XPON_IGMP_ADD_DYNWHITELIST:
		case XPON_IGMP_ADD_STAWHITELIST:
			if (cmd==XPON_IGMP_ADD_STAWHITELIST)
                opt = STATIC_WHITE_LIST;
			COPY_FROM_USER(&temp,(void* __user)argv2,sizeof(xPON_Whitelist_Temp_t));
            ret = xpon_igmp_add_whitelist(opt, argv1, &temp);
			break;
            
		case XPON_IGMP_DEL_DYNWHITELIST:
		case XPON_IGMP_DEL_STAWHITELIST:	
			if (cmd==XPON_IGMP_DEL_STAWHITELIST)
                opt = STATIC_WHITE_LIST;
			ret = xpon_igmp_del_whitelist(opt,argv1,argv2);
			break;
            
		case XPON_IGMP_CLEAR_DYNWHITELIST:
		case XPON_IGMP_CLEAR_STAWHITELIST:	
			if (cmd==XPON_IGMP_CLEAR_STAWHITELIST)
                opt = STATIC_WHITE_LIST;            
			ret = xpon_igmp_clear_whitelist(opt,argv1);
			break;	
            
		case XPON_IGMP_GET_DYNWHITELISTCNT:
		case XPON_IGMP_GET_STAWHITELISTCNT:	
			if (cmd==XPON_IGMP_GET_STAWHITELISTCNT)
                opt = STATIC_WHITE_LIST;                    
			ret = xpon_igmp_get_whitelist_cnt(opt,argv1);
			if (ret >= 0)
			{
				COPY_TO_USER((void* __user)argv2,&ret,sizeof(int));
				ret = 0;
			}
			break;
            
		case XPON_IGMP_GET_DYNWHITELIST:
		case XPON_IGMP_GET_STAWHITELIST:
			if (cmd==XPON_IGMP_GET_STAWHITELIST)
                opt = STATIC_WHITE_LIST;            
			COPY_FROM_USER(&temp,(void* __user)argv2,sizeof(xPON_Whitelist_Temp_t));
            ret = xpon_igmp_get_whitelist(opt, argv1, &temp);
			COPY_TO_USER((void* __user)argv2,&temp,sizeof(xPON_Whitelist_Temp_t));
			break;
            
		default:
			return -1;
	}
	return ret;
}

static int xpon_igmp_user_subscribe_ioctl(unsigned long cmd,unsigned long argv1,unsigned long argv2)
{
	xPON_User_Subscribe_Temp_t temp;
	int ret = 0;
		
	memset(&temp, 0, sizeof(xPON_User_Subscribe_Temp_t));
	switch (cmd)
	{
		case XPON_IGMP_GET_USER_SUBSCRIBE_CNT:
			ret = xpon_igmp_get_user_subscribe_cnt(argv1);
			if (ret >= 0)
			{
				COPY_TO_USER((void* __user)argv2,&ret,sizeof(int));
				ret = 0;
			}
			break;
		case XPON_IGMP_GET_USER_SUBSCRIBE_BY_INDEX:
			COPY_FROM_USER(&temp,(void* __user)argv2,sizeof(xPON_User_Subscribe_Temp_t));
			xpon_igmp_get_user_subscribe_by_index(argv1,temp.index, temp.srcip, temp.program_ip);
			COPY_TO_USER((void* __user)argv2,&temp,sizeof(xPON_User_Subscribe_Temp_t));
			break;
		default:
			return -1;
	}
	return ret;
}

typedef struct xPON_MVLanSwitch_Temp_s
{
	int idx;
	int vid;
	int newvid;
} xPON_MVLanSwitch_Temp_t;

static int xpon_igmp_portvlan_ioctl(unsigned long cmd,unsigned long argv1,unsigned long argv2)
{
	int ret = 0;
	xPON_MVLanSwitch_Temp_t temp;
	switch(cmd)
	{
		case XPON_IGMP_ADD_PORTVLAN:
			ret = xpon_igmp_add_portvlan(argv1,argv2);
			break;
		case XPON_IGMP_DEL_PORTVLAN:
			ret = xpon_igmp_del_portvlan(argv1,argv2);
			break;
		case XPON_IGMP_CLEAR_PORTVLAN:
			ret = xpon_igmp_clear_portvlan(argv1);
			break;
		case XPON_IGMP_GET_PORTVLANCNT:
			ret = xpon_igmp_get_portvlan_cnt(argv1);
			if (ret >= 0)
			{
				COPY_TO_USER((void* __user)argv2,&ret,sizeof(int));
				ret = 0;
			}
			break;
		case XPON_IGMP_GET_PORTVLANID:
			COPY_FROM_USER(&temp,(xPON_MVLanSwitch_Temp_t*)argv2,sizeof(xPON_MVLanSwitch_Temp_t));
			ret = xpon_igmp_get_portvlan(argv1,temp.idx,&temp.vid,&temp.newvid);
			if (ret >= 0)
			{
				COPY_TO_USER((void* __user)argv2,&temp,sizeof(xPON_MVLanSwitch_Temp_t));
				ret = 0;
			}
			break;
		case XPON_IGMP_SET_VLANSWITCHVID:
			COPY_FROM_USER(&temp,(xPON_MVLanSwitch_Temp_t*)argv2,sizeof(xPON_MVLanSwitch_Temp_t));
			ret = xpon_igmp_set_switchvid(argv1,temp.vid,temp.newvid);
			break;
		case XPON_IGMP_SET_PORTVLANFLAG:
			ret = xpon_igmp_set_portvlan_flag(argv1,argv2);
			break;
		case XPON_IGMP_GET_PORTVLANFLAG:
			ret = xpon_igmp_get_portvlan_flag(argv1);
			if (ret >=0)
			{
				COPY_TO_USER((void* __user)argv2,&ret,sizeof(int));
				ret = 0;	
			}		
			break;
		default:
			return -1;
	}
	return ret;
}

static int xpon_igmp_fwdtbl_ioctl(unsigned long cmd,unsigned long argv1,unsigned long argv2)
{
	int ret = 0;
	xPON_Forward_Entry_t temp;

	memset(&temp,0,sizeof(temp));
	switch(cmd)
	{
		case XPON_IGMP_ADD_FWDENTRY:
			COPY_FROM_USER(&temp,(xPON_Forward_Entry_t*)argv2,sizeof(xPON_Forward_Entry_t));
			ret = xpon_igmp_add_fwdentry(temp.type,temp.port,temp.vid,temp.grp_addr,temp.src_ip,temp.client_ip);
			break;
		case XPON_IGMP_DEL_FWDENTRY:
			COPY_FROM_USER(&temp,(xPON_Forward_Entry_t*)argv2,sizeof(xPON_Forward_Entry_t));
			ret = xpon_igmp_del_fwdentry(temp.type,temp.port,temp.vid,temp.grp_addr,temp.src_ip);
			break;
		case XPON_IGMP_GET_FWDENTRYCNT:
			ret = xpon_igmp_get_fwdentry_cnt();
			if (ret >=0)
			{
				COPY_TO_USER((void* __user)argv2,&ret,sizeof(int));
				ret = 0;
			}
			break;
		case XPON_IGMP_GET_FWDENTRY:
			ret = xpon_igmp_get_fwdentry(argv1,&temp.type,&temp.port,&temp.vid,&temp.ruleType,temp.grp_addr,temp.src_ip,&temp.flag,temp.client_ip,&temp.join_time);
			if (ret >= 0)
			{
				COPY_TO_USER((void* __user)argv2,&temp,sizeof(xPON_Forward_Entry_t));
			}
			break;
		case XPON_IGMP_CLEAR_FWDENTRY:
			ret = xpon_igmp_clear_fwdtbl((int)argv2);
			break;
		case XPON_IGMP_SET_FWDMODE:
			ret = xpon_igmp_set_fwdmode(argv2);
			break;
		case XPON_IGMP_GET_FWDMODE:
			ret = xpon_igmp_get_fwdmode();
			if (ret >=0)
			{
				COPY_TO_USER((void* __user)argv2,&ret,sizeof(int));
				ret = 0;
			}
			break;
		default:
			return -1;
	}

	return ret;
}


static int xpon_igmp_mulvlan_ioctl(unsigned long cmd,unsigned long argv1,unsigned long argv2)
{
	int ret = 0;
	switch(cmd)
	{
		case XPON_IGMP_ADD_MULVLAN:
			ret = xpon_igmp_add_mulvlan(argv1);
			break;
		case XPON_IGMP_DEL_MULVLAN:
			ret = xpon_igmp_del_mulvlan(argv1);
			break;
		case XPON_IGMP_CLEAR_MULVLAN:
			ret = xpon_igmp_clear_mulvlan();
			break;
		case XPON_IGMP_GET_MULVLANCNT:
			ret = xpon_igmp_get_mulvlan_cnt();
			if (ret >= 0)
			{
				COPY_TO_USER((void* __user)argv2,&ret,sizeof(int));
				ret = 0;
			}
			break;
		case XPON_IGMP_GET_MULVLANID:
			ret = xpon_igmp_get_mulvlan_id(argv1);
			if (ret >= 0)
			{
				COPY_TO_USER((void* __user)argv2,&ret,sizeof(int));
				ret = 0;
			}
			break;
		case XPON_IGMP_SET_MULVLANFLAG:
			ret = xpon_igmp_set_mulvlan_flag(argv1,argv2);
			break;
		case XPON_IGMP_GET_MULVLANFLAG:
			ret = xpon_igmp_get_mulvlan_flag(argv1);
			if (ret >=0)
			{
				COPY_TO_USER((void* __user)argv2,&ret,sizeof(int));
				ret = 0;	
			}
			break;
		default:
			return -1;
	}
	return ret;

}



static int xpon_igmp_ioctl(unsigned long cmd,unsigned long argv1,unsigned long argv2)
{

	xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n ========>xpon_igmp_ioctl():cmd=%d,argv1=0x%lx,argv2=0x%lx",cmd,argv1,argv2);
	
	switch (cmd)
	{
		case XPON_IGMP_GET_VER:
		case XPON_IGMP_GET_FUNC:
		case XPON_IGMP_GET_FASTLEAVE:
		case XPON_IGMP_GET_UPTCI:	
		case XPON_IGMP_GET_UPTAGCTL:
		case XPON_IGMP_GET_DOWNTCI:
		case XPON_IGMP_GET_DOWNTAGCTL:
		case XPON_IGMP_GET_MAXRATE:
		case XPON_IGMP_GET_ROBUST:
		case XPON_IGMP_GET_UNAUTHOR:
		case XPON_IGMP_GET_QUERYINTERVAL:
		case XPON_IGMP_GET_LASTINTERVAL:
		case XPON_IGMP_GET_QUERYMAXRESP:
		case XPON_IGMP_GET_MAXPLAYGROUP:
		case XPON_IGMP_GET_MAXBANDWIDTH:
		case XPON_IGMP_GET_BWENFORCEMENT:
		case XPON_IGMP_GET_COUNTER_CURRENT_MCAST_BW:
		case XPON_IGMP_GET_COUNTER_JOIN_MSG:
		case XPON_IGMP_GET_COUNTER_BW_EXCEEDED:
		case XPON_IGMP_GET_TAGSTRIP:
		case XPON_IGMP_GET_PORTFLAG:
		case XPON_IGMP_GET_IGMPFLAG:
		case XPON_IGMP_GET_XPONMODE:
		case XPON_IGMP_GET_ONUTYPE:
#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_IGMP_SET_GROUP)
		case XPON_IGMP_GET_GROUPNUM:	
#endif/*TCSUPPORT_COMPILE*/
		case XPON_IGMP_GET_VEIP_ACL:
		case XPON_IGMP_GET_EMPTRY_DYNLIST_PASS:
            return xpon_igmp_get_ioctl(cmd,argv1,argv2);	
		case XPON_IGMP_SET_VER:
		case XPON_IGMP_SET_FUNC:
		case XPON_IGMP_SET_FASTLEAVE:
		case XPON_IGMP_SET_UPTCI:
		case XPON_IGMP_SET_UPTAGCTL:
		case XPON_IGMP_SET_DOWNTCI:
		case XPON_IGMP_SET_DOWNTAGCTL:
		case XPON_IGMP_SET_MAXRATE:
		case XPON_IGMP_SET_ROBUST:
		case XPON_IGMP_SET_UNAUTHOR:
		case XPON_IGMP_SET_QUERYIP:
		case XPON_IGMP_GET_QUERYIP:
		case XPON_IGMP_SET_QUERYINTERVAL:
		case XPON_IGMP_SET_LASTINTERVAL:
		case XPON_IGMP_SET_QUERYMAXRESP:	
		case XPON_IGMP_SET_MAXPLAYGROUP:
		case XPON_IGMP_SET_MAXBANDWIDTH:
		case XPON_IGMP_SET_BWENFORCEMENT:
		case XPON_IGMP_SET_TAGSTRIP:
		case XPON_IGMP_SET_PORTFLAG:
		case XPON_IGMP_SET_IGMPFLAG:
		case XPON_IGMP_SET_XPONMODE:	
        case  XPON_IGMP_SET_DEBUG:
        case XPON_IGMP_SET_DS_BW_CTRL:
        case XPON_IGMP_CLEAR_HW_ENTRY:
        case XPON_IGMP_CLEAR_HW_DROP_ENTRY: 
#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_IGMP_SET_GROUP)
		case XPON_IGMP_SET_GROUPNUM:
#endif/*TCSUPPORT_COMPILE*/
        case XPON_IGMP_SET_CARE_DY_ST_VER:
        case XPON_IGMP_SET_VEIP_ACL:
        case XPON_IGMP_SET_EMPTRY_DYNLIST_PASS:
			return xpon_igmp_set_ioctl(cmd,argv1,argv2);	
		case XPON_IGMP_ADD_DYNWHITELIST:
		case XPON_IGMP_DEL_DYNWHITELIST:	
		case XPON_IGMP_CLEAR_DYNWHITELIST:
		case XPON_IGMP_GET_DYNWHITELISTCNT:
		case XPON_IGMP_GET_DYNWHITELIST:
		case XPON_IGMP_ADD_STAWHITELIST:
		case XPON_IGMP_DEL_STAWHITELIST:	
		case XPON_IGMP_CLEAR_STAWHITELIST:
		case XPON_IGMP_GET_STAWHITELISTCNT:
		case XPON_IGMP_GET_STAWHITELIST:			
			return xpon_igmp_whitelist_ioctl(cmd,argv1,argv2);
   		case XPON_IGMP_GET_USER_SUBSCRIBE_CNT:
   		case XPON_IGMP_GET_USER_SUBSCRIBE_BY_INDEX:
			return xpon_igmp_user_subscribe_ioctl(cmd,argv1,argv2);
		case XPON_IGMP_ADD_PORTVLAN:
		case XPON_IGMP_DEL_PORTVLAN:
		case XPON_IGMP_CLEAR_PORTVLAN:
		case XPON_IGMP_GET_PORTVLANCNT:
		case XPON_IGMP_GET_PORTVLANID:
		case XPON_IGMP_SET_VLANSWITCHVID:
		case XPON_IGMP_GET_PORTVLANFLAG:
		case XPON_IGMP_SET_PORTVLANFLAG:
			return xpon_igmp_portvlan_ioctl(cmd,argv1,argv2);
		case XPON_IGMP_ADD_MULVLAN:
		case XPON_IGMP_DEL_MULVLAN:
		case XPON_IGMP_CLEAR_MULVLAN:
		case XPON_IGMP_GET_MULVLANCNT:
		case XPON_IGMP_GET_MULVLANID:
		case XPON_IGMP_GET_MULVLANFLAG:
		case XPON_IGMP_SET_MULVLANFLAG:	
			return xpon_igmp_mulvlan_ioctl(cmd,argv1,argv2);
		case XPON_IGMP_ADD_FWDENTRY:
		case XPON_IGMP_DEL_FWDENTRY:
		case XPON_IGMP_GET_FWDENTRYCNT:
		case XPON_IGMP_GET_FWDENTRY:
		case XPON_IGMP_CLEAR_FWDENTRY:
		case XPON_IGMP_SET_FWDMODE:
		case XPON_IGMP_GET_FWDMODE:
			return xpon_igmp_fwdtbl_ioctl(cmd,argv1,argv2);
		default:
			return -1;
	}
	return -1;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,35)
long xpon_igmp_dev_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
#else
int xpon_igmp_dev_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg)
#endif
{
	xPON_IGMP_Ioctl_t data;
	unsigned long subcmd = 0;
	unsigned long argv1 = 0;
	unsigned long argv2 = 0;
	int ret = 0;

	memset(&data, 0, sizeof(data));
	COPY_FROM_USER(&data, (xPON_IGMP_Ioctl_t*)arg, sizeof(data));


#ifdef TCSUPPORT_CPU_ARMV8_64
		cmd = cmd & IOCTL_CMD;
#endif

	switch(cmd)
	{
		case XPON_IGMP_IOC_ACTION:		
			subcmd = data.subcmd;
			argv1 = data.argv1;
			argv2 = data.argv2;
			ret = xpon_igmp_ioctl(subcmd, argv1, argv2);
			if(ret != 0){
				printk("\r\nioctl excute error  ========> XPON Igmp ioctl\n");
			}
			break;
		default:
			ret = -1;
			printk("\r\ncmd error  ========> XPON Igmp ioctl\n");
			break;
	}
	return ret;
}

int xpon_igmp_dev_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static struct file_operations xpon_igmp_ioctl_fops = {
	.owner =		THIS_MODULE,
	.write =		NULL,
	.read =			NULL,
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,35)
	.unlocked_ioctl =	xpon_igmp_dev_ioctl,
#ifdef TCSUPPORT_CPU_ARMV8_64
	.compat_ioctl	= 	xpon_igmp_dev_ioctl,
#endif
#else
	.ioctl =		xpon_igmp_dev_ioctl,
#endif
	.open =			xpon_igmp_dev_open,
	.release =		NULL,
};

int xpon_init_dev(void)
{
	int ret = 0;

	ret = register_chrdev(GLOBAL_IGMP_MAJOR, GLOBAL_IGMP_IOC_NAME, &xpon_igmp_ioctl_fops);
	if (ret < 0){
		printk("ERROR! xpon_init_dev,create chrdev xpon_igmp fail\n");
		return ret;
	}
	return 0;
}

int xpon_init_port_conf(void)
{
	int i;
	xPON_PortEntry_t* portEntry=NULL;
	xPON_PortConf_t *portConf=NULL;
	
	//memset(port_conf,0,sizeof(port_conf));
	for(i=0;i< (igmp_conf.uni_num + igmp_conf.veip_num);i++)
	{
		if(i< igmp_conf.uni_num){
			portEntry = &igmp_conf.uni_port[i];
		}
		else{
			portEntry = &igmp_conf.veip_port[i- igmp_conf.uni_num];
		}

		portEntry->port_flag = XPON_BRIDGE_PORT; 
		if(igmp_conf.onu_type  == HGU && i< igmp_conf.uni_num){
			portEntry->port_flag |= XPON_INVALID_PORT;
			printk("init port flag(%x) XPON_INVALID_PORT\n",portEntry->port_flag);
		}

		portEntry->port_vlan->vlan_flag = 0;
		portConf = portEntry->port_conf;
		memset(portConf,0,sizeof(xPON_PortConf_t));
		
		portConf->proto_mask = 0x37;
		portConf->work_mode = 1;
		if(TCSUPPORT_CUC_VAL || TCSUPPORT_CT_C5_HEN_SFU_VAL)
			portConf->fastleave= 1;
		
		portConf->robust = 3;
		portConf->queryinterval = 125;
		portConf->maxresp = 10;
		portConf->lastinterval = 10;	
		portConf->up_vlan_mode = MULTCAST_UPTAG_TRANSPARENT;	

		INIT_LIST_HEAD(&portConf->dyn_list);
		INIT_LIST_HEAD(&portConf->sta_list);		
		INIT_LIST_HEAD(&portConf->dyn_list_ipv6);		
	}
	return 0;
}

int xpon_init_port_status(void)
{
	int i;
	memset(port_status,0,sizeof(port_status));
	for(i=0;i<igmp_conf.uni_num;i++)
	{
	    port_status[i].pmc.join_msg = 0;
	    port_status[i].pmc.bw_exceeded = 0;
	    port_status[i].pmc.current_mcast_bw = 0;
		INIT_LIST_HEAD(&port_status[i].user_subscribe_list);
	}

	memset(veip_port_status,0,sizeof(veip_port_status));
	for(i=0;i< igmp_conf.veip_num ;i++)
	{
	    veip_port_status[i].pmc.join_msg = 0;
	    veip_port_status[i].pmc.bw_exceeded = 0;
	    veip_port_status[i].pmc.current_mcast_bw = 0;
		INIT_LIST_HEAD(&veip_port_status[i].user_subscribe_list);
	}
	
	return 0;
}

int xpon_init_foward_tbl(void)
{
	fwd_tbl.fwd_num = 0;
	fwd_tbl.fwd_mode = MULTCAST_SNOOPING_MODE;
	INIT_LIST_HEAD(&fwd_tbl.fwd_list);
	return 0;
}

//return the type of ONU. 1=SFU 2=HGU
int xpon_get_onu_type(void)
{
	int type = 0;

    ECNT_API_XPON_ONU_TYPE_GET(&type);

	return type;
}

/*TCSUPPORT_PON_IP_HOST start*/
#define BRIDGE_WANIF_TMP_LEN  8
#ifdef TCSUPPORT_OPENWRT
char bridge_wanIf[BRIDGE_WANIF_TMP_LEN] = "pon.0" ;
#else
char bridge_wanIf[BRIDGE_WANIF_TMP_LEN] = "nas1_0" ;
#endif
#if 0
static int pon_bridge_wanIf_read_proc(char *buf, char **start, off_t off, int count, int *eof, void *data)
{
	 int len = 0;

	 if(TCSUPPORT_PON_IP_HOST_VAL)
	 {
		 len = sprintf(buf,"bridge %s\n", bridge_wanIf);

		 if (len < off + count)
			 *eof = 1;
	 
		 len -= off;
		 *start = buf + off;
		 if(len > count)
			 len = count;
		  if(len < 0)
			 len = 0;
	}

	return len;
}

static int pon_bridge_wanIf_write_proc(struct file *file, const char *buffer, unsigned long count, void *data)
{
	char wan_type[BRIDGE_WANIF_TMP_LEN] = {0};
	char wan_name[BRIDGE_WANIF_TMP_LEN] = {0};
	char temp[BRIDGE_WANIF_TMP_LEN<<1] = {0};

	if(TCSUPPORT_PON_IP_HOST_VAL)
	{	
		if ( count > ( (BRIDGE_WANIF_TMP_LEN<<1) - 1 ) )
			return -EFAULT;

		if (copy_from_user(temp, buffer, count))
			return -EFAULT;

		sscanf(temp, "%s %s", wan_type, wan_name);

		if(memcmp(wan_name, "nas", 3) != 0)
			return -EFAULT;

		if(strcmp(wan_type, "bridge") == 0)
		{
			strcpy(bridge_wanIf, wan_name);
			igmp_conf.ani_num = 1;
			ani_port[0].ani_id = 1;
			//strcpy(ani_port[0].ani_name, bridge_wanIf);
			printk("pon_bridge_wanIf_write_proc set ani_name=%s,wan_name=%s\n",ani_port[0].ani_name,bridge_wanIf);
			ani_port[0].ani_flag = 0x7;
		}
		else
			return -EFAULT;
	}
		
	return count;
}
/*TCSUPPORT_PON_IP_HOST end*/

static int ecnt_igmp_mcast_to_cpu_read(char *page, char **start, off_t off,
	int count, int *eof, void *data)
{
	int len = 0;
	len = sprintf(page, "%d\n", multicast_to_cpu);
	
	len -= off;
	*start = page + off;

	if (len > count)
	{
		len = count;
	}
	else
	{
		*eof = 1;
	}

	if (len < 0)
	{
		len = 0;
	}

	return len;
}

static int ecnt_igmp_mcast_to_cpu_write(struct file *file, const char *buffer,
	unsigned long count, void *data)
{
	char val_str[32];
	int va1ue = 0;
	
	if (count > sizeof(val_str) - 1)
	{
		return -EINVAL;
	}

	if (copy_from_user(val_str, buffer, count))
	{
		return -EFAULT;
	}

	val_str[count] = '\0';

	if (sscanf(val_str, "%d", &va1ue) != 1) 
	{
		printk("usage: echo [0,1] > /proc/tc3162/mcast_to_cpu\n");
	}

	multicast_to_cpu = va1ue;
	
	return count;
}
#endif

/*********************************************************************************
 * ecnt multicast hook function implementation setction
 *
 *********************************************************************************/
ecnt_ret_val ecnt_mc_hook_set_recv_cb(struct ecnt_data *in_data)
{
	mc_ctrl_packet_rx *rx_callback = NULL;
	
	rx_callback = (mc_ctrl_packet_rx *)in_data;

	mc_ctrl_packet_rx_cb_func = *rx_callback;
	
	return ECNT_RETURN;
}

void ecnt_igmp_recv_handle(struct sk_buff *skb)
{
	/* Askey requirement: also need to send 
	multicast data pkt to App after multicast hwnat */
	if (NULL == mc_ctrl_packet_rx_cb_func)
	{
		return;
	}
	
	mc_ctrl_packet_rx_cb_func(IGMP_PKT_DATA,skb->data,skb->len);
}

/*___________________________________________________________________________
**      function name: ecnt_mc_hook_recv
**      descriptions:
**      	recv packet from ethernet rx and specific for Askey TLF
**      input parameters:
**		skb: data flow
**      output parameters:
**      	N/A
**
**      return:
**      	success:	> 0
**		failure:	-1
**___________________________________________________________________________
*/
ecnt_ret_val ecnt_mc_hook_recv(struct ecnt_data *in_data)
{
	int cpu_rn = 0;

	unsigned char *dst = NULL;
	struct sk_buff *skb = NULL;
	struct sk_buff *skb2 = NULL;
	ecnt_mc_data_s *p_recv = NULL;

	/* specific requirement */
	if (!TCSUPPORT_CCA_VAL)
	{
		return ECNT_CONTINUE;
	}

	/* default, multicast to cpu function is off */
	if (!multicast_to_cpu)
	{
		return ECNT_CONTINUE;
	}
	/* get recv info */
	p_recv = (ecnt_mc_data_s *)in_data;
	skb = p_recv->skb;
	cpu_rn = p_recv->cpu_reason;
	
	skb_reset_mac_header(skb);

	/* not mcast pkt, return */
	dst = eth_hdr(skb)->h_dest;
	if (!xpon_is_multicast_addr(dst))
	{
		return ECNT_CONTINUE;
	}
	
	/* copy skb to modify */
	skb2 = skb_copy(skb,GFP_ATOMIC);
	if (NULL == skb2)
	{
		return ECNT_HOOK_ERROR;
	}
	
	if (HIT_BIND_MUL_CPU == cpu_rn)
	{
		/* remove stag */
		memmove(skb2->data+4, skb2->data, ETH_ALEN*2);
		skb_pull(skb2, 4);
	}
		
	ecnt_igmp_recv_handle(skb2);

	if (NULL != skb2){
		kfree_skb(skb2);
	}
	
	return ECNT_CONTINUE;
}

struct ecnt_hook_ops ecnt_mc_set_cb_ops = {
    .name = "ecnt_mc_hook_set_recv_cb",
    .hookfn = ecnt_mc_hook_set_recv_cb,
    .is_execute = 1,
    .maintype = ECNT_MULTICAST,
    .subtype = ECNT_MC_SET_RX_CB_HOOK,
};

struct ecnt_hook_ops ecnt_mc_rx_ops = {
    .name = "ecnt_mc_hook_recv",
    .hookfn = ecnt_mc_hook_recv,
    .is_execute = 1,
    .maintype = ECNT_MULTICAST,
    .subtype = ECNT_MC_RECV_HOOK,
};


int xpon_init_igmp_conf(void)
{
	int type = xpon_get_onu_type();

	igmp_conf.onu_type = type;
	igmp_conf.xpon_mode = HGU;
	igmp_conf.flag = XPON_IGMP_MVLAN_DISABLED | XPON_CONTROL_MULTCAST_DISABLE;

	igmp_conf.uni_num =  4;//four port
	uni_port = uni_port_4p;

	igmp_conf.veip_num = 1;
	veip_port = veip_port_1p;
	if(TCSUPPORT_PON_IP_HOST_VAL){
		if(strcmp(bridge_wanIf, "NULL") == 0){
			igmp_conf.ani_num = 0;
			memset(ani_port, 0, sizeof(ani_port));
		}else{
			igmp_conf.ani_num = 1;
			ani_port[0].ani_id = 1;
			strcpy(ani_port[0].ani_name, bridge_wanIf);
			ani_port[0].ani_flag = 0x7;
		}
	}else{
		igmp_conf.ani_num =  sizeof(ani_port) / sizeof(xPON_AniEntry_t);
	    if(TCSUPPORT_GOOGLE_FIBER_VAL)
	        strcpy(ani_port[0].ani_name, "nas1");
	}	

	igmp_conf.mvlan_num = XPON_MULVLAN_NUMBER;
	igmp_conf.ani_port = ani_port;
	igmp_conf.uni_port = uni_port;
	igmp_conf.veip_port = veip_port;
	igmp_conf.veip_acl = 1;
	igmp_conf.empty_dynlist_pass = 1;
	igmp_conf.mul_vlan = mul_vlan;
	igmp_conf.fwd_tbl = &fwd_tbl;
	INIT_LIST_HEAD(&igmp_conf.hwnat_igmp);
	INIT_LIST_HEAD(&igmp_conf.hwnat_drop);	
	return 0;
}


static bool xpon_igmp_ip_in_range(unsigned char* startip,unsigned char* endip,unsigned char* inip)
{
	unsigned int  startip_int,endip_int,inip_int;

	startip_int = (startip[0]<<24)+(startip[1]<<16)+(startip[2]<<8)+startip[3];
	endip_int = (endip[0]<<24)+(endip[1]<<16)+(endip[2]<<8)+endip[3];
	inip_int = (inip[0]<<24)+(inip[1]<<16)+(inip[2]<<8)+inip[3];

	if((inip_int>=startip_int)&&(inip_int<=endip_int))
		return true;
	else
		return false;
}

bool xpon_upstream_dyn_whitelist_access(unsigned char* grp_addr,int port_id)
{
	int i,cnt;
    xPON_Whitelist_Temp_t temp = {0};

	if( SFU != igmp_conf.onu_type ) {
		port_id = MULTICAST_OP_VEIP_PORT_ID;
	}

	cnt = xpon_igmp_get_whitelist_cnt(0,port_id);

	if(0 == cnt){
		return true;
	}
	
	for(i=0;i<cnt;i++)
	{
        temp.idx = i;
		xpon_igmp_get_whitelist(DYNAMIC_WHITE_LIST, port_id, &temp);

		if(xpon_igmp_ip_in_range(temp.startip,temp.endip,grp_addr) ){
			return true;
	}
	}
	
	return false;
}

bool xpon_down_multicast_access_inner(struct sk_buff* skb,int port_id)
{
	int i = 0, cnt = 0;
	unsigned char grp_addr[16] = {0};
	int skb_vid = 0;
    xPON_Whitelist_Temp_t temp = {0};
	
	xpon_get_downstream_grpaddr(grp_addr,skb);
	
	cnt = xpon_igmp_get_whitelist_cnt(DYNAMIC_WHITE_LIST ,port_id);

	if(0 == cnt){
		return true;
	}

	skb_vid = xpon_get_vlan_id(skb);

	for(i=0;i<cnt;i++)
	{
        temp.idx = i;
		xpon_igmp_get_whitelist(DYNAMIC_WHITE_LIST, port_id, &temp);

		if(xpon_igmp_ip_in_range(temp.startip, temp.endip, grp_addr) )
		{
			if((0 == temp.vid) && (-1 == skb_vid) ){
				return true;
			}
			
			if((0 != temp.vid) && (skb_vid == temp.vid) ){
				return true;
		}
	}
	}
	
	return false;
}

bool xpon_down_vlan_general_query_access(struct sk_buff* skb,int port_id)
{
	int i = 0,cnt = 0;
	int skb_vid = 0;
    xPON_Whitelist_Temp_t temp = {0};
	
	cnt = xpon_igmp_get_whitelist_cnt(DYNAMIC_WHITE_LIST, port_id );

	if(0 == cnt){
		return true;
	}

	skb_vid = xpon_get_vlan_id(skb);

	skb->vlan_tags[0] = skb_vid;
	skb->vlan_tag_flag |= VLAN_PACKET;

	for(i=0;i<cnt;i++)
	{
        temp.idx = i;
		xpon_igmp_get_whitelist(DYNAMIC_WHITE_LIST, port_id, &temp);

		if((0 == temp.vid) && (-1 == skb_vid) ){
			return true;
		}
		
		if((0 != temp.vid) && (skb_vid == temp.vid) ){
			return true;
	}
	}
	
	return false;
}


int xpon_sfu_down_vlan_access_control(struct sk_buff* skb,char* dev_name)
{
	xPON_PortEntry_t*    entry = NULL;
	int port = 0; 	

	entry = xpon_port_entry_by_name(dev_name);	
	if(NULL == entry)		
		return -1;	

	port = entry->port_id;

	if(xpon_is_general_query(skb))
	{
		if(false == xpon_down_vlan_general_query_access(skb,port))
			return -1;
		else
			return 0;
	}

	if(false == xpon_down_multicast_access_inner(skb,port))	
		return -1;	

	return 0;	
}

int xpon_store_up_vlan_read(char *buf, char **start, off_t off, int count,int *eof, void *data)
{
	int len = 0;
	struct xPON_IGMP_Up_Vlan_s* pvlan_tmp = NULL;
	struct list_head* tmp = NULL;
	struct list_head* pos = NULL;

	len += sprintf(buf,"port vlan_tci grp_addr\n");
	spin_lock_bh(&up_vlan_lock);
	list_for_each_safe(pos,tmp,&multicast_up_vlan_list)
	{
		pvlan_tmp = list_entry(pos,struct xPON_IGMP_Up_Vlan_s,up_vlan_list);

		len += sprintf(buf+len,"%d%8d%8d.%2d.%2d.%2d\n",pvlan_tmp->port,pvlan_tmp->vlan_tci,
					pvlan_tmp->grp_addr[0],pvlan_tmp->grp_addr[1],pvlan_tmp->grp_addr[2],pvlan_tmp->grp_addr[3]);
	}
	spin_unlock_bh(&up_vlan_lock);
	
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

int xpon_store_upstream_igmp_vlan_tci(struct sk_buff* skb)
{
	unsigned char *dest = eth_hdr(skb)->h_dest;
	unsigned char grp_addr[16];
	struct xPON_IGMP_Up_Vlan_s* pvlan = NULL;
	struct xPON_IGMP_Up_Vlan_s* pvlan_tmp = NULL;
	struct list_head* tmp = NULL;
	int port_id = 0;
	int vlan_tci = 0;
	int exist_flag = 0;
	struct list_head* pos = NULL;
	struct vlan_ethhdr * veth = NULL;

	if (!xpon_is_multicast_addr(dest)||!xpon_is_igmp_pkt(skb))
        		return 0;

	if(TCSUPPORT_VLAN_TAG_VAL)
		skb->vlan_tag_flag |= VLAN_TAG_INSERT_FLAG;

	if(TCSUPPORT_PON_VLAN_VAL)
		skb->pon_vlan_flag |= PON_PKT_INSERT_FLAG;

	if(skb->xpon_igmp_flag&XPON_IGMP_UPSTREAM_RESTORE)
		return 0;

	memset(grp_addr,0,16);
	xpon_get_igmp_grpaddr(grp_addr,skb);

	port_id = xpon_get_igmp_port(skb);
	if(0 == port_id )
		return -1;

	if(false == xpon_upstream_dyn_whitelist_access(grp_addr,port_id))
		return -1;
	
	vlan_tci = xpon_get_vlan_tci(skb);

	spin_lock_bh(&up_vlan_lock);
		
	list_for_each_safe(pos,tmp,&multicast_up_vlan_list)
	{
		pvlan_tmp = list_entry(pos,struct xPON_IGMP_Up_Vlan_s,up_vlan_list);
	
		if((port_id == pvlan_tmp->port)&&(0 == memcmp(grp_addr,pvlan_tmp->grp_addr,16)))
		{
			if(vlan_tci != pvlan_tmp->vlan_tci)
			{
				pvlan_tmp->vlan_tci = vlan_tci;
			}
			
			exist_flag = 1;
		}
	}

	if(1 != exist_flag)
	{
		pvlan = (struct xPON_IGMP_Up_Vlan_s*)kmalloc(sizeof(struct xPON_IGMP_Up_Vlan_s),GFP_KERNEL);
		if(NULL != pvlan)
			memset(pvlan,0,sizeof(struct xPON_IGMP_Up_Vlan_s));
		else
			return -1;

		pvlan->port = port_id;
		pvlan->vlan_tci = vlan_tci;
		memmove(pvlan->grp_addr,grp_addr,16);
		
		list_add(&(pvlan->up_vlan_list),&multicast_up_vlan_list);
	}
	
	spin_unlock_bh(&up_vlan_lock);

	if(-1 != vlan_tci)
	{
		veth = (struct vlan_ethhdr *)skb_mac_header(skb);
		skb->protocol = veth->h_vlan_encapsulated_proto;
		memmove(skb_mac_header(skb) + VLAN_HLEN, skb_mac_header(skb), 12);
		skb->mac_header = skb->mac_header + VLAN_HLEN;

		skb_pull(skb, VLAN_HLEN);
	}

	skb->xpon_igmp_flag |= XPON_IGMP_IS_MULTICAST;
	skb->xpon_igmp_flag |= XPON_IGMP_UPSTREAM_RESTORE;
	return 1;
}

int xpon_upstream_vlan_recovery_by_dynlist(struct sk_buff* skb)
{
	int port_id = 0, i =0, cnt = 0;
	unsigned char grp_addr[16] = {0};
	int recovery_vid = -2;
	struct xPON_IGMP_Up_Vlan_s* pvlan_tmp = NULL;
	struct list_head *pos = NULL, *next = NULL;
    xPON_Whitelist_Temp_t temp = {0};

	if(NULL == skb){
		return -1;
	}

	if(!(skb->xpon_igmp_flag & XPON_IGMP_IS_MULTICAST))
			return 0;

	if(skb->xpon_igmp_flag&XPON_IGMP_UPSTREAM_RECOVERY)
			return 0;

	memset(grp_addr,0,16);
	xpon_get_igmp_grpaddr(grp_addr,skb);

	port_id = xpon_get_igmp_port_by_original_dev(skb);
    
	if(0 == port_id){
		return -1;
	}

	if(SFU != igmp_conf.onu_type){
		port_id = MULTICAST_OP_VEIP_PORT_ID;
	}

	cnt = xpon_igmp_get_whitelist_cnt(DYNAMIC_WHITE_LIST, port_id);
	
	if(0 == cnt)
	{		
		spin_lock_bh(&up_vlan_lock);
		
		list_for_each_safe(pos,next,&multicast_up_vlan_list)
		{
			pvlan_tmp = list_entry(pos,struct xPON_IGMP_Up_Vlan_s,up_vlan_list);

			if((port_id == pvlan_tmp->port)&&(0 == memcmp(grp_addr,pvlan_tmp->grp_addr,16)))
				recovery_vid = pvlan_tmp->vlan_tci;
		}

		spin_unlock_bh(&up_vlan_lock);

		if(-1 == recovery_vid)
		{
			skb->xpon_igmp_flag |= XPON_IGMP_UPSTREAM_RECOVERY;
			return 0;
		}
	}
	else
	{
		for(i=0;i<cnt;i++)
		{
            temp.idx = i;
		    xpon_igmp_get_whitelist(DYNAMIC_WHITE_LIST, port_id, &temp);

			if(xpon_igmp_ip_in_range(temp.startip, temp.endip, grp_addr))
			{
				recovery_vid = temp.vid;
				break;
			}
		}

		if(-2 == recovery_vid)
			return -1;
		else if(0 == recovery_vid)
		{
			skb->xpon_igmp_flag |= XPON_IGMP_UPSTREAM_RECOVERY;
			return 0;
		}
	}
	
	if(NULL == __vlan_put_tag(skb,recovery_vid)){
		return -1;
	}
	if(NULL != skb)
		skb->xpon_igmp_flag |= XPON_IGMP_UPSTREAM_RECOVERY;
	return 0;
}

int xpon_hgu_down_multicast_access_control(struct sk_buff* skb)
{
	int whitelist_port_id = MULTICAST_OP_VEIP_PORT_ID;

	if(xpon_is_general_query(skb))
	{
		if(false == xpon_down_vlan_general_query_access(skb,whitelist_port_id))
			return  -1;
		else
			return 1;
	}
	else if(false == xpon_down_multicast_access_inner(skb,whitelist_port_id))	
		return -1;
	else
		return 0;
}

int xpon_down_multicast_replace_vlan(struct sk_buff** pskb,int vlan_tci)
{
	int skb_vlan_tci = 0;
	struct vlan_ethhdr* veth;
    struct sk_buff* skb = *pskb;

	skb_vlan_tci = xpon_get_vlan_tci(skb);

	if(-1 == skb_vlan_tci)
	{
		/*keep*/
		if(-1 == vlan_tci)
			return 0;
		/*add*/
		else
		{
			if (skb_headroom(skb) < VLAN_HLEN) 
			{
				struct sk_buff *skb2 = skb_realloc_headroom(skb, VLAN_HLEN);
				if (skb2) 
				{
					kfree_skb(skb);
					skb = skb2;
				}
			}            
			__vlan_put_tag(skb,vlan_tci);
		}
	}
	else
	{
	    /*skb unshare*/
		skb = skb_unshare(skb, GFP_ATOMIC);
		if (skb == NULL)
		{
			printk("%s %d skb_unshare fail\n",__FUNCTION__,__LINE__);
			return 0;
		}
		*pskb = skb;
        
		/*remove*/
		if(-1 == vlan_tci)
		{
			memmove(skb_mac_header(skb) + VLAN_HLEN, skb_mac_header(skb), 12);
			skb->mac_header = skb->mac_header + VLAN_HLEN;
			skb_pull(skb, VLAN_HLEN);
		}
		/*replace*/
		else
		{
			veth = (struct vlan_ethhdr *)skb->data;
			veth->h_vlan_TCI = htons(vlan_tci);
		}
	}
    
	return 0;
}

int xpon_replace_general_query_vlan(struct sk_buff** pskb)
{
	struct sk_buff* skb = *pskb;
	int port_id = 0;
	struct list_head *pos = NULL, *next = NULL;
	struct xPON_IGMP_Up_Vlan_s* pvlan_tmp = NULL;
	int i = 0, cnt = 0;
	int skb_vid = 0;
    xPON_Whitelist_Temp_t temp = {0};

	port_id = xpon_get_igmp_port(skb);
	if(0 == port_id)
		return -1;
	
	if(SFU != igmp_conf.onu_type){
		port_id = MULTICAST_OP_VEIP_PORT_ID;
	}

	cnt = xpon_igmp_get_whitelist_cnt(DYNAMIC_WHITE_LIST, port_id);

	if(0 == cnt)
		return 0;

	if(igmp_conf.onu_type == SFU)
		skb_vid = xpon_get_vlan_id(skb);
	else
	{
		if(skb->vlan_tag_flag & VLAN_PACKET)
			skb_vid = skb->vlan_tags[0];
		else
			return 0;
	}

	for(i=0;i<cnt;i++)
	{
        temp.idx = i;
		xpon_igmp_get_whitelist(DYNAMIC_WHITE_LIST, port_id, &temp);

		if(skb_vid == temp.vid)
			break;
	}

	spin_lock_bh(&up_vlan_lock);
	
	list_for_each_safe(pos,next,&multicast_up_vlan_list)
	{
		pvlan_tmp = list_entry(pos,struct xPON_IGMP_Up_Vlan_s,up_vlan_list);

		if((port_id == pvlan_tmp->port)&&xpon_igmp_ip_in_range(temp.startip,temp.endip,pvlan_tmp->grp_addr))
		{
			spin_unlock_bh(&up_vlan_lock);
			xpon_down_multicast_replace_vlan(pskb,pvlan_tmp->vlan_tci);
			return 0;
		}
	}

	spin_unlock_bh(&up_vlan_lock);

	return -1;
}

int xpon_down_multicast_vlan_handle(struct sk_buff** pskb)
{
	struct sk_buff* skb = *pskb;
	unsigned char *dest = eth_hdr(skb)->h_dest;
	unsigned char grp_addr[16];
	int port_id = 0;
	struct list_head* pos = NULL;
	struct xPON_IGMP_Up_Vlan_s* pvlan_tmp = NULL;
	struct list_head* tmp = NULL;

	if (!xpon_is_multicast_addr(dest))
    		return 0;
	
	if (!xpon_is_igmp_pkt(skb) && !xpon_is_data_pkt(skb))
		return 0;

	if(skb->xpon_igmp_flag&XPON_IGMP_DOWNSTREAM_VLAN_HANDLE)
		return 0;
    
	if(xpon_is_general_query(skb))
	{
		if(-1 == xpon_replace_general_query_vlan(pskb))
			return -1;
        
		skb->xpon_igmp_flag |= XPON_IGMP_DOWNSTREAM_VLAN_HANDLE;
		return 0;
	}

	memset(grp_addr,0,16);
	xpon_get_downstream_grpaddr(grp_addr,skb);

	port_id = xpon_get_igmp_port(skb);
	if(0 == port_id )
		return -1;
	
	spin_lock_bh(&up_vlan_lock);
		
	list_for_each_safe(pos,tmp,&multicast_up_vlan_list)
	{
		pvlan_tmp = list_entry(pos,struct xPON_IGMP_Up_Vlan_s,up_vlan_list);

		if((port_id == pvlan_tmp->port)&&(0 == memcmp(grp_addr,pvlan_tmp->grp_addr,16)))
			xpon_down_multicast_replace_vlan(pskb,pvlan_tmp->vlan_tci);
	}

	spin_unlock_bh(&up_vlan_lock);

	skb->xpon_igmp_flag |= XPON_IGMP_DOWNSTREAM_VLAN_HANDLE;
	return 0;
}

extern int (*xpon_igmp_ioctl_hook)(unsigned long cmd,unsigned long argv1,unsigned long argv2);
//extern int (*xpon_bridge_incoming_hook)(struct sk_buff *skb, int clone);
extern int (*xpon_add_delete_port_hook)(struct net_device* dev, int op);

extern int (*xpon_sfu_up_send_multicast_frame_hook)(struct sk_buff *skb, int clone);
extern int (*xpon_sfu_up_multicast_incoming_hook)(struct sk_buff *skb, int clone);
extern int (*xpon_sfu_down_multicast_incoming_hook)(struct sk_buff *skb, int clone);
extern int (*xpon_hgu_down_multicast_incoming_hook)(struct sk_buff *skb, int clone);
extern int (*xpon_hybrid_down_multicast_incoming_hook)(struct sk_buff *skb, int clone);
extern int (*xpon_sfu_up_multicast_vlan_hook)(struct sk_buff *skb, int clone);
extern int (*xpon_sfu_multicast_protocol_hook)(struct sk_buff *skb);
extern int (*xpon_up_igmp_uni_vlan_filter_hook)(struct sk_buff *skb);
extern int (*xpon_up_igmp_ani_vlan_filter_hook)(struct sk_buff *skb);
extern int (*isVlanOperationInMulticastModule_hook)(struct sk_buff *skb);

extern int (*xpon_store_upstream_igmp_vlan_tci_hook)(struct sk_buff *skb);
extern int (*xpon_down_multicast_vlan_handle_hook)(struct sk_buff *skb);
extern int (*xpon_upstream_vlan_recovery_by_dynlist_hook)(struct sk_buff* skb);
extern int (*xpon_hgu_down_multicast_access_control_hook)(struct sk_buff* skb);

extern int (*xpon_hgu_multicast_data_hook)(struct sk_buff *skb);
#ifdef TCSUPPORT_RA_HWNAT
extern int (*xpon_igmp_learn_flow_hook)(struct sk_buff* skb);
extern int (*wan_multicast_drop_hook)(struct sk_buff* skb);
extern int (*wan_multicast_undrop_hook)(void);
extern int (*wan_multicast_undrop_by_grpip_hook)(unsigned char is_ipv6,unsigned char* grp_ip);
extern int (*wan_mvlan_change_hook)(void);
extern int (*multicast_flood_find_entry_hook)(int index);
extern int (*multicast_speed_find_entry_hook)(int index);
extern int (*multicast_speed_learn_flow_hook)(struct sk_buff* skb);
extern int (*hwnat_set_rule_according_to_state_hook)(int index, int state,int mask);


#else
static int (*xpon_igmp_learn_flow_hook)(struct sk_buff* skb) = NULL;
static int (*wan_multicast_drop_hook)(struct sk_buff* skb) = NULL;
static int (*wan_multicast_undrop_hook)(void) = NULL;
static int (*wan_multicast_undrop_by_grpip_hook)(unsigned char is_ipv6,unsigned char* grp_ip) = NULL;
static int (*wan_mvlan_change_hook)(void) = NULL;
static int (*multicast_flood_find_entry_hook)(int index) = NULL;
static int (*multicast_speed_find_entry_hook)(int index) = NULL;
static int (*multicast_speed_learn_flow_hook)(struct sk_buff* skb) = NULL;
static int (*hwnat_set_rule_according_to_state_hook)(int index, int state,int mask) = NULL;
#endif

static struct timer_list xpon_pre_reset_timer;
static long g_start_time = 0;
#define PREVIEW_RST_TIMER_INTERVAL (30*60) /*30 mins interval*/
#define PREVIEW_RST_TIMER (60*60*24) /*24 hours*/

int xpon_igmp_set_port_type(xpon_igmp_port_data_t * port_data)
{
	int i,j = 0;
	
	if(port_data == NULL)
		return ECNT_HOOK_ERROR;

	MULTICAST_CRITIC_INFO("isHybridMode=%d,portNum=%d\n",port_data->isHybridMode,port_data->portNum);
	if(port_data->isHybridMode == 0)
		return ECNT_RETURN;

	for(i=0; i < port_data->portNum; i++)
	{
		MULTICAST_CRITIC_INFO("i=%d,portName=%s,portType=%d\n",i,port_data->portList[i].portName,port_data->portList[i].portType);
		for(j=0; j < igmp_conf.uni_num; j++)
		{
			MULTICAST_CRITIC_INFO("igmp_conf.uni_port[%d].port_flag=0x%x\n",j,igmp_conf.uni_port[j].port_flag);
			if(!strcmp(igmp_conf.uni_port[j].port_name,port_data->portList[i].portName))
			{
				if(port_data->portList[i].portType == XPON_HYBRID_MODE_PORT_TYPE_PPTP)
					igmp_conf.uni_port[j].port_flag &= ~(XPON_INVALID_PORT);
				else if (port_data->portList[i].portType == XPON_HYBRID_MODE_PORT_TYPE_VEIP)
					igmp_conf.uni_port[j].port_flag |= XPON_INVALID_PORT;

				MULTICAST_CRITIC_INFO("new igmp_conf.uni_port[%d].port_flag=0x%x\n",j,igmp_conf.uni_port[j].port_flag);
			}
		}
	}

	xpon_igmp_hybrid_g = 0;
	MULTICAST_CRITIC_INFO("reset xpon_igmp_hybrid_g = 0\n");
	for(j=0; j < igmp_conf.uni_num; j++)
	{
		if(igmp_conf.uni_port[j].port_flag & XPON_INVALID_PORT)  //hgu port
		{
			continue;
		}

		xpon_igmp_hybrid_g = 1;
		MULTICAST_CRITIC_INFO("xpon_igmp_hybrid_g = 1\n");
	}

	xpon_fwdtbl_clear();
    return ECNT_RETURN;

}

int xpon_igmp_set_bridge_wan(char *bridge_wan)
{
	xPON_IGMPConf_t*  xpon_igmp=xpon_get_igmp_conf();

	if(bridge_wan == NULL || xpon_igmp == NULL)
		return ECNT_HOOK_ERROR;

	MULTICAST_CRITIC_INFO("enter bridge_wan=%s ani_num=%d\n",bridge_wan,xpon_igmp->ani_num);

	if(strlen(bridge_wan) > BRIDGE_WANIF_TMP_LEN)
		return ECNT_HOOK_ERROR;
	
	strcpy(bridge_wanIf, bridge_wan);
	if(xpon_igmp->ani_num > 0)
		strcpy(xpon_igmp->ani_port[0].ani_name,bridge_wanIf);

	return ECNT_RETURN;
}

int xpon_igmp_api_dispatch(struct ecnt_data *in_data)
{
	xpon_igmp_api_data_t *api_data=NULL;
	int ret=0;

	if(in_data == NULL)
		return ECNT_HOOK_ERROR;

	api_data = (xpon_igmp_api_data_t *)in_data;
	if(api_data->api_type == XPON_IGMP_API_TYPE_SET)
	{
		switch(api_data->cmd_id)
		{
			case XPON_IGMP_CMD_PORT_TYPE: 
				ret = xpon_igmp_set_port_type(&api_data->data.port_data);
				break;
			case XPON_IGMP_CMD_BRIDGE_WAN: 
				ret = xpon_igmp_set_bridge_wan(api_data->data.bridgeWan);
				break;	
			default :
				break;
		}
	}
	else if(api_data->api_type == XPON_IGMP_API_TYPE_GET)
	{
		//NULL
	}

	return ECNT_RETURN;
}

struct ecnt_hook_ops xpon_igmp_api_dispatch_hook_ops = {
    .name = "xpon_igmp_api_dispatch",
    .hookfn = xpon_igmp_api_dispatch,
    .is_execute = 1,
    .maintype = ECNT_XPON_IGMP,
    .subtype = ECNT_XPON_IGMP_API,
    .priority   = 1,
};

int xpon_igmp_has_control_entry(void)
{
    int i = 0;
    for(i=0;i<igmp_conf.uni_num;i++)
    {
         if(0 != port_conf[i].dyn_cnt)
            return 1;

         if(0 != port_conf[i].sta_cnt)
            return 1;
    }
    return 0;
}

void xpon_igmp_hybrid_mode_hook_init(void)
{
	//HYBRID MODE
	printk("\n---------------------HGU xpon_igmp_init-------------------------------\n");
	rcu_assign_pointer(xpon_sfu_up_send_multicast_frame_hook, xpon_up_send_multicast_frame_hook);
	rcu_assign_pointer(xpon_sfu_up_multicast_incoming_hook,   xpon_up_igmp_incoming_hook);
	rcu_assign_pointer(xpon_hybrid_down_multicast_incoming_hook, xpon_hybrid_down_igmp_incoming_hook);
	rcu_assign_pointer(xpon_sfu_multicast_protocol_hook,      xpon_igmp_protocol_pack);
    rcu_assign_pointer(xpon_sfu_up_multicast_vlan_hook,       xpon_upstream_vlan_handle_hook);
    rcu_assign_pointer(xpon_up_igmp_uni_vlan_filter_hook,      xpon_up_igmp_uni_vlan_filter);
    rcu_assign_pointer(xpon_up_igmp_ani_vlan_filter_hook,      xpon_up_igmp_ani_vlan_filter);
    rcu_assign_pointer(isVlanOperationInMulticastModule_hook,  isVlanOperationInMulticastModule);

    return ;
}

static void xpon_pre_reset_timer_timeout(TIMER_FUN_PAAM arg)
{
	int i = 0;
	xPON_PortConf_t* conf = NULL;
	xPON_WhiteList_Entry_t* entry = NULL;
	struct list_head*  dyn_list; 
	long current_time;
	
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0) 
		struct timeval tv;	
		do_gettimeofday(&tv);
#else
		struct timespec64 tv;
		ktime_get_real_ts64(&tv);
#endif
	current_time = tv.tv_sec;
	if((current_time - g_start_time) < PREVIEW_RST_TIMER){
		mod_timer(&xpon_pre_reset_timer,(jiffies + (PREVIEW_RST_TIMER_INTERVAL*(HZ))));
		return;
	}
	
	/*24hours timeout!*/
	g_start_time = current_time;

	for(i=0;i<igmp_conf.uni_num;i++)
	{
		conf = xpon_port_conf_by_id(i+1);
		if(conf != NULL){			
			dyn_list = &(conf->dyn_list);
			if (list_empty(dyn_list))
				continue;
			
			list_for_each_entry(entry,dyn_list,list)
			{
				if (entry->preview_info.pre_len != 0)
					entry->preview_info.pre_rep_cnt_left = entry->preview_info.pre_rep_cnt;
			}
		}
			
	}
	for(i=0;i<igmp_conf.veip_num;i++)
	{
		conf = xpon_port_conf_by_id(i+igmp_conf.uni_num);
		if(conf != NULL){			
			dyn_list = &(conf->dyn_list);
			if (list_empty(dyn_list))
				continue;
			
			list_for_each_entry(entry,dyn_list,list)
			{
				if (entry->preview_info.pre_len != 0)
					entry->preview_info.pre_rep_cnt_left = entry->preview_info.pre_rep_cnt;
			}
		}
	}

	mod_timer(&xpon_pre_reset_timer,(jiffies + (PREVIEW_RST_TIMER_INTERVAL*(HZ))));

	return;
}

int xpon_igmp_init(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0) 
	struct timeval tv;
#else
	struct timespec64 tv;
#endif

	printk("\n initilize xpon igmp module....");
	init_xpon_igmp_macro_compatible();
	
	memset(port_vlan,0,sizeof(port_vlan));
	memset(mul_vlan,0,sizeof(mul_vlan));

	
	/*start preview reset timer*/
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)	
	setup_timer(&xpon_pre_reset_timer, xpon_pre_reset_timer_timeout, 0);
#else
	timer_setup(&xpon_pre_reset_timer, xpon_pre_reset_timer_timeout, 0); 
#endif
	mod_timer(&xpon_pre_reset_timer,(jiffies + (PREVIEW_RST_TIMER_INTERVAL*(HZ))));
	
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0) 
	do_gettimeofday(&tv);
#else
	ktime_get_real_ts64(&tv);
#endif

	g_start_time = tv.tv_sec;

	xpon_init_igmp_conf();
	xpon_init_port_conf();
	xpon_init_foward_tbl();
	xpon_rate_control_init();
	xpon_init_port_status();

	xpon_init_dev();
	xpon_igmp_ioctl_hook = xpon_igmp_ioctl;
    xpon_igmp_hybrid_mode_hook_init();

	if (ECNT_REGISTER_SUCCESS != ecnt_register_hook(&xpon_igmp_api_dispatch_hook_ops) ){
        panic("Register hook function failed! %s:%d", __FUNCTION__, __LINE__);
    }

	printk("done!\n");
	return 0;
}


int xpon_port_conf_clear(void)
{
	int i;


	for(i=0;i<igmp_conf.uni_num;i++)
	{
		xpon_whitelist_clear(0,i+1);
		xpon_whitelist_clear(1,i+1);	
		xpon_igmp_clear_portvlan(i+1);
	}
	for(i=0;i<igmp_conf.veip_num;i++)
	{
		xpon_whitelist_clear(0,i+igmp_conf.uni_num);
		xpon_whitelist_clear(1,i+igmp_conf.uni_num);	
		xpon_igmp_clear_portvlan(i+igmp_conf.uni_num);
	}
	return 0;
}

int xpon_user_subscribe_clear(int port)
{
	xPON_PortStatus_t* status = xpon_port_status_by_id(port);
	xPON_User_Subscribe_Entry_t* entry, *tmp;
	struct list_head* subscribe_list;
	
	if (!status)
		return 0;

	subscribe_list = &(status->user_subscribe_list);
	
	list_for_each_entry_safe(entry,tmp,subscribe_list,list)
	{
		list_del(&entry->list);
		xpon_free(entry);
	}

	return 0;
}


int xpon_port_status_clear(void)
{
	int i;


	for(i=0;i<igmp_conf.uni_num;i++)
	{
		xpon_user_subscribe_clear(i+1);
        port_status[i].user_subscribe_cnt = 0;		
	    port_status[i].pmc.join_msg = 0;
	    port_status[i].pmc.bw_exceeded = 0;
	    port_status[i].pmc.current_mcast_bw = 0;
	}
	return 0;
}

int xpon_uninit_igmp(void)
{
	xpon_port_conf_clear();
	xpon_fwdtbl_clear();
	//xpon_hwnat_clear_flows();
	xpon_port_status_clear();
	return 0;
}

#define GPON 1
#define EPON 2
int xpon_set_port_conf(int mode)
{
	return xpon_init_port_conf();
}

int xpon_igmp_set_mode(int mode)
{

	if (mode != EPON && mode != GPON)
		return -1;

	if (igmp_conf.xpon_mode == mode)
		return 0;
	
	memset(mul_vlan,0,sizeof(mul_vlan));
	xpon_port_conf_clear();
	xpon_port_status_clear();
	xpon_fwdtbl_clear();
	xpon_rate_control_init();
	xpon_set_port_conf(mode);
	igmp_conf.xpon_mode = mode;
	return 0;
}

void xpon_igmp_fini(void)
{

	xpon_igmp_ioctl_hook = NULL;

	xpon_sfu_up_send_multicast_frame_hook = NULL;
	xpon_sfu_up_multicast_incoming_hook = NULL;
    
    xpon_hybrid_down_multicast_incoming_hook = NULL;
    xpon_sfu_multicast_protocol_hook = NULL;
    xpon_sfu_up_multicast_vlan_hook = NULL;
    xpon_up_igmp_uni_vlan_filter_hook = NULL;
    xpon_up_igmp_ani_vlan_filter_hook = NULL;
    isVlanOperationInMulticastModule_hook = NULL;

	del_timer(&xpon_pre_reset_timer);

	xpon_uninit_igmp();

	ecnt_unregister_hook(&xpon_igmp_api_dispatch_hook_ops);

	return;
}

module_init(xpon_igmp_init);
module_exit(xpon_igmp_fini);
MODULE_LICENSE("GPL");
