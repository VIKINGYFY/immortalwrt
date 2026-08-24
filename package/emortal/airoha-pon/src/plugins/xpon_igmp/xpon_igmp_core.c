
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
	xpon_igmp_core.c
	
	Abstract:the file implements the defined igmp function based on WT-255 and CTC Spec   

	Revision History:
	Who			When			What
	--------	----------		----------------------------------------------
	Name				Date			Modification logs
	lidong.hu		2012/7/28	Create
*/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/times.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/netfilter_bridge.h>
#include <linux/netfilter.h>
#include <linux/jhash.h>
#include <linux/random.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/igmp.h>
#include <linux/if_ether.h>
#include <linux/skbuff.h>
#include <net/ip.h>
#include <net/if_inet6.h>
#include <net/route.h>
#include <linux/if_pppox.h>
#include <linux/in.h>
#include <net/addrconf.h>
#include <linux/version.h>

#include "xpon_igmp_core.h"
#include <linux/libcompileoption.h>
#include <macro_compatible/ecnt_macro_compatible.h>
#include <ecnt_hook/ecnt_hook_pon_mac.h>
#include <ecnt_hook/ecnt_hook_pon_vlan.h>



struct net_bridge_port;
/**********************************************************/
static int dynamic_acl_ctr(int port, int vid, int gem_portid, unsigned char* dest_addr , unsigned char* src_addr);
static int xpon_sfu_send_to_router_by_flag(struct sk_buff* skb,int flag);
static int frame_is_legal_multicast_pkt(struct sk_buff* skb, int direct);
static int xpon_down_igmp_uni_vlan_filter(struct sk_buff* skb, int port);
static int xpon_down_igmp_ani_vlan_filter(struct sk_buff* skb);
int fwdtbl_exist(unsigned char* grp_addr, int proto);
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,36)
extern int ip_route_output_key(struct net *net, struct rtable **rp, struct flowi *flp);
#endif
extern int (*pon_vlan_get_mode_hook)(void);

#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_IGMP_SET_GROUP)
extern int xpon_fwdtbl_cnt(void);
#endif/*TCSUPPORT_COMPILE*/

e_multicast_debug_level g_MULTICAST_DEBUG_LEVEL = E_NO_INFO_LEVEL;

int g_care_ver_dynlist_stalist_op = true;

int g_DS_MCAST_BW_RATE_LIMIT_ENABLE = 1;

int last_query_cnt = DEFAULT_LEAVE_RETRY_CNT;

static unsigned int xpon_igmp_dev_offset = DEV_OFFSET;

#define DISCARD_LEAVE_MESSAGE -100
#define LOCAL_INTERFACE "br0"

#define INVALID_GEMP_ID   4096
#define ALU_XG_BROADCAST_GEM_ID   65533
#define ALU_XG_MULTICAST_GEM_ID   65534
/**********************************************************/
static LIST_HEAD(leave_pkt_list);

typedef struct 
{       
    struct list_head list; 
    int              port;
    int              protocol_flag;
	unsigned char    grp_addr[16];
    struct sk_buff   *skb;
    
}leave_pkt_list_t;

static void add_leave_pkt_entry(int port, unsigned char* grp_addr,int protocol_flag, struct sk_buff* skb);
static void del_leave_pkt_entry(int port, unsigned char* grp_addr,int protocol_flag);
static void xpon_send_leave(int port, unsigned char* grp_addr, int protocol_flag);

/**********************************************************/

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,4,0)
extern void br_forward(struct net_bridge_port *to, struct sk_buff *skb, bool local_rcv, bool local_orig);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36)
extern void br_forward(struct net_bridge_port *to, struct sk_buff *skb,struct sk_buff *skb0);
#else
extern void br_forward(struct net_bridge_port *to, struct sk_buff *skb);
#endif

void xpon_br_forward(struct net_device *dev, struct sk_buff *skb)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,4,0)
	if(dev->rx_handler_data)
		br_forward((struct net_bridge_port*)dev->rx_handler_data,skb,false,false);
	else
		kfree_skb(skb);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36)
	if(dev->rx_handler_data)
		br_forward((struct net_bridge_port*)dev->rx_handler_data,skb,NULL);
	else
		kfree_skb(skb);
#else
	if(dev->br_port)
	 	br_forward(dev->br_port,skb);
	else
		kfree_skb(skb);
#endif
	return;
}


void xpon_igmp_debug(int level,char* fmt,...)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	va_list args;
	char buff[128];

	if ((igmp_conf->dbglevel&level)==0)
		return;
	
	va_start(args, fmt);
	vsnprintf(buff, 128, fmt, args);
	va_end(args);

//	if (igmp_conf->flag & XPON_IGMP_DEBUG)
	printk(buff);

	return;
}

struct net_device* xpon_get_dev_by_name(char* dev_name)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36)
	return dev_get_by_name(&init_net,dev_name);
#else
	return dev_get_by_name(dev_name);
#endif
}


 struct net_device* xpon_get_dev_by_port(int port)
{
	char port_name[8] = {0};

	if(port < 0 || port >= MAX_UNI_PORT_NUM){
		return NULL;
	}

	ENCT_HOOK_XPON_ETH_MAP_PORT_TO_DEV_NAME(port, port_name);
	return xpon_get_dev_by_name(port_name);	
}

int xpon_is_multicast_addr(unsigned char* addr)
{
	char mac[3]  =  { 0x01,0x00,0x5e};
	char mac2[2] = {0x33,0x33};
    
    if(NULL == addr)
    {
        return 0;
    }
    
	if (!memcmp(addr,mac,3))
			return 1;
	if (!memcmp(addr,mac2,2))
			return 1;
	return 0;
}

void xpon_ip6_to_mac(unsigned char* addr_ip6, unsigned char *mac)
{
	if(addr_ip6 == NULL || mac==NULL)
		return;
	*mac = 0x33;
	*(mac + 1) = 0x33;
	*(mac + 2) = addr_ip6[12];
	*(mac + 3) = addr_ip6[13];
	*(mac + 4) = addr_ip6[14];
	*(mac + 5) = addr_ip6[15];
	return;
}

void xpon_ip4_to_mac(unsigned char*  addr_ip4, unsigned char *mac)
{
	if (addr_ip4 == NULL || mac == NULL)
        return;
	*mac = 0x01;
	*(mac + 1) = 0x00;
	*(mac + 2) = 0x5e;
	*(mac + 3) = addr_ip4[1];
	*(mac + 4) = addr_ip4[2];
	*(mac + 5) = addr_ip4[3];
	return;
}

int xpon_get_dest_mac(unsigned char* mac,struct sk_buff* skb)
{
	memcpy(mac,eth_hdr(skb)->h_dest,6);
	return 0;
}

int xpon_get_src_mac(unsigned char* mac,struct sk_buff* skb)
{
	memcpy(mac,eth_hdr(skb)->h_source,6);
	return 0;
}



int xpon_get_packet_type(struct sk_buff* skb)
{
	unsigned int eth_type = ntohs(eth_hdr(skb)->h_proto);
	unsigned char* buff = skb_mac_header(skb);
	buff += MAC_HEADER_LEN;
	while (eth_type == 0x8100 || eth_type == 0x88a8 || eth_type == 0x9100)
	{
		buff += IEEE8021AD_TAG_LEN;
		eth_type = ntohs(*(unsigned short int*)(buff));	
	}
	return eth_type;
}

int xpon_get_vlan_id(struct sk_buff* skb,uint16_t * vlanId, unsigned char * tagNum)
{
	unsigned int eth_type = ntohs(eth_hdr(skb)->h_proto);
	unsigned char* buff = skb_mac_header(skb);
	unsigned char tagCount = 0;

	if(skb == NULL || vlanId == NULL || tagNum == NULL)
		return -1;

	buff += MAC_HEADER_LEN;
		
	if (eth_type == 0x8100 || eth_type == 0x88a8 || eth_type == 0x9100)
	{
		do{
			vlanId[tagCount] = ntohs(*(short int*)(buff+ETHER_TYPE_LEN))&0xfff;
			tagCount ++;
			if(tagCount + 1 == MAX_VLAN_COUNT){
				break;
			}	
			buff += IEEE8021AD_TAG_LEN;
			eth_type = ntohs(*(unsigned short int*)(buff));	
		}while(eth_type == 0x8100 || eth_type == 0x88a8 || eth_type == 0x9100);
		tagCount++; // to get tagNum more intutive instead of matrix index
	}

	*tagNum = tagCount;
	return 0;
}

int xpon_get_outmost_vid(struct sk_buff* skb){
	unsigned int eth_type = ntohs(eth_hdr(skb)->h_proto);
	unsigned char* buff = skb_mac_header(skb);

	if(skb == NULL){
		return -1;
	}
	
	if (eth_type == 0x8100 || eth_type == 0x88a8 || eth_type == 0x9100){
		return ntohs(*(short int*)(buff+14))&0xfff;
	}   
	
	return -1;
}


int xpon_get_vlan_tci(struct sk_buff* skb)
{
	unsigned int eth_type = ntohs(eth_hdr(skb)->h_proto);
	unsigned char* buff = skb_mac_header(skb);
	if (eth_type == 0x8100 || eth_type == 0x88a8 || eth_type == 0x9100)
	{
		return ntohs(*(short int*)(buff+14))&0xefff;	
	}
	return -1;
}

void xpon_igmp_store_src_vlan_info(struct sk_buff* skb)
{
	unsigned int eth_type = ntohs(eth_hdr(skb)->h_proto);
	unsigned char* buff = skb_mac_header(skb);
	if (eth_type == 0x8100 || eth_type == 0x88a8)
	{
	    /* SFU mode, skb->vlan_tags is not used in other ways */
		skb->vlan_tags[0] = eth_type;
		skb->vlan_tags[1] = ntohs(*(unsigned short int*)(buff+14));	
	}
	
	return;
}

void*  xpon_get_network_header(struct sk_buff* skb)
{

	unsigned int eth_type = ntohs(eth_hdr(skb)->h_proto);
	unsigned char* buff = skb_mac_header(skb);

	buff += MAC_HEADER_LEN;
	
	while (eth_type == 0x8100 || eth_type == 0x88a8 || eth_type == 0x9100)
	{
		buff += IEEE8021AD_TAG_LEN;
		eth_type = ntohs(*(unsigned short int*)(buff));	
	}
	buff += ETHER_TYPE_LEN;
	return buff;
}



int xpon_get_ip_type(struct sk_buff* skb)
{
	unsigned int eth_type = xpon_get_packet_type(skb);
	unsigned char* buff;
	struct iphdr*  ih;
	struct ipv6hdr* i6h;
	if (eth_type==PACKET_IPV4)
	{
		ih = (struct iphdr* ) xpon_get_network_header(skb);
		return ih->protocol;
	}

	if (eth_type==PACKET_IPV6)
	{
		buff = xpon_get_network_header(skb);
		i6h = (struct ipv6hdr*)buff;
		return (i6h->nexthdr==0) ? buff[40]:i6h->nexthdr;
	}

	return 0;
}

#define PPP_IP		0x21	/* Internet Protocol */
#define PPP_IPV6	0x57	/* Internet Protocol Version 6 */

int xpon_get_src_addr(unsigned char* src,struct sk_buff* skb)
{

	unsigned int eth_type = xpon_get_packet_type(skb);
	struct iphdr*  ih;
	struct ipv6hdr* i6h;
	unsigned char *pppoe_h;

	if (eth_type==PACKET_IPV4)
	{
		ih = (struct iphdr* ) xpon_get_network_header(skb);
		memcpy(src,(unsigned char*)&ih->saddr,4);
		return 1;
	}

	if (eth_type==PACKET_IPV6)
	{
		i6h = (struct ipv6hdr*) xpon_get_network_header(skb);
		memcpy(src,i6h->saddr.s6_addr,16);
		return 1;
	}
	if(eth_type==ETH_P_PPP_SES){
		__be16 pppoe_proto;

		pppoe_h = (unsigned char *)xpon_get_network_header(skb);

		pppoe_h += sizeof(struct pppoe_hdr); //skip pppoe header
		pppoe_proto = ntohs(*(__be16*)pppoe_h);
		pppoe_h += 2; // skip ppp header
		if (pppoe_proto == PPP_IP){
			ih = (struct iphdr* ) (pppoe_h);
			memcpy(src,(unsigned char*)&ih->saddr,4);
			return 1;
		}
		else if (pppoe_proto == PPP_IPV6){
			i6h = (struct ipv6hdr*) (pppoe_h);
			memcpy(src,i6h->saddr.s6_addr,16);
			return 1;
		}
	}
	return 0;
}

int xpon_get_dest_addr(unsigned char* src,struct sk_buff* skb)
{
	unsigned int eth_type = xpon_get_packet_type(skb);
	struct iphdr*  ih;
	struct ipv6hdr* i6h;
	unsigned char *pppoe_h;

	if (eth_type==PACKET_IPV4)
	{
		ih = (struct iphdr* ) xpon_get_network_header(skb);
		memcpy(src,(unsigned char*)&ih->daddr,4);
		return 1;
	}

	if (eth_type==PACKET_IPV6)
	{
		i6h =  (struct ipv6hdr*) xpon_get_network_header(skb);
		memcpy(src, i6h->daddr.s6_addr,16);
		return 1;
	}
	if(eth_type==ETH_P_PPP_SES){
		__be16 pppoe_proto;
		
		pppoe_h = (unsigned char *)xpon_get_network_header(skb);

		pppoe_h += sizeof(struct pppoe_hdr); //skip pppoe header
		pppoe_proto = ntohs(*(__be16*)pppoe_h);
		pppoe_h += 2; // skip ppp header
		if (pppoe_proto == PPP_IP){
			ih = (struct iphdr* ) (pppoe_h);
			memcpy(src,(unsigned char*)&ih->daddr,4);
			return 1;
		}
		else if (pppoe_proto == PPP_IPV6){
			i6h = (struct ipv6hdr*) (pppoe_h);
			memcpy(src, i6h->daddr.s6_addr,16);
			return 1;
		}
	}
	return 0;
}

void* xpon_get_transport_header(struct sk_buff* skb)
{
	unsigned int eth_type = xpon_get_packet_type(skb);
	struct iphdr*  ih;
	struct ipv6hdr* i6h;
	unsigned char* buff = (unsigned char*) xpon_get_network_header(skb);
	
	if (eth_type==PACKET_IPV4)
	{
		ih = (struct iphdr*)buff;
		return buff + (ih->ihl*4);
	}
	if (eth_type==PACKET_IPV6)
	{
		i6h = (struct ipv6hdr*)buff;
		if (i6h->nexthdr==0)
			return buff+48;
		return buff + 40;
	}
	return NULL;
}




int xpon_get_igmp_grpaddr(unsigned char* grp_addr,struct sk_buff* skb)
{
	int type = xpon_get_ip_type(skb);
	unsigned char* buff = xpon_get_transport_header(skb);

	if (type == PROTOCOL_IGMP)
	{
		if(NULL != buff)
		{	
			if((*buff) == IGMPV3_HOST_MEMBERSHIP_REPORT)		
			{	
				MULTICAST_TRACE_INFO("current igmp version is IGMPv3\n");
				memcpy(grp_addr,buff+12,4);		
			}
			else
			{		
				memcpy(grp_addr,buff+4,4);
			}
		}
	}
	
	if (type == PROTOCOL_ICMPV6)
	{
		if(NULL != buff)
			memcpy(grp_addr,buff+8,16);
	}
	return 0;
}

int xpon_get_igmp_srcip(unsigned char* clientIp,struct sk_buff* skb)
{
	int type = xpon_get_ip_type(skb);
	unsigned char* buff = (unsigned char*) xpon_get_network_header(skb);

	if (type == PROTOCOL_IGMP)
	{
		memcpy(clientIp,buff+12,4);
	}
	if (type == PROTOCOL_ICMPV6)
	{
		memcpy(clientIp,buff+8,16);
	}

	return 0;
}


int xpon_get_igmp_type(struct sk_buff* skb)
{
	int type = xpon_get_ip_type(skb);
	unsigned char* buff = xpon_get_transport_header(skb);
	
	if( buff == NULL )
	{
		return 0;
	}

	if (type == PROTOCOL_IGMP)
	{
		return buff[0];
	}
	if (type == PROTOCOL_ICMPV6)
	{
		return buff[0];	
	}
	return 0;
}

int xpon_is_igmp_pkt(struct sk_buff* skb)
{
	int type = xpon_get_ip_type(skb);
	int igmp_type = xpon_get_igmp_type(skb);

	if (type == PROTOCOL_IGMP)
		return 1;
	
	if (type == PROTOCOL_ICMPV6)
	{
		if (igmp_type==ICMPV6_MGM_REPORT)
			return 1;
		else if (igmp_type==ICMPV6_MLD2_REPORT)
			return 1;
		else if (igmp_type==ICMPV6_MGM_QUERY)
			return 1;
		else if (igmp_type==ICMPV6_MGM_REDUCTION)
			return 1;
		return 0;
	}
	
	return 0;
}

int xpon_is_data_pkt(struct sk_buff* skb)
{
	int type = xpon_get_ip_type(skb);
	
	if (type==PROTOCOL_UDP || type==PROTOCOL_UDP6)
		return 1;

	return 0;
}

int xpon_get_downstream_grpaddr(unsigned char* grp_addr,struct sk_buff* skb)
{
	if(xpon_is_igmp_pkt(skb))
		return xpon_get_igmp_grpaddr(grp_addr,skb);
	else if(xpon_is_data_pkt(skb))
		return xpon_get_dest_addr(grp_addr,skb);

	return 0;
}

int xpon_get_ipv6_type(struct sk_buff* skb)
{
	return xpon_get_ip_type(skb);
}

int xpon_get_mld_type(struct sk_buff* skb)
{
	return xpon_get_igmp_type(skb);
}


void xpon_dump(void* ptr,int len)
{
	unsigned char* str = (unsigned  char*) ptr;
	int i;
	
	printk("\n Dump ptr = %p, len=%d \n",ptr,len);
	for(i=0;i<len;i++)
	{
		printk("%02x ",str[i]);
		if ((i&0x0f)==0x0f)
			printk("\n");
	}
	return ;
}

int xpon_upstream_vlan_handle(struct sk_buff* skb , int port)
{
    xPON_PortConf_t* port_conf = NULL;
    int uptag = 0;
	int uptci = 0;
	short * pvid = NULL;
	int vid = 0;
	short  type = 0;
	unsigned  char * buff = NULL;
	short vid_tmp = 0;

	port_conf = xpon_port_conf_by_id(port);
	uptag = port_conf->up_vlan_mode;
	uptci = port_conf->up_vlan_tci;
	vid = xpon_get_outmost_vid(skb);
	buff = skb_mac_header(skb);
    
    MULTICAST_CRITIC_INFO("uptag = %d, uptci = %d.\n", uptag, uptci);
    
    #if 0
    MULTICAST_CRITIC_INFO("*************************************\n");
	xpon_dump(eth_hdr(skb),32);
    MULTICAST_CRITIC_INFO("*************************************\n");
    #endif

	switch (uptag)
	{
		case MULTCAST_UPTAG_TRANSPARENT:   //transparent
			break;
		case MULTCAST_UPTAG_ADD:   //add 
			type = eth_hdr(skb)->h_proto;
			skb_push(skb,4);
			memmove(skb->data,buff,14);
			skb->mac_header -= 4;
			eth_hdr(skb)->h_proto  = htons(ETH_P_8021Q);
			skb->protocol = ETH_P_8021Q;
			*(short *)(buff + 10) = htons((uptci & 0xefff));
			*(short *)(buff+12) = type;
			break;
		case MULTCAST_UPTAG_REPLACE_VID_PID:  //replace VID+P
		case MULTCAST_UPTAG_REPLACE_VID:  //replace VID
			if (vid < 0)
			{
				type = eth_hdr(skb)->h_proto;
				skb_push(skb,4);
				memmove(skb->data,buff,14);
				skb->mac_header -= 4;
				eth_hdr(skb)->h_proto = htons(ETH_P_8021Q);
				skb->protocol = ETH_P_8021Q;
				*(short *)(buff+12) = type;
			}
			if (vid < 0)
				pvid = (short int*)(buff + 10);
			else 
				pvid = (short int*)(buff + 14);
			
			if (uptag == MULTCAST_UPTAG_REPLACE_VID_PID)
			{
				if (vid < 0)
				{
					*pvid = htons(uptci & 0xefff) ;
				}
				else
				{
				    vid_tmp = ntohs(*pvid) & 0x1000;
				    vid_tmp = vid_tmp | (uptci & 0xefff);
				    *pvid = htons(vid_tmp);
				}
			}

			if (uptag == MULTCAST_UPTAG_REPLACE_VID)
			{
				if (vid >= 0)
				{
				    vid_tmp = ntohs(*pvid) & 0xf000;
				    vid_tmp = vid_tmp | (uptci & 0xfff);
				    *pvid = htons(vid_tmp);
				}
				else
				{
					*pvid = htons(uptci & 0xffff);
				}
			}
			break;
		default:
			break;
	}

    #if 0 
    MULTICAST_CRITIC_INFO("*************************************\n");
    xpon_dump(eth_hdr(skb),32);
    MULTICAST_CRITIC_INFO("*************************************\n");
    #endif
    
	return 0;
}

int xpon_get_trans_vid(int port,int vid)
{
	xPON_PortVLan_t* port_vlan = xpon_port_vlan_by_id(port);
	xPON_PortConf_t* port_conf = xpon_port_conf_by_id(port);
	int i;

#ifdef TCSUPPORT_XPON_IGMP_CTC
	if(NULL == port_vlan){
		return -1;
	}
	for(i=0;i<XPON_PORT_VLAN_CNT;i++)
	{
		if (port_vlan->vlan_id[i] == vid )
		{
			return port_vlan->vlan_trans[i];
		}
	}
#endif	
	if(NULL == port_conf){
		return -1;
	}
	return port_conf->down_vlan_tci;
}

/*return value: In G.988 9.3.27, if Downstream IGMP and multicast  TCI first byte is non-zero,
extend VLAN tagging operation ME is ignored, so return 1.*/
int xpon_downstream_vlan_handle(struct sk_buff* skb,int port)
{
	xPON_PortConf_t* port_conf =  xpon_port_conf_by_id(port);
	int tag = 0;
	int tci = 0;
	int vid = 0;
	short int *pvid = NULL;
	short type = 0;
	short vid_tmp = 0;
	unsigned  char * buff = skb_mac_header(skb);

	if(NULL == port_conf){
		return -1;
	}
	vid = xpon_get_outmost_vid(skb);
	tci = xpon_get_trans_vid(port,vid);
	tag = port_conf->down_vlan_mode;
	//xpon_dump(eth_hdr(skb),32);
	switch (tag)
	{
		case 0:   //transparent
			return 0;
		case 1:   //strip
			if (vid >= 0)
			{
				type = *(short int*)(buff+16);
				memmove(buff+4,buff,14);
				skb_pull(skb,4);
				skb->mac_header += 4;
				eth_hdr(skb)->h_proto = type;
				skb->protocol = ntohs(type);
			}
			break;
		case 2:   //add 
			type = eth_hdr(skb)->h_proto;
			skb_push(skb,4);
			memmove(buff,buff+4,14);
			skb->mac_header -= 4;
			eth_hdr(skb)->h_proto = htons(ETH_P_8021Q);
			skb->protocol = ETH_P_8021Q;
			*(short int*)(buff + 10)= htons(tci & 0xefff);
			*(short int*)(buff+12) = type;
			break;
		case 3:   //replace VID + P +DEL
			if (vid >= 0)
			{
				pvid = (short int*) (buff + 14);
				*pvid = htons(tci & 0xffff);
			}
			break;
		case 4:   //replace VID
			if (vid >= 0)
			{
				pvid = (short int*) (buff + 14);
				vid_tmp = ntohs(*pvid) & 0xf000;
				vid_tmp = vid_tmp | (tci & 0xfff);
				*pvid = htons(vid_tmp);
			}
			break;

		default:
			break;
			
	}
	
	//xpon_dump(eth_hdr(skb),32);

	return 1;
}

int xpon_pass_port_vlan(int port,int vid)
{
	xPON_PortVLan_t* port_vlan = xpon_port_vlan_by_id(port);
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	struct list_head*  fwd_list = xpon_get_forward_list(); 
	xPON_FwdEntry_t* entry = NULL;
	int i;

	if(port_vlan == NULL || igmp_conf == NULL)
	    return 1;
	
	if (!(port_vlan->vlan_flag == 0 
	    && igmp_conf->xpon_mode == 2 
	    && igmp_conf->onu_type == 1) ) //flag = 0 check the multicast vlan, flag = 1 do not check   //only check epon sfu mode
		return 1;

	for(i=0;i<port_vlan->vlan_num;i++)
	{
		if (vid == port_vlan->vlan_id[i])
			return 1;
	}

    	
    /*if onu is snooping, discard fwdtbl. 0:snooping, 1:control*/
    if(xpon_igmp_get_fwdmode()==0)
    {
        /*judge fwd table regular, if exist, pass*/
        if (list_empty(fwd_list))
            return 0;

        rcu_read_lock();
        list_for_each_entry_rcu(entry,fwd_list,list)
        {
            if((entry->port==port)&&(entry->vid==vid)&&(entry->type==MULTCASTCTL_MAC_DA_VLAN)){
                rcu_read_unlock();
                return 1;
            }
        }
        rcu_read_unlock();
    }
	
	return 0;
}


//Func: forward multicast data flow to LAN ports based on forwarding table
int xpon_forward_to_port(struct sk_buff* skb,int port)
{
	struct net_device* dev = NULL;
	char name[8] = {0};
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
    int iret = 0;
	int vid = 0;
    e_vlan_operation_point_t vlan_point = 0;
    
	
    ENCT_HOOK_XPON_ETH_MAP_PORT_TO_DEV_NAME(port, name);

	MULTICAST_NOTICE_INFO("xpon_forward_to_port: port = %d, dev name=%s.\n", port, name);
	
    vid = xpon_get_outmost_vid(skb);
    
	if ((xpon_pass_port_vlan(port,vid)==0) && (!xpon_multi_should_passthrough(skb)))
		goto skb_drop;

	dev =  xpon_get_dev_by_name(name);
	if(dev == NULL)
		goto skb_drop;

	if(TCSUPPORT_XPON_IGMP_CHT_VAL&&(-1 == xpon_sfu_down_vlan_access_control(skb,port)))
		goto skb_drop;
  
    /***************down igmp ANI  filter already operation*****************/
    if(1 == igmp_conf->xpon_mode)
    {
        skb->pon_vlan_flag |= PON_MULTICAST_ANI_FILTER_FLAG;
    }
    /****************down igmp vlan operation**************************/
    
	xpon_downstream_vlan_handle(skb,port);

    /*down vlan fliter and operation no excete in pon vlan,  PON_PKT_INSERT_FLAG */
    /*so we sholud do vlan filter in known lan port*/
    iret = xpon_get_down_vlan_operation_point(port, &vlan_point);
    if(0 == iret && vlan_operation_in_multicast_module == vlan_point)
    {
        /****************down igmp uni filter*******************************/
        skb->pon_vlan_flag |=  PON_PKT_INSERT_FLAG;
        
        /*epon 2, gpon=1*/
        if(1 == igmp_conf->xpon_mode)
        {
            iret = xpon_down_igmp_uni_vlan_filter(skb, port);
            if(-1 == iret)
            {
                goto skb_drop;
            }
			skb->pon_vlan_flag |=  PON_PKT_INSERT_FLAG;
        }
    }
    
    /*epon 2, gpon=1*/
    if(2 == igmp_conf->xpon_mode)
    {
        skb->pon_vlan_flag |=  PON_PKT_INSERT_FLAG;
    }


	xpon_br_forward(dev,skb);
	dev_put(dev);

	MULTICAST_NOTICE_INFO("forward done\n");

	return 1;

skb_drop:
	if(dev!=NULL)
		dev_put(dev);
	kfree_skb(skb);
    MULTICAST_NOTICE_INFO("xpon_forward_to_port: free skb.\n");
	return 0;
}

//Func:Specially used to broadcast IGMP query mesage to all LAN ports
int xpon_broadcast_to_port(struct sk_buff* skb)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	xPON_PortEntry_t* entry;
	struct sk_buff* skb2;
	int i;

	for(i=1;i<igmp_conf->uni_num;i++)
	{
		 entry = &(igmp_conf->uni_port[i]);
		if (!(entry->port_flag & XPON_BRIDGE_PORT))
			continue;
		
		skb2 = skb_copy(skb,GFP_ATOMIC);
		if (skb2==NULL)
		{
			skb->dev->stats.tx_dropped++;
			continue;
		}
		xpon_forward_to_port(skb2,i);
	}
    
	kfree_skb(skb);
	return 0;
}


int xpon_get_play_group_number(int port,int proto)
{
	struct list_head* fwd_list = xpon_get_forward_list();
	xPON_FwdEntry_t* entry;
	int group_num = 0;

    
	list_for_each_entry_rcu(entry,fwd_list,list)
	{
		if (entry->port == port)
        {/* 为247要protocol */
        	//if(TCSUPPORT_BBF_247_VAL || ((entry->flag&0xff) == proto))
        	if((entry->flag&0xff) == XPON_MASK_IGMPV3){
				if((entry->flag&0xff00) == XPON_MODE_INCLUDE){
				group_num++;
        		}
				//different src-ip with same group-ip should count as 1, need optimize here
			}
			else if((entry->flag&0xff) == XPON_MASK_IGMPV2){
				group_num++;
			}
			else if((entry->flag&0xff) == XPON_MASK_IGMPV1){
				group_num++;
			}
			else{
				//MLD v1 v2 ?
			}
        }
	}
	MULTICAST_TRACE_INFO("port=%d, proto = %d, group_num=%d\n",port,proto,group_num);

	return group_num;
}

/*time 1 * 100 = 1 s*/
#define XPON_LASTQUERY_INTERVAL 1000
unsigned long xpon_get_lastquery_interval(int port)
{
	xPON_PortConf_t* port_conf = xpon_port_conf_by_id(port);
	unsigned long interval;

	if(NULL == port_conf){
		return XPON_LASTQUERY_INTERVAL;
	}
	
    if(port_conf->lastinterval > 0)
    {	
        interval = round_jiffies(jiffies) + port_conf->lastinterval * HZ;
    }
    else
    {
	    interval = round_jiffies(jiffies) + XPON_LASTQUERY_INTERVAL;
    }

    /*think overflow*/
	if (interval > 0)
		return interval;
	
	return XPON_LASTQUERY_INTERVAL;
}


#define XPON_IGMP_SIZE (sizeof(struct igmphdr)+sizeof(struct iphdr)+4)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,90)
static int dev_queue_xmit_nf_hook(struct net *pNet, struct sock * pSock, struct sk_buff *skb)
{
    return dev_queue_xmit(skb);
}
#endif

int xpon_send_igmp_query(int port,unsigned char* group)
{
	struct sk_buff *skb;
	struct iphdr *iph;
	struct igmphdr *ih;
	struct ethhdr *eth;
	struct net_device *dev;
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,36)
    struct net_device *net_dev = NULL;
#endif
	int	dst;
	unsigned char dst_addr[ETH_ALEN] = {0};

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,36)
    struct rtable *rt = NULL;
    struct flowi fl = { .oif = net_dev->ifindex,
        .nl_u = { .ip4_u = {
        .daddr = IGMP_ALL_HOSTS } },
        .proto = IPPROTO_IGMP };
#else
/*
    struct flowi4 fl4= {
        //.flowic_oif = net_dev->ifindex,
        .daddr = IGMP_ALL_HOSTS
        //.flowi4_proto = IPPROTO_IGMP
    };
*/
#endif

	dst = (*(int*)group);

	MULTICAST_NOTICE_INFO("xpon_send_igmp_query. \n");

	dev = xpon_get_dev_by_port(port);
	if(dev == NULL)
	{
		MULTICAST_ERROR_INFO("dev is NULL. \n");
		return 0;
	}

	skb=alloc_skb(XPON_IGMP_SIZE+LL_RESERVED_SPACE(dev), GFP_ATOMIC);
	if (skb == NULL) 
		return 0;
	
	skb->dev = dev ; 
	skb->protocol = htons(ETH_P_IP);

	skb_reserve(skb, LL_RESERVED_SPACE(dev));

	skb_reset_network_header(skb);
	iph = ip_hdr(skb);
	skb_put(skb, sizeof(struct iphdr)+4);

	iph->version  = 4;
	iph->ihl      = (sizeof(struct iphdr)+4)>>2;
	iph->tos      = 0;
	iph->frag_off = htons(IP_DF);
	iph->ttl      = 1;
	iph->daddr    = dst;
    
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,36)
    net_dev = xpon_get_dev_by_name(LOCAL_INTERFACE);
    if (NULL == net_dev)
    {
        kfree_skb(skb);
        return 0;
    }

    if (ip_route_output_key(dev_net(net_dev), &rt, &fl)) 
    {
		dev_put(net_dev);
        kfree_skb(skb);
        return 0;
    }
	iph->saddr = rt->rt_src;
#else
	/*do nothing*/
/*
	 if (ip_route_output_key(dev_net(net_dev),&fl4)){
			dev_put(net_dev);
        	kfree_skb(skb);
        	return 0;
    	}
*/
#endif
	iph->protocol = PROTOCOL_IGMP;
	iph->tot_len  = htons(XPON_IGMP_SIZE);
	((u8*)&iph[1])[0] = 0x94;
	((u8*)&iph[1])[1] = 0x04;
	((u8*)&iph[1])[2] = 0;
	((u8*)&iph[1])[3] = 0;
	ip_send_check(iph);
    
	ih = (struct igmphdr *)skb_put(skb, sizeof(struct igmphdr));
	ih->type=IGMP_HOST_MEMBERSHIP_QUERY;
	ih->code=10;    /*default value define in RFC 2236*/
	ih->csum=0;
	ih->group=dst;
	ih->csum=ip_compute_csum((void *)ih, sizeof(struct igmphdr));

	ip_eth_mc_map(dst, dst_addr);
	skb_push(skb, ETH_HLEN);
	skb_reset_mac_header(skb);
	eth = eth_hdr(skb);
	eth->h_proto = htons(ETH_P_IP);
	memcpy(eth->h_dest, dst_addr, ETH_ALEN);
	memcpy(eth->h_source, dev->dev_addr, ETH_ALEN);
	dev_put(dev); 

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,4,90)
	NF_HOOK(PF_BRIDGE, NF_BR_LOCAL_OUT, skb, NULL, skb->dev,&dev_queue_xmit);
#else
	NF_HOOK(PF_BRIDGE, NF_BR_LOCAL_OUT, dev_net(skb->dev), NULL, skb, NULL, skb->dev,&dev_queue_xmit_nf_hook);
#endif

	return 0;
}

typedef struct xPON_MLDQuery_s
{
	unsigned char type;
	unsigned char code;
	short int csum;
	short int mrc;
	short int resv1;
	unsigned char mca[16];
}xPON_MLDQuery_t;
#define MLD_SIZE (sizeof(struct ipv6hdr) + 8 + sizeof(struct xPON_MLDQuery_s))

int xpon_send_mld_query(int port,unsigned char* group)
{
	struct sk_buff *skb = NULL;
	struct ipv6hdr *iph = NULL;
	struct xPON_MLDQuery_s  *ih = NULL;
	struct ethhdr *eth = NULL;
	struct net_device *dev = NULL;
	xPON_PortConf_t * portConf = NULL;
	unsigned char dst_addr[ETH_ALEN] = {0};

	MULTICAST_NOTICE_INFO("xpon_send_mld_query.\n");

	dev = xpon_get_dev_by_port(port);
	if( NULL == dev )
	{
		MULTICAST_NOTICE_INFO("dev is NULL.\n");
		return 0;
	}
	
	skb=alloc_skb(MLD_SIZE + LL_RESERVED_SPACE(dev), GFP_ATOMIC);
	if (skb == NULL) 
		return 0;

	skb->dev = dev ; 
	skb->protocol = htons(ETH_P_IPV6);

	skb_reserve(skb, LL_RESERVED_SPACE(dev));

	skb_reset_network_header(skb);
	iph = ipv6_hdr(skb);
	skb_put(skb, sizeof(struct ipv6hdr) + 8);

	iph->version  = 6;
	iph->payload_len = sizeof(struct xPON_MLDQuery_s) + 8;
	iph->hop_limit = 1;
	iph->nexthdr = 0;
	memset(iph->saddr.s6_addr,0,16);
	memcpy(iph->daddr.s6_addr,group,16);
	((u8*)&iph[1])[0] = 0x3a;
	((u8*)&iph[1])[1] = 0x00;
	((u8*)&iph[1])[2] = 0x05;
	((u8*)&iph[1])[3] = 0x02;
	((u8*)&iph[1])[4] = 0;
	((u8*)&iph[1])[5] = 0;
	((u8*)&iph[1])[6] = 0x01;
	((u8*)&iph[1])[7] = 0;

	ih = (xPON_MLDQuery_t *)skb_put(skb, sizeof(xPON_MLDQuery_t));
	ih->type = ICMPV6_MGM_QUERY;
	ih->code = 0;
	ih->csum = 0;
	portConf = xpon_port_conf_by_id(port);
	if(NULL !=portConf){
		ih->mrc = htons(portConf->maxresp);
	}
	ih->resv1 = 0;
	memcpy(ih->mca,group,16);
	ih->csum = ip_compute_csum((void *)ih, sizeof(xPON_MLDQuery_t));

	ipv6_eth_mc_map(&iph->daddr,dst_addr);
	skb_push(skb, ETH_HLEN);
	skb_reset_mac_header(skb);
	eth = eth_hdr(skb);
	eth->h_proto = htons(ETH_P_IPV6);
	memcpy(eth->h_dest, dst_addr, ETH_ALEN);
	memcpy(eth->h_source, dev->dev_addr, ETH_ALEN);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,4,90)
	NF_HOOK(PF_BRIDGE, NF_BR_LOCAL_OUT, skb, NULL, skb->dev,&dev_queue_xmit);
#else
	NF_HOOK(PF_BRIDGE, NF_BR_LOCAL_OUT, dev_net(skb->dev), NULL, skb, NULL, skb->dev,&dev_queue_xmit_nf_hook);
#endif

	return 0;

}

bool fwdtbl_entry_exist(xPON_FwdEntry_t* entry){
	
	xPON_FwdEntry_t* fwd_entry = NULL;
	struct list_head* fwd_list = NULL; 
    int found_entry = 0;
    fwd_list = xpon_get_forward_list();
    rcu_read_lock();
    list_for_each_entry_rcu(fwd_entry, fwd_list,list)
    {
        if(fwd_entry == entry)
        {
            found_entry = 1;
            break;
        }
    }
    rcu_read_unlock();
    if(1 != found_entry)
    {
        return FALSE;
    }
	else
	{
		return TRUE;
	}
}
void xpon_igmp_timer_timeout(TIMER_FUN_PAAM arg)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	xPON_FwdEntry_t* entry = (xPON_FwdEntry_t* )arg;
#else
	xPON_FwdEntry_t* entry = from_timer(entry,arg,leave_ageing_timer);
#endif
	unsigned long lastquery_time;
	xPON_PortConf_t* port_conf = NULL;
	
	MULTICAST_NOTICE_INFO("xpon_igmp_timer_timeout.\n");
    if(NULL == entry)
    {
        return ;
    }
    else if(!fwdtbl_entry_exist(entry))
    {
        return;
    }

	port_conf = xpon_port_conf_by_id(entry->port);
	
	if( (entry->flag&0xff) == XPON_MASK_IGMPV3)
	{
		if(port_conf == NULL)
		{
			MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
		}else if ((! port_conf->fastleave) && (fwdtbl_entry_exist(entry)))
		{
			del_timer(&entry->leave_ageing_timer);
			xpon_fwdtbl_operate_entry(2,entry);
		}
	}
	else
	{
		entry->leave_count--;
		if (entry->leave_count <= 0)
		{	
            xpon_send_leave(entry->port, entry->grp_addr, XPON_MASK_IGMPV2);

			if(port_conf == NULL)
		    {
		        MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
		    }else if ((! port_conf->fastleave)
				&& (fwdtbl_entry_exist(entry)))
            {	
				del_timer(&entry->leave_ageing_timer);
				xpon_fwdtbl_operate_entry(2,entry);
            }
		}
		else 
		{
	 /*according to CHT's requirement,when in igmp snooping onu do not send query*/
            if(TCSUPPORT_XPON_IGMP_CHT_VAL== 0)
            {	
                xpon_send_igmp_query(entry->port,entry->grp_addr);
            }
			lastquery_time =  xpon_get_lastquery_interval(entry->port);
            /****lastquery_time already * HZ****/
            MULTICAST_NOTICE_INFO("last member query interval time = %lu second.\n", (lastquery_time-round_jiffies(jiffies)) / HZ);
			mod_timer(&entry->leave_ageing_timer,lastquery_time);
		}
	}
	return;
}

void xpon_mld_timer_timeout(TIMER_FUN_PAAM arg)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	xPON_FwdEntry_t* entry = (xPON_FwdEntry_t* )arg;
#else
	xPON_FwdEntry_t* entry = from_timer(entry,arg,leave_ageing_timer);
#endif

	unsigned long lastquery_time;
	
	MULTICAST_NOTICE_INFO("xpon_mld_timer_timeout.\n");
    if(NULL == entry)
    {
        return ;
    }
    else if(!fwdtbl_entry_exist(entry))
    {
        return;
    }

	if( (entry->flag&0xff) == XPON_MASK_MLDV2)
	{
		del_timer(&entry->leave_ageing_timer);
		xpon_fwdtbl_operate_entry(2,entry);
	}
	else
	{
		entry->leave_count--;
		if (entry->leave_count <= 0)
		{
            xpon_send_leave(entry->port, entry->grp_addr, XPON_MASK_MLDV1);
			del_timer(&entry->leave_ageing_timer);
			del_timer(&entry->preview_timer);
			xpon_fwdtbl_operate_entry(2,entry);
		}
		else 
		{
			xpon_send_mld_query(entry->port,entry->grp_addr);
			lastquery_time =  xpon_get_lastquery_interval(entry->port);
			mod_timer(&entry->leave_ageing_timer,lastquery_time);
		}
	}
	return;
}

static void xpon_preview_timer_timeout(TIMER_FUN_PAAM arg)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	xPON_FwdEntry_t* entry = (xPON_FwdEntry_t* )arg;
#else
	xPON_FwdEntry_t* entry = from_timer(entry,arg,preview_timer);
#endif
    
	MULTICAST_NOTICE_INFO("xpon_preview_timer_timeout.\n");
    if(NULL == entry)
    {
        return ;
    }
	
	del_timer(&entry->leave_ageing_timer);
	xpon_fwdtbl_operate_entry(2,entry);

	return;
}

static void xpon_pre_interval_timer_timeout(TIMER_FUN_PAAM arg)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	xPON_WhiteList_Entry_List_t* entry = (xPON_WhiteList_Entry_List_t* )arg;

    if(NULL == entry)
    {
        return ;
    }

	entry->preview_info.rep_interval_flag = FALSE;
	//del_timer(&entry->preview_info.rep_interval_timer);
#else
	xPON_DynPreview_t* entry = from_timer(entry,arg,rep_interval_timer);

	if(NULL == entry)
    {
        return ;
    }
	entry->rep_interval_flag = FALSE;

#endif

    

	return;
}

#define XPON_LISTENER_INTERVAL 1000

unsigned long xpon_get_listenner_interval(int port)
{
	xPON_PortConf_t* port_conf = xpon_port_conf_by_id(port);
	unsigned long interval;

    if(port_conf == NULL){
        return XPON_LISTENER_INTERVAL;
    }

	interval = round_jiffies(jiffies) + (port_conf->robust * port_conf->queryinterval + port_conf->maxresp + random32()%200) * HZ;
	
	MULTICAST_CRITIC_INFO("%s interval is %lu default is %d\n",__FUNCTION__,interval,XPON_LISTENER_INTERVAL);

	if (interval > 0)
		return interval;

	return XPON_LISTENER_INTERVAL;
}


int check_max_group(struct sk_buff* skb, int port, int proto)
{
    int play_num;
    xPON_PortConf_t* port_conf = NULL;
    
    port_conf = xpon_port_conf_by_id(port);
	if(port_conf == NULL)
	{
	    MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
	    return -1;
	}
	
    play_num =	xpon_get_play_group_number(port,proto);
	if ((port_conf->maxgroup > 0) && (port_conf->maxgroup <= play_num))
    {
    	printk("fwd entry %d has exceed max num %d",play_num,port_conf->maxgroup);
        return -1;
    }

    return 0;
}

int upstream_dynamic_acl_ctr_igmpv2(int port,unsigned char* dest_addr, xPON_WhiteList_Entry_List_t** dyn_entry_ptr)
{
    xPON_PortConf_t* port_conf = NULL;
    struct list_head*  dyn_list = NULL;
	xPON_WhiteList_Entry_List_t* entry = NULL;
    
    port_conf = xpon_port_conf_by_id(port);
    
    if((NULL == port_conf) || (NULL == dest_addr))
    {
		MULTICAST_CRITIC_INFO("igmpv2 return MATCH fail\n");
        return NOT_MATCH_DYNAMIC_ACL;
    }
    dyn_list = &(port_conf->dyn_list);

    //unconfig dynamic acl, forward or drop by multicast forward table
    if(list_empty(dyn_list))
    {
    	*dyn_entry_ptr = NULL;
		MULTICAST_NOTICE_INFO("igmpv2 return MATCH Success\n");
        return MATCH_DYNAMIC_ACL;
    }

	list_for_each_entry(entry,dyn_list,list)
	{
        if (xpon_grp_addr_between(dest_addr, entry->grpstart, entry->grpend)==0)
        {
            continue;
        }
        else
        {
        	MULTICAST_NOTICE_INFO("igmpv2 return MATCH Success\n");
			*dyn_entry_ptr = entry;
            return MATCH_DYNAMIC_ACL;        	
        }
	}
    MULTICAST_CRITIC_INFO("igmpv2 return MATCH fail\n");
    return NOT_MATCH_DYNAMIC_ACL;	
}

int upstream_dynamic_acl_ctr_igmpv3(int port,unsigned char* dest_addr,unsigned char* src_addr, xPON_WhiteList_Entry_List_t** dyn_entry_ptr)
{    
    xPON_PortConf_t* port_conf = NULL;
    struct list_head*  dyn_list = NULL;
	xPON_WhiteList_Entry_List_t* entry = NULL;
    
    port_conf = xpon_port_conf_by_id(port);
    
    if((NULL == port_conf) || (NULL == dest_addr))
    {
		MULTICAST_CRITIC_INFO("igmpv3 return MATCH fail\n");
        return NOT_MATCH_DYNAMIC_ACL;
    }
    dyn_list = &(port_conf->dyn_list);

    //unconfig dynamic acl, forward or drop by multicast forward table
    if(list_empty(dyn_list))
    {
    	*dyn_entry_ptr = NULL;
		MULTICAST_NOTICE_INFO("igmpv3 return MATCH Success\n");
        return MATCH_DYNAMIC_ACL;
    }

	list_for_each_entry(entry,dyn_list,list)
	{
        if (xpon_grp_addr_between(dest_addr, entry->grpstart, entry->grpend)==0)
        {
            continue;
        }

        if(0 == xpon_is_non_zero(entry->srcip,16))
        {
			goto match_dynacl;
        }
        else 
        {
            if(0 == memcmp(src_addr,entry->srcip,16))
            {
                goto match_dynacl;
            }
        }
	}
    MULTICAST_CRITIC_INFO("igmpv3 return MATCH fail\n");
    return NOT_MATCH_DYNAMIC_ACL;

match_dynacl:
	MULTICAST_NOTICE_INFO("igmpv3 return MATCH Success\n");
	*dyn_entry_ptr = entry;
	return MATCH_DYNAMIC_ACL;		
}

int upstream_dynamic_acl_ctr_mldv1(int port,unsigned char* dest_addr, xPON_WhiteList_Entry_List_t** dyn_entry_ptr)
{
    xPON_PortConf_t* port_conf = NULL;
    struct list_head*  dyn_list = NULL;
	xPON_WhiteList_Entry_List_t* entry = NULL;
    port_conf = xpon_port_conf_by_id(port);
    
    if((NULL == port_conf) || (NULL == dest_addr))
    {
		MULTICAST_CRITIC_INFO("mldv1 return MATCH fail\n");
        return NOT_MATCH_DYNAMIC_ACL;
    }
    dyn_list = &(port_conf->dyn_list);

    //unconfig dynamic acl, forward or drop by multicast forward table
    if(list_empty(dyn_list))
    {
    	*dyn_entry_ptr = NULL;
		MULTICAST_NOTICE_INFO("mldv1 return MATCH Success\n");
        return MATCH_DYNAMIC_ACL;
    }

	list_for_each_entry(entry,dyn_list,list)
	{
        if (xpon_grp_addr_between(dest_addr, entry->grpstart, entry->grpend)==0)
        {
            continue;
        }
        else
        {
        	MULTICAST_NOTICE_INFO("mldv1 return MATCH Success\n");
			*dyn_entry_ptr = entry;
            return MATCH_DYNAMIC_ACL;        	
        }
	}
    MULTICAST_CRITIC_INFO("mldv1 return MATCH fail\n");
    return NOT_MATCH_DYNAMIC_ACL;	
}

int upstream_dynamic_acl_ctr_mldv2(int port,unsigned char* dest_addr,unsigned char* src_addr, xPON_WhiteList_Entry_List_t** dyn_entry_ptr)
{    
    xPON_PortConf_t* port_conf = NULL;
    struct list_head*  dyn_list = NULL;
	xPON_WhiteList_Entry_List_t* entry = NULL;
    port_conf = xpon_port_conf_by_id(port);
    
	if((NULL == port_conf) || (NULL == dest_addr))
    {
		MULTICAST_CRITIC_INFO("mldv2 return MATCH fail\n");
        return NOT_MATCH_DYNAMIC_ACL;
    }
    dyn_list = &(port_conf->dyn_list);

    //unconfig dynamic acl, forward or drop by multicast forward table
    if(list_empty(dyn_list))
    {
    	*dyn_entry_ptr = NULL;
		MULTICAST_NOTICE_INFO("mldv2 return MATCH Success\n");
        return MATCH_DYNAMIC_ACL;
    }
	
	list_for_each_entry(entry,dyn_list,list)
	{	
        if (xpon_grp_addr_between(dest_addr, entry->grpstart, entry->grpend)==0)
        {
			continue;
        }

        if(0 == xpon_is_non_zero(entry->srcip,16))
        {
			goto match_dynacl;
        }
        else 
        {
            if(0 == memcmp(src_addr,entry->srcip,16))
            {
                goto match_dynacl;
            }
        }
	}
    MULTICAST_CRITIC_INFO("mldv2 return MATCH fail\n");
    return NOT_MATCH_DYNAMIC_ACL;

match_dynacl:
	MULTICAST_NOTICE_INFO("mldv2 return MATCH Success\n");
	*dyn_entry_ptr = entry;
	return MATCH_DYNAMIC_ACL;		
}

static int is_forword_allowed(xPON_WhiteList_Entry_List_t* dyn_entry)
{
	if((dyn_entry == NULL)||(dyn_entry->preview_info.pre_len == 0)||
		((dyn_entry->preview_info.pre_rep_cnt_left >= 1)&&(dyn_entry->preview_info.rep_interval_flag == FALSE)))
		return TRUE;
	else
		return FALSE;

}

static int start_multicast_preview(xPON_WhiteList_Entry_List_t* dyn_entry,xPON_FwdEntry_t* entry)
{
	if((NULL == dyn_entry)||(NULL == entry))
		return 0;
	/*Multicast preview*/
	if((dyn_entry->preview_info.pre_len != 0)&&(dyn_entry->preview_info.pre_rep_cnt_left >= 1)&&(dyn_entry->preview_info.rep_interval_flag == FALSE)){
		/*start preview timer*/
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)	
		setup_timer(&entry->preview_timer, xpon_preview_timer_timeout, (unsigned long) entry);
#else
		timer_setup(&entry->preview_timer, xpon_preview_timer_timeout, 0); 
#endif
		mod_timer(&entry->preview_timer,(jiffies + (dyn_entry->preview_info.pre_len*HZ)));
		dyn_entry->preview_info.pre_rep_cnt_left -= 1;
		/*start preview interval timer*/
		dyn_entry->preview_info.rep_interval_flag = TRUE;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)	
		setup_timer(&dyn_entry->preview_info.rep_interval_timer, 
			xpon_pre_interval_timer_timeout, (unsigned long) dyn_entry);
#else
		timer_setup(&dyn_entry->preview_info.rep_interval_timer, xpon_pre_interval_timer_timeout, 0); 
#endif
		mod_timer(&dyn_entry->preview_info.rep_interval_timer,
			(jiffies + ((dyn_entry->preview_info.pre_len + dyn_entry->preview_info.pre_rep_time)*HZ)));
	}else{
	/*do nonthing!*/
	}
	return 0;
}

int xpon_report_pkt_handle(struct sk_buff* skb, int proto, int port)
{
    xPON_PortConf_t* port_conf = NULL;
    xPON_FwdEntry_t* entry = NULL;
	unsigned char grp_addr[16],src_ip[16],clientIp[16];
	int type,play_num;
	int vid = 0;
    unsigned long listener_time = 0;
	xPON_WhiteList_Entry_List_t* dyn_entry = NULL;
#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_IGMP_SET_GROUP)
    xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
    int groupCnt = 0;
#endif/*TCSUPPORT_COMPILE*/

    
    port_conf = xpon_port_conf_by_id(port);

	if(port_conf == NULL)
	{
	    MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
	    goto skb_drop;
	}

	memset(grp_addr,0,16);
	memset(src_ip,0,16);
	memset(clientIp,0,16);
	vid = xpon_get_outmost_vid(skb);
	xpon_get_igmp_grpaddr(grp_addr,skb);
	xpon_get_igmp_srcip(clientIp,skb);

	if (proto == XPON_MASK_IGMPV2){
		type = MULTCASTCTL_IPV4_DA;
		if(NOT_MATCH_DYNAMIC_ACL == upstream_dynamic_acl_ctr_igmpv2(port, grp_addr,&dyn_entry)){
			MULTICAST_NOTICE_INFO("xpon_report_pkt_handle igmpv2 NOT_MATCH_DYNAMIC_ACL,drop!!\n");
			goto skb_drop;
		}	
	}else if(proto == XPON_MASK_MLDV1){
		type = MULTCASTCTL_IPV6_DA;
		if(NOT_MATCH_DYNAMIC_ACL == upstream_dynamic_acl_ctr_mldv1(port, grp_addr,&dyn_entry)){
			MULTICAST_NOTICE_INFO("xpon_report_pkt_handle mldv1 NOT_MATCH_DYNAMIC_ACL,drop!!\n");
			goto skb_drop;
		}	
	}else
		goto skb_drop;

    /*update list*/
    del_leave_pkt_entry(port, grp_addr, proto);
    
	entry = xpon_fwdtbl_find(type,port,vid,grp_addr,src_ip,proto);
#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_IGMP_SET_GROUP)
	groupCnt = xpon_fwdtbl_cnt();
	if(groupCnt >= igmp_conf->group_num)
	{
		if(entry==NULL && groupCnt==igmp_conf->group_num)
		{
			goto skb_drop;
		}
	}
	else
#endif/*TCSUPPORT_COMPILE*/	
	{
		if (entry==NULL)
		{
			if(TRUE == is_forword_allowed(dyn_entry)){
				play_num =	xpon_get_play_group_number(port,proto);
				if ((port_conf->maxgroup > 0) && (port_conf->maxgroup <= play_num)){
					goto skb_drop;
				}
				entry = xpon_fwdtbl_add(type,port,vid,grp_addr,src_ip,proto,clientIp);
				if (entry==NULL){
					goto skb_drop;
				}
				if (proto == XPON_MASK_IGMPV2){
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)	
					setup_timer(&entry->leave_ageing_timer, xpon_igmp_timer_timeout, (unsigned long) entry);
#else
					timer_setup(&entry->leave_ageing_timer, xpon_igmp_timer_timeout, 0); 
#endif
				}
				else{					
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)	
					setup_timer(&entry->leave_ageing_timer, xpon_mld_timer_timeout, (unsigned long) entry);		
#else
					timer_setup(&entry->leave_ageing_timer, xpon_mld_timer_timeout, 0); 
#endif
				}

				/*Multicast preview*/
				start_multicast_preview( dyn_entry, entry);
			}
		}
	}
	if(entry)
	{
		listener_time = xpon_get_listenner_interval(port);
		entry->leave_count = last_query_cnt;
        
        /****listener_time  already * HZ****/
		mod_timer(&entry->leave_ageing_timer,listener_time);
	}
	else{
		goto skb_drop;
	}
	if (proto == XPON_MASK_IGMPV2)
		goto skb_free;
	
	if (proto == XPON_MASK_MLDV1)
		goto skb_free;

skb_free:
    MULTICAST_NOTICE_INFO("free skb.\n");
	kfree_skb(skb);
	return 1;

skb_drop:
	MULTICAST_NOTICE_INFO("pon_report_pkt_handle: free skb.\n");
	kfree_skb(skb);
	return 0;		
}


int xpon_leave_pkt_handle(struct sk_buff* skb,int proto, int port)
{
    xPON_PortConf_t* port_conf = NULL;
    xPON_FwdEntry_t* entry = NULL;
	unsigned char grp_mac[6]={0},grp_addr[16]={0},src_ip[16]={0};
    unsigned long lastquery_time = 0;
	int vid = 0;
	
	port_conf = xpon_port_conf_by_id(port);

    if(port_conf == NULL)
    {
        MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
        goto skb_drop;
    }

	vid = xpon_get_outmost_vid(skb);
	xpon_get_igmp_grpaddr(grp_addr,skb);

	if (proto == XPON_MASK_IGMPV2)
		xpon_ip4_to_mac(grp_addr,grp_mac);
	else if (proto == XPON_MASK_MLDV1)
		xpon_ip6_to_mac(grp_addr,grp_mac);	
	else 
		MULTICAST_NOTICE_INFO("xpon_leave_pkt_handle(): protocol = %d ",proto);

	entry = xpon_fwdtbl_find_ext(port,vid,grp_mac,grp_addr,src_ip,proto);
	
	if (entry==NULL)
	{
		goto skb_drop;
	}

	if (entry->leave_ageing_timer.function == NULL)
	{
	 	if (proto == XPON_MASK_IGMPV2){
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)				
			setup_timer(&entry->leave_ageing_timer, xpon_igmp_timer_timeout, (unsigned long) entry);
#else
			timer_setup(&entry->leave_ageing_timer, xpon_igmp_timer_timeout, 0); 
#endif
		}
		else{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)				
			setup_timer(&entry->leave_ageing_timer, xpon_mld_timer_timeout, (unsigned long) entry);
#else
			timer_setup(&entry->leave_ageing_timer, xpon_mld_timer_timeout, 0); 
#endif			
		}
	}
	
    //fast leave enable
	if (port_conf->fastleave)
	{	
		del_timer(&entry->leave_ageing_timer);
		xpon_fwdtbl_operate_entry(2,entry);
        if (1 == fwdtbl_exist(grp_addr, proto))
            goto skb_drop;
	}
    //fast leave disable
	else
	{	
		if (proto == XPON_MASK_IGMPV2)
			xpon_send_igmp_query(port,grp_addr);
		else
			xpon_send_mld_query(port,grp_addr);
		
		lastquery_time =  xpon_get_lastquery_interval(port);

		entry->leave_count--; //TC_6.3.16.js
		mod_timer(&entry->leave_ageing_timer,lastquery_time);
		
       // {/* 247组播测试中不能丢弃该报文 */
        //leave count == 0 ,send leave pkt to wan
	       	if(0 < entry->leave_count)
	        {	
				//discard leave pkt, 
	            #if 1 
	            
	            skb->pon_vlan_flag |= PON_LEAVE_PKT_DEAL;
	            
	            del_leave_pkt_entry(port, grp_addr, proto);
	            add_leave_pkt_entry(port, grp_addr, proto, skb);
	            return DISCARD_LEAVE_MESSAGE;
	            #endif
	        } 
			else if (0 == entry->leave_count)
			{	
				del_timer(&entry->leave_ageing_timer);
				xpon_fwdtbl_operate_entry(2,entry);
			}
			else
			{
				MULTICAST_TRACE_INFO("leave_count < 0, the entry has been deleted.\n");
			}
        //}
	}

	if (proto == XPON_MASK_IGMPV2)
		goto skb_free;
	
	if (proto == XPON_MASK_MLDV1)
		goto skb_free;

skb_free:
    MULTICAST_NOTICE_INFO("free skb.\n");
	kfree_skb(skb);
	return 1;

skb_drop:
	kfree_skb(skb);
	return 0;
}


int xpon_query_pkt_handle(struct sk_buff* skb, int proto)
{
	xPON_AniEntry_t* ani_entry;

	MULTICAST_NOTICE_INFO("xpon_query_pkt_handle.\n");

	ani_entry = xpon_get_ani_entry(skb->dev->name);
	if (ani_entry==NULL)
		goto skb_drop;
		
	xpon_broadcast_to_port(skb);
	return 1;

skb_drop:
	MULTICAST_NOTICE_INFO("xpon_query_pkt_handle:free packet.\n");	
	kfree_skb(skb);
	return 0;
}

int xpon_fwdtbl_match_entry(xPON_FwdEntry_t* entry,int vid,unsigned char* dest_mac ,unsigned char* dest_addr ,unsigned char* src_addr)
{
	int type = entry->type;

	if (type==MULTCASTCTL_IPV4_DA)
	{	
	    MULTICAST_NOTICE_INFO("entry->grp_addr = %02x %02x %02x %02x, dest_addr = %02x %02x %02x %02x\n", 
	                            entry->grp_addr[0], entry->grp_addr[1], entry->grp_addr[2], entry->grp_addr[3],
	                            dest_addr[0], dest_addr[1], dest_addr[2], dest_addr[3]);
		if (memcmp(entry->grp_addr,dest_addr,4))
			return 0;
		if ( (entry->flag&0xff)== XPON_MASK_IGMPV3 && memcmp(entry->src_addr,src_addr,4))
			return 2;
		return 1;
	}
	else if (type==MULTCASTCTL_IPV6_DA)
	{
		if (memcmp(entry->grp_addr,dest_addr,16))
			return 0;
		if ((entry->flag&0xff)== XPON_MASK_MLDV2 && memcmp(entry->src_addr,src_addr,16))
			return 2;
		return 1;
	}
#ifdef TCSUPPORT_XPON_IGMP_CTC	
	else if (type==MULTCASTCTL_MAC_DA)
	{
		if (!memcmp(entry->grp_addr,dest_mac,6))
			return 10;
	}
	else if (type==MULTCASTCTL_MAC_DA_VLAN)
	{
		if (!memcmp(entry->grp_addr,dest_mac,6) && vid==entry->vid)
			return 11;
	}
	else if (type==MULTCASTCTL_IPV4_SA_MAC_DA)
	{
	    MULTICAST_NOTICE_INFO("entry->grp_addr = %02x %02x %02x %02x %02x %02x, dest_addr = %02x %02x %02x %02x %02x %02x\n", 
	                            entry->grp_addr[0], entry->grp_addr[1], entry->grp_addr[2], entry->grp_addr[3], entry->grp_addr[4], entry->grp_addr[5],
	                            dest_addr[0], dest_addr[1], dest_addr[2], dest_addr[3], dest_addr[4], dest_addr[5]);
		if (!memcmp(entry->grp_addr,dest_mac,6) && !memcmp(entry->src_addr,src_addr,4))
			return 12;
	}
	else if (type==MULTCASTCTL_IPV4_DA_VLAN)
	{
		if (!memcmp(entry->grp_addr,dest_addr,4) && vid==entry->vid)
			return 13;
	}
	else if (type==MULTCASTCTL_IPV6_DA_VLAN)
	{
		if (!memcmp(entry->grp_addr,dest_addr,16) && vid==entry->vid)
			return 14;	
	}
	else if (type==MULTCASTCTL_IPV6_SA_MAC_DA)
	{
		if (!memcmp(entry->grp_addr,dest_mac,6) && !memcmp(entry->src_addr,src_addr,16))
			return 15;
	}
#endif
	else
	{
		MULTICAST_NOTICE_INFO("xpon_should_forward_flow():type = %d",type);
	}
	return 0;
}

int dataCounter = 0;
int modiTimes = 10;


int  xpon_should_forward_flow(int port,int vid,unsigned char* dest_mac ,unsigned char* dest_addr ,unsigned char* src_addr)
{
	xPON_FwdEntry_t* entry = NULL;
	struct list_head* fwd_list = xpon_get_forward_list();
	int ret,flag = 0;
    int match_static_flag  = NOT_MATCH_STATIC_ACL;
	int match_dynamic_flag = NOT_MATCH_DYNAMIC_ACL;
    int gem_port_id = INVALID_GEMP_ID;
    xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	xPON_FwdEntry_t entry_tmp = {0};

#if 0
    /*0:snooping mode, 1:control mode*/
    if((xpon_igmp_get_fwdmode()==0)&&(xpon_igmp_get_max_playgroup(port)==0)){
        return 0;
    }
#endif

	if (xpon_pass_port_vlan(port,vid)==0)
	{
	    MULTICAST_NOTICE_INFO("return\n");
		return 0;
	}
	MULTICAST_NOTICE_INFO("start match forward rule\n");

    if((1 == igmp_conf->xpon_mode) && g_care_ver_dynlist_stalist_op&&!TCSUPPORT_XPON_IGMP_CHT_VAL)
    {
        /*static ACL*/
        match_static_flag = static_acl_ctr(port, vid, gem_port_id, dest_addr, src_addr, 0);
        if(NOT_MATCH_STATIC_ACL == match_static_flag)
        {
            /*dynamic ACL*/
            match_dynamic_flag = dynamic_acl_ctr(port, vid, gem_port_id, dest_addr, src_addr);
            if(MATCH_DYNAMIC_ACL != match_dynamic_flag)
                return 0;
        }
    }

	dataCounter++;
	if(!(dataCounter % modiTimes))
	{
		tc3162wdog_kick();
	}
	
	rcu_read_lock();
	list_for_each_entry_rcu(entry,fwd_list,list)
	{
		memcpy(&entry_tmp, entry, sizeof(xPON_FwdEntry_t));
		
		if (entry_tmp.port != port)
			continue;

		ret = xpon_fwdtbl_match_entry(&entry_tmp,vid,dest_mac,dest_addr,src_addr);
		MULTICAST_NOTICE_INFO("match forward rule return = %d, flag = %x, type = %d\n", ret, entry_tmp.flag, entry_tmp.type);
		if (ret >= 10)
			goto out;
		
		if (ret > 0)
		{
			if ((entry_tmp.flag & 0xff)== XPON_MASK_IGMPV2 || (entry_tmp.flag & 0xff)==XPON_MASK_MLDV1)
				goto out;
			
			if (ret==1 && (entry_tmp.flag&0xff00)==XPON_MODE_INCLUDE)
				goto out;
			
			if (ret==1 && (entry_tmp.flag & 0xff00)== XPON_MODE_EXCLUDE)
			{
				flag = 1;
			}
			if (flag != 1 && (entry_tmp.flag & 0xff00)== XPON_MODE_EXCLUDE)
			{
				flag = 2;
			}
		}
	}


	if (flag == 2)
	{
		goto out;
	}

	rcu_read_unlock();
	return 0;

out:
    rcu_read_unlock();
	MULTICAST_NOTICE_INFO("end return YES\n");
    return 1;

}


extern int (*wan_multicast_drop_hook)(struct sk_buff* skb);

#define PPP_IP		0x21	/* Internet Protocol */
#define PPP_IPV6	0x57	/* Internet Protocol Version 6 */
int pppoe_wan_is_multicast_pkt(struct sk_buff* skb){
	unsigned char *pppoe_h;
	__be16 pppoe_proto;
	struct iphdr*  ih;
	struct ipv6hdr* i6h;
	
	pppoe_h = (unsigned char *)xpon_get_network_header(skb);

	pppoe_h += sizeof(struct pppoe_hdr); //skip pppoe header
	pppoe_proto = ntohs(*(__be16*)pppoe_h);
	pppoe_h += 2; // skip ppp header
	if (pppoe_proto == PPP_IP){
		ih = (struct iphdr* ) (pppoe_h);
		if(ipv4_is_multicast(ih->daddr))
			return 1;
	}
	else if (pppoe_proto == PPP_IPV6){
		i6h = (struct ipv6hdr*) (pppoe_h);
		if(ipv6_addr_is_multicast(&i6h->daddr))
			return 1;
	}

	return 0;
}

int wan_is_multicast_pkt(struct sk_buff* skb, int direction)
{
	unsigned char mac[3] = {0x01,0x00,0x5e};
	unsigned char* dst = NULL;
	int ip_type = -1;
	int igmp_type = -1;


	if (direction == 1)
		skb_reset_mac_header(skb);

	dst = eth_hdr(skb)->h_dest;
	
	if (memcmp(dst, mac, 3)==0)
		return 1;

	if (dst[0]==0x33 && dst[1] == 0x33){
		ip_type = xpon_get_ip_type(skb);

		if (direction == 0 && ip_type == PROTOCOL_UDP6) // incomming
			return 1;
		else if(ip_type == PROTOCOL_ICMPV6){
			igmp_type = xpon_get_igmp_type(skb);
			
			switch (igmp_type) 
			{
				case ICMPV6_MGM_REPORT:
				case ICMPV6_MLD2_REPORT:
				case ICMPV6_MGM_REDUCTION:
				case ICMPV6_MGM_QUERY:
					return 1;
				default:
					return 0;
			}
		}
	}
	return 0;
}

int xpon_igmp_acl_filter(struct sk_buff* skb){
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	unsigned char dest_addr[16],src_addr[16];
	int port = 0;
	int vid = 0;
   	int match_static_flag  = NOT_MATCH_STATIC_ACL;
	int match_dynamic_flag = NOT_MATCH_DYNAMIC_ACL;
	int gem_port_id = 0;

	if(xpon_get_packet_type(skb) == ETH_P_PPP_SES){
		if(pppoe_wan_is_multicast_pkt(skb) == 0)
			return 1;
	}
	else{
		if (wan_is_multicast_pkt(skb, 0) == 0)
			return 1;
	}
	vid = xpon_get_outmost_vid(skb);

	memset(dest_addr,0,16);
	if (0 == xpon_get_dest_addr(dest_addr,skb))
	{
		return 1;
	}   

	memset(src_addr,0,16);
	if (0 == xpon_get_src_addr(src_addr,skb))
	{
		MULTICAST_ERROR_INFO("igmp multicast address parser error");
		return 1;
	}   

	if(1 == igmp_conf->xpon_mode){
		gem_port_id = skb->gem_port; 
	}  
	/*epon 2, gpon=1*/
	if(2 == igmp_conf->xpon_mode){
		return 1;
	}
	
	if (igmp_conf->uni_port[0].port_flag & XPON_BRIDGE_PORT){

		port = 0;
		/*static ACL*/
		match_static_flag = static_acl_ctr(port, vid, gem_port_id, dest_addr, src_addr, 0);
		if(NOT_MATCH_STATIC_ACL == match_static_flag){
			/*dynamic ACL*/
			match_dynamic_flag = dynamic_acl_ctr(port, vid, gem_port_id, dest_addr, src_addr);
		}

		if(MATCH_STATIC_ACL== match_static_flag || MATCH_DYNAMIC_ACL == match_dynamic_flag){
		    return 1;
		}
	}
	MULTICAST_NOTICE_INFO("igmp_acl_filter drop skb.\n");
	if (wan_multicast_drop_hook)
		wan_multicast_drop_hook(skb);
	return 0;
}

int xpon_data_pkt_handle(struct sk_buff* skb,int proto)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	unsigned char dest_mac[8];
	unsigned char dest_addr[16],src_addr[16];
	int i,port;
	int vid = 0;
	struct sk_buff* skb2;
	int flag = 0;
    int match_static_flag  = NOT_MATCH_STATIC_ACL;
    int match_dynamic_flag = NOT_MATCH_DYNAMIC_ACL;
    int forward_flag       = 0;
    int gem_port_id = 0;
	
	vid = xpon_get_outmost_vid(skb);

	memset(dest_mac,0,8);
	xpon_get_dest_mac(dest_mac,skb);

	memset(dest_addr,0,16);
	xpon_get_dest_addr(dest_addr,skb);

	memset(src_addr,0,16);
	xpon_get_src_addr(src_addr,skb);
    
    if(1 == igmp_conf->xpon_mode)
    {
        gem_port_id = skb->gem_port;  
    }  

	xpon_igmp_store_src_vlan_info(skb);
	for(i=0;i<igmp_conf->uni_num;i++)
	{
		if ((igmp_conf->uni_port[i].port_flag & XPON_BRIDGE_PORT)==0)
			continue;
		port = i;
        
        /*epon 2, gpon=1*/
        if(1 == igmp_conf->xpon_mode)
        {
            if(g_care_ver_dynlist_stalist_op&&!TCSUPPORT_XPON_IGMP_CHT_VAL)
            {
                /*static ACL*/
                match_static_flag = static_acl_ctr(port, vid, gem_port_id, dest_addr, src_addr, 0);
                if(NOT_MATCH_STATIC_ACL == match_static_flag)
                {
                    /*dynamic ACL*/
                    match_dynamic_flag = dynamic_acl_ctr(port, vid, gem_port_id, dest_addr, src_addr);
                    if(MATCH_DYNAMIC_ACL == match_dynamic_flag)
                    {
                        forward_flag = xpon_should_forward_flow(port,vid,dest_mac,dest_addr,src_addr);
                    }
                }
            }
            else
            {
                forward_flag = xpon_should_forward_flow(port,vid,dest_mac,dest_addr,src_addr);
            }
            
            if(MATCH_STATIC_ACL== match_static_flag || 1  == forward_flag)
            {
                skb2 =  skb_copy(skb,GFP_ATOMIC);
                if (skb2 != NULL)
                    xpon_forward_to_port(skb2,port);
                flag = 1;
            }
        }
        else
        {
            if (xpon_should_forward_flow(port,vid,dest_mac,dest_addr,src_addr)==1)
            {
                skb2 =  skb_copy(skb,GFP_ATOMIC);
                if (skb2 != NULL)
                    xpon_forward_to_port(skb2,port);
                flag = 1;
            }       
        }

	}

	if (flag)	
		kfree_skb(skb);
	else
		xpon_hwnat_drop_multicast(skb);
	return 1;
}

#define IGMPV3_GRP_REC_SIZE(x) (sizeof(struct igmpv3_grec) +  (x->grec_nsrcs)*4)

int xpon_igmpv3_pkt_handle(struct sk_buff* skb, int port)
{
	struct igmpv3_grec *grec;
	struct igmpv3_report * report;
	xPON_FwdEntry_t* entry = NULL ;
	int i,j,grp_num,src_num;
	int vid = 0;
	unsigned char grp_addr[16],src_addr[16];
    xPON_PortConf_t* port_conf = NULL;
    unsigned long qtime;
	unsigned char client_ip[16];
	xPON_WhiteList_Entry_List_t* dyn_entry = NULL;
#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_IGMP_SET_GROUP)	
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int groupCnt = 0;
#endif/*TCSUPPORT_COMPILE*/
	unsigned long lastquery_time = 0;

	port_conf = xpon_port_conf_by_id(port);

	if(port_conf == NULL)
    {
        MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
        goto skb_drop;
    }

	MULTICAST_NOTICE_INFO("xpon_igmp_reportv3_handle.\n");

	vid = xpon_get_outmost_vid(skb);
	report = xpon_get_transport_header(skb);
	if( report == NULL )
	{
        MULTICAST_ERROR_INFO("xpon_get_transport_header RETURN NULL Pointer\n");
        goto skb_drop;
	}
	grp_num = ntohs(report->ngrec);

#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_IGMP_SET_GROUP)	
	entry = xpon_fwdtbl_find(MULTCASTCTL_IPV4_DA,port,vid,grp_addr,src_addr,XPON_MASK_IGMPV3);
	groupCnt = xpon_fwdtbl_cnt();
	if(groupCnt >= igmp_conf->group_num)
	{
		if(entry==NULL && groupCnt==igmp_conf->group_num)
		{
			goto skb_drop;
		}
	}
#endif/*TCSUPPORT_COMPILE*/	
	
	if (port_conf->maxgroup > 0 && grp_num > port_conf->maxgroup)
		goto skb_drop;

	memset(client_ip,0,16);
	xpon_get_igmp_srcip(client_ip,skb);
		
	MULTICAST_NOTICE_INFO("xpon_igmp_reportv3_handle: grp num = %d.\n",grp_num);
	
	grec = report->grec;
	for(i = 0; i < grp_num; i++)
	{
		src_num =  ntohs(grec->grec_nsrcs);
		memset(grp_addr,0,16);
		memcpy(grp_addr,(unsigned char*)&grec->grec_mca,4);

		MULTICAST_NOTICE_INFO("xpon_igmp_reportv3_handle: grp addr =%u.%u.%u.%u.\n",grp_addr[0],grp_addr[1],grp_addr[2],grp_addr[3]);
        MULTICAST_NOTICE_INFO("xpon_igmp_reportv3_handle: grec_type=%d, src num = %d.\n",grec->grec_type,src_num);

		if(grec->grec_type == IGMPV3_CHANGE_TO_INCLUDE || grec->grec_type == IGMPV3_MODE_IS_INCLUDE || grec->grec_type == IGMPV3_ALLOW_NEW_SOURCES) {
			if(grec->grec_type == IGMPV3_CHANGE_TO_INCLUDE){
				//changed to include mode, old fwdtbl rule should be removed
				MULTICAST_NOTICE_INFO("===IGMPV3_CHANGE_TO_INCLUDE===\n");
				xpon_fwdtbl_del_by_grp(MULTCASTCTL_IPV4_DA, port, vid, grp_addr);
			}
			
			for(j = 0; j <src_num; j++)
				{
					memset(src_addr,0,16);
					memcpy(src_addr,(unsigned char*)&grec->grec_src[j],4);

					MULTICAST_NOTICE_INFO("xpon_igmp_reportv3_handle include: src addr =%u.%u.%u.%u.\n",src_addr[0],src_addr[1],src_addr[2],src_addr[3]);
					
					entry = xpon_fwdtbl_find(MULTCASTCTL_IPV4_DA,port,vid,grp_addr,src_addr,XPON_MASK_IGMPV3);
					if (entry==NULL)
					{
						//if(-1 == check_max_group(skb, port, XPON_MASK_IGMPV3)
	                    //    || NOT_MATCH_DYNAMIC_ACL == upstream_dynamic_acl_ctr_igmpv3(port,grp_addr,src_addr,&dyn_entry))
	                    //    goto skb_drop;

						if(-1 == check_max_group(skb, port, XPON_MASK_IGMPV3))
	                        goto skb_drop;
						
						if( NOT_MATCH_DYNAMIC_ACL == upstream_dynamic_acl_ctr_igmpv3(port,grp_addr,src_addr,&dyn_entry)){
							if(i+1 == grp_num){
	                        goto skb_drop;
							}
							else{
								break;
							}
						}

						
						if(TRUE == is_forword_allowed(dyn_entry)){
							entry = xpon_fwdtbl_add(MULTCASTCTL_IPV4_DA,port,vid,grp_addr,src_addr,XPON_MASK_IGMPV3 | XPON_MODE_INCLUDE,client_ip);
							if (entry==NULL)
								goto skb_drop;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)				
							setup_timer(&entry->leave_ageing_timer,xpon_igmp_timer_timeout,(unsigned long)entry);
#else
							timer_setup(&entry->leave_ageing_timer, xpon_igmp_timer_timeout, 0); 
#endif
							/*Multicast preview*/
							start_multicast_preview( dyn_entry, entry);
						}
					}else{
						if(XPON_MODE_EXCLUDE == (entry->flag&0xff00)){
							entry->flag = XPON_MASK_IGMPV3 | XPON_MODE_INCLUDE;
							if(IGMPV3_ALLOW_NEW_SOURCES == grec->grec_type){
								//if (port_conf->fastleave){
									MULTICAST_NOTICE_INFO(" MLD2_ALLOW_NEW_SOURCES MLDV2 xpon_leave_pkt_handle():fast leave enable \n");
									del_timer(&entry->leave_ageing_timer);
									xpon_fwdtbl_operate_entry(2,entry);
									break;
								//}
							}
						}
					}
					if (entry)
					{
						qtime =  xpon_get_listenner_interval(port);
						entry->leave_count = last_query_cnt;
						mod_timer(&entry->leave_ageing_timer,qtime);
					}
					else
						goto skb_drop;
				}
				
				if (src_num==0)
				{
					if(IGMPV3_CHANGE_TO_INCLUDE == grec->grec_type){	
						memset(src_addr,0,16);
                        entry = xpon_fwdtbl_find(MULTCASTCTL_IPV4_DA,port,vid,grp_addr,src_addr,XPON_MASK_IGMPV3);
                        if(entry)
                        {
                            del_timer(&entry->leave_ageing_timer);
                            xpon_fwdtbl_operate_entry(2,entry);
                        }
					}
				}
		}
		else if(grec->grec_type == IGMPV3_CHANGE_TO_EXCLUDE || grec->grec_type == IGMPV3_MODE_IS_EXCLUDE || grec->grec_type == IGMPV3_BLOCK_OLD_SOURCES) {
			if(grec->grec_type == IGMPV3_CHANGE_TO_EXCLUDE) {
				//changed to include mode, old fwdtbl rule should be removed
				MULTICAST_NOTICE_INFO("===IGMPV3_CHANGE_TO_EXCLUDE===\n");
				xpon_fwdtbl_del_by_grp(MULTCASTCTL_IPV4_DA, port, vid, grp_addr);
			}
			
			for(j = 0; j < src_num; j++)
			{
				memset(src_addr,0,16);		
				memcpy(src_addr,(unsigned char*)&grec->grec_src[j],4);
	
				MULTICAST_NOTICE_INFO("xpon_igmp_reportv3_handle exclude: src addr =%u.%u.%u.%u.\n",src_addr[0],src_addr[1],src_addr[2],src_addr[3]);
					
				entry = xpon_fwdtbl_find(MULTCASTCTL_IPV4_DA,port,vid,grp_addr,src_addr,XPON_MASK_IGMPV3);
				if (entry==NULL)
				{
					if(-1 == check_max_group(skb, port, XPON_MASK_IGMPV3)
	                   || NOT_MATCH_DYNAMIC_ACL == upstream_dynamic_acl_ctr_igmpv3(port,grp_addr,src_addr,&dyn_entry))
	                   goto skb_drop;
						
					if(TRUE == is_forword_allowed(dyn_entry)){
						MULTICAST_NOTICE_INFO("xpon_igmp_reportv3_handle add fwd table: src addr =%u.%u.%u.%u.\n",src_addr[0],src_addr[1],src_addr[2],src_addr[3]);
						entry = xpon_fwdtbl_add(MULTCASTCTL_IPV4_DA,port,vid,grp_addr,src_addr,XPON_MASK_IGMPV3 | XPON_MODE_EXCLUDE,client_ip);
						if (entry==NULL)
							goto skb_drop;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)				
						setup_timer(&entry->leave_ageing_timer,xpon_igmp_timer_timeout,(unsigned long)entry);			
#else
						timer_setup(&entry->leave_ageing_timer, xpon_igmp_timer_timeout, 0); 
#endif
						/*Multicast preview*/
						start_multicast_preview( dyn_entry, entry);
					}
				}else{
					if(XPON_MODE_INCLUDE == (entry->flag&0xff00)){
						entry->flag = XPON_MASK_IGMPV3 | XPON_MODE_EXCLUDE;
						if(IGMPV3_BLOCK_OLD_SOURCES == grec->grec_type){
							entry->leave_count--;
							lastquery_time = xpon_get_lastquery_interval(port);
							mod_timer(&entry->leave_ageing_timer,lastquery_time);
							if (port_conf->fastleave || entry->leave_count <= 0){
								MULTICAST_NOTICE_INFO(" MLD2_BLOCK_OLD_SOURCES MLDV2 xpon_leave_pkt_handle():fast leave enable \n");
								del_timer(&entry->leave_ageing_timer);
								xpon_fwdtbl_operate_entry(2,entry);
								break;
							}
						}
					}
				}
				if (entry && IGMPV3_BLOCK_OLD_SOURCES != grec->grec_type)
				{	
					entry->leave_count = last_query_cnt;
					qtime =  xpon_get_listenner_interval(port);
                    MULTICAST_NOTICE_INFO("xpon_igmp_reportv3_handle entery change timer interval is %lu\n",qtime);
					mod_timer(&entry->leave_ageing_timer,qtime);
				}
				else
					goto skb_drop;
			}
			if (src_num==0)
			{
				if(IGMPV3_BLOCK_OLD_SOURCES == grec->grec_type){
					MULTICAST_NOTICE_INFO("===IGMPV3_BLOCK_OLD_SOURCES===\n");
					xpon_fwdtbl_del_by_grp(MULTCASTCTL_IPV4_DA, port, vid, grp_addr);	
					break;
				}
				else if(IGMPV3_MODE_IS_EXCLUDE == grec->grec_type){
					MULTICAST_NOTICE_INFO("===IGMPV3_MODE_IS_EXCLUDE===\n");
				}
				else if(IGMPV3_CHANGE_TO_EXCLUDE == grec->grec_type){
					MULTICAST_NOTICE_INFO("===IGMPV3_CHANGE_TO_EXCLUDE===\n");
				}
				
				memset(src_addr,0,16);
                MULTICAST_NOTICE_INFO("xpon_igmp_reportv3_handle exclude src num is 0\n");
				entry = xpon_fwdtbl_find(MULTCASTCTL_IPV4_DA,port,vid,grp_addr,src_addr,XPON_MASK_IGMPV3);
				if (entry==NULL)
				{
					if(-1 == check_max_group(skb, port, XPON_MASK_IGMPV3))
	                   	goto skb_drop;
						
					if(NOT_MATCH_DYNAMIC_ACL == upstream_dynamic_acl_ctr_igmpv3(port,grp_addr,src_addr,&dyn_entry))
						break;
						
					if(TRUE == is_forword_allowed(dyn_entry)){
						entry = xpon_fwdtbl_add(MULTCASTCTL_IPV4_DA,port,vid,grp_addr,src_addr,XPON_MASK_IGMPV3 | XPON_MODE_EXCLUDE,client_ip);
						if (entry==NULL)
							goto skb_drop;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)				
						setup_timer(&entry->leave_ageing_timer,xpon_igmp_timer_timeout,(unsigned long)entry);
#else
						timer_setup(&entry->leave_ageing_timer, xpon_igmp_timer_timeout, 0); 
#endif
						/*Multicast preview*/
						start_multicast_preview( dyn_entry, entry);
					}
				}
				if (entry)
				{
					qtime =  xpon_get_listenner_interval(port);
					entry->leave_count = last_query_cnt;
                    MULTICAST_NOTICE_INFO("xpon_igmp_reportv3_handle entery change timer interval is %lu\n",qtime);
					mod_timer(&entry->leave_ageing_timer,qtime);	
				}
				else
					goto skb_drop;
			}
		}
		else
			goto skb_drop;
		
		grec = (struct igmpv3_grec*)((char*)grec + IGMPV3_GRP_REC_SIZE(grec));
	}
	
    MULTICAST_NOTICE_INFO("free skb.\n");
    kfree_skb(skb);
	return 1;
	
skb_drop:
	MULTICAST_NOTICE_INFO("xpon_igmp_reportv3_handle: free skb");	
	kfree_skb(skb);
	return 0;

}


int xpon_igmp_flow_handle(struct sk_buff* skb)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();

	if (xpon_is_ani_port(skb->dev->name)==0)
		goto skb_drop;
	
	if (igmp_conf->flag & XPON_IGMP_SNOOPING_DISABLED)
		return xpon_broadcast_to_port(skb);

	return xpon_data_pkt_handle(skb,XPON_MASK_IGMPV2);
	
skb_drop:
	kfree_skb(skb);
	return 0;		
}

int xpon_igmp_query_handle(struct sk_buff* skb)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();

	if (xpon_is_ani_port(skb->dev->name)==0)
		goto skb_drop;

	if (igmp_conf->flag & XPON_IGMP_SNOOPING_DISABLED)
		return xpon_broadcast_to_port(skb);

	return xpon_query_pkt_handle(skb,XPON_MASK_IGMPV2);
	
skb_drop:
	kfree_skb(skb);
	return 0;	
}

int xpon_igmp_leave_handle(struct sk_buff* skb)
{
    xPON_PortConf_t* port_conf = NULL;
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int port = 0;
    
    port = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
	if(port == -1){
		MULTICAST_ERROR_INFO("ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT return fail\n");
		goto skb_drop;
	}
	port_conf = xpon_port_conf_by_id(port);

	if(port_conf == NULL)
    {
        MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
        goto skb_drop;
    }


	if (igmp_conf->flag & XPON_IGMP_SNOOPING_DISABLED)
		goto skb_free;
	
	if (!(port_conf->proto_mask & XPON_MASK_IGMPV2))
		goto skb_drop;

    if (igmp_conf->fwd_tbl->fwd_mode == MULTCAST_CONTROL_MODE){
        goto skb_free;
    }

	return xpon_leave_pkt_handle(skb,XPON_MASK_IGMPV2, port);

skb_free:
    MULTICAST_NOTICE_INFO("free skb.\n");
	kfree_skb(skb);
	return 1;

skb_drop:
	kfree_skb(skb);
	return 0;	
}


int xpon_igmp_reportv3_handle(struct sk_buff* skb)
{
    int port = 0;
    xPON_PortConf_t* port_conf = NULL;
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
    
    port = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
	if(port == -1){
		MULTICAST_ERROR_INFO("ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT return fail\n");
		goto skb_drop;
	}
	port_conf = xpon_port_conf_by_id(port);

	if(port_conf == NULL)
    {
        MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
        goto skb_drop;
    }
	
	
	if (igmp_conf->flag & XPON_IGMP_SNOOPING_DISABLED)
		goto skb_free;
		
	if (!(port_conf->proto_mask & XPON_MASK_IGMPV3 ))
		goto skb_drop;

	if (igmp_conf->fwd_tbl->fwd_mode==MULTCAST_CONTROL_MODE)
		goto skb_free;
	
	return xpon_igmpv3_pkt_handle(skb, port);

skb_free:
	MULTICAST_NOTICE_INFO("free skb.\n");
	kfree_skb(skb);
	return 1;

skb_drop:
	kfree_skb(skb);
	return 0;

}

int xpon_igmp_reportv2_handle(struct sk_buff* skb)
{
	int port = 0;
	xPON_PortConf_t* port_conf = NULL;
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
    
    port = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
	if(port == -1){
		MULTICAST_ERROR_INFO("ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT return fail\n");
		goto skb_drop;
	}
	port_conf = xpon_port_conf_by_id(port);

	if(port_conf == NULL)
    {
        MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
        goto skb_drop;
    }


	if (igmp_conf->flag & XPON_IGMP_SNOOPING_DISABLED)
		goto skb_free;
	
	if (!(port_conf->proto_mask & XPON_MASK_IGMPV2 ))
		goto skb_drop;

	if (igmp_conf->fwd_tbl->fwd_mode==MULTCAST_CONTROL_MODE)
		goto skb_free;

	return xpon_report_pkt_handle(skb,XPON_MASK_IGMPV2, port);

skb_free:
    MULTICAST_NOTICE_INFO("free skb.\n");
	kfree_skb(skb);
	return 1;

skb_drop:
	kfree_skb(skb);
	return 0;
}


int xpon_igmp_reportv1_handle(struct sk_buff* skb)
{
    int port = 0;
    xPON_PortConf_t* port_conf = NULL;
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
    
	if(IS_IGMPv1_DISCARD){
        MULTICAST_WARN_INFO("IGMPv1 discard!\n");
        goto skb_drop;
	}
	
    port = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
	if(port == -1){
		MULTICAST_ERROR_INFO("ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT return fail\n");
		goto skb_drop;
	}
	port_conf = xpon_port_conf_by_id(port);

	if(port_conf == NULL)
    {
        MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
        goto skb_drop;
    }

	
	if (igmp_conf->flag & XPON_IGMP_SNOOPING_DISABLED)
		goto skb_free;
	
	if (!(port_conf->proto_mask & XPON_MASK_IGMPV1 ))
		goto skb_drop;
    
	if (igmp_conf->fwd_tbl->fwd_mode == MULTCAST_CONTROL_MODE)
		goto skb_free;
	
	return xpon_report_pkt_handle(skb,XPON_MASK_IGMPV2, port);

skb_free:
    MULTICAST_NOTICE_INFO("free skb.\n");
	kfree_skb(skb);
	return 1;

skb_drop:
	kfree_skb(skb);
	return 0;
}


int xpon_ipv4_incoming_handle(struct sk_buff* skb, ENUM_XPON_IGMP_DIRECTION_t dir)
{
	int ip_type;
	int igmp_type;
    int iret = 0;
	
	ip_type = xpon_get_ip_type(skb);

	if (ip_type == PROTOCOL_UDP)	
		return xpon_igmp_flow_handle(skb);
	
	igmp_type = xpon_get_igmp_type(skb);

	MULTICAST_NOTICE_INFO("igmp_type: %d.\n",igmp_type);

	if(dir == E_XPON_IGMP_DIRECTION_DOWNSTREAM 
		&& (skb->dev != NULL && !strncmp(skb->dev->name, "pon", 3))) /* it means this flow is downstream */
	{   
		if(igmp_type == IGMPV2_HOST_MEMBERSHIP_REPORT
			|| igmp_type == IGMPV3_HOST_MEMBERSHIP_REPORT
			|| igmp_type == IGMP_HOST_LEAVE_MESSAGE) 
		{
			kfree_skb(skb);
			return 0;
		}
	}

	switch (igmp_type) 
	{
		case IGMP_HOST_MEMBERSHIP_REPORT:
                iret = xpon_igmp_reportv1_handle(skb);
			break;
		case IGMPV2_HOST_MEMBERSHIP_REPORT:
			    iret = xpon_igmp_reportv2_handle(skb);
			break;
		case IGMPV3_HOST_MEMBERSHIP_REPORT:
                iret = xpon_igmp_reportv3_handle(skb);
			break;
		case IGMP_HOST_LEAVE_MESSAGE:
           		iret = xpon_igmp_leave_handle(skb);
			break;
		case IGMP_HOST_MEMBERSHIP_QUERY:
                iret = xpon_igmp_query_handle(skb);
			break;
		default:
			kfree_skb(skb);
			return 0;
	}
	return iret;	
}

struct mld2_grecrd
{
	__u8 grec_type;
	__u8 grec_auxwords;
	__be16 grec_nsrcs;
	struct in6_addr grec_mca;
	struct in6_addr grec_src[0];
};

struct mld2_reportpkt
{
	__u8 type;
	__u8 resv1;
	__sum16 csum;
	__be16 rev2;
	__be16 ngrec;
	struct mld2_grecrd grec[0];
};
#define MLD2_MODE_IS_INCLUDE	1
#define MLD2_MODE_IS_EXCLUDE	2
#define MLD2_CHANGE_TO_INCLUDE	3
#define MLD2_CHANGE_TO_EXCLUDE	4
#define MLD2_ALLOW_NEW_SOURCES	5
#define MLD2_BLOCK_OLD_SOURCES	6

#define MLDV2_GRP_REC_SIZE(x) (sizeof(struct mld2_grecrd) + (ntohs(x->grec_nsrcs))*16)

int xpon_mldv2_pkt_handle(struct sk_buff* skb)
{
	struct mld2_grecrd *grec;
	struct mld2_reportpkt * report;
	xPON_FwdEntry_t* entry = NULL ;
	int i,j,grp_num,src_num,port;
	int vid = 0;
	unsigned char grp_addr[16],src_addr[16];
    xPON_PortConf_t* port_conf = NULL;
	unsigned long qtime;
	unsigned char client_ip[16];
	xPON_WhiteList_Entry_List_t* dyn_entry = NULL;
	unsigned long lastquery_timer = 0;

	port = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
	if(port == -1){
		MULTICAST_ERROR_INFO("ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT return fail\n");
		goto skb_drop;
	}
	port_conf = xpon_port_conf_by_id(port);

	if(port_conf == NULL)
    {
        MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
        goto skb_drop;
    }
	
	vid = xpon_get_outmost_vid(skb);

	report = xpon_get_transport_header(skb);
	if( report == NULL )
	{
		MULTICAST_ERROR_INFO("xpon_get_transport_header RETURN NULL Pointer\n");
		goto skb_drop;
	}
	grp_num = ntohs(report->ngrec);
		
	if (port_conf->maxgroup > 0 && grp_num > port_conf->maxgroup)
		goto skb_drop;

	memset(client_ip,0,16);
	xpon_get_igmp_srcip(client_ip,skb);
	
    MULTICAST_NOTICE_INFO("**report->ngrec = %d*****\n", ntohs(report->ngrec));
    
	grec = report->grec;
	for(i = 0; i < grp_num ;i++)
	{
		src_num =  ntohs(grec->grec_nsrcs);
		memset(grp_addr,0,16);
		memcpy(grp_addr,(unsigned char*)&grec->grec_mca,16);
	
        MULTICAST_NOTICE_INFO("**grec_type = %d, src_num = %d*****\n", grec->grec_type, src_num);
		
        switch(grec->grec_type)
		{
			case MLD2_MODE_IS_INCLUDE:
			case MLD2_CHANGE_TO_INCLUDE:
			case MLD2_ALLOW_NEW_SOURCES:
				for(j = 0; j <src_num ; j++)
				{
					memset(src_addr,0,16);
					memcpy(src_addr,(unsigned char*)&grec->grec_src[j],16);					
					entry = xpon_fwdtbl_find(MULTCASTCTL_IPV6_DA,port,vid,grp_addr,src_addr,XPON_MASK_MLDV2);
					if (entry==NULL)
					{
						if(-1 == check_max_group(skb, port, XPON_MASK_MLDV2)
	                        || NOT_MATCH_DYNAMIC_ACL == upstream_dynamic_acl_ctr_mldv2(port,grp_addr,src_addr,&dyn_entry))
	                        goto skb_drop;
						
						if(TRUE == is_forword_allowed(dyn_entry)){
							entry = xpon_fwdtbl_add(MULTCASTCTL_IPV6_DA,port,vid,grp_addr,src_addr, XPON_MASK_MLDV2 | XPON_MODE_INCLUDE,client_ip);
							if (entry==NULL)
								goto skb_drop;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)				
							setup_timer(&entry->leave_ageing_timer,xpon_mld_timer_timeout,(unsigned long)entry);
#else
							timer_setup(&entry->leave_ageing_timer, xpon_mld_timer_timeout, 0); 
#endif
							/*Multicast preview*/
							start_multicast_preview( dyn_entry, entry);
						}
					}else{
						if(XPON_MODE_EXCLUDE == (entry->flag&0xff00)){
							entry->flag = XPON_MASK_MLDV2 | XPON_MODE_INCLUDE;
							if(MLD2_ALLOW_NEW_SOURCES == grec->grec_type){
								//if (port_conf->fastleave){
									del_timer(&entry->leave_ageing_timer);
									xpon_fwdtbl_operate_entry(2,entry);
									break;
								//}
							}
						}
					}
					if(entry)
					{
						entry->leave_count = last_query_cnt;
						qtime =  xpon_get_listenner_interval(port);
						mod_timer(&entry->leave_ageing_timer,qtime);
					}
					else
						goto skb_drop;
				}
				if (src_num==0)
				{
					if(MLD2_CHANGE_TO_INCLUDE == grec->grec_type){	
						memset(src_addr,0,16);
                        entry = xpon_fwdtbl_find(MULTCASTCTL_IPV6_DA,port,vid,grp_addr,src_addr,XPON_MASK_MLDV2);
                        if(entry){
                            del_timer(&entry->leave_ageing_timer);
                            xpon_fwdtbl_operate_entry(2,entry);
                        }
					}
				}
				break;
				
			case MLD2_MODE_IS_EXCLUDE:
			case MLD2_CHANGE_TO_EXCLUDE:
			case MLD2_BLOCK_OLD_SOURCES:

				for(j = 0; j < src_num; j++)
				{
					memset(src_addr,0,16);
					memcpy(src_addr,(unsigned char*)&grec->grec_src[j],16);		
					entry = xpon_fwdtbl_find(MULTCASTCTL_IPV6_DA,port,vid,grp_addr,src_addr,XPON_MASK_MLDV2);
					if (entry==NULL || (entry->flag&0xff)==XPON_MASK_MLDV1)
					{
						if(-1 == check_max_group(skb, port, XPON_MASK_MLDV2)
	                        || NOT_MATCH_DYNAMIC_ACL == upstream_dynamic_acl_ctr_mldv2(port,grp_addr,src_addr,&dyn_entry))
	                        goto skb_drop;
						
						if(TRUE == is_forword_allowed(dyn_entry)){
							entry = xpon_fwdtbl_add(MULTCASTCTL_IPV6_DA,port,vid,grp_addr,src_addr,XPON_MASK_MLDV2 | XPON_MODE_EXCLUDE,client_ip);
							if (entry==NULL)
								goto skb_drop;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)				
							setup_timer(&entry->leave_ageing_timer,xpon_mld_timer_timeout,(unsigned long)entry);
#else
							timer_setup(&entry->leave_ageing_timer, xpon_mld_timer_timeout, 0); 
#endif
							/*Multicast preview*/
							start_multicast_preview( dyn_entry, entry);
						}
					}else{
						if(XPON_MODE_INCLUDE == (entry->flag&0xff00)){
							entry->flag = XPON_MASK_MLDV2 | XPON_MODE_EXCLUDE;
							if(MLD2_BLOCK_OLD_SOURCES == grec->grec_type){
								entry->leave_count--;
								lastquery_timer = xpon_get_lastquery_interval(port);
								mod_timer(&entry->leave_ageing_timer,lastquery_timer);
								if (port_conf->fastleave || entry->leave_count <= 0){								
									del_timer(&entry->leave_ageing_timer);
									xpon_fwdtbl_operate_entry(2,entry);
									break;
								}
							}
						}
					}
					if(entry && MLD2_BLOCK_OLD_SOURCES != grec->grec_type)
					{
						entry->leave_count = last_query_cnt;
						qtime =  xpon_get_listenner_interval(port);
						mod_timer(&entry->leave_ageing_timer,qtime);
					}
					else
						goto skb_drop;
				}
				if (src_num==0)
				{
					memset(src_addr,0,16);
					entry = xpon_fwdtbl_find(MULTCASTCTL_IPV6_DA,port,vid,grp_addr,src_addr,XPON_MASK_MLDV2);
					if (entry==NULL)
					{
						if(-1 == check_max_group(skb, port, XPON_MASK_MLDV2)
	                        || NOT_MATCH_DYNAMIC_ACL == upstream_dynamic_acl_ctr_mldv2(port,grp_addr,src_addr,&dyn_entry))
	                        goto skb_drop;
						
						if(TRUE == is_forword_allowed(dyn_entry)){
							entry = xpon_fwdtbl_add(MULTCASTCTL_IPV6_DA,port,vid,grp_addr,src_addr,XPON_MASK_MLDV2 | XPON_MODE_EXCLUDE,client_ip);
							if (entry==NULL)
								goto skb_drop;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)				
							setup_timer(&entry->leave_ageing_timer,xpon_igmp_timer_timeout,(unsigned long)entry);
#else
							timer_setup(&entry->leave_ageing_timer, xpon_igmp_timer_timeout, 0); 
#endif
							/*Multicast preview*/
							start_multicast_preview( dyn_entry, entry);
						}
					}
					if (entry)
					{
						entry->leave_count = last_query_cnt;
						qtime =  xpon_get_listenner_interval(port);
						mod_timer(&entry->leave_ageing_timer,qtime);	
					}
					else
						goto skb_drop;
				}
				break;	


			default:
				goto skb_drop;
		}
		grec = (struct mld2_grecrd*)((char*)grec + MLDV2_GRP_REC_SIZE(grec));
	}

    MULTICAST_NOTICE_INFO("free skb.\n");
	kfree_skb(skb);
	return 1;
		
skb_drop:
	MULTICAST_NOTICE_INFO("xpon_mldv2_pkt_handle:free skb.\n");	
	kfree_skb(skb);
	return 0;
}


int xpon_mld_reportv2_handle(struct sk_buff* skb)
{
    xPON_PortConf_t* port_conf = NULL;
    xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int port = 0;
    
    port = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
	if(port == -1){
		MULTICAST_ERROR_INFO("ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT return fail\n");
		goto skb_drop;
	}
	port_conf = xpon_port_conf_by_id(port);
    
    if(port_conf == NULL)
    {
        MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
        goto skb_drop;
    }
		
	if (igmp_conf->flag & XPON_MLD_SNOOPING_DISABLED)
		goto skb_free;
    
	MULTICAST_NOTICE_INFO("proto_mask = %d.\n", port_conf->proto_mask);

    if (!(port_conf->proto_mask & XPON_MASK_MLDV2 ))
		goto skb_drop;
    
	if (igmp_conf->fwd_tbl->fwd_mode==MULTCAST_CONTROL_MODE)
		goto skb_free;
    

	return xpon_mldv2_pkt_handle(skb);

skb_free:
    MULTICAST_NOTICE_INFO("free skb.\n");
	kfree_skb(skb);
	return 1;
    
skb_drop:
	kfree_skb(skb);
	return 0;
}

int xpon_mld_reportv1_handle(struct sk_buff* skb)
{
    int port = 0;
    xPON_PortConf_t* port_conf = NULL;
    xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
    
    port = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
	if(port == -1){
		MULTICAST_ERROR_INFO("ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT return fail\n");
		goto skb_drop;
	}
	port_conf = xpon_port_conf_by_id(port);

	if(port_conf == NULL)
    {
        MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
        goto skb_drop;
    }
	
		
	if (igmp_conf->flag & XPON_MLD_SNOOPING_DISABLED)
		goto skb_free;
		
	if (!(port_conf->proto_mask & XPON_MASK_MLDV1 ))
		goto skb_drop;

	if (igmp_conf->fwd_tbl->fwd_mode==MULTCAST_CONTROL_MODE)
		goto skb_free;

	return xpon_report_pkt_handle(skb,XPON_MASK_MLDV1, port);

skb_free:
    MULTICAST_NOTICE_INFO("free skb.\n");
	kfree_skb(skb);
	return 1;
    
skb_drop:
	kfree_skb(skb);
	return 0;	
}

int xpon_mld_leave_handle(struct sk_buff* skb)
{
    xPON_PortConf_t* port_conf = NULL;
    xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int port = 0;
		
	port = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
	if(port == -1){
		MULTICAST_ERROR_INFO("ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT return fail\n");
		goto skb_drop;
	}
	port_conf = xpon_port_conf_by_id(port);

	
	if(port_conf == NULL)
    {
        MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
        goto skb_drop;
    }
    	
	if (igmp_conf->flag & XPON_MLD_SNOOPING_DISABLED)
		goto skb_free;
		
	if (!(port_conf->proto_mask & XPON_MASK_MLDV1))
		goto skb_drop;

	if (igmp_conf->fwd_tbl->fwd_mode==MULTCAST_CONTROL_MODE)
		goto skb_free;

	return xpon_leave_pkt_handle(skb,XPON_MASK_MLDV1, port);

skb_free:
    MULTICAST_NOTICE_INFO("free skb.\n");
	kfree_skb(skb);
	return 1;
    
skb_drop:
	kfree_skb(skb);
	return 0;		
}


int xpon_mld_query_handle(struct sk_buff* skb)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();

	if (xpon_is_ani_port(skb->dev->name)==0)
		goto skb_drop;

	if (igmp_conf->flag & XPON_MLD_SNOOPING_DISABLED)
		return xpon_broadcast_to_port(skb);

	return xpon_query_pkt_handle(skb,XPON_MASK_MLDV1);
	
skb_drop:
	kfree_skb(skb);
	return 0;

}

int xpon_mld_flow_handle(struct sk_buff* skb)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();

	if (xpon_is_ani_port(skb->dev->name)==0)
		goto skb_drop;

	if (igmp_conf->flag & XPON_MLD_SNOOPING_DISABLED){
		MULTICAST_NOTICE_INFO("xpon_mld_flow_handle XPON_MLD_SNOOPING_DISABLED.\n");	
		return xpon_broadcast_to_port(skb);
	}
	return xpon_data_pkt_handle(skb,XPON_MASK_MLDV1);

skb_drop:
	kfree_skb(skb);
	return 0;	
}

int xpon_ipv6_incoming_handle(struct sk_buff* skb, ENUM_XPON_IGMP_DIRECTION_t dir)
{
	int mld_type;
	int ipv6_type = xpon_get_ipv6_type(skb);
	int iret = 0;
	
	if (ipv6_type == PROTOCOL_UDP6)	
		return xpon_mld_flow_handle(skb);
		
	mld_type = xpon_get_mld_type(skb);


	MULTICAST_NOTICE_INFO("xpon_ipv6_incoming_handle(): mld = %d.\n",mld_type);	
	
	if(dir == E_XPON_IGMP_DIRECTION_DOWNSTREAM
		&& (skb->dev != NULL && !strncmp(skb->dev->name, "pon", 3)))/* it means this flow is downstream */
	{
		if(mld_type == ICMPV6_MGM_REDUCTION
			|| mld_type == ICMPV6_MGM_REPORT
			|| mld_type == ICMPV6_MLD2_REPORT)
		{
			kfree_skb(skb);
			return 0;
		}
	}

	switch (mld_type) 
	{
		case ICMPV6_MGM_REPORT:
			iret = xpon_mld_reportv1_handle(skb);
			break;
		case ICMPV6_MLD2_REPORT:
			iret = xpon_mld_reportv2_handle(skb);
			break;
		case ICMPV6_MGM_REDUCTION:
            iret = xpon_mld_leave_handle(skb);	
			break;
		case ICMPV6_MGM_QUERY:
			iret = xpon_mld_query_handle(skb);
			break;
		default:
			kfree_skb(skb);
			return 0;
	}

	return iret;	
}


int xpon_compare_addr(unsigned char*  addr1,unsigned char* addr2,int len)
{
	int i;
	for(i=0;i<len;i++)
	{
		if (addr1[i] > addr2[i])
			return 1;
		if (addr1[i] < addr2[i])
			return -1;
	}	
	return 0;
}

int xpon_grp_addr_between(unsigned char* grp_addr,unsigned char* start, unsigned char* end)
{
	if (xpon_compare_addr(grp_addr,start,16) < 0)
		return 0;

	if (xpon_compare_addr(grp_addr,end,16)> 0)
		return 0;

	return 1;
}


int xpon_pass_access_control(xPON_FwdEntry_t*  fwd_entry)
{
	xPON_PortConf_t* port_conf;
	xPON_WhiteList_Entry_List_t* entry;
	struct list_head*  white_list;

	if (fwd_entry==NULL)
		return 0;
	
	port_conf =  xpon_port_conf_by_id(fwd_entry->port);
	if(port_conf==NULL)
		return 0;
	
	white_list = &(port_conf->dyn_list);

	if(list_empty(white_list))
		return 1;

	if (port_conf->unauthor)
		return 1;


	list_for_each_entry_rcu(entry,white_list,list)
	{	
		if (xpon_grp_addr_between(fwd_entry->grp_addr,entry->grpstart,entry->grpend)==0)	
			continue;
		
		if (fwd_entry->vid>0 && entry->vlanid>0 && fwd_entry->vid != entry->vlanid)
			continue;
				
		if ((fwd_entry->flag&0x100) && xpon_is_non_zero(fwd_entry->src_addr,16) && xpon_is_non_zero(entry->srcip,16) && memcmp(fwd_entry->src_addr,entry->srcip,16))
			continue;
		
		if ((fwd_entry->flag&0x200) && xpon_is_non_zero(fwd_entry->src_addr,16) && xpon_is_non_zero(entry->srcip,16) && memcmp(fwd_entry->src_addr,entry->srcip,16)==0)
			continue;	
		
		return 1;
	}
	
	return 0;
}



typedef struct xPON_RateControl_s
{
	unsigned int last_time;
	unsigned int pkt_num;
	unsigned int pkt_byte;
}xPON_RateControl_t;

static xPON_RateControl_t  rate_control[MAX_UNI_PORT_NUM];

static xPON_RateControl_t*  uni_rate_control[MAX_UNI_PORT_NUM]; 

void xpon_rate_control_init(void)
{
	int i;
	memset(rate_control,0,sizeof(rate_control));
	for(i = 0; i < MAX_UNI_PORT_NUM; i++)
	{
		uni_rate_control[i] = &rate_control[i];
	}
	return ;
}

int xpon_pass_rate_limit(struct sk_buff* skb)
{    
	int port = 0;
	xPON_RateControl_t* rate_control = NULL;
	xPON_PortConf_t*  port_conf =NULL;
	unsigned int  diff = 0;


	port = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
	if(port == -1){
		MULTICAST_ERROR_INFO("ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT return fail\n");
		return  -1;
	}
	
	if (port >= 0)
		rate_control = uni_rate_control[port];
	else
		return -1;

	port_conf = xpon_port_conf_by_id(port);
	if(port_conf == NULL)
    {
        MULTICAST_ERROR_INFO("xpon_port_conf_by_name RETURN NULL Pointer\n");
        return -1;
    }
    
	diff = jiffies - rate_control->last_time;
    
	if(port_conf->maxrate == 0)
		return 1;
	
	xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n xpon_pass_rate_limit(): maxrate = %d port = %d diff = %d",port_conf->maxrate,port,diff);
	
	if (diff > 100)
	{
		rate_control->last_time = jiffies;
		rate_control->pkt_num = 1;	
		rate_control->pkt_byte = skb->len;
	}
	else
	{
		rate_control->pkt_num += 1; 
		if(port_conf->maxrate < rate_control->pkt_num)
			return 0;
	}
	
	return 1;
}


int xpon_uni_incoming_handle(struct sk_buff* skb)
{
	int pkt_type;
	int ret = 0;
	
	xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n ========>xpon_uni_incoming_handle");

	if (xpon_pass_rate_limit(skb)<=0)
		goto skb_drop;

	pkt_type = xpon_get_packet_type(skb);
	
	if (pkt_type == PACKET_IPV4)
    {
        ret = xpon_ipv4_incoming_handle(skb, E_XPON_IGMP_DIRECTION_UPSTREAM);
        if(DISCARD_LEAVE_MESSAGE == ret)
        {
            return DISCARD_LEAVE_MESSAGE;
        }
    }   
	else if (pkt_type == PACKET_IPV6)
    {
        ret = xpon_ipv6_incoming_handle(skb, E_XPON_IGMP_DIRECTION_UPSTREAM);
        if(DISCARD_LEAVE_MESSAGE == ret)
        {
            return DISCARD_LEAVE_MESSAGE;
        }
    }   
	else
		goto skb_drop;

	xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n <========xpon_uni_incoming_handle");

	return ret;

skb_drop:
	kfree_skb(skb);
	xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n <========xpon_uni_incoming_handle: free skb");
	return 0;
}

int xpon_ani_pass_mvlan(int vid)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int i;

	if (igmp_conf->flag & XPON_IGMP_MVLAN_DISABLED)
		return 1;

	for(i=0; i<igmp_conf->mvlan_num;i++)
	{
		if (igmp_conf->mul_vlan[i].mvlan_id==vid)
			return 1;
	}
	
	return 0;
}

int xpon_multi_should_passthrough(struct sk_buff* skb)
{
	int pkt_type = xpon_get_packet_type(skb);
	unsigned char* buff = xpon_get_transport_header(skb);
	unsigned short dst_port = 0;
	int type = xpon_get_ip_type(skb);
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();

	if(!(igmp_conf->flag & XPON_PROOCOL_AUTO_THROUGH))
	{
		return FALSE;
	}

	if(buff == NULL){
		return FALSE;
	}
	else{
		dst_port = ntohs(*((unsigned short*)(buff+2)));
	}

	/* for LLNMR RFC4795 pass through. */
	if(pkt_type == PACKET_IPV4 && (type == PROTOCOL_UDP) && (dst_port == 5355))
	{
		return TRUE;
	}

	if(pkt_type != PACKET_IPV6)
		return FALSE;
	
	/* dhcpv6 will pass through. */
	if((type == PROTOCOL_UDP6) && (dst_port == 546 || dst_port == 547 || dst_port == 5355))
	{
		return TRUE;
	}
	
	/* icmpv6 will pass through. */
	if (type == PROTOCOL_ICMPV6)
	{
		return TRUE;
	}
	
	return FALSE;
}

int xpon_ani_incoming_handle(struct sk_buff* skb)
{
	int vid = 0;
	int pkt_type = xpon_get_packet_type(skb);

	vid = xpon_get_outmost_vid(skb);
	
	if (xpon_multi_should_passthrough(skb))
	{
		skb->is_unknown_mul = 1;
		if(ra_sw_nat_hook_free)
			ra_sw_nat_hook_free(skb);
		xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n <========xpon_ani_incoming_handle: xpon_broadcast_to_port");
		return xpon_broadcast_to_port(skb);
	}

	if (xpon_ani_pass_mvlan(vid) <=0 )
		goto skb_drop;
	
	if (pkt_type==PACKET_IPV4)
		xpon_ipv4_incoming_handle(skb, E_XPON_IGMP_DIRECTION_DOWNSTREAM);
	else if (pkt_type == PACKET_IPV6)
		xpon_ipv6_incoming_handle(skb, E_XPON_IGMP_DIRECTION_DOWNSTREAM);
	else
		goto skb_drop;
	
	return 1;

skb_drop:
	kfree_skb(skb);
	xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n <========xpon_ani_incoming_handle: free skb");
	return 1;

	
}


//It is the hook entry that will be called by bridge module in br_input.c 
int xpon_igmp_incoming_hook(struct sk_buff* skb,int clone)
{
#if 0
	unsigned char *dest = eth_hdr(skb)->h_dest;
	struct sk_buff* skb2;
	struct net_device* dev = skb->dev;
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	int ret = 0;
	
	xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n ========>xpon_igmp_incoming_hook");

	if (!dev)
		return -1;

	///// if non multicast packet, do not handle it
	if (!xpon_is_multicast_addr(dest))
	{
		xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n <========xpon_igmp_incoming_hook: non mul pkt");
		return -1;
	}
	//if IGMP is disable ,do not handle it
	if (igmp_conf->flag & XPON_IGMP_DISABLED)
	{
		xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n <========xpon_igmp_incoming_hook: disabled");	
		return -1;
	}

	if (xpon_is_uni_port(dev->name)==0 && xpon_is_ani_port(dev->name)==0)
	{
		xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n <========xpon_igmp_incoming_hook: invalid port");	
		return -1;
	}
	//// if non IGMP or Flow packet , do not handle it
	if (!xpon_is_igmp_pkt(skb) && !xpon_is_data_pkt(skb))
	{
		xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n <========xpon_igmp_incoming_hook: invalid pkt");
		return 0;
	}
	
	if(clone)	
	{
		skb2 = skb_copy(skb,GFP_ATOMIC);
		if (skb2 == NULL)
		{
			dev->stats.tx_dropped++;
			return 0;
		}
		skb = skb2;
	}

	////Handle packet from UNI sode port
	if (xpon_is_uni_port(dev->name))
		xpon_uni_incoming_handle(skb);

	////Handle packet from ANI side port
	if (xpon_is_ani_port(dev->name))
		 xpon_ani_incoming_handle(skb);
	
	xpon_igmp_debug(XPON_IGMP_DEBUG_TRACE,"\n <========xpon_igmp_incoming_hook");
	
	return 1;
#endif
	return 0;
}

//It is the hook function that monitor the add or delete operation of bridge itterface  
int xpon_igmp_port_hook(struct net_device* dev,int op)
{
/*
	xPON_PortEntry_t*  port_entry;
	xPON_AniEntry_t* ani_entry;
	
	if (!dev)
		return 0;

	port_entry = xpon_port_entry_by_name(dev->name);
	if (port_entry)
	{
		if (op == 1)
		{
			port_entry->port_flag |= XPON_BRIDGE_PORT;
		}
		if (op == 2)
		{
			port_entry->port_flag &= ~XPON_BRIDGE_PORT;	
		}
		return 0;
	}
	
	ani_entry = xpon_get_ani_entry(dev->name);
	if (ani_entry)
	{
		if (op == 1)
		{
			ani_entry->ani_flag |= XPON_BRIDGE_PORT;	
		}
		if (op == 2)
		{
			ani_entry->ani_flag &= ~XPON_BRIDGE_PORT;	
		}
		return 0;
	}
*/	
	return 0;
}

static int xpon_sfu_send_to_router_by_flag(struct sk_buff* skb,int flag)
{
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	struct net_device* dev = NULL;
	char* dev_name = NULL;
	int i = 0;
    unsigned char onu_link_type = 0;
    int port = 0;
    xPON_PortConf_t* conf = NULL;
    
    port = ENCT_HOOK_XPON_ETH_MAP_DEV_NAME_TO_PORT(skb->dev->name);
    
	conf = xpon_port_conf_by_id(port);
	if(NULL == conf){
		goto skb_drop;
	}
    
	for(i=0; i<igmp_conf->ani_num; i++)
	{
		if (igmp_conf->ani_port[i].ani_flag & flag)
		{
			dev_name = igmp_conf->ani_port[i].ani_name;
			break;
		}
	}

	if (dev_name==NULL)
		goto skb_drop;
	
	dev = xpon_get_dev_by_name(dev_name);
	if (dev == NULL)
		goto skb_drop;

    /*epon  ctc multicast, 1 GPON, 2 EPON
       fwd_mode = 1 ctl multicast 
       fwd_mode = 0  snooping*/
    ECNT_API_XPON_MODE_GET(&onu_link_type);
    if(2 == onu_link_type &&  MULTCAST_CONTROL_MODE == igmp_conf->fwd_tbl->fwd_mode)
    {
        conf->up_vlan_mode = MULTCAST_UPTAG_REPLACE_VID_PID;
        conf->up_vlan_tci = port;
        skb->pon_vlan_flag |=  PON_PKT_INSERT_FLAG;
    }
    else if(2 == onu_link_type &&  MULTCAST_SNOOPING_MODE == igmp_conf->fwd_tbl->fwd_mode){
        conf->up_vlan_mode = MULTCAST_UPTAG_TRANSPARENT;
    }

	xpon_br_forward(dev,skb);
	dev_put(dev);
	return 0;

skb_drop:
	MULTICAST_CRITIC_INFO("skb_drop\n");
	kfree_skb(skb);

	return 0;
}

static int frame_is_legal_multicast_pkt(struct sk_buff* skb, int direct) 
{
    unsigned char *dest = eth_hdr(skb)->h_dest;
    struct net_device* dev = skb->dev;
	int port = 0;

    if (NULL == dev)
    {
        return false;
    }

    if (!xpon_is_multicast_addr(dest))
    {
        return false;
    }
    
    if(MULTICAST_UP_STREAM == direct)
    {

        if(skb->pon_vlan_flag & PON_PKT_FROM_CPE)
        {
           MULTICAST_CRITIC_INFO(" packet from CPE.\n");
           return false;
        }
        
        if(0 == (skb->pon_vlan_flag & PON_PKT_FROM_LAN))
        {
            MULTICAST_CRITIC_INFO("fram is not from uni port.\n");
            return false;
        }

		port = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
		if(port == -1){
			MULTICAST_ERROR_INFO("ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT return fail\n");
			return false;
		}
		
        if(!xpon_is_igmp_pkt(skb) && !xpon_multi_should_passthrough(skb))
        {
            MULTICAST_DEBUG_INFO("upstream multicast only deal with protocol frame.\n");
            return false;
        }

    }
    else
    {
        if(skb->pon_vlan_flag & PON_PKT_FROM_CPE)
        {
            MULTICAST_CRITIC_INFO(" packet from CPE.\n");
            return false;
        }

        if(0 == (skb->pon_vlan_flag & PON_PKT_FROM_WAN))
        {
            MULTICAST_CRITIC_INFO("fram is not from wan port.\n");
            return false;
        }
        
        if(!xpon_is_ani_port(dev->name))
        {
            MULTICAST_CRITIC_INFO("fram is not from ani port.\n");
            return false;
        }
        
        //downstream deal with protocol frame and udp data flow 
        if (!xpon_is_igmp_pkt(skb) && !xpon_is_data_pkt(skb))
        {
            MULTICAST_ERROR_INFO("downstream can't deal invalid pkt.\n");
            return false;
        }
    }

    return true;
}

static int xpon_up_igmp_filter_src(unsigned char *src_ip, xPON_WhiteList_Entry_List_t *white_list_entry)
{
    if (0 == white_list_entry->srcip[0]) 
        return 1;
    if (0 == xpon_compare_addr(white_list_entry->srcip, src_ip, 16))
        return 2;
    return 0;
}

xPON_User_Subscribe_Entry_t* xpon_user_subscribe_find(int port, int index, unsigned char* src_ip, unsigned char* program_ip)
{
    xPON_PortStatus_t * status = xpon_port_status_by_id(port);
    xPON_User_Subscribe_Entry_t * entry = NULL;
	struct list_head*  us_list = NULL;

	if (!status)
		return NULL;

	us_list = &(status->user_subscribe_list);
	
	if (list_empty(us_list))
		return NULL;

    if (XPON_PORT_USER_SUBSCRIBE_DONT_CARE_INDEX == index)
    {
    	if (0 == *src_ip)
    	{
        	list_for_each_entry(entry,us_list,list)
        	{
        	    if (0 == xpon_compare_addr(entry->program_ip, program_ip, 16))
        		{
        			return entry;
        		}
        	}
        }
        else
        {
        	list_for_each_entry(entry,us_list,list)
        	{
        		if ((0 == xpon_compare_addr(entry->srcip, src_ip, 16)) && (0 == xpon_compare_addr(entry->program_ip, program_ip, 16)))
        		{
        			return entry;
        		}
        	}
    	}
	}
	else
	{
    	list_for_each_entry(entry,us_list,list)
    	{
    	    if (index == entry->index)
    		{
    			return entry;
    		}
    	}	    
	}
	
	return NULL;
}

int xpon_user_subscribe_add(int port, unsigned char* src_ip, unsigned char* program_ip, xPON_WhiteList_Entry_List_t * white_list_entry)
{
    xPON_User_Subscribe_Entry_t * entry = NULL;
    xPON_PortStatus_t * status = xpon_port_status_by_id(port);
    struct list_head * us_list = NULL;
	
	if (!status)
		return -1;

    us_list = &(status->user_subscribe_list);

	entry = (xPON_User_Subscribe_Entry_t*) xpon_alloc(sizeof(xPON_User_Subscribe_Entry_t));
	if (entry==NULL)
		return -1;
	
	memcpy(entry->srcip, src_ip, 16);
	memcpy(entry->program_ip, program_ip, 16);
	entry->dyn_list_entry = white_list_entry;
	entry->index = status->user_subscribe_cnt;
	status->user_subscribe_cnt++;
	list_add_tail(&entry->list, us_list);
	
	return 0;
}

int xpon_user_subscribe_del(int port, unsigned char* src_ip, unsigned char* program_ip)
{
    xPON_User_Subscribe_Entry_t * entry = NULL;
    xPON_PortStatus_t * status = xpon_port_status_by_id(port);
    struct list_head*  us_list = NULL;
    int index = 0;

    if(status == NULL)
    {
        MULTICAST_ERROR_INFO("Status is NULL!\n");
        return -1;
    }
	us_list = &(status->user_subscribe_list);	
	if (list_empty(us_list))
		return -1;
    
	entry = xpon_user_subscribe_find(port, XPON_PORT_USER_SUBSCRIBE_DONT_CARE_INDEX, src_ip, program_ip);
	
	if (!entry || !status)
		return -1;

	list_del(&entry->list);
	xpon_free(entry);
	status->user_subscribe_cnt--;
    
    list_for_each_entry(entry,us_list,list)
    {
        entry->index = index;
        index++;
    }
    
	return 0;
}

static void xpon_port_status_update_pmc(int port, int join, int bw_exceeded, int used_bw)
{
	xPON_PortStatus_t * port_status = NULL;   
    port_status = xpon_port_status_by_id(port);
	
	if (!port_status)
	{
		return ;
	}
	
	if (join)
	{
	    port_status->pmc.join_msg++;
	}

	if (bw_exceeded)
	{
	    port_status->pmc.bw_exceeded++;
	}

	if (used_bw)
	{
	    port_status->pmc.current_mcast_bw += used_bw;
	}
 
	return;
}

static int xpon_user_subscribe_handle(int port, int action, int vlan,unsigned char *src_ip, unsigned char *program_ip, xPON_WhiteList_Entry_List_t * white_list_entry)
{
    xPON_User_Subscribe_Entry_t * entry = NULL;
    unsigned char null_ip = 0;
    int bw = 0, entry_vid = -1;

    if (action)
    {
        if (NULL != xpon_user_subscribe_find(port, XPON_PORT_USER_SUBSCRIBE_DONT_CARE_INDEX, src_ip, program_ip))
        {
            MULTICAST_TRACE_INFO("Same Entry Found!\n");
            return XPON_PORT_USER_SUBSCRIBE_FOUND_SAME_ENTRY;
        }
        else if (NULL != xpon_user_subscribe_find(port, XPON_PORT_USER_SUBSCRIBE_DONT_CARE_INDEX, &null_ip, program_ip))
        {
            MULTICAST_TRACE_INFO("Same Program Found!\n");
            xpon_user_subscribe_add(port, src_ip, program_ip, white_list_entry);
            return XPON_PORT_USER_SUBSCRIBE_FOUND_SAME_PROGRAM;
        }
        else
        {
            xpon_user_subscribe_add(port, src_ip, program_ip, white_list_entry);            
        }
        MULTICAST_TRACE_INFO("Add new subscribe entry!\n");
    }
    else
    {
        entry = xpon_user_subscribe_find(port, XPON_PORT_USER_SUBSCRIBE_DONT_CARE_INDEX, src_ip, program_ip);
        if (entry)
        {
            bw = entry->dyn_list_entry->bandwidth;
            entry_vid = entry->dyn_list_entry->vlanid;
            if (((0 > vlan) && (0 == entry_vid)) || /*Untag rule*/
                (vlan == entry_vid)) /* Single-Tag rule*/
            { 
                xpon_user_subscribe_del(port, src_ip, program_ip);
                if (NULL == xpon_user_subscribe_find(port, XPON_PORT_USER_SUBSCRIBE_DONT_CARE_INDEX, &null_ip, program_ip))
                {                	
                    xpon_port_status_update_pmc(port, 0, 0, (0 - bw)); //TC_6.3.15.js
                }
                MULTICAST_TRACE_INFO("Delete subscribe entry!\n");
            }
            return XPON_PORT_USER_SUBSCRIBE_FOUND_SAME_ENTRY;
        }
    }
 	return XPON_PORT_USER_SUBSCRIBE_NOT_FOUND;
}

static int xpon_check_igmp_type(struct sk_buff* skb)
{
	int pkt_type, igmp_type, mld_type, ret = -1;

	pkt_type = xpon_get_packet_type(skb);	

	if (pkt_type == PACKET_IPV4)
    {
        igmp_type = xpon_get_igmp_type(skb);
    	switch (igmp_type) 
    	{
    		case IGMP_HOST_MEMBERSHIP_REPORT:
    		case IGMPV2_HOST_MEMBERSHIP_REPORT:
    		case IGMPV3_HOST_MEMBERSHIP_REPORT:
    		    ret = 1;
    			break;
    		case IGMP_HOST_LEAVE_MESSAGE:
    		    ret = 0;
    			break;
    		default:
                ret = -1;
    			break;
    	}
    }   
	else if (pkt_type == PACKET_IPV6)
    {
        mld_type = xpon_get_mld_type(skb);
    	switch (mld_type) 
    	{
    		case ICMPV6_MGM_REPORT:
    		case ICMPV6_MLD2_REPORT:
    			ret = 1;
    			break;
    		case ICMPV6_MGM_REDUCTION:
                ret = 0;
                break;
    		default:
                ret = -1;
                break;
    	}
    }  

    return ret;
}

static char * xpon_get_ingress_dev_info(struct sk_buff* skb)
{
#if/*TCSUPPORT_COMPILE*/ defined(TCSUPPORT_CT)
#ifdef TCSUPPORT_PORTBIND//CONFIG_PORT_BINDING
    return skb->orig_dev->name;
#endif
#else/*TCSUPPORT_COMPILE*/	
#ifdef TCSUPPORT_PORTBIND /*CONFIG_PORT_BINDING*/
    return skb->orig_dev_name;
#endif
#endif/*TCSUPPORT_COMPILE*/
    return NULL;
}

static int xpon_ds_mcast_rate_limit_handle(struct sk_buff* skb)
{
	xPON_PortConf_t * port_conf = NULL;    
	xPON_PortStatus_t * port_status = NULL;    
	xPON_WhiteList_Entry_List_t * white_list_entry = NULL;
	xPON_PortEntry_t * port_entry = NULL;
	struct list_head * static_white_list = NULL;
	struct list_head * dynamic_white_list = NULL;  
    unsigned char grp_addr[16] = {0}, src_ip[16] = {0};
    //const unsigned char *dest = eth_hdr(skb)->h_dest;
    int port_maxbw = 0, dy_entry_bw = 0;
    unsigned char port_bwe = 0, static_found = 0, dynamic_found = 0, src_match = 0, null_ip = 0;
    int ret = XPON_PORT_USER_SUBSCRIBE_NOT_FOUND;
	int eth_type = 0;
	int pkt_vlan = -1;
	int igmp_type = IGMPV2_HOST_MEMBERSHIP_REPORT;
	struct igmpv3_grec *grec = NULL;
	struct igmpv3_report * report = NULL;
	int port =0;

	if (NULL != skb)
	{
		igmp_type = xpon_get_igmp_type(skb);
		eth_type = xpon_get_packet_type(skb);
	}

	if (IGMPV3_HOST_MEMBERSHIP_REPORT == igmp_type)
	{	
		report = xpon_get_transport_header(skb);

		if (report != NULL)
		{
			grec = report->grec;
		}

		MULTICAST_TRACE_INFO("grec->grec_type = %d\n", grec->grec_type);
	}


	xpon_get_igmp_grpaddr(grp_addr, skb);
	xpon_get_igmp_srcip(src_ip, skb);
	pkt_vlan = xpon_get_outmost_vid(skb);

    DBG_PRINT_CLIENT_INFO(eth_type,pkt_vlan, src_ip, grp_addr);

	port = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
	if(port == -1){
		MULTICAST_ERROR_INFO("ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT return fail\n");
		return false;
	}
	
	port_entry = xpon_port_entry_by_id(port);
    if(NULL == port_entry)
        return false;

    /* if pkt is leave, we need to update user subscribe table */
    if ((1 != xpon_check_igmp_type(skb)) || ((grec != NULL) && IGMPV3_CHANGE_TO_INCLUDE == grec->grec_type))
    {
        if ((0 == xpon_check_igmp_type(skb)) || ((grec != NULL) && IGMPV3_CHANGE_TO_INCLUDE == grec->grec_type))
        {
        	MULTICAST_TRACE_INFO("port = %d, vlan = %d, src_ip = "NIPQUAD_FMT", grp_addr = "NIPQUAD_FMT"\n", port, pkt_vlan, NIPQUAD(src_ip), NIPQUAD(grp_addr));
            xpon_user_subscribe_handle(port, 0, pkt_vlan, src_ip, grp_addr, NULL);
        }
        return true;
    }
    
    port_conf = xpon_port_conf_by_id(port);
    port_status = xpon_port_status_by_id(port);
    
	if (!port_conf || !port_status)
		return false;
	
	port_maxbw = port_conf->maxbw;
	port_bwe = port_conf->bw_enforce;
	static_white_list  = &(port_conf->sta_list);
	dynamic_white_list = &(port_conf->dyn_list);

    MULTICAST_TRACE_INFO("1.Check static white list\n");
    /* 1.Check static white list */
	if (!list_empty(static_white_list))
    {
        list_for_each_entry(white_list_entry, static_white_list, list)
        {
            DBG_PRINT_ACL_ENTRY_INFO(eth_type, white_list_entry->index, white_list_entry->vlanid, white_list_entry->srcip, white_list_entry->grpstart, white_list_entry->grpend);
        	if (xpon_up_igmp_filter_src(src_ip, white_list_entry))
        	{
                if (xpon_grp_addr_between(grp_addr, white_list_entry->grpstart, white_list_entry->grpend))	
                {
                    if (((0 > pkt_vlan) && (0 == white_list_entry->vlanid)) || /*Untag rule*/
                        (pkt_vlan == white_list_entry->vlanid)) /* Single-Tag rule*/
                    {
                        static_found = 1;
                        break;
                    }
                }
        	}
    	}
    }
      
    if (!static_found)
    {
        /* 2.Check dynamic white list */ 
        MULTICAST_TRACE_INFO("2.Check dynamic white list\n");
        white_list_entry = NULL;
        if (!list_empty(dynamic_white_list))
        {
        	list_for_each_entry(white_list_entry, dynamic_white_list, list)
        	{
                DBG_PRINT_ACL_ENTRY_INFO(eth_type, white_list_entry->index, white_list_entry->vlanid, white_list_entry->srcip, white_list_entry->grpstart, white_list_entry->grpend);
                src_match = xpon_up_igmp_filter_src(src_ip, white_list_entry);
                if (src_match)
                {
                    if (xpon_grp_addr_between(grp_addr, white_list_entry->grpstart, white_list_entry->grpend))	
                    {
                        if (((0 > pkt_vlan) && (0 == white_list_entry->vlanid)) || /*Untag rule*/
                            (pkt_vlan == white_list_entry->vlanid)) /* Single-Tag rule*/
                        {
                            dy_entry_bw = white_list_entry->bandwidth;
                            dynamic_found = 1;
                            break;
                        }
                    }
                }
        	}          
        }

        if (!dynamic_found)
        {
            /* Join info not include in any white list */
            MULTICAST_TRACE_INFO("==>Not Found in any white list!\n");
            /* Update - Join messages counter */
            xpon_port_status_update_pmc(port, 1, 0, 0);
            return true;
        }
    }
    else
    {
        MULTICAST_TRACE_INFO("==>Found match iptv entry in static ACL table!\n");
        /* So far, we don't record the subscribe info about static ACL entry */
        /* (MSM ME)
           Update - Join messages counter */
        xpon_port_status_update_pmc(port, 1, 0, 0);                
        return true;
    }

    MULTICAST_TRACE_INFO("3.Check entry BW control function, dy_entry_bw = %d\n",dy_entry_bw);
    /* 3.Check BW control function enable */
    if (dy_entry_bw)
    {
        if (port_maxbw)
        {
            if (port_maxbw < (port_status->pmc.current_mcast_bw + dy_entry_bw))
            {
                /* 4.Check BW enforcement function enable */
                if (port_bwe)
                {
                    //ret = xpon_user_subscribe_handle(port_entry->port_id, 1, pkt_vlan, src_ip, grp_addr, white_list_entry);
                    //if (XPON_PORT_USER_SUBSCRIBE_NOT_FOUND != ret)                    
                    if (xpon_user_subscribe_find(port, XPON_PORT_USER_SUBSCRIBE_DONT_CARE_INDEX, &null_ip, grp_addr))                    
                    {
                        /* Because sum bw have exceed port_max_bw, we have find entry first, then decide to add subscribe info or not 
                           If we call xpon_user_subscribe_handle() directly, function will add subscribe automatically */
                        xpon_user_subscribe_handle(port, 1, pkt_vlan, src_ip, grp_addr, white_list_entry);
                        xpon_port_status_update_pmc(port, 1, 0, 0);        
                        MULTICAST_TRACE_INFO("==>Enforcement Enable : This join match exist entry, Accept!\n");                                              
                        return true;
                    }
                    else
                    {
                        MULTICAST_TRACE_INFO("==>Enforcement Enable : This join will exceed max bandwidth, Drop!\n"); 
                        xpon_port_status_update_pmc(port, 0, 1, 0);
                        return false;
                    }                    
                }
                else
                {
                    MULTICAST_TRACE_INFO("==>Enforcement Disable : This join will exceed max bandwidth, Accept!\n");
                    ret = xpon_user_subscribe_handle(port, 1, pkt_vlan, src_ip, grp_addr, white_list_entry);
                    if (XPON_PORT_USER_SUBSCRIBE_NOT_FOUND != ret)
                        xpon_port_status_update_pmc(port, 1, 0, 0);
                    else
                        xpon_port_status_update_pmc(port, 1, 1, dy_entry_bw);
                    return true;    
                }
            }
            else
            {
                MULTICAST_TRACE_INFO("==>This join still in acceptable bandwidth range!\n");
                /* (MSM ME)
                   Update - Join messages counter 
                          - Current mcast bw counter */
                ret = xpon_user_subscribe_handle(port, 1, pkt_vlan, src_ip, grp_addr, white_list_entry);
                if (XPON_PORT_USER_SUBSCRIBE_NOT_FOUND != ret)
                    xpon_port_status_update_pmc(port, 1, 0, 0);
                else
                    xpon_port_status_update_pmc(port, 1, 0, dy_entry_bw);
                return true;                  
            }
        }
        else
        {
            MULTICAST_TRACE_INFO("==>Port have not setup MAX BW!\n");
            /* (MSM ME)
               Update - Join messages counter 
                      - Current mcast bw counter */
            ret = xpon_user_subscribe_handle(port, 1, pkt_vlan, src_ip, grp_addr, white_list_entry);
            if (XPON_PORT_USER_SUBSCRIBE_NOT_FOUND != ret)
                xpon_port_status_update_pmc(port, 1, 0, 0);
            else
                xpon_port_status_update_pmc(port, 1, 0, dy_entry_bw);
            return true;         
        }
    }
    else
    {
        MULTICAST_TRACE_INFO("==>This entry have not setup bandwidth control!\n");
        /* (MSM ME)
           Update - Join messages counter */
        xpon_user_subscribe_handle(port, 1, pkt_vlan, src_ip, grp_addr, white_list_entry);  
        xpon_port_status_update_pmc(port, 1, 0, 0);  
        return true;    
    }
    
	return true;
}

int xpon_up_send_multicast_frame_hook(struct sk_buff* skb,int clone)
{
    int                   iret = 0;
    int              available = false;
    struct sk_buff*       skb2 = NULL;
    struct net_device*     dev = skb->dev;
    xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
    unsigned int      eth_type = xpon_get_packet_type(skb);
    e_vlan_operation_point_t vlan_point = 0;
	int port = 0; 

    if(NULL == dev)
    {
        return -1;
    }
    
    if (igmp_conf->flag & XPON_IGMP_DISABLED)
    {
        //MULTICAST_CRITIC_INFO("multicast disabled.\n");	
        return -1;
    }
    
    available = frame_is_legal_multicast_pkt(skb, MULTICAST_UP_STREAM);
    if(!available)
    {
        return -1;
    }

    port = ENCT_HOOK_XPON_ETH_MAP_DEV_NAME_TO_PORT(skb->dev->name);
    
    if(-1 == port)
    {
        return -1;
    }
    
    DUMP_PKT(skb->dev->name, xpon_get_ingress_dev_info(skb), skb->data, skb->len);

    if(clone)	
    {
        skb2 = skb_copy(skb,GFP_ATOMIC);
        if (skb2 == NULL)
        {
            dev->stats.tx_dropped++;
            return 0;
        }
        skb = skb2;
    }
    
    
    /*decide where vlan operation point*/
    iret = xpon_get_up_vlan_operation_point(port, &vlan_point);
    if(0 == iret && vlan_operation_in_multicast_module == vlan_point)
    {
        skb->pon_vlan_flag |=  PON_PKT_INSERT_FLAG;
    }
    
    if (eth_type==PACKET_IPV4)
    {
        xpon_sfu_send_to_router_by_flag(skb,XPON_IGMP_ROUTER_PORT);
    }
    else if (eth_type==PACKET_IPV6)
    {
        xpon_sfu_send_to_router_by_flag(skb, XPON_MLD_ROUTER_PORT);
    }
    else
    {
        MULTICAST_ERROR_INFO("eth_type = %x.\n", eth_type);
		if( clone )
		{
			kfree_skb(skb);	
		}
        return -1;
    }

     /* only for upstream ICMPv6 packets send to local. */
    if(xpon_multi_should_passthrough(skb))
        return 0;

    return 1;
}

bool xpon_is_general_query(struct sk_buff* skb)
{
	unsigned char  dst_addr[16];
	int igmp_type = -1;

	igmp_type = xpon_get_igmp_type(skb);

	if(IGMP_HOST_MEMBERSHIP_QUERY != igmp_type)
		return false;

	memset(dst_addr,0,16);
	xpon_get_dest_addr(dst_addr,skb);

	/*general query dst_addr 224.0.0.1*/
	if((224 == dst_addr[0])&&(0 == dst_addr[1])&&(0 == dst_addr[2])&&(1 == dst_addr[3]))
		return true;
	else
		return false;
}

int xpon_upstream_vlan_handle_hook(struct sk_buff* skb, int clone)
{
    int iret = 0;
	int available = false;
    int port = 0;
    char port_name[8] = {0};
    e_vlan_operation_point_t vlan_point = 0;
	struct net_device* dev = skb->dev;
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	struct sk_buff* skb2 = NULL;
    
    if (igmp_conf->flag & XPON_IGMP_DISABLED)
    {
        //MULTICAST_CRITIC_INFO("multicast disabled.\n");	
        return -1;
    }

    if(NULL == dev)
    {
        return -1;
    }

    available = frame_is_legal_multicast_pkt(skb, MULTICAST_UP_STREAM);
    if(!available)
    {
        return -1;
    }

    DUMP_PKT(skb->dev->name, xpon_get_ingress_dev_info(skb), skb->data, skb->len);
    
    /*decide where vlan operation point*/
    port = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
	if(port == -1){
		MULTICAST_ERROR_INFO("ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT return fail\n");
		return 1;
	}
    iret = xpon_get_up_vlan_operation_point(port, &vlan_point);
    MULTICAST_CRITIC_INFO("port_name = %s, port = %d, vlan_point = %d \n", port_name,port,vlan_point);
    if(0 != iret)
    {
        return 1;
    }
    else if(vlan_operation_in_vlan_module == vlan_point)
    {
        return 1;
    }
    
    if(clone)	
    {
        skb2 = skb_copy(skb,GFP_ATOMIC);
        if (skb2 == NULL)
        {
            dev->stats.tx_dropped++;
            return 0;
        }
        skb = skb2;
    }

    xpon_upstream_vlan_handle(skb, port);
    
    return 1;
}

int xpon_igmp_protocol_pack(struct sk_buff* skb)
{
    unsigned char *dest = NULL;
	
    /*avoid board crah*/
    skb_reset_mac_header(skb);
    dest = eth_hdr(skb)->h_dest;
	
    if(IS_ERR_OR_NULL(dest))
    {
        #if 0 
        printk("*************line= %d******start****\n", __LINE__);
        xpon_dump(skb->data,skb->len);
        printk("*************line= %d******to****\n", __LINE__);       
        printk("eth_hdr(skb) = %x.\n",eth_hdr(skb));
        printk("eth_hdr(skb)->h_dest = %x.\n",eth_hdr(skb)->h_dest);        
        printk("dest = %x.\n",dest);
        printk("*************line= %d******to****\n", __LINE__);
        #endif

        return false; 
    }
    
    if (!xpon_is_multicast_addr(dest))
    {
        return false;
    }
    
    if(!xpon_is_igmp_pkt(skb))
    {
        return false;
    }

    return true;
}

int xpon_up_igmp_incoming_hook(struct sk_buff* skb,int clone)
{
	int available = false;
	struct net_device* dev = skb->dev;
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	struct sk_buff* skb2 = NULL;
    int iret = 0;

    DUMP_PKT(skb->dev->name, xpon_get_ingress_dev_info(skb), skb->data, skb->len);

    if (igmp_conf->flag & XPON_IGMP_DISABLED)
    {
        //MULTICAST_CRITIC_INFO("multicast disabled.\n");	
        return -1;
    }

    if(NULL == dev)
    {
        return -1;
    }

    available = frame_is_legal_multicast_pkt(skb, MULTICAST_UP_STREAM);
    if(!available)
    {
        return -1;
    }

    if (g_DS_MCAST_BW_RATE_LIMIT_ENABLE)
    {
        available = xpon_ds_mcast_rate_limit_handle(skb);
        if(!available)
        {
            return -1;
        }
    }
    
    if(clone)	
    {
        skb2 = skb_copy(skb,GFP_ATOMIC);
        if (skb2 == NULL)
        {
            dev->stats.tx_dropped++;
            return 0;
        }
        skb = skb2;
    }
    
    iret = xpon_uni_incoming_handle(skb);
    if(DISCARD_LEAVE_MESSAGE == iret)
    {
        return DISCARD_LEAVE_MESSAGE;
    }
  
    return iret;
}

int xpon_down_igmp_incoming_hook(struct sk_buff* skb,int clone)
{
	int              available = false;
    int                   iret = 0;
	struct net_device*     dev = skb->dev;
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
	struct sk_buff*       skb2 = NULL;
	
    if (NULL == dev)
    {
        return -1;
    }
    
    if (igmp_conf->flag & XPON_IGMP_DISABLED)
    {
        MULTICAST_CRITIC_INFO("multicast disabled.\n");	
        return -1;
    }
    
    available = frame_is_legal_multicast_pkt(skb, MULTICAST_DOWN_STREAM);
    if(!available)
    {
        return -1;
    }
    
    //notice drop pkt, igmp deal 
    if(1 == igmp_conf->xpon_mode)
    {
        iret = xpon_down_igmp_ani_vlan_filter(skb);
        if(1 > iret)
        {
            return 1;
        }
    }

    if(clone)	
    {
        skb2 = skb_copy(skb,GFP_ATOMIC);
        if (skb2 == NULL)
        {
            dev->stats.tx_dropped++;
            return 1;
        }
        skb = skb2;
    }

    if(XPON_HWNAT_FWD_THRESHOLD<xpon_check_hwfwd_list_threshold()){
        xpon_hwnat_drop_multicast(skb);
    }else{
        xpon_ani_incoming_handle(skb);
    }

    return 1;
}


int xpon_hgu_downstream_vlan_handle(struct sk_buff* skb)
{
	unsigned char *dest = eth_hdr(skb)->h_dest;
	struct net_device*     dev = skb->dev;
	xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();

	if (NULL == dev)
	 {
	     return -1;
	 }
	 
	 if (igmp_conf->flag & XPON_IGMP_DISABLED)
	 {
	     MULTICAST_CRITIC_INFO("multicast disabled.\n");	
	     return -1;
	 }

	if (!xpon_is_multicast_addr(dest))
	 {
	     return -1;
	 }

	if(skb->pon_vlan_flag & PON_PKT_FROM_CPE)
	     {
	         MULTICAST_CRITIC_INFO(" packet from CPE.\n");
	         return -1;
	     }

	if(0 == (skb->pon_vlan_flag & PON_PKT_FROM_WAN))
	     {
	         MULTICAST_CRITIC_INFO("fram is not from wan port.\n");
	         return -1;
	     }

	return xpon_downstream_vlan_handle(skb,MULTICAST_OP_VEIP_PORT_ID);
}

static int xpon_down_igmp_ani_vlan_filter(struct sk_buff* skb)
{

    int iret = 0;
    Vlan_Filter_Rule_t rule = {};
    int vlan_tci = xpon_get_vlan_tci(skb);
    
    rule.vlan_tag = vlan_tci;
    if(-1 == vlan_tci)
    {
       rule.vlan_type = GPON_VLAN_FILTR_TYPE_UNTAGGED;
    }
    else
    {
        rule.vlan_type = GPON_VLAN_FILTR_TYPE_TAGGED;
    }
    
    rule.port = skb->v_if;
    rule.portType = GPON_VLAN_FILTR_PORT_TYPE_ANI;
    rule.dir = GPON_VLAN_FILTR_RULE_DIR_RX;

    iret = ECNT_API_XPON_MATCH_VLAN_FILTER_RULE_OP(&rule);
    if(GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE == iret)
    {
        return -1;
    }
    
    return 1;
}

static int xpon_down_igmp_uni_vlan_filter(struct sk_buff* skb, int port)
{

    int iret = 0;
    Vlan_Filter_Rule_t rule = {};
    int vlan_tci = xpon_get_vlan_tci(skb);
    rule.vlan_tag = vlan_tci;

    if(-1 == vlan_tci)
    {
       rule.vlan_type = GPON_VLAN_FILTR_TYPE_UNTAGGED;
    }
    else
    {
       rule.vlan_type = GPON_VLAN_FILTR_TYPE_TAGGED;
    }


    rule.port = port;
    rule.portType = GPON_VLAN_FILTR_PORT_TYPE_LAN;
    rule.dir = GPON_VLAN_FILTR_RULE_DIR_TX;
    
    iret = ECNT_API_XPON_MATCH_VLAN_FILTER_RULE_OP(&rule);
    if(GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE == iret)
    {
        return -1;
    }
    
    return 1;
}

int xpon_up_igmp_uni_vlan_filter(struct sk_buff* skb)
{

    int iret = 0;
    int port = -1;
    Vlan_Filter_Rule_t rule = {};
    int vlan_tci = 0;


	if(skb == NULL)
    {
		MULTICAST_NOTICE_INFO("\n(%s) SKB is NULL\n", __FUNCTION__);
		return -1;
	}
    vlan_tci = xpon_get_vlan_tci(skb);
    rule.vlan_tag = vlan_tci;
    if (-1 == vlan_tci)
        rule.vlan_type = GPON_VLAN_FILTR_TYPE_UNTAGGED;
    else
        rule.vlan_type = GPON_VLAN_FILTR_TYPE_TAGGED;


    /*_____________________________________________
    ** skb->mark can be used to distinquish comming from which LAN interface,  
    ** use the highest 4 bits.
    **
    ** eth0     0x10000000
    ** eth0.1   0x10000000
    ** eth0.2   0x20000000
    ** eth0.3   0x30000000
    ** eth0.4   0x40000000
    **_________________________________________
    */
    port = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);	
	if(port == -1){		
		MULTICAST_ERROR_INFO("ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT return fail\n");		
		return 0;	
	}
	
        if(skb->pon_vlan_flag & PON_PKT_FROM_LAN)
        {
            rule.port = port;
        }
        else
        {
            MULTICAST_NOTICE_INFO("\nreturn fail because sfu packet is not from lan\n");
            return -1;
        }

    rule.portType = GPON_VLAN_FILTR_PORT_TYPE_LAN;
    rule.dir = GPON_VLAN_FILTR_RULE_DIR_RX;

    iret = ECNT_API_XPON_MATCH_VLAN_FILTER_RULE_OP(&rule);
    if(GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE == iret)
    {
        return -1;
    }
    
    return 1;
}

int xpon_up_igmp_ani_vlan_filter(struct sk_buff* skb)
{

    int iret = 0;
    unsigned int eth_type;
    unsigned char* buff;
    Vlan_Filter_Rule_t rule = {};

	if(skb == NULL)
    {
		MULTICAST_NOTICE_INFO("\n(%s) SKB is NULL\n", __FUNCTION__);
		return -1;
	}

    eth_type = ntohs(eth_hdr(skb)->h_proto);
    buff = skb_mac_header(skb);
    
    if (eth_type == 0x8100 || eth_type == 0x88a8)
    {
        rule.vlan_type = GPON_VLAN_FILTR_TYPE_TAGGED;
        rule.vlan_tag = ntohs(*(short int*)(buff+14)) & 0xefff;
    }
    else
    {
        rule.vlan_type = GPON_VLAN_FILTR_TYPE_UNTAGGED;
    }
    
    
    rule.port = skb->v_if;
    rule.portType = GPON_VLAN_FILTR_PORT_TYPE_ANI;
    rule.dir = GPON_VLAN_FILTR_RULE_DIR_TX;

    iret = ECNT_API_XPON_MATCH_VLAN_FILTER_RULE_OP(&rule);
    if(GPON_VLAN_FILTER_HANDLE_RESULT_FILTER_TYPE == iret)
    {
        return -1;
    }
    
    return 1; //find a matched rule in vlanFilter
}

int isVlanOperationInMulticastModule(struct sk_buff* skb)
{
    int                   iret = 0;
    struct net_device*     dev;
    xPON_IGMPConf_t* igmp_conf = xpon_get_igmp_conf();
    e_vlan_operation_point_t vlan_point = 0;
	int port = 0;
    int available = 0;

	if(skb == NULL)
    {
		MULTICAST_NOTICE_INFO("\n(%s) SKB is NULL\n", __FUNCTION__);
		return -1;
	}

    dev = skb->dev;


    if(NULL == dev)
    {
        return 0;
    }
    
    if (igmp_conf->flag & XPON_IGMP_DISABLED)
    {
        return 0;
    }
  
    available = frame_is_legal_multicast_pkt(skb, MULTICAST_UP_STREAM);
    if(!available)
    {
        return 0;
    }

    port = ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT(skb);
	if(port == -1){
		MULTICAST_ERROR_INFO("ENCT_HOOK_XPON_ETH_MAP_MARK_TO_PORT return fail\n");
		return 1;
	}
 
    /*decide where vlan operation point*/
    iret = xpon_get_up_vlan_operation_point(port, &vlan_point);  
    if((0 == iret) && (vlan_operation_in_multicast_module == vlan_point))
        return 1;
    else
        return 0;

}


/************************************************************************/
static void add_leave_pkt_entry(int port, unsigned char* grp_addr,int protocol_flag, struct sk_buff* skb)
{
    leave_pkt_list_t *entry = NULL;  

    entry = kmalloc(sizeof(leave_pkt_list_t), GFP_ATOMIC);
    
    if(NULL == entry)        
    {            
        printk("kmalloc memory is failed!.\n"); 
        return ;
    }
    memset(entry, 0, sizeof(leave_pkt_list_t));
    entry->port = port;
    entry->protocol_flag = protocol_flag;
    if(XPON_MASK_IGMPV2 == protocol_flag)
    {
        memcpy(entry->grp_addr, grp_addr, 4);
    }
    else
    {
        memcpy(entry->grp_addr, grp_addr, 16);
    }
    
    entry->skb = skb;
    
    list_add_tail(&entry->list, &leave_pkt_list);

    return ;
}

static void del_leave_pkt_entry(int port, unsigned char* grp_addr,int protocol_flag)
{
    leave_pkt_list_t *entry = NULL;  
    leave_pkt_list_t *ptr = NULL;  

    list_for_each_entry_safe(entry, ptr, &leave_pkt_list, list)    
    {
        if((entry->port == port) && (entry->protocol_flag == protocol_flag))
        {
            if((XPON_MASK_IGMPV2 == protocol_flag) && (!memcmp(grp_addr, entry->grp_addr, 4)))
            {
                kfree_skb(entry->skb);
                entry->skb = NULL;
                list_del(&entry->list);
                kfree(entry);
				break;
            }
            else if((XPON_MASK_MLDV1 == protocol_flag) && (!memcmp(grp_addr, entry->grp_addr, 16)))
            {
                kfree_skb(entry->skb);
                entry->skb = NULL;
                list_del(&entry->list);
                kfree(entry);
				break;
            }
        }
    }
    
    return ;
}

static void xpon_send_leave(int port, unsigned char* grp_addr, int protocol_flag)
{
    leave_pkt_list_t *entry = NULL;  
    leave_pkt_list_t *ptr = NULL; 
	struct net_device* dev = NULL; 
    struct sk_buff   *skb = NULL;
    
	dev = xpon_get_dev_by_name("pon");
	if (dev == NULL)
    {
        return ;
    }   
    
    list_for_each_entry_safe(entry, ptr, &leave_pkt_list, list)    
    {
        if((entry->port == port) && (entry->protocol_flag == protocol_flag))
        {
            if((XPON_MASK_IGMPV2 == protocol_flag) && (!memcmp(grp_addr, entry->grp_addr, 4)))
            {
                
                skb = skb_copy(entry->skb, GFP_ATOMIC);
                if(NULL == skb)
                {               						
					goto need_dev_put;
                }
                
                ECNT_API_XPON_WAN_NET_START_XMIT(skb, dev);

                kfree_skb(entry->skb);
                entry->skb = NULL;
                list_del(&entry->list);
				break;
            }
            else if((XPON_MASK_MLDV1 == protocol_flag) && (!memcmp(grp_addr, entry->grp_addr, 16)))
            {
                skb = skb_copy(entry->skb, GFP_ATOMIC);
                if(NULL == skb)
                {
					goto need_dev_put;
                }
                
                ECNT_API_XPON_WAN_NET_START_XMIT(skb, dev);
                
                kfree_skb(entry->skb);
                entry->skb = NULL;
                list_del(&entry->list);
				break;
            }
        }
    }

need_dev_put:	
	dev_put(dev);
    return ;
    
}

static int dynamic_acl_ctr(int port, int vid, int gem_portid, unsigned char* dest_addr , unsigned char* src_addr)
{    
    xPON_PortConf_t* port_conf = NULL;
    struct list_head*  dyn_list = NULL;
	xPON_WhiteList_Entry_List_t* entry = NULL;
    
    port_conf = xpon_port_conf_by_id(port);
    
    if((NULL == port_conf) ||  (0 > gem_portid) || (NULL == dest_addr) || (NULL == src_addr))
    {
        return NOT_MATCH_DYNAMIC_ACL;
    }
	
	dyn_list = &(port_conf->dyn_list);

    //unconfig dynamic acl, forward or drop by multicast forward table
    if(list_empty(dyn_list))
    {
        return MATCH_DYNAMIC_ACL;
    }

	list_for_each_entry(entry,dyn_list,list)
	{	
        if (xpon_grp_addr_between(dest_addr, entry->grpstart, entry->grpend)==0)
        {
            continue;
        }
        
        if (vid != entry->vlanid)
        {
            continue;
        }      
        //65533 & 65534 ALU XGPON multicast gem, OLT configued 2047 gem, mismatch!
		if((INVALID_GEMP_ID != gem_portid) && (gem_portid != entry->gemid)	&& (gem_portid != ALU_XG_BROADCAST_GEM_ID) && (gem_portid != ALU_XG_MULTICAST_GEM_ID))
        {
            continue;
        }
        
        if(0 == xpon_is_non_zero(entry->srcip,16))
        {
            return MATCH_DYNAMIC_ACL;
        }
        else 
        {
            if(0 == memcmp(src_addr,entry->srcip,16))
            {
                return MATCH_DYNAMIC_ACL;
            }
        }
	}


    return NOT_MATCH_DYNAMIC_ACL;
}

int static_acl_ctr(int port, int vid, int gem_portid, unsigned char* dest_addr , unsigned char* src_addr, int hw_flag)
{    
    xPON_PortConf_t* port_conf = NULL;
    struct list_head*  static_list = NULL;
	xPON_WhiteList_Entry_List_t* entry = NULL;
    
    port_conf = xpon_port_conf_by_id(port);
    
    if((NULL == port_conf) ||  (0 > gem_portid) || (NULL == dest_addr) || (NULL == src_addr))
    {
        return NOT_MATCH_STATIC_ACL;
    }

    static_list = &(port_conf->sta_list);
    
    //unconfig dynamic acl
    if(list_empty(static_list))
    {
        return NOT_MATCH_STATIC_ACL;
    }

	list_for_each_entry(entry,static_list,list)
	{	
        if (xpon_grp_addr_between(dest_addr, entry->grpstart, entry->grpend)==0)
        {
            continue;
        }
        
        if(0 == hw_flag)
        {
            if (vid != entry->vlanid)
            {
                continue;
            }  
        }

        if((INVALID_GEMP_ID != gem_portid) && gem_portid != entry->gemid)
        {
            continue;
        }
        
        if(0 == xpon_is_non_zero(entry->srcip,16))
        {
            return MATCH_STATIC_ACL;
        }
        else 
        {
            if(0 == memcmp(src_addr,entry->srcip,16))
            {
                return MATCH_STATIC_ACL;
            }
        }
	}

    return NOT_MATCH_STATIC_ACL;
}

int multicast_data_pack(struct sk_buff* skb)
{
    unsigned char *dest = NULL; 
    int flag = false;
    if(NULL == skb || NULL == skb->dev)
    {
        return false;
    }
    dest = eth_hdr(skb)->h_dest;
    if (!xpon_is_multicast_addr(dest))
    {
        return false;
    }
    flag = xpon_is_data_pkt(skb);
    return flag;
}

void init_xpon_igmp_macro_compatible(void)
{
	xpon_igmp_dev_offset = dev_offset_macro_compatible();

	return ;
}
    



