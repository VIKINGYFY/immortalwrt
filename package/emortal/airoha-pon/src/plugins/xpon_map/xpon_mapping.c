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
 * contains	confidential trade secret material of MediaTeK Tech. Any attempt
 * or participation	in deciphering,	decoding, reverse engineering or in	any
 * way altering	the	source code	is stricitly prohibited, unless	the	prior
 * written consent of MediaTeK, Inc. is obtained.
 ***************************************************************************

	Module Name:
	xpon_mapping.c

	Abstract:

	Revision History:
	Who			When			What
	--------	----------		----------------------------------------------
	Name		Date			Modification logs
	andy.Yi		2013/3/20	Create
*/
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/in.h>
#include <linux/skbuff.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/if_pppox.h>
#include <linux/ppp_defs.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <net/ip.h>
#include <linux/libcompileoption.h>


#include "xpon_mapping.h"
#include <lan_port/lan_port_info.h>
#include <macro_compatible/ecnt_macro_compatible.h>
#include <ecnt_hook/ecnt_hook_xpon_mapping.h>
#include <ecnt_hook/ecnt_hook_pon_mac.h>

MODULE_DESCRIPTION("XPONMAP");
MODULE_LICENSE("GPL");

extern u8 isSfu;
extern PortLlidMap_t uni_llid_map[32];
#if defined(TCSUPPORT_GPON_MAPPING)
extern int (*gpon_mapping_hook)(struct sk_buff *skb);
extern int (*xpon_mode_get_hook)(void);
#if defined(TCSUPPORT_RA_HWNAT) && defined(TCSUPPORT_RA_HWNAT_ENHANCE_HOOK)
extern int (*ra_sw_nat_hook_drop_packet) (struct sk_buff * skb);
#endif
int (*gpon_queue_mapping_hook)(struct sk_buff *skb);
EXPORT_SYMBOL(gpon_queue_mapping_hook);


#if defined(TCSUPPORT_GPON_DOWNSTREAM_MAPPING)
extern int (*gpon_downstream_mapping_hook)(struct sk_buff *skb);
extern int (*gpon_downstream_mapping_stag_hook)(struct sk_buff **skb);
#endif

#ifdef TCSUPPORT_UPSTREAM_VLAN_POLICER
extern int (*upstream_vlan_policer_hook)(struct sk_buff *skb);
#endif

u8 gponmapDebugFlag = 0;
u8 gponmapQosMode = OMCI_CONFIG;
u8 gponmapSwPktDrop = 0;

unsigned int xpon_map_lanif_mask = 0xf0000000;
unsigned int xpon_map_dev_offset = 28;


#define QOS_8021p_MARK			0x0F00 	/* 8~11 bits used for 802.1p */

#define XPON_MAPPING_START_TIMER(timer,para)			{ timer.expires = para; mod_timer(&timer, (jiffies + ((timer.expires*HZ)/1000))) ; }
#define XPON_MAPPING_STOP_TIMER(timer)	do	    \
			{                                   \
				if(in_interrupt()) {            \
					del_timer(&timer) ;         \
				} else {                        \
					del_timer_sync(&timer) ;    \
				}                               \
			}while(0)

MappingCfgStatus_t g_mappingCfgStatus;

#define MAX_PORT_NUM 32
int uni_port_num = 6;
static uni_port_info_t uni_port_info[MAX_PORT_NUM] = {
{"pon", 	48, 2},
{"eth0.1", 	47, 0},
{"eth0.2", 	47, 0},
{"eth0.3", 	47, 0},
{"eth0.4", 	47, 0},
{"eth0.8",	49, 0},
{"eth0.7",	49, 0}
};

int set_uni_port_info(PortInfoIOCtl_t *port_info){
	if((port_info->port_num >= MAX_PORT_NUM) || (port_info->port_num <= 0)){
		return -1;
	}
	
	uni_port_num = port_info->port_num-1;
	memcpy(uni_port_info,port_info->port_info,(sizeof(uni_port_info_t)*(port_info->port_num)));
	return 0;
}


static int gponmap_debug_read_proc(char *page, char **start, off_t off,
	int count, int *eof, void *data)
{
	int len;

	len = sprintf(page, "%d\n", gponmapDebugFlag);

	len -= off;
	*start = page + off;

	if (len > count)
		len = count;
	else
		*eof = 1;

	if (len < 0)
		len = 0;

	return len;
}

static int gponmap_debug_write_proc(struct file *file, const char *buffer,
	unsigned long count, void *data)
{
	char val_string[64], cmd[32], subcmd[32] ;
	u32 action ;
	u32  Pbit, Port;
	u32  uCtrlFlag, dscp;
	u32 gemtype, pqmode, queue,uGemport,uTag, uVid, allocId;
	gemPortMappingIoctl_t gemData;
	gponQueueMappingIoctl_t queData;

	if (count > sizeof(val_string) - 1)
		return -EINVAL;

	if (copy_from_user(val_string, buffer, count))
		return -EFAULT;

	sscanf(val_string, "%s %s %x", cmd, subcmd, &action) ;
	memset(&gemData,0,sizeof(gemPortMappingIoctl_t));
	memset(&queData,0,sizeof(gponQueueMappingIoctl_t));

#ifdef GPON_MAPPING_DBG
	if(!strcmp(cmd, "msg")) 
	{
		u8 level = gponmapDebugFlag;

		 if(!strcmp(subcmd, "trace")) {
			gponmapDebugFlag = (action) ? (level|GPONMAP_MSG_TRACE) : (level&~GPONMAP_MSG_TRACE) ;
		} else if(!strcmp(subcmd, "warning")) {
			gponmapDebugFlag = (action) ? (level|GPONMAP_MSG_WARN) : (level&~GPONMAP_MSG_WARN) ;
		} else if(!strcmp(subcmd, "debug")) {
			gponmapDebugFlag= (action) ? (level|GPONMAP_MSG_DBG) : (level&~GPONMAP_MSG_DBG) ;
		} else if(!strcmp(subcmd, "err")) {
			gponmapDebugFlag = (action) ? (level|GPONMAP_MSG_ERR) : (level&~GPONMAP_MSG_ERR) ;
		}
		printk("Debug Level: %x\n", gponmapDebugFlag) ; 
	}  
	else if(!strcmp(cmd, "qos")) 
	{
		 if(!strcmp(subcmd, "mode")) {
			gponmapQosMode = (action) ? LOCAL_CONFIG: OMCI_CONFIG;
		} 
		printk("Gponmap Qos Mode: %s\n", gponmapQosMode ? "Local Config" : "OMCI Config") ; 
	}
	else if(!strcmp(cmd, "pktDrop"))
	{
		if(!strcmp(subcmd, "enable")) {
			gponmapSwPktDrop = 1;
		}
		else if(!strcmp(subcmd, "disable")) {
			gponmapSwPktDrop = 0;
		}
		printk("Gponmap sw pkt drop : %s\n", gponmapSwPktDrop ? "Enable" : "Disable") ; 	
	}
	else if(!strcmp(cmd, "gemportrule"))
	{
		if(!strcmp(subcmd, "showlist"))
		{
			displayAllGemPortMappingRule();
			return count;
		}
		sscanf(val_string, "%*s %*s %x %d %d %d %d %x %d",
			&uCtrlFlag,&uTag,&Port,&uVid,&dscp,&Pbit,&uGemport) ;	

		gemData.tagCtl = uCtrlFlag;
		gemData.tagFlag = uTag;
		gemData.vid = uVid;
		gemData.userPort = Port;
		gemData.dscp = dscp;
		gemData.pbit = Pbit;
		gemData.gemPort = uGemport;
		if(!strcmp(subcmd, "add"))
		{
			addGemPortMappingRule(&gemData);
			printk("add gemport mapping uCtrlFlag:%x, Tag:%d, Port:%d, uVid:%d, dscp:%d,Pbit:%x, uGemport%d \n", 
			uCtrlFlag,uTag,Port,uVid,dscp,Pbit,uGemport) ;
		}
		else if(!strcmp(subcmd, "del"))
		{
			delGemPortMappingRule(&gemData);
			printk("del gemport mapping uCtrlFlag:%x, Tag:%d, Port:%d, uVid:%d, dscp:%d,Pbit:%x, uGemport%d \n", 
			uCtrlFlag,uTag,Port,uVid,dscp,Pbit,uGemport) ;			
		}
	}
	else if(!strcmp(cmd, "queuerule"))
	{
		if(!strcmp(subcmd, "showlist"))
		{
			displayAllQueueMappingRule();
			return count;
		}
		sscanf(val_string, "%*s %*s %d %d %d %d %d ", 
			&uGemport,&gemtype,&pqmode,&allocId,&queue);
		queData.gemPort = uGemport;
		queData.gemType = gemtype;
		queData.pqMode  = pqmode;
		queData.allocId = allocId;
		queData.queue   = queue;
		if(!strcmp(subcmd, "add"))
		{
			addQueueMappingRule(&queData);
			printk("add queue mapping uGemport:%d gemtype:%d pqmode:%d allocId:%d queue:%d \n", 
				uGemport,gemtype,pqmode,allocId,queue) ;			
		}
		else if(!strcmp(subcmd, "del"))
		{
			delQueueMappingRule(&queData);
			printk("del queue mapping uGemport:%d gemtype:%d pqmode:%d allocId:%d queue:%d \n", 
				uGemport,gemtype,pqmode,allocId,queue) ;				
		}
	}	
#endif
	return count;
}


static int port_info_read_proc(char *page, char **start, off_t off,
	int count, int *eof, void *data)
{
	int i = 0;
	char unit_type_str[8][8]={"1G","VEIP","10G","2.5G","5G","25G","40G"};
	char eth_type_str[3][8]={"AUTO","SFU","HGU"};
	uint8_t unit_type_offset = 0;
	printk("LAN_Port \t ETH_Name \t Unit_Type \t ETH_Type\n");
	for(i = 0; i <= uni_port_num; i++){
		printk("%7d \t %8s \t ", i, uni_port_info[i].dev_name);

		if(uni_port_info[i].unit_type >=47 && uni_port_info[i].unit_type <= 53){
			unit_type_offset = uni_port_info[i].unit_type - 47;
			printk("%d(%5s) \t ",uni_port_info[i].unit_type, unit_type_str[unit_type_offset]); 
		}else{
			printk("%d(%5s) \t ",uni_port_info[i].unit_type, "N/A");
		}

		if(uni_port_info[i].eth_type >=0 && uni_port_info[i].eth_type <=2){
			printk("%d(%5s)\n",uni_port_info[i].eth_type, eth_type_str[uni_port_info[i].eth_type]);
		}else{
			printk("%d(%5s)\n",uni_port_info[i].eth_type, "N/A"); 
		}
	}
	

	return 0;
}

static int port_info_write_proc(struct file *file, const char *buffer,
	unsigned long count, void *data)
{
	char val_string[64] = {0};
	char argv1[16] = {0};
	char argv2[16] = {0};
	char argv3[16] = {0};
	long value = 0;
	long portid = 0;

	if (count > sizeof(val_string) - 1)
		return -EINVAL;

	if (copy_from_user(val_string, buffer, count))
		return -EFAULT;
	
	sscanf(val_string, "%s %s %s", argv1, argv2, argv3);

	if(strcmp(argv1,"uni_num") == 0){
		if(kstrtol(argv2, 10, &value) < 0){
			printk("uni_num value = %s translate to DEC fail\n", argv2);
		}else{
			if(value > 0 && value<= MAX_ECNT_ETHER_PORT_NUM){
				uni_port_num = value;
				printk("set support uni_num = %d\n", uni_port_num);
			}else{
				printk("uni_num value = 1~%d\n", MAX_ECNT_ETHER_PORT_NUM);
			}
		}
	}else{

		if(kstrtol(argv1, 10, &portid) < 0){
			printk("portid = %s translate to DEC fail\n", argv1);
			return count;
		}else{
		}
		
		if(portid > MAX_ECNT_ETHER_PORT_NUM || portid < 0){
			printk("ERROR: portid=%ld out of bound(1~%d)!\n", portid, MAX_ECNT_ETHER_PORT_NUM);
			return count;
		}else if(portid == 0){
			printk("WARN: portid=%ld is VEIP, do not be set!\n", portid);
			return count;
		}else{}
		
		if(strcmp(argv2,"dev_name") == 0){
			if(strlen(argv3) == 0){
				printk("dev_name Value = NULL\n");
			}else{
				memset(uni_port_info[portid].dev_name, 0, DEV_NAME_LEN);
				strncpy(uni_port_info[portid].dev_name,argv3, DEV_NAME_LEN-1);
			}
		}else if(strcmp(argv2,"unit_type") == 0){
			if(kstrtol(argv3, 10, &value) < 0){
				printk("unit_type Value = %s translate to DEC fail\n", argv3);
			}else{
				uni_port_info[portid].unit_type = value;
			}
		}else if(strcmp(argv2,"eth_type") == 0){
			if(kstrtol(argv3, 10, &value) < 0){
				printk("eth_type Value = %s translate to DEC fail\n", argv3);
			}else{
				if(value >= 0 && value <= 2){
					uni_port_info[portid].eth_type = value;
				}else{
					printk("ARG: eth_type value = 2(HGU) | 1(SFU) | 0(AUTO)\n");
				}
			}
		}
		else{
			printk("CMD1: echo <port_id> <type> <value> > /proc/gponmap/port_info \n");
			printk("CMD1 ARG: port_id = 1~%d\n", MAX_ECNT_ETHER_PORT_NUM);
			printk("CMD1 ARG: type = dev_name | unit_type | eth_type \n");
			printk("CMD1 ARG: dev_name value = eth0.x | eth1(obsolete) \n");
			printk("CMD1 ARG: unit_type value = 47(1G) | 48(VEIP) | 49(10G) | 50(2.5G) \n");
			printk("CMD1 ARG: eth_type value = 2(HGU) | 1(SFU) | 0(AUTO)\n");
			printk("CMD2: echo uni_num	<value> > /proc/gponmap/port_info \n");
			printk("CMD2 ARG: uni_num value = 1~%d\n", MAX_ECNT_ETHER_PORT_NUM);
		}
	}

	return count;
}

int mark2port(struct sk_buff *skb)
{
	int portid = -1;
	char dev_name1[DEV_NAME_LEN] = {0};
	char dev_name2[DEV_NAME_LEN] = {0};
	int mark = 0;
	int mark2 = 0;
	int i = 0;

	mark = GET_LAN_ITF_MARK(skb->mark);
	mark2 = GET_XFI_LAN_ITF_MARK(skb->mark2);
	
	if(mark == 0 && mark2 == 0){
		return 0;
	}
	
	if(mark2){
		snprintf(dev_name2, DEV_NAME_LEN, "eth%d", mark2);
	}
	if(mark){
		snprintf(dev_name1, DEV_NAME_LEN, "eth0.%d", mark);
	}
	
	for(i = 0; i <= uni_port_num; i++){
		if(strcmp(uni_port_info[i].dev_name, dev_name1) == 0){
			portid = i;
			break;
		}
		if(strcmp(uni_port_info[i].dev_name, dev_name2) == 0){
			portid = i;
			break;
		}
	}
	return portid;
}

int name2port(char *dev_name){
	int portid = -1;
	int i = 0;
	
	for(i = 0; i <= uni_port_num; i++){
		if(strcmp(uni_port_info[i].dev_name, dev_name) == 0){
			portid = i;
			break;
		}
	}
	
	return portid;
}

int port2name(int portid, char *dev_name){

	if(portid >=1 && portid <= uni_port_num){
		strcpy(dev_name, uni_port_info[portid].dev_name);
		return XPON_ETH_MAP_SUCCESS;
	}

	return XPON_ETH_MAP_FAILURE;
}
 
int port2ethtype(int portid){
	
	if(portid >=1 && portid <= uni_port_num){
		return uni_port_info[portid].eth_type;
	}

	return -1;
}

static inline __u8 ipv4_get_dscp(struct iphdr *iph)
{
	return (((iph->tos) & (~0x03)) >> 2);
}


static inline __u8 ipv6_get_dscp(struct ipv6hdr *ipv6h)
{
	return (((ntohs(*(__be16 *) ipv6h) >> 4) & (~0x03)) >> 2);
}

static inline int getVlanType(unsigned short TPID)
{
	switch(TPID)
	{
		case ETH_P_8021Q:
			return ETH_P_8021Q;
		case 0x88a8:
			return ETH_P_8021Q;
		case 0x9100:
			return ETH_P_8021Q;
		default:
			return -1;
	}
}

#ifdef TCSUPPORT_UPSTREAM_VLAN_POLICER
#define MAX_VLAN_UPSTRM_POLICER_NUM 8

static gpon_upstream_vlan_policer_ioctl_t
    upStrmPolicerTable[MAX_VLAN_UPSTRM_POLICER_NUM];
   
static unsigned char upStrmPolicerMask = 0;

#define SET_UPSTRM_POLICER(x)                \
    do{                                      \
        upStrmPolicerMask |= ( 1 << (x) );   \
    }while(0)

#define CLEAR_UPSTRM_POLICER(x)              \
    do{                                      \
        upStrmPolicerMask &= ~( 1 << (x) );  \
    }while(0)
    
#define IS_VALID_UPSTRM_POLICER(x)  ( (upStrmPolicerMask & ( 1 << (x) ) ) )

static int
addUpPolicerRule(gpon_upstream_vlan_policer_ioctl_t * pRule)
{
        unsigned char i = 0;

        // first check if the same entry exits
        for(i = 0; i < MAX_VLAN_UPSTRM_POLICER_NUM; ++ i){
                if(upStrmPolicerTable[i].ethID == pRule->ethID     && \
                   upStrmPolicerTable[i].entryID == pRule->entryID && \
                   IS_VALID_UPSTRM_POLICER(i))
                {
                        upStrmPolicerTable[i].chanID = pRule->chanID;
                        upStrmPolicerTable[i].vid = pRule->vid;
                        return 0;
                }
        }        

        // then check if there is empty room for this new entry
        for(i = 0; i < MAX_VLAN_UPSTRM_POLICER_NUM; ++ i){
                if (!IS_VALID_UPSTRM_POLICER(i)){
                        memcpy(upStrmPolicerTable + i, pRule, 
                               sizeof(gpon_upstream_vlan_policer_ioctl_t) );
                        SET_UPSTRM_POLICER(i);
                        return 0;
                }
        }

        
        return -1;
}

static int
delUpPolicerRule(gpon_upstream_vlan_policer_ioctl_t * pRule)
{
        unsigned char i = 0;

        for(i = 0; i < MAX_VLAN_UPSTRM_POLICER_NUM; ++ i){
                if(upStrmPolicerTable[i].ethID  != pRule->ethID   || \
                   upStrmPolicerTable[i].entryID!= pRule->entryID || \
                   !IS_VALID_UPSTRM_POLICER(i))
                {
                        continue;
                }
                
                CLEAR_UPSTRM_POLICER(i);                
                return 0;
        }
        
        return -1;
}

#define UP_STRM_POLICER_IRRELEVANT 0
#define UP_STRM_POLICER_ENABLE     1

int upstream_vlan_policer(struct sk_buff *skb )
{
        if(!skb){
                goto not_care;
        }
        const char * devName = skb->dev->name;
        unsigned  char ethID = 0;
        struct vlan_hdr *vhdr =  NULL;
        u16 vlan_id = 0;
        u16 vlan_tci = 0;
        int i = 0;
        
        if( htons(ETH_P_8021Q) != skb->protocol){
                goto not_care;
        }
        
        if( devName[0] != 'e' || strlen(devName) < 6 ){
                goto not_care;
        }
                
        ethID = devName[5] - '0';
        vhdr = (struct vlan_hdr *)skb->data;
        vlan_tci = ntohs(vhdr->h_vlan_TCI);
        vlan_id = vlan_tci & VLAN_VID_MASK;

        for( i = 0; i < MAX_VLAN_UPSTRM_POLICER_NUM; ++ i ){
                if( !IS_VALID_UPSTRM_POLICER(i)){
                        continue;
                }

                if( ethID == upStrmPolicerTable[i].ethID && \
                    vlan_id == upStrmPolicerTable[i].vid ){
                        skb->up_strm_policer_flag = UP_STRM_POLICER_ENABLE;
                        skb->up_strm_policer_trtcm_id = upStrmPolicerTable[i].chanID;
                        return 0;
                }
        }
        
not_care:
        skb->up_strm_policer_flag = UP_STRM_POLICER_IRRELEVANT;
        return 0;
}

void gpon_set_upstream_policer(struct sk_buff *skb)
{
	if ( UP_STRM_POLICER_ENABLE == skb->up_strm_policer_flag )
	{
		setTse(skb->pon_mark, TSENABLE);
		setTsID(skb->pon_mark, skb->up_strm_policer_trtcm_id);
	}
	return;
}

#else

void gpon_set_upstream_policer(struct sk_buff *skb)
{
	return;
}

#endif // #ifdef TCSUPPORT_UPSTREAM_VLAN_POLICER


int gpon_ds_uni_mapping(struct sk_buff *skb)
{
	gemPortMappingIoctl_t pktInfo;
	gemPortMapping_ptr gemport_rule_p = NULL;
    unsigned char *dp = skb->data;
    __be16 h_proto = 0;
	//static int cnt = 0;

	memset(&pktInfo, 0, sizeof(pktInfo));
	dp +=  ETH_ALEN+ETH_ALEN;
	h_proto = ntohs(*(__be16*)dp);   

    if (getVlanType(h_proto) == -1)
	{
		// untag packet
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "%s packet is untag just return\n",__FUNCTION__);
		return -1;
	}

	//vid
	//pktInfo.tagCtl |= GEMPORT_MAPPING_VID;
	pktInfo.vid = getVID(ntohs(*(__be16*)(dp+2)));

	
	//pbit
	//pktInfo.tagCtl |= GEMPORT_MAPPING_PBIT;
	pktInfo.pbit = getPbit(ntohs(*(__be16*)(dp+2)));
    
	dp += VLAN_HLEN;
	h_proto = ntohs(*(__be16*)dp);
	//check second vlan tag
	if (getVlanType(h_proto) != -1)
		dp += VLAN_HLEN;

	
    //gemport id
    //pktInfo.tagCtl |= GEMPORT_MAPPING_GEMPORT;
    pktInfo.gemPort = skb->gem_port;

    GPONMAP_PRINT(GPONMAP_MSG_TRACE, "%s taged packet info gem :%u, vlan :%u, pbit :%u skb->mark=0x%x\n",
        __FUNCTION__,pktInfo.gemPort,pktInfo.vid,pktInfo.pbit,skb->mark);
 
    gemport_rule_p = findUniByMappingRule(&pktInfo);
    if(gemport_rule_p == NULL)
    {
    	GPONMAP_PRINT(GPONMAP_MSG_ERR, "%s findUniByMappingRule fail return -1\n",__FUNCTION__);
    	return -1;
    }

	//if(cnt ++  % 50 == 0)
	GPONMAP_PRINT(GPONMAP_MSG_WARN, "%s gem=%d,userPort=%d,skb->mark=0x%x\n",__FUNCTION__,skb->gem_port,gemport_rule_p->userPort,skb->mark);

    return gemport_rule_p->userPort;
}

int gpon_ds_uni_mapping_ext(struct sk_buff *skb)
{
	gemPortMappingIoctl_t pktInfo;
    unsigned char *dp = skb->data;
    __be16 h_proto = 0;
	//static int cnt = 0;

	memset(&pktInfo, 0, sizeof(pktInfo));
	dp +=  ETH_ALEN+ETH_ALEN;
	h_proto = ntohs(*(__be16*)dp);   

    if (getVlanType(h_proto) == -1)
	{
		// untag packet
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "%s packet is untag just return\n",__FUNCTION__);
		return -1;
	}

	//vid
	//pktInfo.tagCtl |= GEMPORT_MAPPING_VID;
	pktInfo.vid = getVID(ntohs(*(__be16*)(dp+2)));

	
	//pbit
	//pktInfo.tagCtl |= GEMPORT_MAPPING_PBIT;
	pktInfo.pbit = getPbit(ntohs(*(__be16*)(dp+2)));
    
	dp += VLAN_HLEN;
	h_proto = ntohs(*(__be16*)dp);
	//check second vlan tag
	if (getVlanType(h_proto) != -1)
		dp += VLAN_HLEN;

	
    //gemport id
    //pktInfo.tagCtl |= GEMPORT_MAPPING_GEMPORT;
    pktInfo.gemPort = skb->gem_port;

    GPONMAP_PRINT(GPONMAP_MSG_TRACE, "%s taged packet info gem :%u, vlan :%u, pbit :%u skb->mark=0x%x\n",
        __FUNCTION__,pktInfo.gemPort,pktInfo.vid,pktInfo.pbit,skb->mark);

    return findUniByMappingRuleExt(&pktInfo);
}

/*******************************************************************************************
* function name
*	gpon_mapping
* description:
*	gpon mapping hook: find the tc->gem port mapping and gem port->pq rule, and add the gemport/tcont/pq info for skb
* retrun :
*
* parameter:
* 	
********************************************************************************************/
int gpon_mapping(struct sk_buff *skb)
{
	gemPortMappingIoctl_t pktInfo;
	gemPortMapping_ptr gemport_rule_p = NULL;
	gponQueueMapping_ptr queue_rule_p = NULL;
	unsigned char level = LEVEL_ETHER;
		
	unsigned char *dp = skb->data;
	__be16 h_proto;
	unsigned char queue=0;
	int onu_type = 0;

	memset(&pktInfo, 0, sizeof(pktInfo));
	dp +=  ETH_ALEN+ETH_ALEN;
	h_proto = htons(*(__be16*)dp);
	// check vlan tag
	if (getVlanType(h_proto) == -1)
	{
		pktInfo.tagCtl |= GEMPORT_MAPPING_TAGFLAG;
		pktInfo.tagFlag = UNTAGGED; 
	}
	else
	{
		//tagFlag
		pktInfo.tagCtl |= GEMPORT_MAPPING_TAGFLAG;
		pktInfo.tagFlag = TAGGED;

		//vid
		pktInfo.tagCtl |= GEMPORT_MAPPING_VID;
		pktInfo.vid = getVID(htons(*(__be16*)(dp+2)));
	
		//pbit
		pktInfo.tagCtl |= GEMPORT_MAPPING_PBIT;
		pktInfo.pbit = getPbit(htons(*(__be16*)(dp+2)));
		dp += VLAN_HLEN;
		h_proto = htons(*(__be16 *)dp);
		//check second vlan tag
		if (getVlanType(h_proto) != -1)
			dp += VLAN_HLEN;
	}
	
	//user port
	pktInfo.tagCtl |= GEMPORT_MAPPING_USERPORT;

	if(TCSUPPORT_PON_IP_HOST_VAL && (skb->pon_vlan_flag & PON_PKT_VOIP_TX))
		pktInfo.userPort = GPON_MAP_IPHOST_VOICE_PORT;
	else
	{
		pktInfo.userPort = mark2port(skb);
        ECNT_API_XPON_ONU_TYPE_GET(&onu_type);
		if(onu_type != 1 || pktInfo.userPort == GPON_MAP_IPHOST_VOICE_PORT){ //hgu or iphost vocie port
			if(skb->pon_vlan_flag & PON_PKT_FROM_HYBRID_PPTP){
				/*hybrid SFU port,keep original userport*/
			}else{
				pktInfo.userPort = GPON_MAP_VEIP_PORT;
			}
		}
	}
	
	//dscp
	// check IPv4 or IPv6 packet
	if ((h_proto != ETH_P_IP) && (h_proto != ETH_P_PPP_SES) && (h_proto != ETH_P_IPV6))
	{
		//only ether packet
		goto mapping;		
	}
	
	dp += 2; // skip ether type
	
	// check PPPoE packet
	if (h_proto == ETH_P_PPP_SES){
		__be16 pppoe_proto;
	
		dp += sizeof(struct pppoe_hdr);
		pppoe_proto = ntohs(*(__be16*)dp);
	
		dp += 2; // skip ppp header
	
		if (pppoe_proto == PPP_IP)
			h_proto = ETH_P_IP;
		else if (pppoe_proto == PPP_IPV6)
			h_proto = ETH_P_IPV6;
		else
			goto mapping;
	}
	
	// IPv4 packet
	if (h_proto == ETH_P_IP){
		struct iphdr *iph = (struct iphdr*)dp;

		if (iph->version != 4)
			goto mapping;
	
		level = LEVEL_IP;
	
		pktInfo.tagCtl |= GEMPORT_MAPPING_DSCP;
		pktInfo.dscp = ipv4_get_dscp(iph); //get dscp from ipv4 header
	}
	// IPv6 packet
	else if (h_proto == ETH_P_IPV6){
		struct ipv6hdr *ip6hdr = (struct ipv6hdr*)dp;
		if (ip6hdr->version != 6)
			goto mapping;
	
		level = LEVEL_IP;
		pktInfo.tagCtl |= GEMPORT_MAPPING_DSCP;
		pktInfo.dscp = ipv6_get_dscp(ip6hdr);	//get dscp from ipv6 header
	}
		
mapping:
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "gpon_mapping_hook:\n");
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "pktInfo.tagCtl = %x\n", pktInfo.tagCtl);
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "pktInfo.tagFlag = %x\n", pktInfo.tagFlag);
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "pktInfo.vid	= %x\n", pktInfo.vid);
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "pktInfo.pbit = %x\n", pktInfo.pbit);
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "pktInfo.userPort = %d\n", pktInfo.userPort);
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "pktInfo.dscp = %x\n", pktInfo.dscp);
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "pkt level = %s\n", (level == LEVEL_ETHER)? "ether pkt":"ip pkt");
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "skb->dev->name = %s\n", skb->dev->name);
	
		rcu_read_lock();
		gemport_rule_p = findGemPortMappingRule(&pktInfo);
		if (gemport_rule_p != NULL)
		{
			skb->gem_port = gemport_rule_p->gemPort;
			queue_rule_p = findQueueMappingRule(skb->gem_port);
			if (queue_rule_p != NULL)
			{
				/*multicast gemport drop*/
				if (queue_rule_p->mode & (1<<MODE_OFFSET_GEM_TYPE))
				{
					goto free_skb;
				}

				/* traffic shaping*/
				if (queue_rule_p->mode & (1<<MODE_OFFSET_TRAFFIC_SHAPING))
				{
					/* traffic shaping id*/
					setTse(skb->pon_mark, TSENABLE);
					setTsID(skb->pon_mark, ((queue_rule_p->queue & 0xF8) >> QUEUE_TRAFFIC_SHAPING_OFFSET));
				}
				else
				{
					setTse(skb->pon_mark, TSDISABLE);
					setTsID(skb->pon_mark, 0);
				}

				if (gponmapQosMode == OMCI_CONFIG)
				{
					/*traffic scheduler*/
					if (queue_rule_p->mode & (1<<MODE_OFFSET_TRAFFIC_SCHEDULER))
					{
						/*queue id*/
						setQueueID(skb->mark, pktInfo.pbit);
					}
					else
					{
						/*queue id*/
						setQueueID(skb->mark, (queue_rule_p->queue & 0x07));
					}
				}
				else	 if (!TCSUPPORT_CPU_EN75XX_VAL && TCSUPPORT_CT_PON_VAL && gponmapQosMode == LOCAL_CONFIG)
				{/*hgu qos local config */
						/*queue id*/
					if (skb->mark & QOS_8021p_MARK)
					{
						setQueueID(skb->mark, pktInfo.pbit);	
					}
					else
					{	
							/* In EN751221, queue mapping is move qdma drivers. */
							queue = (skb->mark & QOS_FILTER_MARK) >> 4;
							if (queue != 0)
							{
								queue = abs(7-queue) & 0x7;
							}
							setQueueID(skb->mark, queue);	
					}
				}
			}
			else
			{
				goto free_skb;
			}
			GPONMAP_PRINT(GPONMAP_MSG_TRACE, "matched, gemPort = %x, queue = %x,tse = %x, tsChannelId = %x\n", 
											skb->gem_port,getQueueID(skb->mark),getTse(skb->pon_mark), getTsID(skb->pon_mark));
		}
		else
		{	
			goto free_skb;
		}

#ifdef TCSUPPORT_UPSTREAM_VLAN_POLICER 
        if ( UP_STRM_POLICER_ENABLE == skb->up_strm_policer_flag )
        {
                setTse(skb->pon_mark, TSENABLE);
                setTsID(skb->pon_mark, skb->up_strm_policer_trtcm_id);
        }
#endif // #ifdef TCSUPPORT_UPSTREAM_VLAN_POLICE
	rcu_read_unlock();
	return 0;

free_skb:
	rcu_read_unlock();
	
#if defined(TCSUPPORT_RA_HWNAT) && defined(TCSUPPORT_RA_HWNAT_ENHANCE_HOOK)
	if(ra_sw_nat_hook_drop_packet)
		ra_sw_nat_hook_drop_packet(skb);
#endif
	GPONMAP_PRINT(GPONMAP_MSG_TRACE, "not matched, gemPort = %x\n", skb->gem_port);
//	kfree_skb(skb);
	return -1;
}

int gpon_queue_mapping(struct sk_buff *skb)
{
	gponQueueMapping_ptr queue_rule_p = NULL;
	unsigned char pbit = 0;
	unsigned char queue=0;
	unsigned char* dp = NULL;
	__be16 h_proto;

	if((skb == NULL)||(skb->data == NULL)){
		GPONMAP_PRINT(GPONMAP_MSG_ERR, "SKB/skb->data is NULL: Fail\r\n");
		return -1;
	}
	dp = skb->data;
	dp +=  ETH_ALEN+ETH_ALEN;
	h_proto = htons(*(__be16*)dp);

	if (getVlanType(h_proto) == -1)
		pbit = 0;
	else
		pbit = getPbit(htons(*(__be16*)(dp+2)));
	
	rcu_read_lock();

	queue_rule_p = findQueueMappingRule(skb->gem_port);
	if (queue_rule_p != NULL)
	{
		/*multicast gemport drop*/
		if (queue_rule_p->mode & (1<<MODE_OFFSET_GEM_TYPE))
		{
			goto free_skb;
		}

		/* traffic shaping*/
		if (queue_rule_p->mode & (1<<MODE_OFFSET_TRAFFIC_SHAPING))
		{
			/* traffic shaping id*/
			setTse(skb->pon_mark, TSENABLE);
			setTsID(skb->pon_mark, ((queue_rule_p->queue & 0xF8) >> QUEUE_TRAFFIC_SHAPING_OFFSET));
		}
		else
		{
			setTse(skb->pon_mark, TSDISABLE);
			setTsID(skb->pon_mark, 0);
		}

		if (gponmapQosMode == OMCI_CONFIG)
		{
			/*traffic scheduler*/
			if (queue_rule_p->mode & (1<<MODE_OFFSET_TRAFFIC_SCHEDULER))
			{
				/*queue id*/
				setQueueID(skb->mark, pbit);
			}
			else
			{
				/*queue id*/
				setQueueID(skb->mark, (queue_rule_p->queue & 0x07));
			}
		}
		else	 if (!TCSUPPORT_CPU_EN75XX_VAL && TCSUPPORT_CT_PON_VAL && gponmapQosMode == LOCAL_CONFIG)
		{/*hgu qos local config */
				/*queue id*/
			if (skb->mark & QOS_8021p_MARK)
			{
				setQueueID(skb->mark, pbit);	
			}
			else
			{	
				queue = (skb->mark & QOS_FILTER_MARK) >> 4;
				if (queue != 0)
				{
					queue = abs(7-queue) & 0x7;
				}
				setQueueID(skb->mark, queue);	
			}
		}
	}
	else
	{
		goto free_skb;
	}

	if(TCSUPPORT_UPSTREAM_VLAN_POLICER_VAL){
		gpon_set_upstream_policer(skb);
	}
	rcu_read_unlock();
	return 0;

free_skb:
	rcu_read_unlock();
	
	if(TCSUPPORT_RA_HWNAT_VAL && TCSUPPORT_RA_HWNAT_ENHANCE_HOOK_VAL){
		if(ra_sw_nat_hook_drop_packet)
			ra_sw_nat_hook_drop_packet(skb);
	}
	GPONMAP_PRINT(GPONMAP_MSG_TRACE, "not matched, gemPort = %x\n", skb->gem_port);
//	kfree_skb(skb);
	return -1;
}


#if defined(TCSUPPORT_GPON_DOWNSTREAM_MAPPING)
int gpon_downstream_mapping_option(struct sk_buff *skb)
{
	struct net_device *out_dev = NULL;
	u32 ifmask = 0,tmp = 0;

	if (gponmapSwPktDrop)
	{
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "\r\nskb  is droped,dev name is %s",skb->dev->name);
		goto drop2;
	}

	
	//packet is not from wan,do nothing.
	if(!(skb->pon_mark & DS_PKT_FORM_WAN))
		return 0;

	out_dev = skb->dev;
	//packet is not sent to lan,do nothing.
	if(!((out_dev->name[0] == 'e' && out_dev->name[4] == '.') || out_dev->name[0] == 'r' || out_dev->name[0] == 'u'))
		return 0;
		
	GPONMAP_PRINT(GPONMAP_MSG_TRACE, "\r\nskb gemport is %d,dev name is %s",skb->gem_port,out_dev->name);
	if(get_if_mask_by_gem_port(skb,&ifmask) == -1)
		return -1;

    GPONMAP_PRINT(GPONMAP_MSG_TRACE, "\r\n skb if_mask is %x ",ifmask);
    GPONMAP_PRINT(GPONMAP_MSG_ERR, "get_if_mask_by_gem_port success gem %d skb mark is %x \n",skb->gem_port, skb->mark);

	if(getDownQueueEnable(skb->pon_mark))
	{	
	//specify gem port is not found,handle as before.
	if(ifmask == 0)
		return 0;

	//set check bit
	if(out_dev->name[0] == 'e')
		tmp = 1 << (out_dev->name[5] - '1');
	else
	{
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "\r\n Not do gpon downstream mapping option\n");
		return 0;
	}

	//if the out if is in the group,set the mapping flag,otherwise drop packet
	if((ifmask & tmp) != 0)
	{
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "\r\nmatch mapping rule,set mapping flag 1 here");
		
		/*
			we can not always build HWNAT rule because Multicast GEM port may mapping to multi lan port.
			module don't care about the GEM port's type,but we need a flag to show if this GEM port mapping to multi lan port.
			we store this flag to bit31 of ifmask,and set this bit when build mapping rule.
		*/
		if((ifmask & 0x80000000) != 0)
		{
			skb->pon_mark &= ~DS_PKT_MAPPING_MARK;
			skb->pon_mark |= DS_PKT_MAPPING_TO_ONE;
		}
		else
		{
			skb->pon_mark &= ~DS_PKT_MAPPING_MARK;
			skb->pon_mark |= DS_PKT_MAPPING_TO_MULTI;
		}
	}
	}

	return 0;

drop2:
	kfree_skb(skb);
	return -1;
}

static int is_stag(u16 * stag)
{
	if(stag == NULL){
		return 0;
	}
	if(*stag == htons(ETH_P_8021Q) || *stag == htons(ETH_P_QinQ_88a8) || *stag == htons(ETH_P_QinQ_9100)){
		return 1;
	}
	return 0;
}

int gpon_ds_mapping_assign_queue(struct sk_buff **pskb)
{
	u16 * stag = NULL;
    u16 cos = 0;
    unsigned char *dstMacAddr=NULL;
    struct sk_buff *skb = *pskb;
    
    if (NULL == skb ){
        return 0;
    }	

	stag = (u16*)(skb->data + 12);
	if(!is_stag(stag))
		return 0;

	if(getDownTrtcmEnable(skb->pon_mark) || (skb->pon_mark & DS_PKT_MAPPING_MARK))
    {
	    /* skb_unshare */
		skb = skb_unshare(skb,GFP_ATOMIC);
	    if(skb == NULL)
	    {
	    	printk("%s %d skb_unshare fail\n",__FUNCTION__,__LINE__);
	        return 0;
	    }
	    *pskb = skb;    	
    }
	if(getDownTrtcmEnable(skb->pon_mark))
	{
		stag = (u16*)(skb->data + 12);
		*stag &= htons(0x03ff);//clean FUP and UPRI and DVP and DRM	
		*stag |= htons((getDownTrtcmID(skb->pon_mark) << 10));//set trtcm id
		
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "\r\nassign trtcm finished,trtcm is %d,stag is 0x%x",getDownTrtcmID(skb->pon_mark), htons(*stag));	
	}
	else if(skb->pon_mark & DS_PKT_MAPPING_MARK)//flag not 0 mean this packet need mapping to queue
	{        
		stag = (u16*)(skb->data + 12);
		*stag &= htons(0x0fff);//clean FUP and UPRI
		*stag |= htons(0x8000);//set FUP = 1,force PPE user priority
		*stag |= htons((getDownQueueID(skb->pon_mark) << 12));//set queue index
		GPONMAP_PRINT(GPONMAP_MSG_TRACE, "\r\nassign queue finished,queue is %d,stag is 0x%x",getDownQueueID(skb->pon_mark), htons(*stag));
	}
	else
	    return 0;
	/*Get skb destination MAC*/
	// multicast packet mapping to queue according to cos
  	dstMacAddr = skb->data;
	if(dstMacAddr != NULL){
		if(dstMacAddr[0] & 1){
			stag = (u16*)(skb->data + 12);
	        *stag &= htons(0x83FF);
	        cos = *((u16*)(skb->data + 14)) & htons(0xE000);
	        *stag |= cos >> 3;
	    }
	}
	return 0;
}


#endif
#endif

#if defined(TCSUPPORT_EPON_MAPPING)
extern u8 mappingDbgLevel;
extern int (*epon_sfu_clsfy_hook)(struct sk_buff *skb, int port);
extern int (*epon_mapping_hook)(struct sk_buff *skb);

/*
 * port: the eth port index [0~PortNum-1]
 */
int epon_sfu_clsfy(struct sk_buff *skb,  int port)
{
	QosPktInfo_t pktInfo;
	QosResultRule_Ptr pRule = NULL;
	unsigned char level = LEVEL_ETHER;	
	u8 queue = 0, pbit = 0;
	
	struct ethhdr *pEthHdr = eth_hdr(skb);
	unsigned char *dp = skb->data-2;
	unsigned short h_proto = ntohs(pEthHdr->h_proto);

	if (!isSfu) return 0;
	memset(&pktInfo, 0, sizeof(pktInfo));

	#if 0
	dp +=  ETH_ALEN+ETH_ALEN;
	h_proto = *(__be16*)dp;

	// check TC2206 special tag
	if (h_proto == 0x8901){
		dp += 8;
		h_proto = *(__be16*)dp;
	}
	else if ((h_proto&0xFF00) == 0x8000){
		dp += 4; // RT63365 special tag
		h_proto = *(__be16*)dp;
	}
	#endif
	QOS_PRINT(DBG_TRACE, "h_proto=%4X\n",  h_proto);
	// check vlan tag
	if (h_proto == ETH_P_8021Q || h_proto== 0x88a8 || h_proto == 0x9100){
		__be16 *pVlanData = (__be16*)(dp+2);
		
		QOS_PRINT(DBG_TRACE, "Data=0x%08x 0x%08x 0x%08x\n", ntohl(*((__be32*)dp)), ntohl(*((__be32*)(dp+4))), ntohl(*((__be32*)(dp+8))));
		pktInfo.pbit = (ntohs(*pVlanData))>>13;
		pktInfo.vid = (ntohs(*pVlanData))&0x0FFF;
		
		QOS_PRINT(DBG_TRACE, "pbit =%d, vid=%d\n", pktInfo.pbit, pktInfo.vid);

		dp += VLAN_HLEN;
		h_proto = ntohs(*(__be16 *)dp);

		// TODO: check second vlan tag, and need to process more tags!
		if (h_proto == ETH_P_8021Q || h_proto== 0x88a8 || h_proto == 0x9100){
			dp += VLAN_HLEN;
			h_proto = ntohs(*(__be16 *)dp);
		}		
	}else{ // no vlan tag, set default vid =1 && pbit = 0
		pktInfo.vid = 1;
		pktInfo.pbit = 0;
		pktInfo.res = 1;  // no vlan tag
	}
	
	if (!getMappingEnable(port)){
		queue = pktInfo.pbit; // mapping queue default by ether priority
		pbit = pktInfo.pbit;
		goto findLLIDQueueRule;
		
		//skb->v_if = 0;
		//setQueueID(skb->mark, pktInfo.pbit);
		//setNewPriority(skb->gem_port, 0xFF);// default don't remark pbit
		//return 1;
	}else if (getMappingResultNum(port) == 0){ // clear port classification rules
		queue = 0;
		pbit = 0;
		goto findLLIDQueueRule;
	}
	
	memcpy(pktInfo.dmac, pEthHdr->h_dest, ETH_ALEN);
	memcpy(pktInfo.smac, pEthHdr->h_source, ETH_ALEN);

	// check IPv4 or IPv6 packet
	if ((h_proto != ETH_P_IP) && (h_proto != ETH_P_PPP_SES) && (h_proto != ETH_P_IPV6))
	{
		//only ether packet
		goto findMappingRule;		
	}

	dp += 2; // skip ether type

	// check PPPoE packet
	if (h_proto == ETH_P_PPP_SES){
		__be16 pppoeProto;

		dp += sizeof(struct pppoe_hdr);
		pppoeProto = ntohs(*(__be16*)dp);

		dp += 2; // skip ppp header

		if (pppoeProto == PPP_IP)
			h_proto = ETH_P_IP;
		else if (pppoeProto == PPP_IPV6)
			h_proto = ETH_P_IPV6;
		else
			goto findMappingRule;
	}
	// IPv4 packet
	else if (h_proto == ETH_P_IP){
		struct iphdr *iph = (struct iphdr*)dp;

		if ((iph->version != 4)/* || (iph->ihl != 5) || (iph->frag_off & htons(IP_OFFSET|IP_MF))*/)
			goto findMappingRule;

		level = LEVEL_IP;

		pktInfo.ipver = 4;
		pktInfo.L3.v4.sip = ntohl(iph->saddr);
		pktInfo.L3.v4.dip = ntohl(iph->daddr);
		pktInfo.ipp = iph->protocol;
		pktInfo.hdr.bits.dscp = (iph->tos)>>2;// ltm new

		QOS_PRINT(DBG_TRACE, "sip=%x, dip=%x\n", iph->saddr, iph->daddr);

		if ((ntohs(iph->frag_off) & (IP_OFFSET|IP_MF))){
			goto findMappingRule;
		}

		if (iph->protocol == IPPROTO_TCP){
			struct tcphdr *tcph = (struct tcphdr*)(dp + iph->ihl*4);

			level = LEVEL_TRANS;
			pktInfo.sport = ntohs(tcph->source);
			pktInfo.dport = ntohs(tcph->dest);
		}else if (iph->protocol == IPPROTO_UDP){
			struct udphdr *udph = (struct udphdr*)(dp + iph->ihl*4);

			level = LEVEL_TRANS;
			pktInfo.sport = ntohs(udph->source);
			pktInfo.dport = ntohs(udph->dest);
		}
	}
	// IPv6 packet
	else{ 	// h_proto == ETH_P_IPV6
		struct ipv6hdr *ip6hdr = (struct ipv6hdr*)dp;
		if (ip6hdr->version != 6)
			goto findMappingRule;

		level = LEVEL_IP;

		pktInfo.ipver = 6;
		memcpy(&(pktInfo.L3.v6.sip), &(ip6hdr->saddr), 16);
		memcpy(&(pktInfo.L3.v6.dip), &(ip6hdr->daddr), 16);
		pktInfo.ipp = ip6hdr->nexthdr;
		pktInfo.hdr.value = ntohl(*(__be32*)ip6hdr);// ltm new

		if (ip6hdr->nexthdr == IPPROTO_TCP){
			struct tcphdr *tcph = (struct tcphdr*)(dp+sizeof(struct ipv6hdr));
				pktInfo.sport = ntohs(tcph->source);
				pktInfo.dport = ntohs(tcph->dest);
		}else if (ip6hdr->nexthdr == IPPROTO_UDP){
			struct udphdr *udph = (struct udphdr*)(dp+sizeof(struct ipv6hdr));
		
			level = LEVEL_TRANS;
			pktInfo.sport = ntohs(udph->source);
			pktInfo.dport = ntohs(udph->dest);
		}
	}

findMappingRule:	
	pktInfo.ethtype = ntohs(h_proto);
	pRule = findMappingResult(port, level, &pktInfo);
	if (pRule != NULL){
		queue = pRule->queue;
		pbit = pRule->pbit;
		skb->pon_vlan_flag |= PON_CLASSIFICATION_REMARK;
	}else{ // if no classification rule matched, use pbit from pkt.
		queue = pktInfo.pbit;
		pbit = pktInfo.pbit;
	}
	QOS_PRINT(DBG_TRACE, "priQueue=%d, newPbit=%d\n", queue, pbit);
	
findLLIDQueueRule:	
	skb->epon_queue = queue;
	skb->epon_pbit = pbit;

	return 1;
}

int epon_mapping(struct sk_buff *skb)
{
	u8 queue=0;
	u8 pbit=0;
	int uni_1g = 0;
	int uni_10g = 0;
	int skb_mark = 0;
	int skb_mark2 = 0;
	QueueMapping_Ptr pMap = NULL;

	QOS_PRINT(DBG_INFO, "enter  skb->mark=%x\n", skb->mark);
	if (isSfu){ 
		queue = skb->epon_queue;
		pbit = skb->epon_pbit;
	}else{
		queue = (skb->mark & QOS_FILTER_MARK) >> 4;
		if(!TCSUPPORT_CPU_EN75XX_VAL) {
			if (TCSUPPORT_CT_PON_VAL && queue != 0){
				//queue = abs(4-queue) & 0x7;
				queue = abs(7-queue) & 0x7;	//ctcom 7 qos queue
			}
		}
        
        if(TCSUPPORT_CT_PON_VAL && queue >= LLID_QUEUE_NUM_MAX){
			queue = 0;
        }
	}

	pMap = findLlidQueue(queue);
	if (pMap != NULL){
		skb->v_if = pMap->llid;
		setQueueID(skb->mark, pMap->queue);
		QOS_PRINT(DBG_INFO, "  skb->mark=%x pMap->queue=%d\n", skb->mark, pMap->queue);
	}else{
		//skb->v_if = 0;	// set llid = 0 default 
		skb_mark = skb->mark;
		skb_mark2 = skb->mark2;
		
		uni_1g = GET_LAN_ITF_MARK(skb_mark);
		uni_10g = GET_XFI_LAN_ITF_MARK(skb_mark2)+MAX_ECNT_1GETHER_PORT_NUM;
		if(uni_1g){
			if(uni_llid_map[uni_1g-1].enable){
				skb->v_if = uni_llid_map[uni_1g-1].default_llid;
				QOS_PRINT(DBG_INFO, "map enabled, 1G :skb->v_if = %d\n", skb->v_if);
			}
			else{
				skb->v_if = 0;
				QOS_PRINT(DBG_INFO, "map disabled 1G :skb->v_if = %d\n", skb->v_if);
			}
		}
		else
		{
			if(uni_llid_map[uni_10g-1].enable){
				skb->v_if = uni_llid_map[uni_10g-1].default_llid;
				QOS_PRINT(DBG_INFO, "map enabled, 10G :skb->v_if = %d\n", skb->v_if);
			}
			else{
				skb->v_if = 0;
				QOS_PRINT(DBG_INFO, "map disabled 10G :skb->v_if = %d\n", skb->v_if);
			}
		}
		setQueueID(skb->mark, pbit); // set queue = pbit
		QOS_PRINT(DBG_INFO, "  skb->mark=%x pbit=%d\n", skb->mark, pbit);
	}
	if(pMap != NULL)
	{
	    if(pMap->sla_eable)
	    {
	        setTse(skb->pon_mark, TSENABLE);
		    setTsID(skb->pon_mark, queue);
		    QOS_PRINT(DBG_INFO, "  setTsID tsId=%d\n", getQueueID(skb->mark));
	    }
	    else
	    {
	        setTse(skb->pon_mark, TSDISABLE);
		    setTsID(skb->pon_mark, 0);
	    }
	}
	QOS_PRINT(DBG_INFO, "findLLIDQueueRule: llid=%d queue=%d newPbit=%d\n", skb->v_if, getQueueID(skb->mark), pbit);

	return 1;	
}
#endif


static void xpon_mapping_done(TIMER_FUN_PAAM arg)
{
	if(g_mappingCfgStatus.DoFlag)
	{
		g_mappingCfgStatus.DoFlag = 0;
		if(g_mappingCfgStatus.timerFlag)
			XPON_MAPPING_START_TIMER(g_mappingCfgStatus.mappingDoneTimer,3000);
	}
	else
	{
		clearAllHwnatRules();
	}
	return ;
}

static inline void need_clean_hw_nat(void)
{
	g_mappingCfgStatus.timerFlag = 0;
	XPON_MAPPING_STOP_TIMER(g_mappingCfgStatus.mappingDoneTimer);
	g_mappingCfgStatus.timerFlag = 1;
	XPON_MAPPING_START_TIMER(g_mappingCfgStatus.mappingDoneTimer,3000);
	g_mappingCfgStatus.DoFlag = 1;
	return ;
}
	
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,35)
long xponmap_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
#else
int xponmap_ioctl (struct inode *inode, struct file *filp,
                  unsigned int cmd, unsigned long arg)
#endif
{
	int result = 0; 
#if defined(TCSUPPORT_GPON_MAPPING)
    	gemPortMappingIoctl_t opt ;
    	gponQueueMappingIoctl_t opt2;
#if defined(TCSUPPORT_GPON_DOWNSTREAM_MAPPING)
	gpon_downstream_mapping_ioctl opt3;
#endif
#endif

	PortInfoIOCtl_t opt4;
	
#ifdef TCSUPPORT_UPSTREAM_VLAN_POLICER
        gpon_upstream_vlan_policer_ioctl_t upStrmPolicerRule;
#endif // TCSUPPORT_UPSTREAM_VLAN_POLICER

#if defined(TCSUPPORT_EPON_MAPPING)
	u8 portId, llid;
	PortLlidMap_t portllidmap;
	QosIOCtl_t data;
	QosClsfyIOCtl_t clsfyCtl;
	QosQueueMappingIOCtl_t queueMapCtl;
#endif

#if defined(TCSUPPORT_GPON_MAPPING)
	memset(&opt, 0, sizeof(opt));
	memset(&opt2, 0, sizeof(opt2));
#if defined(TCSUPPORT_GPON_DOWNSTREAM_MAPPING)
	memset(&opt3, 0, sizeof(opt3));
#endif
#endif
#if defined(TCSUPPORT_EPON_MAPPING)
	memset(&data, 0, sizeof(QosIOCtl_t));
#endif

#ifdef TCSUPPORT_CPU_ARMV8_64
		cmd = cmd & IOCTL_CMD;
#endif

	GPONMAP_PRINT(GPONMAP_MSG_TRACE, "gponmap_ioctl: cmd = %x!\n", cmd);
	switch(cmd)
	{
		case SET_PORT_MAP:
			if (copy_from_user(&opt4, (PortInfoIOCtl_t*)arg, sizeof(opt4)))
			{
				return -EFAULT;
			}				
			result = set_uni_port_info(&opt4);
			break;
#if defined(TCSUPPORT_GPON_MAPPING)
	    	case GEMPORT_MAPPING_ADD_ENTRY:
			if (copy_from_user(&opt, (gemPortMappingIoctl_t*)arg, sizeof(opt)))
			{
				return -EFAULT;
			}				
			need_clean_hw_nat();
			result = addGemPortMappingRule(&opt);
			break;
	   	case GEMPORT_MAPPING_DEL_ENTRY:
			if (copy_from_user(&opt, (gemPortMappingIoctl_t*)arg, sizeof(opt)))
			{
				return -EFAULT;
			}	
			need_clean_hw_nat();
			result = delGemPortMappingRule(&opt);
			break;
	  	case QUEUE_MAPPING_ADD_ENTRY:
			if (copy_from_user(&opt2, (gponQueueMappingIoctl_t*)arg, sizeof(opt2)))
			{
				return -EFAULT;
			}	
			result = addQueueMappingRule(&opt2);
			break;
	   	case QUEUE_MAPPING_DEL_ENTRY:
			if (copy_from_user(&opt2, (gponQueueMappingIoctl_t*)arg, sizeof(opt2)))
			{
				return -EFAULT;
			}	
			result = delQueueMappingRule(&opt2);
			break;
		case QUEUE_MAPPING_GET_ENTRY:
			if (copy_from_user(&opt2, (gponQueueMappingIoctl_t*)arg, sizeof(opt2)))
			{
				return -EFAULT;
			}
			result = getQueueMappingRule(&opt2);
			if (0 != copy_to_user((void* __user)arg,&opt2,sizeof(gponQueueMappingIoctl_t))){
				return -EFAULT;
			}
			break;
		case QUEUE_MAPPING_RECFG_ENTRY:
			if (copy_from_user(&opt2, (gponQueueMappingIoctl_t*)arg, sizeof(opt2)))
			{
				return -EFAULT;
			}
			result = reCfgQueueMappingRule(&opt2);
			break;
	   	case GEMPORT_MAPPING_DUMP_ALL_ENTRY:
			result = displayAllGemPortMappingRule();
			break;		
		case QUEUE_MAPPING_DUMP_ALL_ENTRY:
			result = displayAllQueueMappingRule();
			break;					
#if defined(TCSUPPORT_GPON_DOWNSTREAM_MAPPING)
		case DOWNSTREAM_SWITCH_OPT:
			if (copy_from_user(&opt3, (gpon_downstream_mapping_ioctl*)arg, sizeof(opt3)))
			{
				return -EFAULT;
			}
			result = gpon_downstream_mapping_switch_option(&opt3,(void*)arg);
			break;
						
		case DOWNSTREAM_MAPPING_RULE_OPT:
			if (copy_from_user(&opt3, (gpon_downstream_mapping_ioctl*)arg, sizeof(opt3)))
			{
				return -EFAULT;
			}
			result = gpon_downstream_mapping_rule_option(&opt3,(void*)arg);
			break;
#endif
#endif

#ifdef TCSUPPORT_UPSTREAM_VLAN_POLICER
                case UPSTREAM_VLAN_POLICER_ADD_RULE:
                        if (copy_from_user(&upStrmPolicerRule, (gpon_upstream_vlan_policer_ioctl_t*)arg, 
                            sizeof(upStrmPolicerRule))){
                                return -EFAULT;
                        }
                        result = addUpPolicerRule(&upStrmPolicerRule);
                        break;
                
                case UPSTREAM_VLAN_POLICER_DEL_RULE:
                        if (copy_from_user(&upStrmPolicerRule, (gpon_upstream_vlan_policer_ioctl_t*)arg, 
                            sizeof(upStrmPolicerRule))){
                                return -EFAULT;
                        }
                        result = delUpPolicerRule(&upStrmPolicerRule);
                        break;
#endif

#if defined(TCSUPPORT_EPON_MAPPING)
		case EPONMAP_IOC_ENABLE:
			if (copy_from_user(&portId, (u8*)arg, sizeof(u8))){
				return -EFAULT;
			}
			setMappingEnable(portId, EPONMAP_ENABLE);
		break;

		case EPONMAP_IOC_DISABLE:
			if (copy_from_user(&portId, (u8*)arg, sizeof(u8))){
				return -EFAULT;
			}
			setMappingEnable(portId, EPONMAP_DISABLE);
		break;
		
		case EPONMAP_IOC_DELETE:
			if (copy_from_user(&data, (QosIOCtl_Ptr)arg, sizeof(QosIOCtl_t))){
				return -EFAULT;
			}
			if (FALSE == rmMappingResult(data.portId, &data.result, data.matchNum, data.matchs))
				return -EFAULT;
		break;

		case EPONMAP_IOC_ADD:
			if (copy_from_user(&data, (QosIOCtl_Ptr)arg, sizeof(QosIOCtl_t))){
				return -EFAULT;
			}
			need_clean_hw_nat();
			if (FALSE == addMappingResult(data.portId, &data.result, data.matchNum, data.matchs))
				return -EFAULT;
		break;

		case EPONMAP_IOC_PORTLLIDM:
			if(copy_from_user(&portllidmap, (PortLlidMap_Ptr)arg, sizeof(PortLlidMap_t))){
				return -EFAULT;
			}
			if (FALSE == addPortLlidMap(&portllidmap))
				return -EFAULT;
		break;

		case EPONMAP_IOC_CLEAR:
			if (copy_from_user(&portId, (u8*)arg, sizeof(u8))){
				return -EFAULT;
			}
			clearMappingResult(portId);
		break;

		case EPONMAP_IOC_SHOW:
			if (copy_from_user(&portId, (u8*)arg, sizeof(u8))){
				return -EFAULT;
			}
			showMappingResult(portId);
		break;

		case EPONMAP_IOC_GETNUM:
			if (copy_from_user(&clsfyCtl, (QosClsfyIOCtl_Ptr)arg, sizeof(clsfyCtl))){
				return -EFAULT;
			}
			clsfyCtl.clsfyNum = getMappingResultNum(clsfyCtl.portId);
			if (copy_to_user((QosClsfyIOCtl_Ptr)arg, &clsfyCtl, sizeof(clsfyCtl)) != 0){
				return -EFAULT;
			}
		break;

		case EPONMAP_IOC_GETRULE:
			if (copy_from_user(&data, (QosIOCtl_Ptr)arg, sizeof(QosIOCtl_t))){
				return -EFAULT;
			}
			if (TRUE != getMappingResult(data.portId, data.ruleIdx, &data.result, &data.matchNum, data.matchs)){
				return -EFAULT;
			}
			if (0 != copy_to_user((QosIOCtl_Ptr)arg, &data, sizeof(data))){
				return -EFAULT;
			}
		break;

		case EPONMAP_IOC_GETLLIDQ:
			if (copy_from_user(&queueMapCtl, (QosQueueMappingIOCtl_Ptr)arg, sizeof(QosQueueMappingIOCtl_t))){
				return -EFAULT;
			}
			if (TRUE != getLlidQueueMap(queueMapCtl.llid, &queueMapCtl.num, queueMapCtl.queueWts)){
				return -EFAULT;
			}
			if (0 != copy_to_user((QosQueueMappingIOCtl_Ptr)arg, &queueMapCtl, sizeof(QosQueueMappingIOCtl_t))){
				return -EFAULT;
			}
		break;

		case EPONMAP_IOC_SETLLIDQ:
			if (copy_from_user(&queueMapCtl, (QosQueueMappingIOCtl_Ptr)arg, sizeof(QosQueueMappingIOCtl_t))){
				return -EFAULT;
			}
			if (TRUE != setLlidQueueMap(queueMapCtl.llid, queueMapCtl.num, queueMapCtl.queueWts)){
				return -EFAULT;
			}
		break;

		case EPONMAP_IOC_CLEARLLIDQ:
			if (copy_from_user(&llid, (u8*)arg, sizeof(u8))){
				return -EFAULT;
			}
			if (FALSE == clearLlidQueueMap(llid)){
				return -EFAULT;
			}
		break;

		case EPONMAP_IOC_SHOWLLIDQ:
			if (copy_from_user(&llid, (u8*)arg, sizeof(u8))){
				return -EFAULT;
			}
			if (FALSE == showLlidQueueMap(llid)){
				return -EFAULT;
			}
		break;

		case EPONMAP_IOC_DBG_LVL:
			if (copy_from_user(&portId, (u8*)arg, sizeof(u8))){
				return -EFAULT;
			}
			setMappingDbgLevel(portId);
		break;

		case EPONMAP_IOC_RESETALL:
			resetEponMapping();
		break;
#endif
		default:
			return -EINVAL;
	}

	return result;
}

int xponmap_open(struct inode *inode, struct file *filp)
{
	return 0;
}



static struct file_operations xponmap_fops = {
	.owner =		THIS_MODULE,
	.write =		NULL,
	.read =		NULL,
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,35)
	.unlocked_ioctl = xponmap_ioctl,
#ifdef TCSUPPORT_CPU_ARMV8_64
	.compat_ioctl	= 	xponmap_ioctl,
#endif
#else
	.ioctl = xponmap_ioctl,
#endif
	.open =		xponmap_open,
	.release =	NULL,
};

static struct proc_dir_entry *gponmap_proc_dir = NULL;


static void init_xpon_mapping_macro_compatible(void)
{
	xpon_map_lanif_mask = lanif_mask_macro_compatible();
	xpon_map_dev_offset = dev_offset_macro_compatible();

	return ;
}

static int gpon_flow_mapping_api_dispatch(struct ecnt_data *in_data)
{
    gpon_flow_api_data_t * api_data = (gpon_flow_api_data_t *)in_data;
    switch(api_data->api_type) 
	{
	    case GPON_FLOW_API_TYPE_UPSTREAM_ANI:
			if(-1 == gpon_mapping(api_data->skb)){
				api_data->ret = GPON_FLOW_FAILURE;
			}else{
				api_data->ret = GPON_FLOW_SUCCESS;
			}
	        break;
	    case GPON_FLOW_API_TYPE_UPSTREAM_UNI:
	    case GPON_FLOW_API_TYPE_DOWNSTREAM_UNI:
	    case GPON_FLOW_API_TYPE_DOWNSTREAM_ANI:
			api_data->ret = GPON_FLOW_SUCCESS;
	        break;
	    default:
	        dump_stack();
	        printk("unknown api_data->api_type: %d\n", api_data->api_type);
	        api_data->ret = GPON_FLOW_NO_API;
			return ECNT_RETURN;
	}
    return ECNT_RETURN;
}

struct ecnt_hook_ops gpon_flow_mapping_hook_ops = {
    .name = "gpon_flow_mapping_dispatch",
    .hookfn = gpon_flow_mapping_api_dispatch,
    .is_execute = 1,
    .maintype = ECNT_GPON_FLOW,
    .subtype = ECNT_GPON_FLOW_API,
    .priority = 1,
};

EXPORT_SYMBOL(gpon_flow_mapping_hook_ops);

static int xpon_mapping_api_dispatch(struct ecnt_data *in_data){
    xpon_mapping_api_data_t * api_data = (xpon_mapping_api_data_t *)in_data;
	int uniPort = -1;
	int txq = -1;

	GPONMAP_PRINT(GPONMAP_MSG_TRACE, " cmd_id %x ",api_data->cmd_id);

    switch(api_data->cmd_id){
        case XPON_MAPPING_GET_DWONSTREAM_UNI:
			uniPort = gpon_ds_uni_mapping(api_data->ds_uni.skb);
            if(-1 == uniPort){
                api_data->ret = XPON_MAPPING_FAILURE;
            }else{
				api_data->ds_uni.uni= uniPort;
				api_data->ret = XPON_MAPPING_SUCCESS;
				//printk("XPON MAP ECNT: uni is %d\n", api_data->ds_uni.uni);
            }
            break;
		case XPON_MAPPING_GET_DWONSTREAM_UNI_EXT:
			uniPort = gpon_ds_uni_mapping_ext(api_data->ds_uni.skb);
            if(-1 == uniPort){
				api_data->ds_uni.uni= -1;
                api_data->ret = XPON_MAPPING_FAILURE;
            }else{
				api_data->ds_uni.uni= uniPort;
				api_data->ret = XPON_MAPPING_SUCCESS;
				//printk("XPON MAP ECNT: uni is %d\n", api_data->ds_uni.uni);
            }
			break;
#ifdef TCSUPPORT_GPON_DOWNSTREAM_MAPPING
		case XPON_MAPPING_GET_DWONSTREAM_TXQ:
			txq = gpon_ds_queue_mapping(api_data->ds_uni.skb, api_data->ds_uni.txQos);
				
            if(-1 == txq){
                api_data->ret = XPON_MAPPING_FAILURE;
            }else{
				api_data->ds_uni.txq= txq;
				api_data->ret = XPON_MAPPING_SUCCESS;
				//printk("XPON MAP ECNT: txq is %d\n", api_data->ds_uni.txq);
            }			
			break;
#endif
        default:
           api_data->ret = XPON_MAPPING_NO_API; 
    }
    return ECNT_RETURN;
}

struct ecnt_hook_ops xpon_mapping_hook_ops = {
    .name = "xpon_mapping_disaptch",
    .hookfn = xpon_mapping_api_dispatch,
    .is_execute = 1,
    .maintype = ECNT_XPON_MAPPING,
    .subtype = ECNT_XPON_MAPPING_API_TYPE_GET,
    .priority = 1,
};


static int xpon_eth_map_api_dispatch(struct ecnt_data *in_data)
{
	port_info_api_data_t * api_data = (port_info_api_data_t *)in_data;
	int portid = -1;
	int eth_type = -1;

	GPONMAP_PRINT(GPONMAP_MSG_TRACE, " cmd_id %x ",api_data->cmd_id);

    switch(api_data->cmd_id){
        case XPON_ETH_MAP_MARK_TO_PORT:
			portid = mark2port(api_data->skb);
            if(-1 == portid){
                api_data->ret = XPON_ETH_MAP_FAILURE;
            }else{
				api_data->portid= portid;
				api_data->ret = XPON_ETH_MAP_SUCCESS;
            }
            break;

		case XPON_ETH_MAP_NAME_TO_PORT:
			portid = name2port(api_data->dev_name);
            if(-1 == portid){
                api_data->ret = XPON_ETH_MAP_FAILURE;
            }else{
				api_data->portid= portid;
				api_data->ret = XPON_ETH_MAP_SUCCESS;
            }
            break;

		case XPON_ETH_MAP_PORT_TO_NAME:
            api_data->ret = port2name(api_data->portid, api_data->dev_name);
            break;

		case XPON_ETH_MAP_PORT_TO_ETH_TYPE:
			eth_type = port2ethtype(api_data->portid);
            if(-1 == eth_type){
                api_data->ret = XPON_ETH_MAP_FAILURE;
            }else{
				api_data->eth_type= eth_type;
				api_data->ret = XPON_ETH_MAP_SUCCESS;
            }
            break;
			
        default:
           api_data->ret = XPON_ETH_MAP_NO_API; 
    }
	return ECNT_RETURN;
}

struct ecnt_hook_ops xpon_eth_map_hook_ops = {
	.name = "xpon_eth_map",
    .hookfn = xpon_eth_map_api_dispatch,
    .is_execute = 1,
    .maintype = ECNT_GPON_FLOW,
    .subtype = ECNT_XPON_ETH_MAP,
    .priority = 1,
};


static int  xponmap_init(void)
{
	int status = 0;
	struct proc_dir_entry *gponmap_proc;
	struct proc_dir_entry *port_info_proc;
	
	init_xpon_mapping_macro_compatible();

	if (initXponMapping() != 0)
		return -1;
	
	status = register_chrdev(XPONMAP_MAJOR, XPONMAP_DEV, &xponmap_fops);
	if (status < 0){
		printk("xponmap: can't get major %d\n", XPONMAP_MAJOR);
       	return status;
    	}

	memset(&g_mappingCfgStatus, 0, sizeof(MappingCfgStatus_t) );
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)	
	init_timer(&(g_mappingCfgStatus.mappingDoneTimer));
	g_mappingCfgStatus.mappingDoneTimer.function = xpon_mapping_done;
#else
	timer_setup(&(g_mappingCfgStatus.mappingDoneTimer), xpon_mapping_done, 0); 
#endif	
	g_mappingCfgStatus.timerFlag = 1;
	g_mappingCfgStatus.mappingDoneTimer.expires = 3000;


#if defined(TCSUPPORT_GPON_MAPPING)
    gponmap_proc_dir = proc_mkdir("gponmap", NULL);
    if(NULL == gponmap_proc_dir){      
        printk("Error while creating %s directory\n", "gponmap");
        return -1;
    }   
    gponmap_proc = create_proc_entry("gponmap/gponmap_debug", 0, NULL);
	gponmap_proc->read_proc = gponmap_debug_read_proc;
	gponmap_proc->write_proc = gponmap_debug_write_proc;

    port_info_proc = create_proc_entry("gponmap/port_info", 0, NULL);
	port_info_proc->read_proc = port_info_read_proc;
	port_info_proc->write_proc = port_info_write_proc;
#if defined(TCSUPPORT_GPON_DOWNSTREAM_MAPPING)
	if(init_downstream_mapping_rule() == -1)
		return -1;
#if 1
    /* HGU need to downstream rate control function */
	rcu_assign_pointer(gpon_downstream_mapping_hook, gpon_downstream_mapping_option);
	rcu_assign_pointer(gpon_downstream_mapping_stag_hook, gpon_ds_mapping_assign_queue);
#else
	if (isSfu)
	{
		rcu_assign_pointer(gpon_downstream_mapping_hook, gpon_downstream_mapping_option);
		rcu_assign_pointer(gpon_downstream_mapping_stag_hook, gpon_ds_mapping_assign_queue);
	}
	else
	{
		rcu_assign_pointer(gpon_downstream_mapping_hook, NULL);
		rcu_assign_pointer(gpon_downstream_mapping_stag_hook, NULL);
	}
#endif	
#endif // #if defined(TCSUPPORT_GPON_DOWNSTREAM_MAPPING)
	rcu_assign_pointer(gpon_mapping_hook, gpon_mapping);
	rcu_assign_pointer(xpon_mode_get_hook, xpon_mode_get);

#ifdef TCSUPPORT_UPSTREAM_VLAN_POLICER
        rcu_assign_pointer(upstream_vlan_policer_hook, upstream_vlan_policer);
#endif

#endif // #if defined(TCSUPPORT_GPON_MAPPING)

	rcu_assign_pointer(gpon_queue_mapping_hook, gpon_queue_mapping);

#if defined(TCSUPPORT_EPON_MAPPING)
	if (isSfu)
		rcu_assign_pointer(epon_sfu_clsfy_hook, epon_sfu_clsfy);
	rcu_assign_pointer(epon_mapping_hook, epon_mapping);
#endif

	if (ECNT_REGISTER_SUCCESS != ecnt_register_hook(&gpon_flow_mapping_hook_ops) ){
        panic("Register hook function failed! %s:%d", __FUNCTION__, __LINE__);
    }

	if (ECNT_REGISTER_SUCCESS != ecnt_register_hook(&xpon_mapping_hook_ops) ){
		panic("Register hook function failed! %s:%d", __FUNCTION__, __LINE__);
	}
	
	if (ECNT_REGISTER_SUCCESS != ecnt_register_hook(&xpon_eth_map_hook_ops) ){
        panic("Register hook function failed! %s:%d", __FUNCTION__, __LINE__);
    }
	
	printk("XPON Mapping Module init OK!\n");
	return 0;
}

static void  xponmap_exit(void)
{
	exitXponMapping();
	unregister_chrdev(XPONMAP_MAJOR, "xponmap");

#if defined(TCSUPPORT_GPON_MAPPING)
	remove_proc_entry("gponmap/gponmap_debug", NULL);
	remove_proc_entry("gponmap/port_info", NULL);
    remove_proc_entry("gponmap", NULL);
#if defined(TCSUPPORT_GPON_DOWNSTREAM_MAPPING)
	clean_downstream_mapping_rule();
	rcu_assign_pointer(gpon_downstream_mapping_hook, NULL);
	rcu_assign_pointer(gpon_downstream_mapping_stag_hook, NULL);
#endif
	rcu_assign_pointer(gpon_mapping_hook, NULL);
	rcu_assign_pointer(xpon_mode_get_hook, NULL);
#ifdef TCSUPPORT_UPSTREAM_VLAN_POLICER
        rcu_assign_pointer(upstream_vlan_policer_hook, NULL);
#endif        
#endif

	rcu_assign_pointer(gpon_queue_mapping_hook, NULL);

#if defined(TCSUPPORT_EPON_MAPPING)
	if (isSfu)
		rcu_assign_pointer(epon_sfu_clsfy_hook, NULL);
	rcu_assign_pointer(epon_mapping_hook, NULL);
#endif

	ecnt_unregister_hook(&gpon_flow_mapping_hook_ops);
	ecnt_unregister_hook(&xpon_mapping_hook_ops);
	ecnt_unregister_hook(&xpon_eth_map_hook_ops);
	printk("XPON Mapping Module exit OK!\n");
}

module_init(xponmap_init);
module_exit(xponmap_exit);
