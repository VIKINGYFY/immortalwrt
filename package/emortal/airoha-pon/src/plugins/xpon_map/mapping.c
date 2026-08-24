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
	mapping.c

	Abstract:

	Revision History:
	Who			When			What
	--------	----------		----------------------------------------------
	Name		Date			Modification logs
	andy.Yi		2013/3/20	Create
*/

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/inet.h>
#include <linux/list.h>
#include <linux/libcompileoption.h>
#include <ecnt_hook/ecnt_hook_qdma_7510_20.h>
#include <ecnt_hook/ecnt_hook_pon_mac.h>

#include "mapping.h"
#include "xpon_mapping.h"

u8 isSfu = 1;
volatile u8 isClearQueueList=0;
DEFINE_SPINLOCK(queueLock);

#if defined(TCSUPPORT_GPON_MAPPING)
/* gpon mapping variable and function define here
 gloable table define */
static LIST_HEAD(gemPort_list);
static LIST_HEAD(queue_list);


/*******************************************************************************************
* function name
*	gemPortMappingRuleFree
* description:
*	free gem port  mapping rule.
* retrun :
*
* parameter:
* 	
********************************************************************************************/

static void gemPortMappingRuleFree(struct rcu_head *head)
{
	struct gemPortMapping_s *entry
		= container_of(head, struct gemPortMapping_s, rcu);
	kfree(entry);
	GPONMAP_PRINT(GPONMAP_MSG_DBG, "gemPortMappingRuleFree\n");
	return ;
}

/*******************************************************************************************
* function name
*	queueMappingRuleFree
* description:
*	free pq mapping rule.
* retrun :
*
* parameter:
* 	
********************************************************************************************/

static void queueMappingRuleFree(struct rcu_head *head)
{
	struct gponQueueMapping_s  *entry
		= container_of(head, struct gponQueueMapping_s, rcu);
	kfree(entry);
	GPONMAP_PRINT(GPONMAP_MSG_DBG, "queueMappingRuleFree\n");
	return ;
}

int initGponMapping(void)
{
	return GPONMAP_SUCCESS;
}

int exitGponMapping(void)
{
	gemPortMapping_ptr curGemPortRule = NULL;
	gponQueueMapping_ptr curQueueRule = NULL;

	list_for_each_entry(curGemPortRule, &gemPort_list, list)
	{
		GPONMAP_PRINT(GPONMAP_MSG_DBG, "exitGponMapping enter gemport_list\n");
		//del gem port mapping rule
		list_del_rcu(&curGemPortRule->list);
		call_rcu(&(curGemPortRule->rcu), gemPortMappingRuleFree);
		GPONMAP_PRINT(GPONMAP_MSG_DBG, "exitGponMapping end gemport_list\n");
	}
	spin_lock_bh(&queueLock);
	if(isClearQueueList){
		spin_unlock_bh(&queueLock);
		return GPONMAP_SUCCESS;			
	}
	isClearQueueList =1;
	spin_unlock_bh(&queueLock);
	
	list_for_each_entry(curQueueRule, &queue_list, list)
	{
		GPONMAP_PRINT(GPONMAP_MSG_DBG, "exitGponMapping enter queue_list\n");
		if (ECNT_API_XPON_GEMPORT_REMOVE(curQueueRule->gemPort) != 0)
		{
			GPONMAP_PRINT(GPONMAP_MSG_ERR, "delQueueMappingRule: xmcs_remove_gem_port fail on line = %d\n", __LINE__);		
			//return GPONMAP_FAIL;
		}

		//del queue mapping rule
		list_del_rcu(&curQueueRule->list);
		call_rcu(&(curQueueRule->rcu), queueMappingRuleFree);
		GPONMAP_PRINT(GPONMAP_MSG_DBG, "exitGponMapping end queue_list\n");
	}
	isClearQueueList =0;
	
	return GPONMAP_SUCCESS;
}

/*******************************************************************************************
* function name
*	matchGemPortMappingRule
* description:
*	match the gem port mapping rule
* retrun :
*
* parameter:
* 	
********************************************************************************************/
static inline int matchGemPortMappingRule(gemPortMapping_ptr curRule, gemPortMappingIoctl_ptr tableEntry)
{
	if (curRule->tagCtl != tableEntry->tagCtl)
		return -1;
	if (curRule->tagFlag != tableEntry->tagFlag)
		return -1;
	if (curRule->userPort != tableEntry->userPort)
		return -1;
	if (curRule->vid != tableEntry->vid)
		return -1;
	if (curRule->dscp != tableEntry->dscp)
		return -1;
	if (curRule->pbit != tableEntry->pbit)
		return -1;

	if (curRule->gemPort != tableEntry->gemPort)
		return -1;
	
	return 0;
}

/*******************************************************************************************
* function name
*	getNumOfEnableBit
* description:
*	get the enable bit num of tagCtl.
* retrun :
*
* parameter:
* 	
********************************************************************************************/
static inline int getNumOfEnableBit(u16 tagCtl)
{
	int num = 0;

	if (tagCtl & GEMPORT_MAPPING_TAGFLAG)
		num++;
	if (tagCtl & GEMPORT_MAPPING_USERPORT)
		num++;
	if(TCSUPPORT_ALPHION_PON_VAL)
	{
		if (tagCtl & GEMPORT_MAPPING_ANI_PORT)
			num++;
	}
	if (tagCtl & GEMPORT_MAPPING_VID)
		num++;
	if (tagCtl & GEMPORT_MAPPING_DSCP)
		num++;
	if (tagCtl & GEMPORT_MAPPING_PBIT)
		num++;
	if(TCSUPPORT_ALPHION_PON_VAL)
	{
		if (tagCtl & GEMPORT_MAPPING_GEMPORT)
			num++;
	}
	if(TCSUPPORT_GOOGLE_FIBER_VAL)
	{
		if (tagCtl & GEMPORT_MAPPING_DEF_PBIT)
			num++;
	}
	return num;
}
static int checkBridgePort(u8 userPort, u16 gemPort)
{
	gemPortMapping_ptr curRule = NULL;
	list_for_each_entry_rcu(curRule, &gemPort_list, list)
	{
		if(curRule->userPort == userPort && curRule->gemPort == gemPort)
		{
			return GPONMAP_SUCCESS;
		}
	}
    
    if (GPON_MAP_IPHOST_VOICE_PORT == userPort){
        /*do not check VOIP wan */
        return GPONMAP_SUCCESS;
    } else {
	    return GPONMAP_FAIL;
    }

}
/*******************************************************************************************
* function name
*	findGemPortMappingRule
* description:
*	find gem port mapping rule.
* retrun :
*	success: Rule
*	fail: NULL
* parameter:
* 	
********************************************************************************************/
gemPortMapping_ptr findGemPortMappingRule(gemPortMappingIoctl_ptr tableEntry)
{
	gemPortMapping_ptr curRule = NULL;

	if (tableEntry == NULL)
		return NULL;

	list_for_each_entry_rcu(curRule, &gemPort_list, list)	
	{	
		if(tableEntry->tagCtl & GEMPORT_MAPPING_USERPORT)
		{
			if((tableEntry->userPort == GPON_MAP_IPHOST_VOICE_PORT) && !(curRule->tagCtl & GEMPORT_MAPPING_VID))
				continue;
		}
		//tagFlag
		if (tableEntry->tagCtl & GEMPORT_MAPPING_TAGFLAG) 
		{
			if ((curRule->tagCtl & GEMPORT_MAPPING_TAGFLAG) && (curRule->tagFlag != tableEntry->tagFlag))
				continue;
		}
		else	
		{	
			if (curRule->tagCtl & GEMPORT_MAPPING_TAGFLAG)
				continue;
		}
		//vid	
		if (tableEntry->tagCtl & GEMPORT_MAPPING_VID) 
		{
			if ((curRule->tagCtl & GEMPORT_MAPPING_VID) && (curRule->vid != tableEntry->vid))
				continue;
		}
		else		
		{
			if (curRule->tagCtl & GEMPORT_MAPPING_VID)
				continue;	
		}
		//userport
		if (tableEntry->tagCtl & GEMPORT_MAPPING_USERPORT) 
		{
			if ((curRule->tagCtl & GEMPORT_MAPPING_USERPORT) && (curRule->userPort != tableEntry->userPort))
				continue;
		}
		else		
		{
			if (curRule->tagCtl & GEMPORT_MAPPING_USERPORT)
				continue;	
		}
		
		//dscp
		if (tableEntry->tagCtl & GEMPORT_MAPPING_DSCP) 
		{
			if ((curRule->tagCtl & GEMPORT_MAPPING_DSCP) && (curRule->dscp != tableEntry->dscp))
				continue;
		}
		else	
		{	
			if (curRule->tagCtl & GEMPORT_MAPPING_DSCP)
				continue;	
		}
		//pbit	
		if (tableEntry->tagCtl & GEMPORT_MAPPING_PBIT) 
		{
			if ((curRule->tagCtl & GEMPORT_MAPPING_PBIT) && (curRule->pbit != tableEntry->pbit))
				continue;
		}
		else		
		{
			if (curRule->tagCtl & GEMPORT_MAPPING_PBIT)
				continue;	
		}

		if(checkBridgePort(tableEntry->userPort,curRule->gemPort) != GPONMAP_SUCCESS)
			continue;

		return curRule;
	}
	
	return NULL;
}

/*******************************************************************************************
* function name
*	displayAllGemPortMappingRule
* description:
*	display all  gem port mapping rule.
* retrun :
*
* parameter:
* 	
********************************************************************************************/
int displayAllGemPortMappingRule(void)
{
	gemPortMapping_ptr mapping_ptr = NULL;
	int bitOffset = 0;

	enum{
        BIT_POS_TAG_FLAG,
        BIT_POS_USER_PORT,
        BIT_POS_ANI_PORT,
        BIT_POS_VID,
        BIT_POS_DSCP,
        BIT_POS_PBIT,
        BIT_POS_GEMPORT,
        BIT_POS_MAX,
	};

	printk("gemPortmappingRule-->\n");
	/*printk("tagctl	tagFlag	uni	vid	dscp	pbit	gemPort\n");	*/
	printk("tagFlag	uni	vid	dscp	pbit	gemPort\n");
	rcu_read_lock();
	list_for_each_entry_rcu(mapping_ptr, &gemPort_list, list)	
	{
		/*printk("%02x	%02d	%02d	%02d	%02d	%02d	%02d\n",
			mapping_ptr->tagCtl, mapping_ptr->tagFlag, mapping_ptr->userPort,mapping_ptr->vid, mapping_ptr->dscp,
			mapping_ptr->pbit, mapping_ptr->gemPort);*/
        /*printk("%02x	", mapping_ptr->tagCtl);*/
        
		for(bitOffset = 0; bitOffset < BIT_POS_MAX; bitOffset++)
		{
		    if(bitOffset == BIT_POS_ANI_PORT)
		        continue;
		    if( (mapping_ptr->tagCtl >> bitOffset ) & 1)
		    {
		        switch(bitOffset)
		        {
		            case BIT_POS_TAG_FLAG:
		                printk("%02d	", mapping_ptr->tagFlag); 
		                break;
		            case BIT_POS_USER_PORT:
		                printk("%02d	", mapping_ptr->userPort); 
		                break;
		            case BIT_POS_VID:
		                printk("%02d	", mapping_ptr->vid); 
		                break;
		            case BIT_POS_DSCP:
		                printk("%02d	", mapping_ptr->dscp); 
		                break;
		            case BIT_POS_PBIT:
		                printk("%02d	", mapping_ptr->pbit); 
		                break;
		            case BIT_POS_GEMPORT:
		                printk("%02d	", mapping_ptr->gemPort); 
		                break;
		        }
		    }
		    else{
		        printk("N/A	");
		    }
		}
		
		printk("\n");
	}
	rcu_read_unlock();

	return GPONMAP_SUCCESS;
}



/*******************************************************************************************
* function name
*	findQueueMappingRule
* description:
*	find gem port--> pq mapping rule.
* retrun :
*
* parameter:
* 	
********************************************************************************************/
gponQueueMapping_ptr findQueueMappingRule(u16 gemPort)
{
	gponQueueMapping_ptr curRule = NULL;

	list_for_each_entry_rcu(curRule, &queue_list, list)	
	{
		if (curRule->gemPort == gemPort)
		{
			return curRule;
		}
	}
	
	return NULL;
}

/*******************************************************************************************
* function name
*	getNumOfQueueBit
* description:
*	get the enable bit num of queue id.
* retrun :
*
* parameter:
* 	
********************************************************************************************/
static inline int getNumOfQueue(u8 queueInfo)
{
	int i = 0;
	int queueNum = 0;

	for (i = 0; i < 8; i++)
	{
		if (queueInfo & (1 << i))
			queueNum ++;
	}

	return queueNum;
}

/*******************************************************************************************
* function name
*	searchMultiQueueQosEnable
* description:
*	if exist multi queue, enable qos, else disable qos.
* return :
*
* parameter:
* 	
********************************************************************************************/
static int searchMultiQueueQosEnable(void)
{
	gponQueueMapping_ptr curRule = NULL;
	int i = 0;
	u8 qosEnable = 0;
	struct XMCS_TcontInfo_S tcontInfo;
	int maxQueueNum = 0;
	int curQueueNum = 0;
	int allQueueNum = 0;

	memset(&tcontInfo, 0, sizeof(tcontInfo));
	for (i = 0; i < GPON_TCONT_MAX_NUM; i++)
	{
		tcontInfo.info[i].allocId = 0xFF;
	}

	list_for_each_entry_rcu(curRule, &queue_list, list)	
	{	
		GPONMAP_PRINT(GPONMAP_MSG_DBG, "searchMultiQueueQosEnable: curRule->gemPort = %d, line = %d \n", curRule->gemPort, __LINE__);			
		if ( (curRule->mode & (1 << MODE_OFFSET_GEM_TYPE)) == 0)
		{	
			for (i = 0; i < GPON_TCONT_MAX_NUM; i++)
			{		
				if (tcontInfo.info[i].allocId == 0xFF)/*not find the curRule->allocId in tcontInfo table*/
				{
					tcontInfo.info[i].allocId = curRule->allocId;
					tcontInfo.info[i].channel |= (1 << (curRule->queue & 0x07));
					break;
				}
				else if (tcontInfo.info[i].allocId == curRule->allocId)/*the curRule->allocId is exist in tcontInfo table*/
				{
					tcontInfo.info[i].channel |= (1 << (curRule->queue & 0x07));
					break;
				}
			}			
		}
	}

	for (i = 0; i < GPON_TCONT_MAX_NUM; i++)
	{
		curQueueNum = getNumOfQueue(tcontInfo.info[i].channel) ;
		allQueueNum += curQueueNum ;
		if (maxQueueNum < curQueueNum)
			maxQueueNum = curQueueNum ;
	}
	qosEnable = (allQueueNum > 1) ? XPON_ENABLE: XPON_DISABLE;
     //del data service, allQueueNum = 0,turn on green drop, to protect omci config 
    if(0 == allQueueNum)
    {
        qosEnable = XPON_ENABLE;
        maxQueueNum = 2;
    }
	GPONMAP_PRINT(GPONMAP_MSG_DBG, "searchMultiQueueQosEnable:allQueueNum = %d,  maxQueueNum = %d, line = %d\n",allQueueNum, maxQueueNum, __LINE__);	
	ECNT_API_XPON_QOS_SET(qosEnable, maxQueueNum);
	if(isSfu)
		ECNT_QDMA_GREEN_DROP_CTRL_HOOK(ECNT_QDMA_SET_QOS_FLAG,qosEnable);
	return -1;
}


int isLessGemMappingRule(gemPortMapping_ptr curRule,gemPortMappingIoctl_ptr tableEntry,int num)
{
	int curNum = 0; //num of enable bits in curRule->tagCtl

	if(!TCSUPPORT_GOOGLE_FIBER_VAL)
        return 0;

	curNum = getNumOfEnableBit(curRule->tagCtl);
	if ((curNum >= num) || (tableEntry->gemPort != curRule->gemPort) || (0!= (~(tableEntry->tagCtl) & curRule->tagCtl)))
	{
		return FALSE;
	}
	else //search the bits less than the tableEntry->tagCtl
	{
		if((curRule->tagCtl & GEMPORT_MAPPING_USERPORT) 
        && (curRule->userPort != tableEntry->userPort))
        	return FALSE;

        if((curRule->tagCtl & GEMPORT_MAPPING_VID) 
        /*&& (curRule->vid != tableEntry->vid)*/)
        	return FALSE;

        if((curRule->tagCtl & GEMPORT_MAPPING_DSCP) 
        && (curRule->dscp != tableEntry->dscp))
        	return FALSE;

        if((curRule->tagCtl & GEMPORT_MAPPING_PBIT) 
        && (curRule->pbit != tableEntry->pbit))
        	return FALSE;
     }

    return TRUE;
}

int checkAndDelLessMappingRule(gemPortMappingIoctl_ptr tableEntry)
{
	gemPortMapping_ptr curRule = NULL;
    int num = 0;   //num of  enable bits in tableEntry->tagCtl
	if(!TCSUPPORT_GOOGLE_FIBER_VAL)
        return 0;
   
	rcu_read_lock();	
	num = getNumOfEnableBit(tableEntry->tagCtl);	
	list_for_each_entry_rcu(curRule, &gemPort_list, list)
	{
		if(TRUE == isLessGemMappingRule(curRule,tableEntry,num))
		{
            GPONMAP_PRINT(GPONMAP_MSG_ERR, "Del less GemPortMappingRule: tagCtl %x uni %d vid %d dscp %d pbit %d gemport %d\n",
        		curRule->tagCtl,curRule->userPort,curRule->vid,curRule->dscp,curRule->pbit,curRule->gemPort);
            /* del less gemport rule */
        	list_del_rcu(&curRule->list);
            call_rcu(&(curRule->rcu), gemPortMappingRuleFree);
            //return 0;
		}
	}
    
    rcu_read_unlock();
    return 0;
}


/*******************************************************************************************
* function name
*	addGemPortMappingRule
* description:
*	add one GemPort Mapping Rule.
* retrun :
*
* parameter:
* 	
********************************************************************************************/
int addGemPortMappingRule(gemPortMappingIoctl_ptr tableEntry)
{
	gemPortMapping_ptr newRule = NULL;
	gemPortMapping_ptr curRule = NULL;
	gponQueueMapping_ptr curQueueRule = NULL;
	struct list_head * insert_pos = NULL;
	int num = 0;   //num of  enable bits in tableEntry->tagCtl
	int curNum = 0; //num of enable bits in curRule->tagCtl
	if (tableEntry == NULL)
		return GPONMAP_FAIL;

	rcu_read_lock();
	list_for_each_entry(curQueueRule, &queue_list, list)
	{
		if ((curQueueRule->gemPort == tableEntry->gemPort) && (curQueueRule->mode & (1<<MODE_OFFSET_GEM_TYPE)))
		{
	              rcu_read_unlock();	
			return GPONMAP_SUCCESS;
		}
	}
	rcu_read_unlock();

	if(TCSUPPORT_GOOGLE_FIBER_VAL)
    	checkAndDelLessMappingRule(tableEntry);

    
	GPONMAP_PRINT(GPONMAP_MSG_ERR, "addGemPortMappingRule: tagCtl %x uni %d vid %d dscp %d pbit %d gemport %d\n",
        tableEntry->tagCtl,tableEntry->userPort,tableEntry->vid,tableEntry->dscp,tableEntry->pbit,tableEntry->gemPort);
	num = getNumOfEnableBit(tableEntry->tagCtl);
	rcu_read_lock();

	if(list_empty(&gemPort_list))
	{
		insert_pos = &gemPort_list;
	}
	else
	{
		list_for_each_entry_rcu(curRule, &gemPort_list, list)
		{
			curNum = getNumOfEnableBit(curRule->tagCtl);
			if (curNum == num)
			{
				if (matchGemPortMappingRule(curRule, tableEntry) == 0)
				{
					GPONMAP_PRINT(GPONMAP_MSG_DBG, "addGemPortMappingRule: rule exist!\n");
					rcu_read_unlock();
					return GPONMAP_ENTRY_EXIST;
				}
			}
			else if (curNum < num) //search the bits less than the tableEntry->tagCtl
			{
				GPONMAP_PRINT(GPONMAP_MSG_DBG, "addGemPortMappingRule: rule can be inserted in %d cell end!\n", num);
				break;
			}

		}
		insert_pos = &curRule->list;
	}

	GPONMAP_PRINT(GPONMAP_MSG_DBG, "addGemPortMappingRule: add a rule\n");
	newRule = kmalloc(1 * sizeof(gemPortMapping_t), GFP_ATOMIC);
	if (newRule == NULL)
	{
		rcu_read_unlock();
		return GPONMAP_FAIL;
	}

	newRule->tagCtl = tableEntry->tagCtl;
	newRule->tagFlag = tableEntry->tagFlag;
	newRule->userPort = tableEntry->userPort;
	newRule->vid = tableEntry->vid;
	newRule->dscp= tableEntry->dscp;
	newRule->pbit = tableEntry->pbit;
	newRule->gemPort = tableEntry->gemPort;

	INIT_LIST_HEAD(&newRule->list);
	
	list_add_tail_rcu(&newRule->list, insert_pos);

    rcu_read_unlock();
  

	return GPONMAP_SUCCESS;
}

/*******************************************************************************************
* function name
*	delGemPortMappingRule
* description:
*	del one GemPort Mapping Rule.
* retrun :
*
* parameter:
* 	
********************************************************************************************/
int delGemPortMappingRule(gemPortMappingIoctl_ptr tableEntry)
{
	gemPortMapping_ptr curRule = NULL;
	
	if (tableEntry == NULL)
		return GPONMAP_FAIL;

	list_for_each_entry(curRule, &gemPort_list, list)
	{
		if (matchGemPortMappingRule(curRule, tableEntry) == 0)
		{
			if (curRule->gemPort == tableEntry->gemPort)
			{
				//del gem port mapping rule
				list_del_rcu(&curRule->list);
                call_rcu(&(curRule->rcu), gemPortMappingRuleFree);
           
				return GPONMAP_SUCCESS;
			}
		}

	}
			
	return GPONMAP_ENTRY_NOT_FOUND;
}

/*******************************************************************************************
* function name
*	addQueueMappingRule
* description:
*	add one GemPort --> pq Mapping Rule.
* retrun :
*
* parameter:
* 	
********************************************************************************************/
int addQueueMappingRule(gponQueueMappingIoctl_ptr tableEntry)
{
	gponQueueMapping_ptr newRule = NULL;
	gponQueueMapping_ptr oldRule = NULL;
	struct XMCS_TcontInfo_S tcontInfo;
	struct XMCS_GemPortCreate_S gemPortTcont;
	int i = 0;
	gponQueueMapping_ptr mapping_ptr = NULL;

	if (tableEntry == NULL)
		return GPONMAP_FAIL;

	if (tableEntry->gemPort == RECONFIG_GPONMAP_RULE)
	{
		if(isClearQueueList){
			return GPONMAP_SUCCESS;		
		}	
		GPONMAP_PRINT(GPONMAP_MSG_DBG, "gemport-->tcont mapping reconfig-->\n");
		memset(&tcontInfo, 0, sizeof(tcontInfo));	
		if (ECNT_API_XPON_TCONT_INFO_GET((void *)&tcontInfo) != 0)
		{
			GPONMAP_PRINT(GPONMAP_MSG_ERR, "addQueueMappingRule: xmcs_get_tcont_info fail on line = %d\n", __LINE__);		
			return GPONMAP_FAIL;
		}
		rcu_read_lock();
		list_for_each_entry_rcu(mapping_ptr, &queue_list, list) 
		{	
			memset(&gemPortTcont, 0, sizeof(struct XMCS_GemPortCreate_S));
			gemPortTcont.gemPortId = mapping_ptr->gemPort;
			gemPortTcont.gemType = (mapping_ptr->mode & (1 << MODE_OFFSET_GEM_TYPE) )? GPON_MULTICAST_GEM: GPON_UNICAST_GEM;
			
			GPONMAP_PRINT(GPONMAP_MSG_DBG, "addQueueMappingRule: tcontInfo.entryNum = %d, on line = %d\n",	tcontInfo.entryNum,__LINE__);
			for (i = 0; i < tcontInfo.entryNum; i++)	
			{
				GPONMAP_PRINT(GPONMAP_MSG_DBG, "addQueueMappingRule: i = %d, on line = %d\n",  i, __LINE__);
				if (tcontInfo.info[i].allocId == mapping_ptr->allocId)
				{
					break;
				}
			}
            if(i == tcontInfo.entryNum && gemPortTcont.gemType == GPON_UNICAST_GEM)
            {
                continue;
            }
			gemPortTcont.allocId = tcontInfo.info[i].allocId;
			GPONMAP_PRINT(GPONMAP_MSG_DBG, "addQueueMappingRule: xmcs_create_gem_port on line = %d, gemPortId = %x, gemType = %s, allocId = %x\n", 
				__LINE__, gemPortTcont.gemPortId, (gemPortTcont.gemType == GPON_MULTICAST_GEM) ? "multicast": "unicast",	gemPortTcont.allocId);
			if (ECNT_API_XPON_GEMPORT_CREATE((void *)&gemPortTcont) != 0)
			{
				GPONMAP_PRINT(GPONMAP_MSG_ERR, "addQueueMappingRule: xmcs_create_gem_port fail on line = %d\n", __LINE__);
				rcu_read_unlock();
				return GPONMAP_FAIL;
			}
			if(isClearQueueList){
				rcu_read_unlock();
				return GPONMAP_SUCCESS;		
			}
		}
		rcu_read_unlock();
		goto successHandle;
	}

/* 
	get gpon drivers api
    	get tcont id info
*/
	if (tableEntry->gemType != GPON_MULTICAST_GEM)
	{
		memset(&tcontInfo, 0, sizeof(tcontInfo));	
		if (ECNT_API_XPON_TCONT_INFO_GET((void *)&tcontInfo) != 0)
		{
				GPONMAP_PRINT(GPONMAP_MSG_ERR, "addQueueMappingRule: xmcs_get_tcont_info fail on line = %d\n", __LINE__);		
				return GPONMAP_FAIL;
		}

		for (i = 0; i < tcontInfo.entryNum; i++)
		{
			if (tcontInfo.info[i].allocId == tableEntry->allocId)
			{
				break;
			}
		}
			
		if (i == tcontInfo.entryNum)
		{
			GPONMAP_PRINT(GPONMAP_MSG_ERR, "addQueueMappingRule: allocId is invalid  on line = %d\n", __LINE__);		
			goto addRule;
			//return GPONMAP_FAIL;
		}
	}

/* 
	set to gpon drivers api
    	create a gem port mapping to alloc id
*/
	memset(&gemPortTcont, 0, sizeof(struct XMCS_GemPortCreate_S));
	gemPortTcont.gemType = tableEntry->gemType;
	gemPortTcont.gemPortId = tableEntry->gemPort;
	gemPortTcont.allocId = tableEntry->allocId;
	GPONMAP_PRINT(GPONMAP_MSG_DBG, "addQueueMappingRule: xmcs_create_gem_port on line = %d, gemPortId = %x, gemType = %s, allocId = %x\n", 
		__LINE__, gemPortTcont.gemPortId, (gemPortTcont.gemType == GPON_MULTICAST_GEM) ? "multicast": "unicast",	gemPortTcont.allocId);
	if (ECNT_API_XPON_GEMPORT_CREATE((void *)&gemPortTcont) != 0)
	{
		GPONMAP_PRINT(GPONMAP_MSG_ERR, "addQueueMappingRule: xmcs_create_gem_port fail on line = %d\n", __LINE__);
		return GPONMAP_FAIL;
	}

addRule:
/* 
	add rule to queue mapping list .
*/
	rcu_read_lock();
	if ((oldRule = findQueueMappingRule(tableEntry->gemPort) ) != NULL)
	{
		oldRule->mode = (tableEntry->pqMode) | (tableEntry->tsEnable << MODE_OFFSET_TRAFFIC_SHAPING) | (tableEntry->gemType << MODE_OFFSET_GEM_TYPE) ;
		oldRule->gemPort = tableEntry->gemPort;
		oldRule->allocId = tableEntry->allocId;
		oldRule->queue = (tableEntry->queue) |(tableEntry->tsChannelId << QUEUE_TRAFFIC_SHAPING_OFFSET);
		rcu_read_unlock();
		
		goto successHandle;	
	}
	rcu_read_unlock();

	newRule = kmalloc(1 * sizeof(gponQueueMapping_t), GFP_ATOMIC);
	if (newRule == NULL)
	{
		return GPONMAP_FAIL;
	}
	newRule->mode = (tableEntry->pqMode) | (tableEntry->tsEnable << MODE_OFFSET_TRAFFIC_SHAPING) | (tableEntry->gemType << MODE_OFFSET_GEM_TYPE) ;
	newRule->gemPort = tableEntry->gemPort;
	newRule->allocId = tableEntry->allocId;
	newRule->queue = (tableEntry->queue) |(tableEntry->tsChannelId << QUEUE_TRAFFIC_SHAPING_OFFSET);

	INIT_LIST_HEAD(&newRule->list);
	list_add_rcu(&newRule->list, &queue_list);
	
successHandle:
	searchMultiQueueQosEnable();
	clearAllHwnatRules();
	return GPONMAP_SUCCESS;
}

/*******************************************************************************************
* function name
*	delQueueMappingRule
* description:
*	del  one GemPort --> pq Mapping Rule.
* retrun :
*
* parameter:
* 	
********************************************************************************************/
int delQueueMappingRule(gponQueueMappingIoctl_ptr tableEntry)
{
	gponQueueMapping_ptr curRule = NULL;
	gemPortMapping_ptr gemPortRule = NULL;
	
	if (tableEntry == NULL)
		return GPONMAP_FAIL;

	if (tableEntry->gemPort == EMPTY_GPONMAP_RULE)
	{
		ECNT_API_XPON_QOS_SET(XPON_ENABLE, 1);
		exitGponMapping();
		goto successHandle;
	}
	
	if(TCSUPPORT_CF_JOYMEV2_PON_VAL)
	{
		list_for_each_entry(gemPortRule, &gemPort_list, list)
		{
			if(gemPortRule->gemPort == tableEntry->gemPort)
			{
				return GPONMAP_SUCCESS;
			}
		}	
	}
	list_for_each_entry(curRule, &queue_list, list)
	{
		if (curRule->gemPort == tableEntry->gemPort)
		{	
			if (ECNT_API_XPON_GEMPORT_REMOVE(tableEntry->gemPort) != 0)
			{
				GPONMAP_PRINT(GPONMAP_MSG_ERR, "delQueueMappingRule: xmcs_remove_gem_port fail on line = %d\n", __LINE__);		
				//return GPONMAP_FAIL;
			}
			
			//del queue mapping rule
			list_del_rcu(&curRule->list);
			call_rcu(&(curRule->rcu), queueMappingRuleFree);
			
			searchMultiQueueQosEnable();
			goto successHandle;
		}
	}
			
	return GPONMAP_ENTRY_NOT_FOUND;

successHandle:
	clearAllHwnatRules();
	return GPONMAP_SUCCESS;
}

/*******************************************************************************************
* function name
*	getQueueMappingRule
* description:
*	get the queue mapping rule.
* retrun :
*
* parameter:
* 
********************************************************************************************/
int getQueueMappingRule(gponQueueMappingIoctl_ptr tableEntry)
{
	gponQueueMapping_ptr tmpEntry = NULL;

	rcu_read_lock();
	list_for_each_entry_rcu(tmpEntry, &queue_list, list)
	{
		if(tmpEntry->gemPort == tableEntry->gemPort){
			tableEntry->allocId = tmpEntry->allocId;
			tableEntry->queue = tmpEntry->queue & 0x07;
		}
	}
	rcu_read_unlock();

	return GPONMAP_SUCCESS;
}


/*******************************************************************************************
* function name
*	reCfgQueueMappingRule
* description:
*	reconfigure the queue mapping rule
* retrun :
*
* parameter:
* 
********************************************************************************************/
int reCfgQueueMappingRule(gponQueueMappingIoctl_ptr tableEntry)
{
	gponQueueMapping_ptr tmpEntry = NULL;

	rcu_read_lock();
	list_for_each_entry_rcu(tmpEntry, &queue_list, list)
	{
		if(tmpEntry->gemPort == tableEntry->gemPort){
			GPONMAP_PRINT(GPONMAP_MSG_DBG, "reCfgQueueMappingRule gemPort=%d, oldQueue=%d, newQueue=%d\n",tmpEntry->gemPort,tmpEntry->queue & 0x07 ,tableEntry->queue & 0x07);
			tmpEntry->queue = (tableEntry->queue & 0x07)  | (tmpEntry->queue & 0xf8);
		}
	}
	rcu_read_unlock();

	clearAllHwnatRules();

	return GPONMAP_SUCCESS;
}

/*******************************************************************************************
* function name
*	displayAllQueueMappingRule
* description:
*	display all  queue mapping rule.
* retrun :
*
* parameter:
* 	
********************************************************************************************/
int displayAllQueueMappingRule(void)
{
	gponQueueMapping_ptr mapping_ptr = NULL;

	printk("queuemapping-->\n");
	printk("gemPort	pqMode	tcont	queue	tse	tsChannelId\n");	
	rcu_read_lock();
	list_for_each_entry_rcu(mapping_ptr, &queue_list, list)	
	{
		printk("%02d	%02d	%02d	%02d	%02d	%02d\n", 
			mapping_ptr->gemPort, (mapping_ptr->mode & 0x01) , mapping_ptr->allocId, (mapping_ptr->queue & 0x07), 
			((mapping_ptr->mode & 0x02) >> MODE_OFFSET_TRAFFIC_SHAPING), ((mapping_ptr->queue & 0xf8) >> QUEUE_TRAFFIC_SHAPING_OFFSET));
	}
	rcu_read_unlock();
	return GPONMAP_SUCCESS;
}


#if defined(TCSUPPORT_GPON_DOWNSTREAM_MAPPING)
#define CHECK_RULE_MATCH(tagMask,ruleTagctl,ruleVal,pkgVal)   \
    if(!((ruleTagctl & tagMask) && (ruleVal == pkgVal))) \
    { \
    	continue; \
    }
#define CHECK_RULE_MATCH_UNSTRICT(tagMask,ruleTagctl,ruleVal,pkgVal)   \
    if((ruleTagctl & tagMask) && (ruleVal != pkgVal)) \
    { \
    	continue; \
    }
#define CHECK_RULE_MATCH_STRICT(tagMask,ruleTagctl,ruleVal,pkgVal)   \
		if(!(ruleTagctl & tagMask) || (ruleVal != pkgVal)) \
		{ \
			continue; \
		}

gemPortMapping_ptr findUniByMappingRule(gemPortMappingIoctl_ptr tableEntry)
{
	gemPortMapping_ptr curRule = NULL;

	if (tableEntry == NULL)
		return NULL;

	list_for_each_entry_rcu(curRule, &gemPort_list, list)	
	{
		if(!(curRule->tagCtl & GEMPORT_MAPPING_USERPORT)) continue;
    	
		//gemport
		CHECK_RULE_MATCH_UNSTRICT(GEMPORT_MAPPING_GEMPORT,curRule->tagCtl,curRule->gemPort,tableEntry->gemPort);
        
		//vid	
		CHECK_RULE_MATCH_UNSTRICT(GEMPORT_MAPPING_VID,    curRule->tagCtl,curRule->vid,tableEntry->vid);

		//pbit	
		CHECK_RULE_MATCH_UNSTRICT(GEMPORT_MAPPING_PBIT,   curRule->tagCtl,curRule->pbit,tableEntry->pbit);

		return curRule;
	}
	
	return NULL;	
}

int findUniByMappingRuleExt(gemPortMappingIoctl_ptr tableEntry)
{
	gemPortMapping_ptr curRule = NULL;
	int match_uni_num = 0;
	int uni_port = -1;
	int exist_bind_vlan = 0;	
	int equal_bind_vlan = 0;

	if (tableEntry == NULL)
		return -1;

	list_for_each_entry_rcu(curRule, &gemPort_list, list)	
	{
		if(!(curRule->tagCtl & GEMPORT_MAPPING_USERPORT)) continue;
    	
		//gemport
		CHECK_RULE_MATCH_UNSTRICT(GEMPORT_MAPPING_GEMPORT,curRule->tagCtl,curRule->gemPort,tableEntry->gemPort);
        
		//vid	
		CHECK_RULE_MATCH_UNSTRICT(GEMPORT_MAPPING_VID,    curRule->tagCtl,curRule->vid,tableEntry->vid);

		//pbit	
		CHECK_RULE_MATCH_UNSTRICT(GEMPORT_MAPPING_PBIT,   curRule->tagCtl,curRule->pbit,tableEntry->pbit);

		if(uni_port != curRule->userPort){
			match_uni_num++;
		}
		
		if(uni_port == -1){
			uni_port = curRule->userPort;
			GPONMAP_PRINT(GPONMAP_MSG_TRACE, "****match: uni=%d, gemPort=%d, vlan=%d, pbit=%d, ctrl=%d\n",curRule->userPort,curRule->gemPort,curRule->vid,curRule->pbit,curRule->tagCtl);
		}

		if(match_uni_num > 1){
			GPONMAP_PRINT(GPONMAP_MSG_TRACE, "****match_uni_num=%d, uni=%d, gemPort=%d, vlan=%d, pbit=%d, ctrl=%d\n",match_uni_num,curRule->userPort,curRule->gemPort,curRule->vid,curRule->pbit,curRule->tagCtl);
			return -1;
		}
	}

	if(uni_port != -1){		
		list_for_each_entry_rcu(curRule, &gemPort_list, list){			
			if(!(curRule->tagCtl & GEMPORT_MAPPING_USERPORT)) 
				continue;			
			if(curRule->userPort != uni_port) 
				continue;			
			if(curRule->tagCtl & GEMPORT_MAPPING_VID){				
				if(!exist_bind_vlan){					
					exist_bind_vlan = 1;				
					}				
				if(curRule->vid == tableEntry->vid && !equal_bind_vlan){					
					equal_bind_vlan = 1;					
					break;				
					}			
				}		
			}		
		if(exist_bind_vlan && !equal_bind_vlan){			
			return -1;		
			}	
		}
		
	return uni_port;	
}

#undef CHECK_RULE_MATCH_UNSTRICT
#undef CHECK_RULE_MATCH

gpon_downstream_mapping gpon_downstream_mapping_data;

int init_downstream_mapping_rule(void)
{
	int i = 0;

	gpon_downstream_mapping_data.mapping_switch = XPON_DISABLE;
	gpon_downstream_mapping_data.rule_counter = 0;

	gpon_downstream_mapping_data.rule = kmalloc(sizeof(gpon_downstream_mapping_rule) * MAX_GEM_PORT_NUM,GFP_ATOMIC);
	if(gpon_downstream_mapping_data.rule == NULL)
	{
		printk("malloc downstream rule error,out of memory\n");
		return -1;
	}
	
	for(i = 0; i < MAX_GEM_PORT_NUM; i++)
	{
		gpon_downstream_mapping_data.rule[i].gem_port_num = 4096;
		gpon_downstream_mapping_data.rule[i].if_mask = 0;
		gpon_downstream_mapping_data.rule[i].queue = 8;
		gpon_downstream_mapping_data.rule[i].ds_pq_enable = 0;
		gpon_downstream_mapping_data.rule[i].ds_trtcm_enable= 0;
	}
	return 0;
}

int clean_downstream_mapping_rule(void)
{
	kfree(gpon_downstream_mapping_data.rule);
	return 0;
}

int get_gpon_downstream_mapping_switch(void)
{
	return gpon_downstream_mapping_data.mapping_switch;
}

int gpon_downstream_mapping_switch_option(gpon_downstream_mapping_ioctl * data,void * arg)
{
	if(data->option_flag == OPT_GET)
	{
		data->downstream_mapping_switch = gpon_downstream_mapping_data.mapping_switch;
		if (copy_to_user((void __user *)arg, data, sizeof(gpon_downstream_mapping_ioctl)) == -1)
		{
			printk("\r\ncopy to user error ====> downstream Switch opt");
			return -1;
		}
		return 0;
	}
	else if(data->option_flag == OPT_SET)
	{
		gpon_downstream_mapping_data.mapping_switch = data->downstream_mapping_switch;
		return 0;
	}
	
	return -1;
}

//calc the index which the specify gem port can insert.
int Get_Insert_Index(u16 port,int * insert_flag)
{
	int i = 0;
	
	if(gpon_downstream_mapping_data.rule_counter == MAX_GEM_PORT_NUM)
		return -1;

	for(i = 0; i < gpon_downstream_mapping_data.rule_counter; i++)
	{
		if(gpon_downstream_mapping_data.rule[i].gem_port_num == port)
		{
			*insert_flag = RULE_CHANGE;
			return i;
		}
		if(gpon_downstream_mapping_data.rule[i].gem_port_num > port)
		{
			*insert_flag = RULE_INSERT;
			return i;
		}
	}
	*insert_flag = RULE_INSERT;
	return i;
}

int gpon_downstream_mapping_rule_option(gpon_downstream_mapping_ioctl * data,void * arg)
{
	int index = 0,flag = 0,i = 0,j = 0;

	switch(data->option_flag)
	{
		case OPT_GET:
			//do nothing here
			return 0;

		case OPT_SET:
		/*
		when set the rule which gem port has been set,new rule will overwrite the old one.
		*/
			index = Get_Insert_Index(data->gem_port_num, &flag);

			if(index == -1)
				return -1;
			
			if(flag == RULE_CHANGE)
			{
				gpon_downstream_mapping_data.rule[index].if_mask = data->if_mask;
				gpon_downstream_mapping_data.rule[index].if_mask |= 0x80000000;
				j = 0;
				for(i = 0; i < MAX_LAN_PORT; i++)
				{
					if((gpon_downstream_mapping_data.rule[index].if_mask & (1 << i)) != 0)
					{
						if(j == 0)
						{
							//find first mapping port
							j = 1;
							continue;
						}
						else
						{
							//find second mapping port.clean bit31.
							gpon_downstream_mapping_data.rule[index].if_mask &= 0x7fffffff;
							break;
						}
					}
				}
				gpon_downstream_mapping_data.rule[index].queue = data->queue;
				gpon_downstream_mapping_data.rule[index].trtcmId= data->trtcmId;
				gpon_downstream_mapping_data.rule[index].ds_pq_enable= data->ds_pq_enable;
				gpon_downstream_mapping_data.rule[index].ds_trtcm_enable= data->ds_trtcm_enable;
				gpon_downstream_mapping_data.rule[index].weight= data->weight;
			}
			else if(flag == RULE_INSERT)
			{
				for(i = gpon_downstream_mapping_data.rule_counter; i > index; i--)
				{
					gpon_downstream_mapping_data.rule[i].gem_port_num = gpon_downstream_mapping_data.rule[i - 1].gem_port_num;
					gpon_downstream_mapping_data.rule[i].if_mask = gpon_downstream_mapping_data.rule[i - 1].if_mask;
					gpon_downstream_mapping_data.rule[i].queue = gpon_downstream_mapping_data.rule[i - 1].queue;
					gpon_downstream_mapping_data.rule[i].trtcmId = gpon_downstream_mapping_data.rule[i - 1].trtcmId;
					gpon_downstream_mapping_data.rule[i].ds_pq_enable= gpon_downstream_mapping_data.rule[i - 1].ds_pq_enable;
				    gpon_downstream_mapping_data.rule[i].ds_trtcm_enable= gpon_downstream_mapping_data.rule[i - 1].ds_trtcm_enable;
					gpon_downstream_mapping_data.rule[i].weight= gpon_downstream_mapping_data.rule[i - 1].weight;
				}
				gpon_downstream_mapping_data.rule[index].gem_port_num = data->gem_port_num;
				gpon_downstream_mapping_data.rule[index].if_mask = data->if_mask;
				gpon_downstream_mapping_data.rule[index].if_mask |= 0x80000000;
				j = 0;
				for(i = 0; i < MAX_LAN_PORT; i++)
				{
					if((gpon_downstream_mapping_data.rule[index].if_mask & (1 << i)) != 0)
					{
						if(j == 0)
						{
							j = 1;
							continue;
						}
						else
						{
							gpon_downstream_mapping_data.rule[index].if_mask &= 0x7fffffff;
							break;
						}
					}
				}
				gpon_downstream_mapping_data.rule[index].queue = data->queue;
				gpon_downstream_mapping_data.rule[index].trtcmId = data->trtcmId;
				gpon_downstream_mapping_data.rule[index].ds_pq_enable= data->ds_pq_enable;
				gpon_downstream_mapping_data.rule[index].ds_trtcm_enable= data->ds_trtcm_enable;
				gpon_downstream_mapping_data.rule[index].weight= data->weight;
				gpon_downstream_mapping_data.rule_counter++;
			}
			else
				return -1;
		
			return 0;

		case OPT_DEL://del by gem port
			for(i = 0; i < gpon_downstream_mapping_data.rule_counter; i++)
			{
				if(gpon_downstream_mapping_data.rule[i].gem_port_num == data->gem_port_num)
					break;
			}
			for(j = i; j < (gpon_downstream_mapping_data.rule_counter - 1); j++)
			{
				gpon_downstream_mapping_data.rule[j].gem_port_num = gpon_downstream_mapping_data.rule[j + 1].gem_port_num;
				gpon_downstream_mapping_data.rule[j].if_mask = gpon_downstream_mapping_data.rule[j + 1].if_mask;
				gpon_downstream_mapping_data.rule[j].queue = gpon_downstream_mapping_data.rule[j + 1].queue;
				gpon_downstream_mapping_data.rule[j].trtcmId = gpon_downstream_mapping_data.rule[j + 1].trtcmId;
				gpon_downstream_mapping_data.rule[j].ds_pq_enable= gpon_downstream_mapping_data.rule[j + 1].ds_pq_enable;
				gpon_downstream_mapping_data.rule[j].ds_trtcm_enable= gpon_downstream_mapping_data.rule[j + 1].ds_trtcm_enable;
				gpon_downstream_mapping_data.rule[j].weight= gpon_downstream_mapping_data.rule[j + 1].weight;
			}
			gpon_downstream_mapping_data.rule_counter--;
			return 0;
			
		case OPT_CLEAN:
			for(i = 0; i < MAX_GEM_PORT_NUM; i++)
			{
				gpon_downstream_mapping_data.rule[i].gem_port_num = 4096;
				gpon_downstream_mapping_data.rule[i].if_mask = 0;
				gpon_downstream_mapping_data.rule[i].queue = 8;
				gpon_downstream_mapping_data.rule[i].trtcmId = 32;
				gpon_downstream_mapping_data.rule[i].ds_pq_enable = 0;
				gpon_downstream_mapping_data.rule[i].ds_trtcm_enable = 0;
				gpon_downstream_mapping_data.rule[i].weight = 0;
			}
			gpon_downstream_mapping_data.rule_counter = 0;
			return 0;

		case OPT_SHOW:
			printk("\r\ntotal rule num is %d ",gpon_downstream_mapping_data.rule_counter);
			for(i = 0; i < gpon_downstream_mapping_data.rule_counter; i++)
			{
				printk("\r\nGem Port:%d, If Mask:%08x, queue Index:%d, trtcm Id:%d, queue_enable:%d, trtcm_enable:%d, down weight:%d",gpon_downstream_mapping_data.rule[i].gem_port_num,
					gpon_downstream_mapping_data.rule[i].if_mask,gpon_downstream_mapping_data.rule[i].queue, gpon_downstream_mapping_data.rule[i].trtcmId,
					gpon_downstream_mapping_data.rule[i].ds_pq_enable, gpon_downstream_mapping_data.rule[i].ds_trtcm_enable, gpon_downstream_mapping_data.rule[i].weight);
			}
			return 0;

		default:
			printk("\r\noption error");
			return -1;
	}
}

/*
	get the interface name by gem port num.
	the parameter ifgroup is used to store interface group.
	this parameter set to "" mean the gem port is not found
*/
int get_if_mask_by_gem_port(struct sk_buff *skb, u32 * ifmask)
{
	int pos = 0,counter = 0,offset = 0;
	
	if(skb == NULL || ifmask == NULL)
		return -1;

	if(gpon_downstream_mapping_data.rule_counter == 0)
		return 0;
	
	counter = gpon_downstream_mapping_data.rule_counter;
	
	while(counter != 0)
	{
		offset = (counter - 1)/2;

		//printk("\r\nskb gemport is %d,rule gemport is %d",skb->gem_port,gpon_downstream_mapping_data.Rule[pos + offset].gem_port_num);
		if(skb->gem_port == gpon_downstream_mapping_data.rule[pos + offset].gem_port_num)
		{
			*ifmask = gpon_downstream_mapping_data.rule[pos + offset].if_mask;
			//skb->pon_mark &= DS_PKT_FORM_WAN;
			if (gpon_downstream_mapping_data.rule[pos + offset].ds_pq_enable== XPON_ENABLE)
			{
				setDownQueueID(skb->pon_mark, gpon_downstream_mapping_data.rule[pos + offset].queue);
				setDownQueueEnable(skb->pon_mark, TSENABLE);
			}
			else
			{
			    setDownQueueID(skb->pon_mark, gpon_downstream_mapping_data.rule[pos + offset].queue);
				setDownQueueEnable(skb->pon_mark, TSDISABLE);
			}
			if (gpon_downstream_mapping_data.rule[pos + offset].ds_trtcm_enable == TRAFFIC_SHAPING_ENABLE)
			{
				setDownTrtcmID(skb->pon_mark, gpon_downstream_mapping_data.rule[pos + offset].trtcmId);			
				setDownTrtcmEnable(skb->pon_mark, TSENABLE);	
			}
			else
			{
			    setDownTrtcmID(skb->pon_mark, gpon_downstream_mapping_data.rule[pos + offset].trtcmId);			
				setDownTrtcmEnable(skb->pon_mark, TSDISABLE);
			}
			return 0;
		}
		else if(skb->gem_port > gpon_downstream_mapping_data.rule[pos + offset].gem_port_num)
		{
			if(counter == 1)
				return 0;
			counter = counter - 1 - offset;
			pos = pos + offset + 1;
		}
		else
		{
			if(offset == 0)
				return 0;
			counter = offset;
		}
	}

	return 0;
}

int gpon_ds_queue_mapping(struct sk_buff *skb, QDMA_TxQosScheduler_T *txQos)
{
	int i = 0;
	int j = 0;
	int wrrCnt = 0;
	int queueIdx = 0;

	for(i = 0; i < gpon_downstream_mapping_data.rule_counter; i++)
	{
		if (gpon_downstream_mapping_data.rule[i].weight != 0) 
		{
			wrrCnt ++;
		}
		queueIdx = gpon_downstream_mapping_data.rule[i].queue;
		txQos->queue[queueIdx].weight = gpon_downstream_mapping_data.rule[i].weight;
	}
	
	switch(wrrCnt){
			case 0:
			case 1:
				txQos->qosType = XMCS_IF_QOS_TYPE_SP;
				break;
			case 2:
				txQos->qosType = XMCS_IF_QOS_TYPE_SPWRR2;
				break;
			case 3:
				txQos->qosType = XMCS_IF_QOS_TYPE_SPWRR3;
				break;
			case 4:
				txQos->qosType = XMCS_IF_QOS_TYPE_SPWRR4;
				break;
			case 5:
				txQos->qosType = XMCS_IF_QOS_TYPE_SPWRR5;
				break;
			case 6:
				txQos->qosType = XMCS_IF_QOS_TYPE_SPWRR6;
				break;
			case 7:
				txQos->qosType = XMCS_IF_QOS_TYPE_SPWRR7;
				break;
			case 8:
				txQos->qosType = XMCS_IF_QOS_TYPE_WRR;
				break;
			default:
				break;
	}
	

	for(i = 0; i < gpon_downstream_mapping_data.rule_counter; i++)
	{
		if(skb->gem_port == gpon_downstream_mapping_data.rule[i].gem_port_num)
		{
			for (j = 0; j < 4; j++)
			{		
				if ((gpon_downstream_mapping_data.rule[i].if_mask & 0xf) & (1 << j)) 
				{
						txQos->channel = j + 1;
						break;
				}	
			}
			skb->mark |= ( (1+gpon_downstream_mapping_data.rule[i].queue) & 0x7)<<4; 
			GPONMAP_PRINT(GPONMAP_MSG_TRACE, "[%s] skb gemport is %d,rule gemport is %d,  skb->mark is 0x%x\n\n",__FUNCTION__,
				skb->gem_port,gpon_downstream_mapping_data.rule[i].gem_port_num,skb->mark);

			return gpon_downstream_mapping_data.rule[i].queue;
		}
	}
	
	return -1;
}

#endif /* TCSUPPORT_GPON_DOWNSTREAM_MAPPING */
#endif /* TCSUPPORT_GPON_MAPPING */


#if defined(TCSUPPORT_EPON_MAPPING)
u8 mappingDbgLevel = 0;
/* gloable classification rules manager */
MappingMgr_t mappingMgr;

static void mappingResultFree(struct rcu_head *head){
	QosResultRule_Ptr entry = container_of(head, QosResultRule_t, rcu);
	if (entry->matchNum > 0){
		kfree(entry->table);
	}
	entry->table = NULL;
	kfree(entry);
}

/* ------------------------OUT interface functions for Module.------------------------ */
inline void setMappingDbgLevel(u8 lvl)
{
	mappingDbgLevel = lvl;
	printk("%s : Set DBG level %d Done!\n", __FUNCTION__,lvl);
}
inline void setMappingEnable(u8 port, u8 enable)
{
	if (enable == EPONMAP_ENABLE){
		mappingMgr.enable |= (EPONMAP_ENABLE<<port);
		QOS_PRINT(DBG_INFO, "Enable Qos Classification&Marking on Port %d!\n", port);
	}
	else{
		mappingMgr.enable &= ~(EPONMAP_ENABLE<<port);
		QOS_PRINT(DBG_INFO, "Disable Qos Classification&Marking on Port %d!\n", port);
	}
}
inline uint getMappingEnable(u8 port)
{
    QOS_PRINT(DBG_TRACE, "enable=%X\n",  mappingMgr.enable);
	return (mappingMgr.enable & (EPONMAP_ENABLE<<port));
}
int initEponMapping(void)
{
	u8 i;
	
	mappingMgr.enable = EPONMAP_DISABLE; //EPONMAP_ENABLE;
	for (i = 0; i < PORT_NUM; ++i)
	{
		mappingMgr.portResultNum[i] = 0;
		INIT_LIST_HEAD(&mappingMgr.portResultlist[i]);
		
	}
	
	initLlidQueueMap();
	
	return 0;
}


int exitEponMapping(void)
{
	u8 i;
	QosResultRule_Ptr pCur = NULL;

	QOS_PRINT(DBG_TRACE, "clear all qos rules.\n");
	for (i = 0; i < PORT_NUM; ++i)
	{
		if (mappingMgr.portResultNum[i] != 0){
			list_for_each_entry_rcu(pCur, &(mappingMgr.portResultlist[i]), list)
			{
				list_del_rcu(&(pCur->list));
				call_rcu(&(pCur->rcu), mappingResultFree);
			}
			mappingMgr.portResultNum[i] = 0;
		}
	}
	mappingMgr.enable = EPONMAP_DISABLE;
	return 0;
}

int    resetEponMapping(void)
{
	exitEponMapping();
	initEponMapping();
	return TRUE;
}

QosResultRule_Ptr findMappingResult(u8 port, u8 level, QosPktInfo_Ptr pKey)
{
	QosResultRule_Ptr pCur = NULL;

	if (port >= PORT_NUM || pKey == NULL)
		return NULL;
	list_for_each_entry_rcu(pCur, &(mappingMgr.portResultlist[port]), list)
	{
		if ((matchRules(pCur, level, pKey) == TRUE)
		//	&& (pCur->level <= level)	//the packet's level must higher than rule's.
		){
			return pCur;
		}
	}
	QOS_PRINT(DBG_TRACE, "not find matched rules.\n");

    return NULL;
}

PortLlidMap_t uni_llid_map[MAX_UNILLID_MAP_NUM] = {0};

int addPortLlidMap(PortLlidMap_t *portllidmap){

	if((portllidmap->uni_port < 1) || (portllidmap->uni_port > MAX_UNILLID_MAP_NUM)){
		printk("Index is out of range.\n");
	}
	else{
		memcpy(&uni_llid_map[portllidmap->uni_port-1], portllidmap, sizeof(PortLlidMap_t));
		printk("uni_port %d is now combinded with llid_mask:%d, forward to llid%d by default, and enable flag is %d\n", uni_llid_map[portllidmap->uni_port-1].uni_port, uni_llid_map[portllidmap->uni_port-1].llid_mask, uni_llid_map[portllidmap->uni_port-1].default_llid, uni_llid_map[portllidmap->uni_port-1].enable);
	}
	return TRUE;
}

/* 
 * Need to delete all hwnat rules to make the Qos runs correctly.
 */
int addMappingResult(u8 port, QosResult_Ptr pResult, u8 matchNum, QosMatchRule_Ptr pMatchs)
{
	QosResultRule_Ptr pRR = NULL;

	#ifdef PON_QOS_DBG
	u8 i;
	QOS_PRINT(DBG_TRACE, "PortId=%d; Precedence=%d; Pbit=%d; PriQueue=%d; MatchRuleNum=%d;\n",\
		port, pResult->precedence, pResult->priority, pResult->queueMapped, matchNum);
	matchNum = matchNum > MAX_FIELD_NUM? MAX_FIELD_NUM:matchNum;
	for (i = 0; i < matchNum; i++){
		QOS_PRINT(DBG_TRACE, "\tmatchRule %d: ", i);
		if(mappingDbgLevel>=DBG_TRACE){
			printMappingMatch(&pMatchs[i]);
		}
	}
	#endif

	if (port >= PORT_NUM || pResult == NULL)
		return FALSE;

	// TODO: if max. rm last
	if (mappingMgr.portResultNum[port] >= CLSFY_NUM_PER_PORT)
		return FALSE;

	pRR = kmalloc(sizeof(QosResultRule_t), GFP_ATOMIC);
	if (!pRR){
		QOS_PRINT(DBG_WARING, "Warning: %s kmalloc QosResultRule_t failed.\n", __FUNCTION__);
		return FALSE;
	}
	
	pRR->table = kmalloc(matchNum * sizeof(QosMatchRule_t), GFP_ATOMIC);
	if (pRR->table == NULL){
		kfree(pRR);
		QOS_PRINT(DBG_WARING, "Warning: %s kmalloc QosMatchRule_t failed.\n", __FUNCTION__);
		return FALSE;
	}

	pRR->pbit       = pResult->priority;
	pRR->queue      = pResult->queueMapped;
	pRR->precedence = pResult->precedence;
	pRR->matchNum   = matchNum;
	INIT_LIST_HEAD(&(pRR->list));

	memcpy(pRR->table, pMatchs, matchNum*sizeof(QosMatchRule_t));

	insertMappingResult(port, pRR);
	setMappingEnable(port, EPONMAP_ENABLE);

	if(isSfu)
		ECNT_QDMA_GREEN_DROP_CTRL_HOOK(ECNT_QDMA_SET_QOS_FLAG,XPON_ENABLE);

	return TRUE;
}

/* 
 * Need to delete all hwnat rules to make the Qos runs correctly.
 */
int rmMappingResult(u8 port, QosResult_Ptr pResult, u8 matchNum, QosMatchRule_Ptr pMatchs)
{
	QosResultRule_t  result;
	QosResultRule_Ptr pRR = NULL;
	u8 i = 0;
	u8 eponQosFlag = 0;

	#ifdef PON_QOS_DBG
	QOS_PRINT(DBG_TRACE, "rmEponMappingResultRules: PortId=%d; Precedence=%d; Pbit=%d; PriQueue=%d; MatchRule=%d;\n",\
		port, pResult->precedence, pResult->priority, pResult->queueMapped, matchNum);
	matchNum = matchNum > MAX_FIELD_NUM? MAX_FIELD_NUM:matchNum;
	for (i = 0; i < matchNum; i++){
		QOS_PRINT(DBG_TRACE, "\tmatchRule %d: ", i);
		if(mappingDbgLevel>=DBG_TRACE){
			printMappingMatch(&pMatchs[i]);
		}
	}
	#endif

	if (port >= PORT_NUM || pResult == NULL)
		return FALSE;

	result.pbit       = pResult->priority;
	result.queue      = pResult->queueMapped;
	result.precedence = pResult->precedence;
	result.matchNum   = matchNum;
#if 0
	result.table = kmalloc(matchNum*sizeof(QosMatchRule_t), GFP_ATOMIC);
	memcpy(result.table, pMatchs, matchNum*sizeof(QosMatchRule_t));
#else
	result.table = pMatchs;
#endif

	pRR = deleteMappingResult(port, &result);

	if (pRR){
		QOS_PRINT(DBG_TRACE, "DelQosRule: find same rule, delete ok!\n");
		freeMappingResult(pRR);

		clearAllHwnatRules();
	}

	for(i = 0;i < PORT_NUM;i++)
	{
		if(mappingMgr.portResultNum[i]>0)
		{
			eponQosFlag = 1;
			break;
		}
	}

	if(!eponQosFlag&&isSfu)
		ECNT_QDMA_GREEN_DROP_CTRL_HOOK(ECNT_QDMA_SET_QOS_FLAG,XPON_DISABLE);
#if 0	
	kfree(result.table);
#endif
	return TRUE;
}

u8 getMappingResultNum(u8 port)
{
	if (port >= PORT_NUM)
		return 0;

	return mappingMgr.portResultNum[port];
}

/* Param: pMatchNum: must be the size of the pMatchs. */
int getMappingResult(u8 port, u8 index, QosResult_Ptr pRlt, u8 *pMatchNum, QosMatchRule_Ptr pMatchs)
{
	u8 i = 0;
	QosResultRule_Ptr pCur = NULL;
	
	if (port >= PORT_NUM || mappingMgr.portResultNum[port] <= index)
		return FALSE;
	
	rcu_read_lock();

	list_for_each_entry_rcu(pCur, &(mappingMgr.portResultlist[port]), list)
	{
		if (i == index)
			break;
		i++;
	}

	if (*pMatchNum < pCur->matchNum){
		QOS_PRINT(DBG_TRACE, "getMappingResult: the size of pMatchs is not enough!\n");
		rcu_read_unlock();
		return FALSE;
	}

	pRlt->priority    = pCur->pbit;
	pRlt->precedence  = pCur->precedence;
	pRlt->queueMapped = pCur->queue;
	
	*pMatchNum = pCur->matchNum;

	memcpy(pMatchs, pCur->table, pCur->matchNum * sizeof(QosMatchRule_t));
	rcu_read_unlock();
	
	return TRUE;
}

/* 
 * Need to delete all hwnat rules to make the Qos runs correctly.
 */
void clearMappingResult(u8 port)
{
	QosResultRule_Ptr pCur = NULL;

	if (port >= PORT_NUM){
		u8 i = 0;
		for (; i < PORT_NUM; i++){
			list_for_each_entry(pCur, &(mappingMgr.portResultlist[i]), list)
			{
				list_del_rcu(&(pCur->list));
				call_rcu(&(pCur->rcu), mappingResultFree);
			}
			mappingMgr.portResultNum[i] = 0;
		}
		QOS_PRINT(DBG_TRACE, "clear all qos classification&marking rule!\n");
		return ;
	}

	list_for_each_entry(pCur, &(mappingMgr.portResultlist[port]), list)
	{
		list_del_rcu(&(pCur->list));
		call_rcu(&(pCur->rcu), mappingResultFree);
	}

	setMappingEnable(port, EPONMAP_DISABLE);
	mappingMgr.portResultNum[port] = 0;
	QOS_PRINT(DBG_TRACE, "clear port%d's classification&marking rule!\n", port);
}

void showMappingResult(u8 port)
{
	QosResultRule_Ptr pCur = NULL;

	if (port >= PORT_NUM){
		u8 i = 0;
		for (; i < PORT_NUM; ++i)
		{
			printk("PortID %d: Clsfy Rules: %d\n", i, mappingMgr.portResultNum[i]);

			list_for_each_entry_rcu(pCur, &(mappingMgr.portResultlist[i]), list)
			{
				printMappingResult(pCur);
			}
		}
	}else{
	
		printk("PortID %d: Clsfy Rules: %d\n", port, mappingMgr.portResultNum[port]);
		
		list_for_each_entry_rcu(pCur, &(mappingMgr.portResultlist[port]), list)
		{
			printMappingResult(pCur);
		}
	}
}

/* -----------------------Qos classification manager functions----------------------- */


void freeMappingResult(QosResultRule_Ptr pResult)
{
	if (pResult)
	{
		if (pResult->matchNum > 0)
			kfree(pResult->table);
		pResult->table = NULL;
		kfree(pResult);
	}
}

/* compare the two mac addr
 * mac[1] > mac[2] return 1
 * mac[1] = mac[2] return 0
 * mac[1] < mac[2] return -1
 */
inline int compareMac(u8 mac1[ETH_ALEN], u8 mac2[ETH_ALEN])
{
    int i;

    for (i = 0; i < ETH_ALEN; i++)
    {
        if (mac1[i] > mac2[i])
            return GREATER;
        else if (mac1[i] < mac2[i])
            return LESS;
    }
    return EQUAL;
}

inline int compareIPv6(struct in6_addr *ip1, struct in6_addr *ip2)
{
	int i;

	for (i = 0; i < 4; ++i)
	{
		if (ip1->s6_addr32[i] > ip2->s6_addr32[i])
			return GREATER;
		else if (ip1->s6_addr32[i] < ip2->s6_addr32[i])
			return LESS;
	}
	return EQUAL;
}
int compareIP6Prex(struct in6_addr *ip1, struct in6_addr *ipPrex, int len)
{
	int i, m = len/8, n = len%8;
	u8 mask = ((1<<n)-1)<<(8-n);
	
	for (i = 0; i < m; ++i)
	{
		if (ip1->s6_addr[i] > ipPrex->s6_addr[i])
			return GREATER;
		else if (ip1->s6_addr[i] < ipPrex->s6_addr[i])
			return LESS;
	}

	if ((ip1->s6_addr[m]&mask) > (ipPrex->s6_addr[m]&mask))
		return GREATER;
	else if ((ip1->s6_addr[m]&mask) < (ipPrex->s6_addr[m]&mask))
		return LESS;
	return EQUAL;
}

/*
 * # return: TRUE or FALSE
 * @ res: 0: equal; 1: bigger than; -1: less than; 2: not exist
 * @ op: the operation of the match value.
 */
inline u8 matchOpRes(int res, u8 op)
{
	switch(op){
		case OP_EQUAL:
			if (res == EQUAL)
				return TRUE;
		break;
		case OP_NEVER_MATCH:
			return FALSE;
		break;
		case OP_NOT_EQUAL:
			if ((res != EQUAL) && (res != NOTEXIST))
				return TRUE;
		break;
		case OP_LESS_THAN:
			if (res == LESS || res == EQUAL)
				return TRUE;
		break;
		case OP_GREATER_THAN:
			if (res == GREATER || res == EQUAL)
				return TRUE;
		break;
		case OP_EXISTS:
			if (res == NOTEXIST)
				return FALSE;
			return TRUE;
		break;
		case OP_NOT_EXIST:
			if (res == NOTEXIST)
				return TRUE;
		break;
		case OP_ALWAYS_MATCH:
			return TRUE;
		break;
	}
	return FALSE;
}
inline u8 matchRule(QosMatchRule_Ptr pMatch, u8 level, QosPktInfo_Ptr pKey)
{
	int ret = LESS;

	switch(pMatch->field){
		case FIELD_SMAC:
			ret = compareMac(pKey->smac, pMatch->mac);
		break;
		case FIELD_DMAC:
			ret = compareMac(pKey->dmac, pMatch->mac);
		break;
		case FIELD_PBIT:
			if (pKey->pbit > pMatch->v8)
				ret = GREATER;
			else if (pKey->pbit == pMatch->v8)
				ret = EQUAL;
		break;
		case FIELD_VLANID:
			if (pKey->vid > pMatch->v16)
				ret = GREATER;
			else if (pKey->vid == pMatch->v16)
				ret = EQUAL;
		break;
		case FIELD_ETHTYPE:
			if (pKey->ethtype > pMatch->v16)
				ret = GREATER;
			else if (pKey->ethtype == pMatch->v16)
				ret = EQUAL;
		break;

		case FIELD_IPVER:
			if (level < LEVEL_IP)
				ret = NOTEXIST;
			else if (pKey->ipver > pMatch->v8)
				ret = GREATER;
			else if (pKey->ipver == pMatch->v8)
				ret = EQUAL;
		break;
		case FIELD_IPPROTO4:
			if (level < LEVEL_IP || pKey->ipver != 4)
				ret = NOTEXIST;
			else if (pKey->ipp > pMatch->v8)
				ret = GREATER;
			else if (pKey->ipp == pMatch->v8)
				ret = EQUAL;
		break;
		case FIELD_IPPROTO6:
			if (level < LEVEL_IP || pKey->ipver != 6)
				ret = NOTEXIST;
			else if (pKey->ipp > pMatch->v8)
				ret = GREATER;
			else if (pKey->ipp == pMatch->v8)
				ret = EQUAL;
		break;
		case FIELD_IPDSCP4:
			if (level < LEVEL_IP || pKey->ipver != 4)
				ret = NOTEXIST;
			else if (pKey->hdr.bits.dscp > pMatch->v8)
				ret = GREATER;
			else if (pKey->hdr.bits.dscp == pMatch->v8)
				ret = EQUAL;
		break;
		case FIELD_IPDSCP6:
			if (level < LEVEL_IP || pKey->ipver != 6)
				ret = NOTEXIST;
			else if (pKey->hdr.bits.dscp > pMatch->v8)
				ret = GREATER;
			else if (pKey->hdr.bits.dscp == pMatch->v8)
				ret = EQUAL;
		break;
		case FIELD_FLOWLABEL6:
			if (level < LEVEL_IP || pKey->ipver != 6)
				ret = NOTEXIST;
			else if (pKey->hdr.bits.flowLbl > pMatch->ip4)
				ret = GREATER;
			else if (pKey->hdr.bits.flowLbl == pMatch->ip4)
				ret = EQUAL;
		break;
		case FIELD_SIP4:
			if ((level < LEVEL_IP) || (pKey->ipver != 4))
				ret = NOTEXIST;
			else if (pKey->L3.v4.sip > pMatch->ip4)
				ret = GREATER;
			else if (pKey->L3.v4.sip == pMatch->ip4){
				ret = EQUAL;
			}
			QOS_PRINT(DBG_TRACE, "pkey->ip=%X, match-ip=%X, ret=%d\n", pKey->L3.v4.sip, pMatch->ip4, ret);
		break;
		case FIELD_DIP4:
			if ((level < LEVEL_IP) || (pKey->ipver != 4))
				ret = NOTEXIST;
			else if (pKey->L3.v4.dip > pMatch->ip4)
				ret = GREATER;
			else if (pKey->L3.v4.dip == pMatch->ip4){
				ret = EQUAL;
			}
			QOS_PRINT(DBG_TRACE, "pkey->ip=%X, match-ip=%X, ret=%d\n", pKey->L3.v4.dip, pMatch->ip4, ret);
		break;
		case FIELD_SIP6:
			if ((level < LEVEL_IP) || (pKey->ipver != 6))
				ret = NOTEXIST;
			else 
				ret = compareIPv6(&(pKey->L3.v6.sip), &(pMatch->ip6));
		break;
		case FIELD_DIP6:
			if ((level < LEVEL_IP) || (pKey->ipver != 6))
				ret = NOTEXIST;
			else
				ret = compareIPv6(&(pKey->L3.v6.dip), &(pMatch->ip6));
		break;
		case FIELD_SIP6PREX:
			if ((level < LEVEL_IP) || (pKey->ipver != 6))
				ret = NOTEXIST;
			else 
				ret = compareIP6Prex(&(pKey->L3.v6.sip), &(pMatch->ip6), pMatch->ip6.s6_addr[15]);
		break;
		case FIELD_DIP6PREX:
			if ((level < LEVEL_IP) || (pKey->ipver != 6))
				ret = NOTEXIST;
			else
				ret = compareIP6Prex(&(pKey->L3.v6.dip), &(pMatch->ip6), pMatch->ip6.s6_addr[15]);
		break;

		case FIELD_SPORT:
			if (level < LEVEL_TRANS)
				ret = NOTEXIST;
			else if (pKey->sport > pMatch->v16)
				ret = GREATER;
			else if (pKey->sport == pMatch->v16)
				ret = EQUAL;
		break;
		case FIELD_DPORT:
			if (level < LEVEL_TRANS)
				ret = NOTEXIST;
			else if (pKey->dport > pMatch->v16)
				ret = GREATER;
			else if (pKey->dport == pMatch->v16)
				ret = EQUAL;
		break;
		default:
		return FALSE;
	}
	return matchOpRes(ret, pMatch->op);
}

/* the function doesn't check the input params */
u8 matchRules(QosResultRule_Ptr pResult, u8 level, QosPktInfo_Ptr pKey)
{
	u8 i, num = pResult->matchNum;
	QosMatchRule_Ptr pMatch = NULL;

	for (i = 0; i < num; ++i)
	{
		pMatch = &(pResult->table[i]);
		if (FALSE == matchRule(pMatch, level, pKey))
			return FALSE;
	}
	return TRUE;
}

int equalMappingMatch(QosMatchRule_Ptr pa, QosMatchRule_Ptr pb)
{
	if (pa->field != pb->field)
		return FALSE;
	if (pa->op != pb->op)
		return FALSE;

	switch(pa->field){
		case FIELD_SMAC:
		case FIELD_DMAC:
			if (compareMac(pa->mac, pb->mac) != EQUAL)
				return FALSE;
		break;
		case FIELD_PBIT:
		case FIELD_IPVER:
		case FIELD_IPPROTO4:
		case FIELD_IPPROTO6:
		case FIELD_IPDSCP4:
		case FIELD_IPDSCP6:
			if (pa->v8 != pb->v8)
				return FALSE;
		break;
		case FIELD_VLANID:
		case FIELD_ETHTYPE:
		case FIELD_SPORT:
		case FIELD_DPORT:
			if (pa->v16 != pb->v16)
				return FALSE;
		break;
		case FIELD_SIP4:
		case FIELD_DIP4:
		case FIELD_FLOWLABEL6:
			if (pa->ip4 != pb->ip4)
				return FALSE;
		break;
		case FIELD_SIP6:
		case FIELD_DIP6:
		case FIELD_SIP6PREX:
		case FIELD_DIP6PREX:
			if (compareIPv6(&(pa->ip6), &(pb->ip6)) != EQUAL)
				return FALSE;
		break;
	}
	return TRUE;
}

int equalMappingResult(QosResultRule_Ptr pm, QosResultRule_Ptr pn)
{
	int ret;
	u8 i, j;
	QosMatchRule_Ptr pa, pb;

	if (pm->pbit != pn->pbit)
		return FALSE;
	if (pm->queue != pn->queue)
		return FALSE;
	if (pm->matchNum != pn->matchNum)
		return FALSE;
	
	// equal the two match rules list??
	for	(i = 0; i < pm->matchNum; ++ i)
	{
		ret = FALSE;
		pa = &(pm->table[i]);
		for (j = 0; j < pn->matchNum; ++ j)
		{
			pb = &(pn->table[j]);
			if (equalMappingMatch(pa, pb) == 0)
			{
				ret = TRUE;
				break;
			}
		}
		if (ret == FALSE)
			return FALSE;
	}
	return TRUE;
}
/* Qos classification rules operation functions */
/* the function doesn't do memory alloc */
int insertMappingResult(u8 port, QosResultRule_Ptr pResult)
{
	QosResultRule_Ptr pCur = NULL;
	u8 precedence = pResult->precedence;
	bool found = false;
	
	rcu_read_lock();
	list_for_each_entry_rcu(pCur, &(mappingMgr.portResultlist[port]), list)
	{
		if (pCur->precedence >= precedence){
			found = true;
			break;
		}
	}
	
	if (!found) // insert after of all tail.
		list_add_tail_rcu(&(pResult->list), &(mappingMgr.portResultlist[port]));
	else {// insert before pCur
		list_add_tail_rcu(&(pResult->list), &(pCur->list));

		pCur = pResult;
		list_for_each_entry_continue(pCur, &(mappingMgr.portResultlist[port]), list)
		{
			if (pCur->precedence != precedence)
				break;

			QOS_PRINT(DBG_TRACE, "find same precedence = %u!\n", precedence);
			pCur->precedence ++;
			precedence ++;
		}
	}

	mappingMgr.portResultNum[port] ++;

	rcu_read_unlock();
	return TRUE;
}

/* This function does't do memory free */
QosResultRule_Ptr deleteMappingResult(u8 port, QosResultRule_Ptr pResult)
{
	QosResultRule_Ptr pCur = NULL;

	rcu_read_lock();
	list_for_each_entry_rcu(pCur, &(mappingMgr.portResultlist[port]), list)
	{
		if ((pCur->precedence >= pResult->precedence) &&
			(equalMappingResult(pCur, pResult) == 0) )
		{
			list_del_init(&(pCur->list));
			mappingMgr.portResultNum[port] --;
			rcu_read_unlock();
			QOS_PRINT(DBG_TRACE, "find result to delete!\n");
			return pCur;
		}
		
	}

	rcu_read_unlock();
	return NULL;
}

void printMappingMatch(QosMatchRule_Ptr pMatch)
{
	// printk("Op: %d; ", pMatch->op);
	switch(pMatch->op){
		case OP_NEVER_MATCH:
			printk("Op: never match; ");
		break;
		case OP_EQUAL:
			printk("Op: equal;       ");
		break;
		case OP_NOT_EQUAL:
			printk("Op: not equal;   ");
		break;
		case OP_LESS_THAN:
			printk("Op: less;        ");
		break;
		case OP_GREATER_THAN:
			printk("Op: greater;     ");
		break;
		case OP_EXISTS:
			printk("Op: exist;       ");
		break;
		case OP_NOT_EXIST:
			printk("Op: not exist;   ");
		break;
		case OP_ALWAYS_MATCH:
			printk("Op: always match;");
		break;
	}

	switch(pMatch->field){
		case FIELD_DMAC:
			printk("DMAC: %02X:%02X:%02X:%02X:%02X:%02X;", pMatch->mac[0],\
				pMatch->mac[1],pMatch->mac[2],pMatch->mac[3],pMatch->mac[4],pMatch->mac[5]);
		break;
		case FIELD_SMAC:
			printk("SMAC: %02X:%02X:%02X:%02X:%02X:%02X;", pMatch->mac[0],\
				pMatch->mac[1],pMatch->mac[2],pMatch->mac[3],pMatch->mac[4],pMatch->mac[5]);
		break;
		case FIELD_PBIT:
			printk("Pbit: %d;", pMatch->v8);
		break;
		case FIELD_VLANID:
			printk("VID: %d;", pMatch->v16);
		break;
		case FIELD_ETHTYPE:
			printk("EtherType: %.4X;", pMatch->v16);
		break;
		case FIELD_DIP4:
			printk("DIP: %d.%d.%d.%d;", (pMatch->ip4>>24), ((pMatch->ip4>>16)& 0x000000FF),\
				((pMatch->ip4>>8)& 0x000000FF), (pMatch->ip4 & 0x000000FF));
		break;
		case FIELD_SIP4:
			printk("SIP: %d.%d.%d.%d;", (pMatch->ip4>>24), ((pMatch->ip4>>16)& 0x000000FF),\
				((pMatch->ip4>>8)& 0x000000FF), (pMatch->ip4 & 0x000000FF));
		break;
		case FIELD_IPPROTO4:
			printk("IPProto: %d;", pMatch->v8);
		break;
		case FIELD_IPDSCP4:
			printk("IPDscp4: %d;", pMatch->v8);
		break;
		case FIELD_IPDSCP6:
			printk("IPDscp6: %d;", pMatch->v8);
		break;
		case FIELD_FLOWLABEL6:
			printk("IP6 FlowLable: 0x%08X\n", pMatch->ip4);
		break;
		case FIELD_SPORT:
			printk("SPort: %d;", pMatch->v16);
		break;
		case FIELD_DPORT:
			printk("DPort: %d;", pMatch->v16);
		break;
		case FIELD_IPVER:
			printk("IPVersion: %d;", pMatch->v8);
		break;
		case FIELD_DIP6:
			printk("DIP6: %x:%x:%x:%x:%x:%x:%x:%x; ", pMatch->ip6.s6_addr16[0], pMatch->ip6.s6_addr16[1],\
				pMatch->ip6.s6_addr16[2], pMatch->ip6.s6_addr16[3], pMatch->ip6.s6_addr16[4], \
				pMatch->ip6.s6_addr16[5], pMatch->ip6.s6_addr16[6], pMatch->ip6.s6_addr16[7]);
		break;
		case FIELD_SIP6:
			printk("SIP6: %x:%x:%x:%x:%x:%x:%x:%x; ", pMatch->ip6.s6_addr16[0], pMatch->ip6.s6_addr16[1],\
				pMatch->ip6.s6_addr16[2], pMatch->ip6.s6_addr16[3], pMatch->ip6.s6_addr16[4], \
				pMatch->ip6.s6_addr16[5], pMatch->ip6.s6_addr16[6], pMatch->ip6.s6_addr16[7]);
		break;
		case FIELD_DIP6PREX:
			printk("DIP6Prex: %x:%x:%x:%x:%x:%x:%x:%x; ", pMatch->ip6.s6_addr16[0], pMatch->ip6.s6_addr16[1],\
				pMatch->ip6.s6_addr16[2], pMatch->ip6.s6_addr16[3], pMatch->ip6.s6_addr16[4], \
				pMatch->ip6.s6_addr16[5], pMatch->ip6.s6_addr16[6], pMatch->ip6.s6_addr16[7]);
		break;
		case FIELD_SIP6PREX:
			printk("SIP6Prex: %x:%x:%x:%x:%x:%x:%x:%x; ", pMatch->ip6.s6_addr16[0], pMatch->ip6.s6_addr16[1],\
				pMatch->ip6.s6_addr16[2], pMatch->ip6.s6_addr16[3], pMatch->ip6.s6_addr16[4], \
				pMatch->ip6.s6_addr16[5], pMatch->ip6.s6_addr16[6], pMatch->ip6.s6_addr16[7]);
		break;
		case FIELD_IPPROTO6:
			printk("IPProto: %d;", pMatch->v8);
		break;
	}
	printk("\n");
}
void printMappingResult(QosResultRule_Ptr pResult)
{
	u8 i;
	QosMatchRule_Ptr pMatch;

	printk("  Precedence: %d; PBit: %d; PriQueue: %d; MatchsNum: %d;\n",
		pResult->precedence, pResult->pbit, pResult->queue, pResult->matchNum);

	for(i=0; i < pResult->matchNum; i++)
	{
		pMatch = &(pResult->table[i]);
		printk("\tMatchRule %d: ", i);
		printMappingMatch(pMatch);
		printk("\n");
	}
}








/* --------------------------------Pon MAC Register Set/Get-------------------------------- */
#define read_reg_word(reg) 			VPint(reg)
#define write_reg_word(reg, wdata) 	VPint(reg)=(wdata)

#if 0
/*
 * Set common WRR scheduling setting for all llid.
 * Param:
 * 	weightBase: 0:by packet; 1:by byte.
 */
void qosSchWeightBaseWrite(u8 weightBase)
{
	u32 reg;

	reg = read_reg_word(TXQoS_CHN0_7_CFG);

	reg &= ~(WEIGHT_BASE);
	reg |= (weightBase<<WEIGHT_BASE_SHIFT) & WEIGHT_BASE;

	write_reg_word(TXQoS_CHN0_7_CFG, reg);
}

u8 qosSchWeightBaseRead(void)
{
	u32 reg = read_reg_word(TXQoS_CHN0_7_CFG);

	u8 w = (reg & WEIGHT_BASE) >> WEIGHT_BASE_SHIFT;

	return w;
}

/*
 * Set the llid's wrr type
 * Param:
 * 	llid: the index of llid, value must be 0~7.
 *	spNum: the num of the queue which is sp, value must be 0~8.
 *			0:    0: WRR (Q7~Q0)
 *			7,8: 1: SP (Q7>Q6>...>Q1>Q0)
 *			1:    2: SP+WRR7(SP: Q7>WRR: Q6~Q0)
 *			2:    3: SP+WRR6(SP: Q7>Q6>WRR: Q5~Q0)
 *			3:    4: SP+WRR5(SP: Q7>Q6>Q5>WRR: Q4~Q0)
 *			4:    5: SP+WRR4(SP: Q7>Q6>Q5>Q4>WRR: Q3~Q0)
 *			5:    6: SP+WRR3(SP: Q7>Q6>Q5>Q4>Q3>WRR: Q2~Q0)
 *			6:    7: SP+WRR2(SP: Q7>Q6>Q5>Q4>Q3>Q2>WRR: Q1~Q0)
 */
int qosLlidQueueWrite(u8 llid, u8 spNum)
{
	u32 reg;
	u8 sch = 0;

	if (spNum > 8){
		return -1;
	}else if (spNum== 0){
		sch = 0;
	}else if (spNum == 8 || spNum ==7 ){
		sch = 1;
	}else{
		sch = spNum+1;
	}

	reg = read_reg_word(TXQoS_CHN0_7_CFG);

	reg &= ~ (CHN_SCH_DATA << (llid * CHN_DATA_SHIFT)) ;

	reg |=  sch <<(llid * CHN_DATA_SHIFT);

	write_reg_word(TXQoS_CHN0_7_CFG, reg);
	QOS_PRINT(DBG_WARING,"%s llid=%d, spNum=%d TxQoS_CFG: %x\n", __FUNCTION__, llid, spNum, reg);
	return 0;
}

/*
 * Set the llid's queue's wrr weight
 * Param:
 * 	llid: the index of llid, value must be 0~7.
 *	wrrNum: the num of the queue which is wrr, value must be 0~8.
 */
void qosWrrWrite(u8 llid, u8 queue, u8 weight)
{
	u32 reg;
	u32 cnt = 1000;

	do {
		reg = read_reg_word(TXQoS_WRR_CFG);
		cnt --;
	}while(reg & WRR_RWCMD_DONE);

	reg = WRR_RWCMD | (weight << WRR_VALUE_SHIFT) | ((llid&CHN_IDX_DATA) << CHN_IDX_SHIFT) | (queue & QUEUE_IDX_DATA);

	write_reg_word(TXQoS_WRR_CFG, reg);
	QOS_PRINT(DBG_WARING, "%s llid=%d queue=%d weight=%d TXQoS_WRR_CFG: %x\n", __FUNCTION__, llid, queue, weight, reg);

	cnt = 1000;
	do {
		reg = read_reg_word(TXQoS_WRR_CFG);
		cnt --;
	}while (reg & WRR_RWCMD_DONE);
	
}

u8 qosWrrRead(u8 llid, u8 queue)
{
	u32 reg;
	u32 cnt = 1000;
	u8  weight;
	
	do {
		reg = read_reg_word(TXQoS_WRR_CFG);
		cnt --;
	}while(reg & WRR_RWCMD_DONE);

	reg = ((llid&CHN_IDX_DATA) << CHN_IDX_SHIFT) | (queue & QUEUE_IDX_DATA);

	write_reg_word(TXQoS_WRR_CFG, reg);

	cnt = 1000;
	do {
		reg = read_reg_word(TXQoS_WRR_CFG);
		cnt --;
	}while (reg & WRR_RWCMD_DONE);

	weight = (reg >> WRR_VALUE_SHIFT) & WRR_VALUE_DATA;
	return weight;
}
#else
void qosSchWeightBaseWrite(u8 weightBase)
{
	printk("EPON MAP: %s weightBase = %d\n", __FUNCTION__, weightBase);
}
u8 qosSchWeightBaseRead(void)
{
	printk("EPON MAP: %s return weightBase = %d\n", __FUNCTION__, 20);
	return 20;
}
int qosLlidQueueWrite(u8 llid, u8 spNum)
{
	printk("EPON MAP: %s  llid = %d spNum=%d\n", __FUNCTION__, llid, spNum);
	return 0;
}
void qosWrrWrite(u8 llid, u8 queue, u8 weight)
{
	printk("EPON MAP: %s  llid = %d queue=%d weight=%d\n", __FUNCTION__, llid, queue, weight);
}
u8 qosWrrRead(u8 llid, u8 queue){
	printk("EPON MAP: %s  llid = %d queue=%d return weight=%d\n", __FUNCTION__, llid, queue, 20);
	return 20;
}

#endif
/* --------------------------------Pon MAC Register Set/Get-------------------------------- */

/* -------------------------Qos Priority queue mapping to LLID's queue------------------------- */
QueueMapping_t queueMap[PRIORITY_QUEUE_NUM_MAX] = {{0}};

void initLlidQueueMap(void )
{
	// init default mapping rule
	int i = 0; 
	for (i = 0; i < PRIORITY_QUEUE_NUM_MAX; i++){
		queueMap[i].llid = i / 8;
		queueMap[i].queue = i % 8;
		queueMap[i].weight = 0;
		queueMap[i].enable = QUEUEMAP_ENABLE;
		queueMap[i].sla_eable = FALSE;
	}
}

QueueMapping_Ptr findLlidQueue(u8 priQueue)
{
	QueueMapping_Ptr pQM = NULL;

	QOS_PRINT(DBG_TRACE, "priQueue=%d\n", priQueue);
	
	if (priQueue >= PRIORITY_QUEUE_NUM_MAX)
		return NULL;

	pQM = &(queueMap[priQueue]);
	if (pQM->enable == QUEUEMAP_ENABLE)
		return pQM;

	QOS_PRINT(DBG_TRACE, "not find!\n");
	return NULL;
}

/* 
 * Need to Set the queue Weight to PON MAC 
 */
int insertLlidQueueMap(u8 priQueue, u8 llid, u8 queue, QueueWeight_t PQweight)
{
	if ((priQueue >= PRIORITY_QUEUE_NUM_MAX)
		|| (llid  >= LLID_NUM_MAX)
		|| (queue >= LLID_QUEUE_NUM_MAX) )
		return FALSE;

	QOS_PRINT(DBG_TRACE, "priQueue=%d llid=%d queue=%d weight=%d\n", priQueue, llid, queue, PQweight.weight);
	queueMap[priQueue].enable = QUEUEMAP_ENABLE;
	queueMap[priQueue].llid   = llid;
	queueMap[priQueue].queue  = queue;
	queueMap[priQueue].weight = PQweight.weight;
	queueMap[priQueue].sla_eable = PQweight.sla_enable;
	return TRUE;
}

int clearLlidQueueMap(u8 llid)
{
	u8 i;

	if (llid >= LLID_NUM_MAX){
		for (i = 0; i < PRIORITY_QUEUE_NUM_MAX; ++i){
			queueMap[i].enable = QUEUEMAP_DISABLE;
			queueMap[i].sla_eable = FALSE;
		}
		QOS_PRINT(DBG_TRACE, "clear all llid's queue mapping!\n");
	}else{
		for (i = 0; i < PRIORITY_QUEUE_NUM_MAX; ++ i)
		{
			if ((queueMap[i].enable == QUEUEMAP_ENABLE) && (queueMap[i].llid == llid)){
				queueMap[i].enable = QUEUEMAP_DISABLE;
				queueMap[i].sla_eable = FALSE;
		    }
		}
		QOS_PRINT(DBG_TRACE, "clear llid%d's queue mapping!\n", llid);
	}

	clearAllHwnatRules();
	return TRUE;
}

int showLlidQueueMap(u8 llid)
{
	u8 i;

	if (llid >= LLID_NUM_MAX)
		return FALSE;

	printk("LLID: %d,  the queue list is:\n", llid);
	for (i = 0; i < PRIORITY_QUEUE_NUM_MAX; ++i)
	{
		if ((queueMap[i].enable == QUEUEMAP_ENABLE) && (queueMap[i].llid == llid)){
			printk("\tPriQueue: %d, and weight: %d, llid's queue: %d, sla enable: %d\n", i, queueMap[i].weight, queueMap[i].queue, queueMap[i].sla_eable);
		}
	}
	return TRUE;
}

int getLlidQueueMap(u8 llid, u8 *pNum, QueueWeight_Ptr pQWeights)
{
	u8 i;

	if (llid >= LLID_NUM_MAX)
		return FALSE;

	*pNum = 0;
	for (i = 0; i < PRIORITY_QUEUE_NUM_MAX; ++ i)
	{
		if ((queueMap[i].enable == QUEUEMAP_ENABLE) && (queueMap[i].llid == llid))
		{
			pQWeights->priQueue = i;
			pQWeights->weight = queueMap[i].weight;

			pQWeights ++;
			(*pNum) += 1;
		}
	}
	return TRUE;
}

int sortQueueWeightForSP(u8 num, QueueWeight_Ptr pQWeights)
{
	u8 max, maxQ, maxW;
	u8 i =0, j = 0;

	for (i = 0; i < num-1; i++){
		if (pQWeights[i].weight != 0){
			continue;
		}
		
		max = i;
		maxQ = pQWeights[i].priQueue;
		
		for (j = i+1; j < num; j++){
			if ((pQWeights[j].weight == 0) && (maxQ < pQWeights[j].priQueue)){
				maxQ = pQWeights[j].priQueue;
				max = j;
			}
		}

		if (max != i){
			maxW = pQWeights[max].weight;
			pQWeights[max].priQueue = pQWeights[i].priQueue;
			pQWeights[max].weight = pQWeights[i].weight;
			
			pQWeights[i].priQueue = maxQ;
			pQWeights[i].weight = maxW;
		}
	}
	return 0;
}

/* 
 * Need to delete all hwnat rules to make the Qos runs correctly.
 */
int setLlidQueueMap(u8 llid, u8 num, QueueWeight_Ptr pQWeights)
{
	u8 i, j = num-1, k = 0, queue;
	u8 spNum = 8;
	struct XMCS_ChannelQoS_S scheduler;

	if (llid >= LLID_NUM_MAX || num > LLID_QUEUE_NUM_MAX)
		return FALSE;

	QOS_PRINT(DBG_TRACE, "llid=%d num=%d\n", llid, num);
	memset(&scheduler, 0, sizeof(scheduler));
	
	clearLlidQueueMap(llid);
	
	sortQueueWeightForSP( num, pQWeights);
	for (i = 0; i < num; i++)
	{
		if (pQWeights[i].weight == 0 || pQWeights[i].weight > 100){ // sp
			queue = j;
			j --;
		}else{ // wrr
			spNum --;
			queue = k;
			k ++;
			//qosWrrWrite(llid, queue, pQWeights[i].weight);
			scheduler.queue[queue].weight = pQWeights[i].weight;
		}
		if (FALSE == insertLlidQueueMap(pQWeights[i].priQueue, llid, queue, pQWeights[i]))
			return FALSE;
	}

	scheduler.channel = llid;
	switch (spNum){
		case 0:
			scheduler.qosType = XMCS_IF_QOS_TYPE_WRR;
		break;
		case 1:
			scheduler.qosType = XMCS_IF_QOS_TYPE_SPWRR7;
		break;
		case 2:
			scheduler.qosType = XMCS_IF_QOS_TYPE_SPWRR6;
		break;
		case 3:
			scheduler.qosType = XMCS_IF_QOS_TYPE_SPWRR5;
		break;
		case 4:
			scheduler.qosType = XMCS_IF_QOS_TYPE_SPWRR4;
		break;
		case 5:
			scheduler.qosType = XMCS_IF_QOS_TYPE_SPWRR3;
		break;
		case 6:
			scheduler.qosType = XMCS_IF_QOS_TYPE_SPWRR2;
		break;
		case 7:
		case 8:
			scheduler.qosType = XMCS_IF_QOS_TYPE_SP;
		break;
	}

	if (0!= ECNT_API_XPON_CHANNEL_QOS_SET((void *)&scheduler))
		return FALSE;
	//qosLlidQueueWrite( llid, spNum);
	
	return TRUE;
}


#endif

//todo:
void clearAllHwnatRules(void)
{
#if defined(TCSUPPORT_RA_HWNAT) && defined(TCSUPPORT_RA_HWNAT_ENHANCE_HOOK)
extern int (*ra_sw_nat_hook_clean_table) (void);
	if (ra_sw_nat_hook_clean_table)
		ra_sw_nat_hook_clean_table();
#endif
}

int xpon_mode_get(void)
{
	struct XMCS_WanLinkConfig_S xponMode;

	if (ECNT_API_XPON_WANLINK_CONFIG_GET((void *)&xponMode) == 0)
	{
		 if (xponMode.linkStart == XPON_DISABLE) 
			return -1;
		 
		 if((xponMode.detectMode != XMCS_IF_WAN_DETECT_MODE_EPON) &&
		 	(xponMode.linkStatus == XMCS_IF_WAN_LINK_GPON))
		 {
		 	return XMCS_IF_WAN_LINK_GPON;		 
		 }
		 else if ((xponMode.detectMode != XMCS_IF_WAN_DETECT_MODE_GPON) &&
		 	(xponMode.linkStatus == XMCS_IF_WAN_LINK_EPON))
		 {
			 return XMCS_IF_WAN_LINK_EPON;		  
		 }
		 else
		 {
			return XMCS_IF_WAN_LINK_OFF;
		 }
		 
	}

	return -1;
}

#define OMCI_CONFIG 				0
#define LOCAL_CONFIG 				1
extern u8 gponmapQosMode;

int initXponMapping(void)
{
	int ret = 0,type = 0;

    ECNT_API_XPON_ONU_TYPE_GET(&type);
		
	if (type != XMCS_IF_ONU_TYPE_HGU)
	{
		isSfu = 1;
		gponmapQosMode = OMCI_CONFIG;
	}
	else
	{
		isSfu = 0;	
		gponmapQosMode = LOCAL_CONFIG;
		if(TCSUPPORT_ALPHION_PON_VAL)
		{
			gponmapQosMode = OMCI_CONFIG;
		}
	}
	isClearQueueList = 0;
#if defined(TCSUPPORT_GPON_MAPPING)
	ret = initGponMapping();
	if (ret != 0)
	{
		goto fail;
	}
#endif
#if defined(TCSUPPORT_EPON_MAPPING)
	ret = initEponMapping();	
	if (ret != 0)
	{
		goto fail;
	}
#endif

	return 0;
fail:
	return -1;
}

int exitXponMapping(void)
{
	int ret = 0;
#if defined(TCSUPPORT_GPON_MAPPING)
	ret = exitGponMapping();
	if (ret != 0)
	{
		goto fail;
	}
#endif
#if defined(TCSUPPORT_EPON_MAPPING)
	ret = exitEponMapping();	
	if (ret != 0)
	{
		goto fail;
	}
#endif

fail:
	return -1;
}


	
