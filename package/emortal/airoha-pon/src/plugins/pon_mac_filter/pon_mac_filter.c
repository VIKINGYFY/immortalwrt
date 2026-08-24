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
	Reid.Ma		2013/3/22	Create
*/

#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/proc_fs.h>
#include <linux/if_vlan.h>
#include <linux/if_ether.h>
#include <linux/time.h>
#include <linux/version.h>
#include <linux/libcompileoption.h>
#include <linux/fs.h>
#include <linux/module.h>
#include "pon_mac_filter.h"
#include <ecnt_hook/ecnt_hook_pon_mac.h>


MODULE_DESCRIPTION("Pon_mac_filter");
MODULE_LICENSE("GPL");

#define KERNEL_2_6_36 		(LINUX_VERSION_CODE > KERNEL_VERSION(2,6,31))

extern int (*pon_check_mac_hook)(struct sk_buff *skb);
extern int (*pon_mac_filter_get_mode_hook)(void);
extern int xpon_get_vport(void);
#if defined(TCSUPPORT_RA_HWNAT) && defined(TCSUPPORT_RA_HWNAT_ENHANCE_HOOK)
extern int (*ra_sw_nat_hook_drop_packet) (struct sk_buff * skb);
extern int (*ra_sw_nat_hook_clean_table) (void);
#endif


static pon_mac_filter_all pon_mac_filter_all_data;

static int DBG_Level = 0;

#if KERNEL_2_6_36
long pon_mac_filter_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
#else
int pon_mac_filter_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg);
#endif

int pon_mac_filter_open(struct inode *inode, struct file *filp);
#define isdigit(x)	((x)>='0'&&(x)<='9')

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

/*
	only used for find downstream port data.
	we get upstream port data by ani port index directly.
*/
static inline pon_mac_filter * get_pon_mac_filter_by_port(int port)
{
	int i = 0;
	pon_mac_filter * tmp;
	
	for(i = 0; i < pon_mac_filter_all_data.total_port_count; i++)
	{
		tmp = &(pon_mac_filter_all_data.downstream_data[i]);
		if(DBG_Level > 1)
			printk("\r\nframe port is %d,mac filter data port is %d ====>get pon mac filter by port",port,atoi(tmp->port_index));
		if(atoi(tmp->port_index) == port)
		{
			if(DBG_Level > 0)
				printk("\r\nfind port %d ====>get pon mac filter by port",port);
			return tmp;
		}
	}
	if(DBG_Level > 0)
		printk("\r\ncan't find port ====>get pon mac filter by port");
	return NULL;
}

static int pon_mac_filter_same_rule_check(pon_mac_filter_rule * r1,pon_mac_filter_rule * r2)
{
	if(memcmp(r1,r2,sizeof(pon_mac_filter_rule)) == 0)
		return 1;
	else
		return 0;
}

static inline pon_mac_filter * get_filter_opt_data(struct sk_buff *skb)
{
	pon_mac_filter * tmp = NULL;
	struct net_device *dev = skb->dev;
	int port = 0,i = 0,flag = 0;

	//return &pon_mac_filter_all_data.downstream_data[0];
	if(skb->pon_mac_filter_flag & PKT_SEND_TO_WAN)
	{
		if(DBG_Level > 1)
			printk("\r\npacket send to wan,virtual if is %d",skb->v_if);
		tmp = &pon_mac_filter_all_data.upstream_data[skb->v_if];
	}
	else if(skb->pon_mac_filter_flag & PON_MAC_FILTER_RX_CALL_HOOK)
	{
		if (strcmp(dev->name,"pon") == 0)
		{
			if(DBG_Level > 1)
				printk("\r\nHGU mode check lan 1");
			tmp = &pon_mac_filter_all_data.downstream_data[0];/*use lan1 as HGU port*/
		}
		else
		{
			if(DBG_Level > 1)
				printk("\r\nSFU mode,get lan port");
			if(dev->name[0] == 'e')
			{
				if(pon_mac_filter_all_data.lan_port_count == 1)/*single port case.return lan port 1*/
					port = 11;
				else
					port = 10 + (dev->name[5] - '0');
			}
			else if(dev->name[0] == 'r')
				port = 20 + (dev->name[2] - '0');
			else if(dev->name[0] == 'u')
				port = 30 + (dev->name[3] - '0');
			else
				return NULL;
			if(DBG_Level > 1)
				printk("\r\nSFU mode,lan port is %d ",port);

			for(i = 0; i < pon_mac_filter_all_data.total_port_count; i++)
			{
				if(atoi(pon_mac_filter_all_data.downstream_data[i].port_index) == port)
				{
					tmp = &pon_mac_filter_all_data.downstream_data[i];
					flag = 1;
				}
			}

			if(flag != 1)
			{
				if(DBG_Level > 1)
					printk("\r\ncan't find port,return NULL");
				return NULL;
			}
		}
	}
	else if(skb->pon_mac_filter_flag & PON_MAC_FILTER_TX_CALL_HOOK)
	{
		if(DBG_Level > 1)
			printk("\r\nSFU mode,get lan port");
		if(dev->name[0] == 'e')
		{
			if(pon_mac_filter_all_data.lan_port_count == 1)//single port case.return lan port 1
				port = 11;
			else
				port = 10 + (dev->name[5] - '0');
		}
		else if(dev->name[0] == 'r')
			port = 20 + (dev->name[2] - '0');
		else if(dev->name[0] == 'u')
			port = 30 + (dev->name[3] - '0');
		else
			return NULL;
		if(DBG_Level > 1)
			printk("\r\nSFU mode,lan port is %d ",port);

		for(i = 0; i < pon_mac_filter_all_data.total_port_count; i++)
		{
			if(atoi(pon_mac_filter_all_data.downstream_data[i].port_index) == port)
			{
				tmp = &pon_mac_filter_all_data.downstream_data[i];
				flag = 1;
				break;
			}
		}

		if(flag != 1)
		{
			if(DBG_Level > 1)
				printk("\r\ncan't find port,return NULL");
			return NULL;
		}
	}
	else
		return NULL;

	if(tmp->unicast_rule_counter == 0 && tmp->multicast_rule_counter == 0)
		return NULL;//no rule for pkt
	else
		return tmp;
}

int check_rule(struct sk_buff *skb,pon_mac_filter * data)
{
	int i = 0,j = 0,match = 0;
	pon_mac_filter_rule * rule = NULL;
	unsigned char * temp = NULL;
	unsigned char * mac = NULL;
	__u16 ethertype = 0;
	struct net_device *dev = skb->dev;
	int mac_filter_mode = pon_mac_filter_get_mode_hook();
	int is_rx = 0; 

	if (PON_MAC_FILTER_RX_CALL_HOOK ==(skb->pon_mac_filter_flag & PON_MAC_FILTER_RX_CALL_HOOK)
			&& (strcmp(dev->name,"pon") != 0))
	{
		/* PON_MAC_FILTER_RX_CALL_HOOK only for multicast rule */
		is_rx = 1;
	}
	
	if((data->unicast_rule_counter != 0) && (!is_rx))/*we check unicast rule first*/
	{
		for(i = 0; i < data->unicast_rule_counter; i++)
		{
			rule = &data->unicast_rule[i];
			if(DBG_Level > 1)
				printk("\r\nbegin match rule %d",i);
			if(rule->ethertype != 0)
			{
				if (TCSUPPORT_PON_VLAN_VAL)
				{
					mac = skb->data + 12 + 4 * skb->pon_tag_num;
					ethertype = (*mac << 8) + *(mac + 1);
				}
				else
				{
					mac = skb->data + 12;
					while(((*mac << 8) + *(mac + 1)) == 0x8100)
						mac += 4;
					ethertype = (*mac << 8) + *(mac + 1);
				}
				if(ethertype != rule->ethertype)
					continue;
			}
			if(DBG_Level > 1)
				printk("\r\nfinish match ethertype,rule mac type is %s",(rule->mac_type == DES_MAC)?"des mac":"src mac");

			if(rule->mac_type == DES_MAC)
				mac = skb->data;
			else if(rule->mac_type == SRC_MAC)
				mac = skb->data + 6;
			else
				continue;//rule error,check next

			if(skb->pon_mac_filter_flag & PON_MAC_FILTER_RX_CALL_HOOK)
				mac -= 14;
			
			if(DBG_Level > 1)
			{
				printk("\r\nskb->data is ");
				for(j = 0; j < 12; j++)
				{
					printk("%02x",skb->data[j]);
				}
			}
			match = 0;
			for(j = 0; j < 6; j++)
			{
				if(mac[j] >= rule->start_mac[j] && mac[j] <= rule->end_mac[j])
					match++;
				else
					break;
			}
			if(DBG_Level > 1)
				printk("\r\nfinish match mac,match value is %d",match);

			if(match == 6)//match rule
			{
				if(rule->filter_type == RULE_FORWARD)
					return PKT_FORWARD;
				else if(rule->filter_type == RULE_DISCARD)
					return PKT_DISCARD;
				else
					continue;
			}
		}
	}
	
	if(data->multicast_rule_counter != 0)//now we check multicast rule
	{
		if (PON_MAC_FILTER_RX_CALL_HOOK ==(skb->pon_mac_filter_flag & PON_MAC_FILTER_RX_CALL_HOOK)
			&& (MODE_SFU == mac_filter_mode)
			&& (skb->dev->name[0] == 'e'))
		{
			//temp = skb->mac_header;
			temp = skb_mac_header(skb);
			
			if (TCSUPPORT_PON_VLAN_VAL)
			{
				mac = temp + 12 + 4 * skb->pon_tag_num;
				ethertype = (*mac << 8) + *(mac + 1);
			}
			else
			{
				mac = temp + 12;
				while(((*mac << 8) + *(mac + 1)) == 0x8100)
					mac += 4;
				ethertype = (*mac << 8) + *(mac + 1);
			}
		}
		else
		{
			temp = skb->data;
			if (TCSUPPORT_PON_VLAN_VAL)
			{
				mac = temp + 12 + 4 * skb->pon_tag_num;
				ethertype = (*mac << 8) + *(mac + 1);
			}
			else
			{
				mac = temp + 12;
				while(((*mac << 8) + *(mac + 1)) == 0x8100)
					mac += 4;
				ethertype = (*mac << 8) + *(mac + 1);
			}
		}
		for(i = 0; i < data->multicast_rule_counter; i++)
		{
			rule = &data->multicast_rule[i];
			if(rule->ethertype != 0)
			{
				if(ethertype != rule->ethertype)
					continue;
			}
			if(rule->mac_type == DES_MAC)
				mac = temp;
			else if(rule->mac_type == SRC_MAC)
				mac = temp + 6;
			else
				continue;//rule error,check next

			match = 0;
			for(j = 0; j < 6; j++)
			{
				if(mac[j] >= rule->start_mac[j] && mac[j] <= rule->end_mac[j])
					match++;
				else
					break;
			}
			if(match == 6)//match rule
			{
				if(rule->filter_type == RULE_FORWARD)
					return PKT_FORWARD;
				else if(rule->filter_type == RULE_DISCARD)
					return PKT_DISCARD;
				else
					continue;
			}
		}
	}

	if(data->unicast_rule_counter != 0)
	{
		if(DBG_Level > 1)
			printk("\r\ncan't find unicast rule");
	
		if(data->multicast_rule_counter != 0)
		{
			if(DBG_Level > 1)
				printk("\r\ncan't find multicast rule");
			if(data->unicast_rule[0].filter_type != data->multicast_rule[0].filter_type)
				return PKT_DISCARD;
			else
			{
				if(data->unicast_rule[0].filter_type == RULE_FORWARD)
					return PKT_DISCARD;
				else
					return PKT_FORWARD;
			}
		}
		else
		{
			if(DBG_Level > 1)
				printk("\r\nno multicast rule");
			if(data->unicast_rule[0].filter_type == RULE_FORWARD)
					return PKT_DISCARD;
				else
					return PKT_FORWARD;
		}
	}
	if(data->multicast_rule_counter != 0)
	{
		if(DBG_Level > 1)
			printk("\r\nno unicast rule");
		if(data->multicast_rule[0].filter_type == RULE_FORWARD)
			return PKT_DISCARD;
		else
			return PKT_FORWARD;
	}
	if(DBG_Level > 1)
		printk("\r\nno rule here,is shouldn't be happen");
	return PKT_FORWARD;
}

int pon_check_mac(struct sk_buff *skb)
{
	struct net_device *out_dev = NULL;
	pon_mac_filter * filter_data = NULL;
	
	if(skb == NULL)
	{
		printk("\r\nskb is NULL ====> pon check mac");
		return -1;
	}
	out_dev = skb->dev;
	if(pon_mac_filter_all_data.enable_flag == DISABLE)
		return 0;

	if(DBG_Level > 1)
		printk("\r\noutdev is %s",out_dev->name);
	
	if(skb->pon_mac_filter_flag & PKT_FILTER_FLAG)
		return 0;

	if((((skb->pon_mac_filter_flag & PON_MAC_FILTER_RX_CALL_HOOK) &&  (strcmp(out_dev->name,"pon") == 0))
		|| (skb->pon_mac_filter_flag & PON_MAC_FILTER_TX_CALL_HOOK)/*HGU mode,check tx/rx hook */ )
		|| (skb->pon_mac_filter_flag & PKT_SEND_TO_WAN))
		skb->pon_mac_filter_flag |= PKT_FILTER_FLAG;
	else
		return 0;

	filter_data = get_filter_opt_data(skb);
	
	if(filter_data == NULL)
	{
		if(DBG_Level > 1)
			printk("\r\nno rule for this packet,do nothing");
		return 0;
	}
	else
	{
		if(DBG_Level > 1)
			printk("\r\nfind mac filter rule");
	}

	if(check_rule(skb,filter_data) == PKT_FORWARD)
	{
		if(DBG_Level > 1)
			printk("\r\nno rule for packet or match forward rule,pass it");
		return 0;
	}
	else
	{
		if(DBG_Level > 0)
			printk("\r\npkt match discard rule or unmatch forward rule");
		#if defined(TCSUPPORT_RA_HWNAT) && defined(TCSUPPORT_RA_HWNAT_ENHANCE_HOOK)
			if(ra_sw_nat_hook_drop_packet)
				ra_sw_nat_hook_drop_packet(skb);
		#endif
		//if(!(skb->pon_mac_filter_flag & PKT_SEND_TO_WAN))
		//	kfree_skb(skb);
		//skb = NULL;
		return -1;
	}
	
	return 0;
}


int pon_mac_filter_get_mode(void)
{
	return pon_mac_filter_all_data.onu_mode;
}


static int pon_mac_filter_switch_option(pon_mac_filter_ioctl_data * data,void * arg)
{
	if(data->option_flag == OPT_GET)
	{
		data->enable_flag = pon_mac_filter_all_data.enable_flag;
		if (copy_to_user((void __user *)arg, data, sizeof(pon_mac_filter_ioctl_data)) == -1)
		{
			printk("\r\ncopy to user error ====> pon mac filter switch opt");
			return -1;
		}
		return 0;
	}
	else if(data->option_flag == OPT_SET)
	{
		pon_mac_filter_all_data.enable_flag = data->enable_flag;
		return 0;
	}
	if(DBG_Level > 0)
		printk("\r\noption error ====> pon mac filter switch opt");
	return -1;
}

static int pon_mac_filter_rule_option(pon_mac_filter_ioctl_data * data,void * arg)
{
	int i = 0;
	pon_mac_filter * tmp = NULL;
	uint32_t ani_index = 0;
	
	if(data->direction == PON_MAC_FILTER_UPSTREAM)
	{
		ani_index = data->ani_index;
		if(ani_index >= MAC_FILTER_ANI_PORT_NUM)
		{
			printk("ani_index is %d, greater %d\n", ani_index, MAC_FILTER_ANI_PORT_NUM);
			return -1;
		}
		tmp = &pon_mac_filter_all_data.upstream_data[ani_index];
	}
	else
		tmp = get_pon_mac_filter_by_port(data->uni_index);
	if(data->option_flag == OPT_GET)
	{
		if(tmp != NULL)
		{
			if(data->rule_type == G988_936)
			{
				if(tmp->unicast_rule_counter == 0)
					return 0;
				if(data->rule_index >= tmp->unicast_rule_counter)
				{
					if(DBG_Level > 0)
						printk("\r\nindex out of up rule count ====> pon mac filter rule opt");
					return -1;
				}
				memcpy(&data->rule,&tmp->unicast_rule[data->rule_index],sizeof(pon_mac_filter_rule));
				data->rule_index++;

				if(data->rule_index == tmp->unicast_rule_counter)
					data->rule_index = 0xff;
			}
			else if(data->rule_type == G988_937)
			{
				if(tmp->multicast_rule_counter == 0)
					return 0;
				if(data->rule_index >= tmp->multicast_rule_counter)
				{
					if(DBG_Level > 0)
						printk("\r\nindex out of up rule count ====> pon mac filter rule opt");
					return -1;
				}
				memcpy(&data->rule,&tmp->multicast_rule[data->rule_index],sizeof(pon_mac_filter_rule));
				data->rule_index++;

				if(data->rule_index == tmp->multicast_rule_counter)
					data->rule_index = 0xff;
			}
			else
				return -1;

			if (copy_to_user((void __user *)arg, data, sizeof(pon_mac_filter_ioctl_data)) == -1)
			{
				if(DBG_Level > 0)
					printk("\r\ncopy to user error ====> pon mac filter rule opt");
				return -1;
			}
			return 0;
			
		}
		if(DBG_Level > 0)
			printk("\r\nget mac filter rule error,can't find port====> pon mac filter rule opt");
		return -1;
	}
	else if(data->option_flag == OPT_SET)
	{
		if(tmp != NULL)
		{
			if(data->rule_type == G988_936)
			{
				if(tmp->unicast_rule_counter >= MAC_FILTER_UNICAST_RULE_LIMIT)
				{
					if(DBG_Level > 0)
					{
						if(data->direction == PON_MAC_FILTER_DOWNSTREAM)
							printk("\r\nRule count has reach the limit on ANI port %d ====> pon mac filter rule opt",data->ani_index);
						else
							printk("\r\nRule count has reach the limit on UNI port %d ====> pon mac filter rule opt",data->uni_index);
					}
					return -1;
				}

				if(data->rule.filter_type != tmp->unicast_rule->filter_type && tmp->unicast_rule_counter != 0)
				{
					if(DBG_Level > 0)
					{
						if(data->direction == PON_MAC_FILTER_DOWNSTREAM)
							printk("\r\nrule type is different from rule 0 on ANI port %d ====> pon mac filter rule opt",data->ani_index);
						else
							printk("\r\nrule type is different from rule 0 on UNI port %d ====> pon mac filter rule opt",data->uni_index);
					}
					return -1;
				}
					
				for(i = 0; i < tmp->unicast_rule_counter; i++)
				{
					if(pon_mac_filter_same_rule_check(&data->rule,((tmp->unicast_rule) + i)) == 1)
						return 0;
				}
				memcpy(&tmp->unicast_rule[tmp->unicast_rule_counter],&data->rule,sizeof(pon_mac_filter_rule));
				tmp->unicast_rule_counter++;
			}
			else if(data->rule_type == G988_937)
			{
				if(tmp->multicast_rule_counter >= MAC_FILTER_MULTICAST_RULE_LIMIT)
				{
					if(DBG_Level > 0)
					{
						if(data->direction == PON_MAC_FILTER_DOWNSTREAM)
							printk("\r\nRule count has reach the limit on ANI port %d ====> pon mac filter rule opt",data->ani_index);
						else
							printk("\r\nRule count has reach the limit on UNI port %d ====> pon mac filter rule opt",data->uni_index);
					}
					return -1;
				}

				if(data->rule.filter_type != tmp->multicast_rule->filter_type && tmp->multicast_rule_counter != 0)
				{
					if(DBG_Level > 0)
					{
						if(data->direction == PON_MAC_FILTER_DOWNSTREAM)
							printk("\r\nrule type is different from rule 0 on ANI port %d ====> pon mac filter rule opt",data->ani_index);
						else
							printk("\r\nrule type is different from rule 0 on UNI port %d ====> pon mac filter rule opt",data->uni_index);
					}
					return -1;
				}

				for(i = 0; i < tmp->multicast_rule_counter; i++)
				{
					if(pon_mac_filter_same_rule_check(&data->rule,((tmp->multicast_rule) + i)) == 1)
						return 0;
				}
				memcpy(&tmp->multicast_rule[tmp->multicast_rule_counter],&data->rule,sizeof(pon_mac_filter_rule));
				tmp->multicast_rule_counter++;
			}
			else 
				return -1;
			if(DBG_Level > 0)
				printk("\r\nAdd mac filter rule success ====> pon mac filter rule opt");
			return 0;
		}
		if(DBG_Level > 0)
			printk("\r\nAdd mac filter rule error,can't find port====> pon mac filter rule opt");
		return -1;
	}
	else if(data->option_flag == OPT_DEL)
	{
		if(tmp != NULL)
		{
			if(data->rule_type == G988_936)
			{
				if(data->rule_index >= tmp->unicast_rule_counter)
				{
					if(DBG_Level > 0)
						printk("\r\ndel index is larger than rule num ====> pon vlan rule opt");
					return -1;
				}
				memcpy(&tmp->unicast_rule[data->rule_index],&tmp->unicast_rule[data->rule_index + 1],sizeof(pon_mac_filter_rule)*(tmp->unicast_rule_counter - data->rule_index + 1));
				tmp->unicast_rule_counter--;
			}
			else if(data->rule_type == G988_937)
			{
				if(data->rule_index >= tmp->multicast_rule_counter)
				{
					if(DBG_Level > 0)
						printk("\r\ndel index is larger than rule num ====> pon vlan rule opt");
					return -1;
				}
				memcpy(&tmp->multicast_rule[data->rule_index],&tmp->multicast_rule[data->rule_index + 1],sizeof(pon_mac_filter_rule)*(tmp->multicast_rule_counter - data->rule_index + 1));
				tmp->multicast_rule_counter--;
			}
			else
				return -1;
			if(DBG_Level > 0)
				printk("\r\ndelete rule success ====> pon vlan rule opt");
			return 0;
		}
		if(DBG_Level > 0)
			printk("\r\ndelete mac filter rule error,can't find port====> pon mac filter rule opt");
		return -1;
	}
	else if(data->option_flag == OPT_CLEAN)
	{
		if(tmp != NULL)
		{
			if(data->rule_type == G988_936)
			{
				memset(tmp->unicast_rule,0,sizeof(pon_mac_filter_rule) * MAC_FILTER_UNICAST_RULE_LIMIT);
				tmp->unicast_rule_counter = 0;
			}
			else if(data->rule_type == G988_937)
			{
				memset(tmp->multicast_rule,0,sizeof(pon_mac_filter_rule) * MAC_FILTER_UNICAST_RULE_LIMIT);
				tmp->multicast_rule_counter = 0;
			}
			else
				return -1;
			if(DBG_Level > 0)
				printk("\r\nClean rule success ====> pon mac filter rule opt");
			return 0;
		}
		if(DBG_Level > 0)
			printk("\r\nclean mac filter rule error,can't find port====> pon mac filter rule opt");
		return -1;
	}
	else if(data->option_flag == OPT_CLEAN_ALL)
	{
		tmp = pon_mac_filter_all_data.upstream_data;
		for(i = 0; i < MAC_FILTER_ANI_PORT_NUM; i++)
		{
			tmp->unicast_rule_counter = 0;
			tmp->multicast_rule_counter = 0;
			memset(tmp->unicast_rule,0,sizeof(pon_mac_filter_rule) * MAC_FILTER_UNICAST_RULE_LIMIT);
			memset(tmp->multicast_rule,0,sizeof(pon_mac_filter_rule) * MAC_FILTER_UNICAST_RULE_LIMIT);
			tmp++;
		}
		tmp = pon_mac_filter_all_data.downstream_data;
		for(i = 0; i < pon_mac_filter_all_data.total_port_count; i++)
		{
			tmp->unicast_rule_counter = 0;
			tmp->multicast_rule_counter = 0;
			memset(tmp->unicast_rule,0,sizeof(pon_mac_filter_rule) * MAC_FILTER_UNICAST_RULE_LIMIT);
			memset(tmp->multicast_rule,0,sizeof(pon_mac_filter_rule) * MAC_FILTER_UNICAST_RULE_LIMIT);
			tmp++;
		}
		if(DBG_Level > 0)
			printk("\r\nclean all mac filter rule success ====> pon mac filter rule opt");
		return 0;
	}
	else if(data->option_flag == OPT_SHOW)
	{
		if(tmp != NULL)
		{
			printk("\r\nMac filter status is %d",pon_mac_filter_all_data.enable_flag);
			printk("\r\nNow mode is %s",(pon_mac_filter_all_data.onu_mode == MODE_HGU)?"HGU":"SFU");
			if(data->direction == PON_MAC_FILTER_DOWNSTREAM)
				printk("\r\nwe will show all on ANI port %d",data->ani_index);
			else
				printk("\r\nwe will show all on UNI port %d",data->uni_index);
			printk("\r\nunicast rule counter is %d,multicast rule counter is %d",tmp->unicast_rule_counter,tmp->multicast_rule_counter);
			printk("\r\nfilter_type mac_type ethertype  start_mac      end_mac");
			printk("\r\nunicast rule");
			for(i = 0; i < tmp->unicast_rule_counter; i++)
			{
				printk("\r\n%s   %s    %x    %02x:%02x:%02x:%02x:%02x:%02x   %02x:%02x:%02x:%02x:%02x:%02x",(tmp->unicast_rule[i].filter_type == 0)?"forward":"discard",
					(tmp->unicast_rule[i].mac_type == 0)?"des mac":"src mac",tmp->unicast_rule[i].ethertype,
					tmp->unicast_rule[i].start_mac[0],tmp->unicast_rule[i].start_mac[1],tmp->unicast_rule[i].start_mac[2],
					tmp->unicast_rule[i].start_mac[3],tmp->unicast_rule[i].start_mac[4],tmp->unicast_rule[i].start_mac[5],
					tmp->unicast_rule[i].end_mac[0],tmp->unicast_rule[i].end_mac[1],tmp->unicast_rule[i].end_mac[2],
					tmp->unicast_rule[i].end_mac[3],tmp->unicast_rule[i].end_mac[4],tmp->unicast_rule[i].end_mac[5]);
			}
			printk("\r\nmulticast rule");
			for(i = 0; i < tmp->multicast_rule_counter; i++)
			{
				printk("\r\n%s   %s    %x    %02x:%02x:%02x:%02x:%02x:%02x   %02x:%02x:%02x:%02x:%02x:%02x",(tmp->multicast_rule[i].filter_type == 0)?"forward":"discard",
					(tmp->multicast_rule[i].mac_type == 0)?"des mac":"src mac",tmp->multicast_rule[i].ethertype,
					tmp->multicast_rule[i].start_mac[0],tmp->multicast_rule[i].start_mac[1],tmp->multicast_rule[i].start_mac[2],
					tmp->multicast_rule[i].start_mac[3],tmp->multicast_rule[i].start_mac[4],tmp->multicast_rule[i].start_mac[5],
					tmp->multicast_rule[i].end_mac[0],tmp->multicast_rule[i].end_mac[1],tmp->multicast_rule[i].end_mac[2],
					tmp->multicast_rule[i].end_mac[3],tmp->multicast_rule[i].end_mac[4],tmp->multicast_rule[i].end_mac[5]);
			}
			printk("\r\n");
			return 0;
		}
		if(DBG_Level > 0)
			printk("\r\nshow mac filter rule error,can't find port====> pon mac filter rule opt");
		return -1;
	}
	if(DBG_Level > 0)
		printk("\r\noption error ====> pon mac filter switch opt");
	return -1;
}


#if KERNEL_2_6_36
static struct file_operations pon_mac_filter_fops = {
	.owner =		THIS_MODULE,
	.write =		NULL,
	.read =			NULL,
	.unlocked_ioctl =	pon_mac_filter_ioctl,
#ifdef TCSUPPORT_CPU_ARMV8_64
	.compat_ioctl	= 	pon_mac_filter_ioctl,
#endif
	.open =			pon_mac_filter_open,
	.release =		NULL,
};	
#else
static struct file_operations pon_mac_filter_fops = {
	.owner =		THIS_MODULE,
	.write =		NULL,
	.read =		NULL,
	.ioctl =		pon_mac_filter_ioctl,
	.open =		pon_mac_filter_open,
	.release =	NULL,
};
#endif

#if KERNEL_2_6_36
long pon_mac_filter_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
#else
int pon_mac_filter_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg)
#endif
{
	pon_mac_filter_ioctl_data data;
	pon_mac_filter_ioctl_data dupData;
	int ret = 0;

	if(filp == NULL)
	{
		printk("\r\nfilp is NULL return ====> pon mac filter ioctl");
		return -1;
	}
	
	memset(&data, 0, sizeof(data));
	if (copy_from_user(&data, (pon_mac_filter_ioctl_data*)arg, sizeof(data)))
	{
		return -EFAULT;
	}

#ifdef TCSUPPORT_CPU_ARMV8_64
		cmd = cmd & IOCTL_CMD;
#endif

	switch(cmd)
	{
		case PONMACFILTER_IOC_SWITCH_OPT:
			ret = pon_mac_filter_switch_option(&data,(void*)arg);
			break;

		case PONMACFILTER_IOC_RULE_OPT:
			memcpy(&dupData, &data,sizeof(pon_mac_filter_ioctl_data));
			ret = pon_mac_filter_rule_option(&data,(void*)arg);
			if(dupData.direction == PON_MAC_FILTER_UPSTREAM){
				if((dupData.option_flag == OPT_SET) || (dupData.option_flag == OPT_DEL)){
                    int mulitcast_ani = 0;
				    ECNT_API_XPON_MULITCAST_ANI_GET(&mulitcast_ani);
					if((mulitcast_ani >=0) && (mulitcast_ani < MAC_FILTER_ANI_PORT_NUM)){ //add or delete mac filter rule for mutlicast gemport
						dupData.ani_index = mulitcast_ani;
						pon_mac_filter_rule_option(&dupData,(void*)arg);
					}
				}
			}
			break;

		case PONMACFILTER_IOC_DBG_LEVEL_OPT:
			if(data.dbg_level == 3)
			{
				if(pon_mac_filter_all_data.onu_mode == MODE_HGU)
					pon_mac_filter_all_data.onu_mode = MODE_SFU;
				else
					pon_mac_filter_all_data.onu_mode = MODE_HGU;
				printk("\r\nnow mode is %s",(pon_mac_filter_all_data.onu_mode == MODE_HGU)?"HGU":"SFU");
			}
			else
			{
				DBG_Level = data.dbg_level;
				printk("\r\nset dbg level success,now value is %d",DBG_Level);
			}
			break;
			
		default:
			ret = -1;
			printk("\r\ncmd error  ========> pon mac filter ioctl");
			break;
	}
	return ret;
}

int pon_mac_filter_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int filter_data_init(pon_mac_filter_all * data)
{
	int i = 0;
	pon_mac_filter * port_data = NULL;

	if(data == NULL)
	{
		printk("\r\ndata is NULL pointer,return -1 ====> mac filter data init");
		return -1;
	}

	port_data = data->upstream_data;
	for(i = 0; i < MAC_FILTER_ANI_PORT_NUM; i++)
	{
		sprintf(port_data->port_index,"%d",i);
		port_data->unicast_rule_counter = 0;
		port_data->multicast_rule_counter = 0;
		port_data->unicast_rule = kmalloc(sizeof(pon_mac_filter_rule) * MAC_FILTER_UNICAST_RULE_LIMIT,GFP_ATOMIC);
		if(port_data->unicast_rule == NULL)
		{
			printk("\r\nmalloc mac filter data error,out of memory");
			return -1;
		}
		memset(port_data->unicast_rule,0,sizeof(pon_mac_filter_rule) * MAC_FILTER_UNICAST_RULE_LIMIT);
		port_data->multicast_rule = kmalloc(sizeof(pon_mac_filter_rule) * MAC_FILTER_MULTICAST_RULE_LIMIT,GFP_ATOMIC);
		if(port_data->multicast_rule == NULL)
		{
			printk("\r\nmalloc mac filter data error,out of memory");
			return -1;
		}
		memset(port_data->multicast_rule,0,sizeof(pon_mac_filter_rule) * MAC_FILTER_UNICAST_RULE_LIMIT);
		port_data++;
	}

	port_data = data->downstream_data;
	for(i = 0; i < data->total_port_count; i++)
	{
		port_data->unicast_rule_counter = 0;
		port_data->multicast_rule_counter = 0;
		memset(port_data->port_index,0,PORT_INDEX_LEN);

		if(i < data->lan_port_count)
		{
			port_data->port_index[0] = '1';
			port_data->port_index[1] = (i + 49);
			port_data->port_index[2] = '\0';
		}
		else if(i < data->lan_port_count + data->wlan_port_count)
		{
			port_data->port_index[0] = '2';
			port_data->port_index[1] = (i + 49 - data->lan_port_count);
			port_data->port_index[2] = '\0';
		}
		else if(i < data->lan_port_count + data->wlan_port_count + data->usb_port_count)
		{
			port_data->port_index[0] = '3';
			port_data->port_index[1] = (i + 49 - data->wlan_port_count - data->lan_port_count);
			port_data->port_index[2] = '\0';
		}
		else
		{
			return -1;
		}
		
		port_data->unicast_rule_counter = 0;
		port_data->multicast_rule_counter = 0;
		port_data->unicast_rule = kmalloc(sizeof(pon_mac_filter_rule) * MAC_FILTER_UNICAST_RULE_LIMIT,GFP_ATOMIC);
		if(port_data->unicast_rule == NULL)
		{
			printk("\r\nmalloc mac filter data error,out of memory");
			return -1;
		}
		memset(port_data->unicast_rule,0,sizeof(pon_mac_filter_rule) * MAC_FILTER_UNICAST_RULE_LIMIT);
		port_data->multicast_rule = kmalloc(sizeof(pon_mac_filter_rule) * MAC_FILTER_MULTICAST_RULE_LIMIT,GFP_ATOMIC);
		if(port_data->multicast_rule == NULL)
		{
			printk("\r\nmalloc mac filter data error,out of memory");
			return -1;
		}
		memset(port_data->multicast_rule,0,sizeof(pon_mac_filter_rule) * MAC_FILTER_UNICAST_RULE_LIMIT);
		port_data++;
	}
	
	return 0;
}

static int filter_data_clean(pon_mac_filter_all * data)
{
	int i = 0;
	pon_mac_filter * port_data = NULL;

	if(data == NULL)
	{
		printk("\r\ndata is NULL pointer,return -1 ====> mac filter data init");
		return -1;
	}

	port_data = data->upstream_data;
	for(i = 0; i < MAC_FILTER_ANI_PORT_NUM; i++)
	{
		kfree(port_data->unicast_rule);
		kfree(port_data->multicast_rule);
		port_data++;
	}
	port_data = data->downstream_data;
	for(i = 0; i < data->total_port_count; i++)
	{
		kfree(port_data->unicast_rule);
		kfree(port_data->multicast_rule);
		port_data++;
	}
	kfree(data->upstream_data);
	kfree(data->downstream_data);
	data->upstream_data = NULL;
	data->downstream_data = NULL;

	return 0;
}


static int  pon_mac_filter_init(void)
{
	int status = 0,i = 0;
	printk("%s\n", __FUNCTION__);

	pon_mac_filter_all_data.enable_flag = DISABLE;

	ECNT_API_XPON_ONU_TYPE_GET(&i);
	if(i == 1)
		pon_mac_filter_all_data.onu_mode = MODE_SFU;
	else
		pon_mac_filter_all_data.onu_mode = MODE_HGU;
	
    
	printk("\r\nMulti Lan port");
	pon_mac_filter_all_data.lan_port_count = 4;
	pon_mac_filter_all_data.wlan_port_count = 4;
	pon_mac_filter_all_data.usb_port_count = 1;
	pon_mac_filter_all_data.total_port_count = pon_mac_filter_all_data.lan_port_count + pon_mac_filter_all_data.wlan_port_count + pon_mac_filter_all_data.usb_port_count;


	pon_mac_filter_all_data.upstream_data = kmalloc(sizeof(pon_mac_filter) * MAC_FILTER_ANI_PORT_NUM,GFP_ATOMIC);
	memset(pon_mac_filter_all_data.upstream_data,0,sizeof(pon_mac_filter) * MAC_FILTER_ANI_PORT_NUM);
	pon_mac_filter_all_data.downstream_data = kmalloc(sizeof(pon_mac_filter) * (pon_mac_filter_all_data.total_port_count),GFP_ATOMIC);
	memset(pon_mac_filter_all_data.downstream_data,0,sizeof(pon_mac_filter) * (pon_mac_filter_all_data.total_port_count));
	filter_data_init(&pon_mac_filter_all_data);
	
	status = register_chrdev(PONMACFILTER_MAJOR, "ponmacfilter", &pon_mac_filter_fops);
	if (status < 0)
		return status;
	
	rcu_assign_pointer(pon_check_mac_hook, pon_check_mac);	
	rcu_assign_pointer(pon_mac_filter_get_mode_hook, pon_mac_filter_get_mode);
	return 0;
}

static void  pon_mac_filter_exit(void)
{
	filter_data_clean(&pon_mac_filter_all_data);
	
	unregister_chrdev(PONMACFILTER_MAJOR, "ponmacfilter");
	rcu_assign_pointer(pon_check_mac_hook, NULL);
	rcu_assign_pointer(pon_mac_filter_get_mode_hook, NULL);
	return;
}


module_init(pon_mac_filter_init);
module_exit(pon_mac_filter_exit);

