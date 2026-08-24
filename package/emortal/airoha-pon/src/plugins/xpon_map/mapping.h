/*
 ***************************************************************************
 * MediaTeK Inc.
 * 4F, No. 2 Technology 5th     Rd.
 * Science-based Industrial     Park
 * Hsin-chu, Taiwan, R.O.C.
 *
 * (c) Copyright 2002-2012, MTK.
 *
 * All rights reserved. MediaTeK's source       code is an unpublished work     and     the
 * use of a     copyright notice does not imply otherwise. This source code
 * contains     confidential trade secret material of MediaTeK Tech. Any attemp
 * or participation     in deciphering, decoding, reverse engineering or in     any
 * way altering the     source code     is stricitly prohibited, unless the     prior
 * written consent of MediaTeK, Inc. is obtained.
 ***************************************************************************

        Module Name:
        mapping.h

        Abstract:

        Revision History:
        Who                     When                    What
        --------        ----------              ----------------------------------------------
        Name            Date                    Modification logs
        andy.Yi         2013/3/20       Create
*/
#ifndef _MAPPING_H_
#define _MAPPING_H_

#include "xpon_ioctl_if.h"
#include "xpon_map_ioctl.h"

#include <ecnt_hook/ecnt_hook_bbf247.h>



#if defined(TCSUPPORT_GPON_MAPPING)
#define GPON_MAPPING_DBG
extern unsigned char gponmapDebugFlag;

typedef enum {
        GPONMAP_MSG_ERR         = 0x01,
        GPONMAP_MSG_WARN        = 0x02,
        GPONMAP_MSG_TRACE       = 0x04,
        GPONMAP_MSG_DBG         = 0x08
} GPON_DebugMsg_t ;

#ifdef GPON_MAPPING_DBG
        #define GPONMAP_PRINT(level, F, B...)   { \
                                                                                if(gponmapDebugFlag & level)    \
                                                                                        printk(F, /*strrchr(__FILE__, '/')+1, __LINE__,*/ ##B) ; \
                                                                }
#else
        #define GPONMAP_PRINT(level, F, B...)   
#endif


#define RECONFIG_GPONMAP_RULE           (0xFFFFFFFF)
#define EMPTY_GPONMAP_RULE              (0xFFFF)


#define GEMPORT_MAPPING_TAGFLAG                         (1<<0)
#define GEMPORT_MAPPING_USERPORT                        (1<<1)
#define GEMPORT_MAPPING_ANI_PORT                        (1<<2)
#define GEMPORT_MAPPING_VID                             (1<<3)
#define GEMPORT_MAPPING_DSCP                            (1<<4)
#define GEMPORT_MAPPING_PBIT                            (1<<5)
#define GEMPORT_MAPPING_GEMPORT                         (1<<6)
#define GEMPORT_MAPPING_DEF_PBIT                        (1<<9)


#define TSENABLE                1
#define TSDISABLE               0
#define TAGGED                  1
#define UNTAGGED                0

/* ------------------------------GPON mapping structure------------------------------ */

/*
**********************************************************************************
gpon traffic class to GEM port mapping table structure.
**********************************************************************************
*/
typedef struct gemPortMapping_s{
        struct list_head list;
        struct rcu_head rcu;
/*tagctl: if the bit value is 1, then this bit function is valid in this rule.
        bit             description
        0               tag flag, 0:untagged, 1:tagged
        1               user port
        2               ani port
        3               vid
        4               dscp
        5               pbit
        6               gem port
        7~15    reserved
*/      
        u16 tagCtl;
        u8 tagFlag;//0:untagged,1:tagged
        u8 userPort;//0xff: this value indicates all user ports.
        u16 vid;
        u8 dscp;
        u8 pbit;
        u16 gemPort;    
}gemPortMapping_t, *gemPortMapping_ptr;

/*
**********************************************************************************
GEM port to priority queue mapping table structure.
**********************************************************************************
*/

#define MODE_OFFSET_TRAFFIC_SCHEDULER                           0
#define MODE_OFFSET_TRAFFIC_SHAPING                             1
#define MODE_OFFSET_GEM_TYPE                                    2
#define QUEUE_TRAFFIC_SHAPING_OFFSET                            3


typedef struct gponQueueMapping_s{
        struct list_head list;
        struct rcu_head rcu;
        u16 gemPort;
        u8 mode;        //bit1:0--traffic scheduler, gemport-->specfic PQ; bit2:1--traffic scheduler, gemport-->t-cont. via p-bit mapping PQ. bit2:2--traffic descriptor. bit3:gemport type
        u16 allocId;
        u8 queue;       //bit1-3: queue id, bit4-8: traffic shaping id.
}gponQueueMapping_t, *gponQueueMapping_ptr;



#if defined(TCSUPPORT_GPON_DOWNSTREAM_MAPPING)
#define RULE_CHANGE 0
#define RULE_INSERT 1

#define MAX_LAN_PORT 8
#define setDownTrtcmID(x,y)             do{(x) &= (~DS_TRTCM_ID_MARK); (x)  |= ((y)&DS_TRTCM_ID_MARK);}while(0) 
#define getDownTrtcmID(x)               ((x) & DS_TRTCM_ID_MARK)
#define setDownTrtcmEnable(x,y)                 (x = (y ? (x|DS_TRTCM_ENABLE_MARK) : (x & ~DS_TRTCM_ENABLE_MARK)))
#define getDownTrtcmEnable(x)                   ((x & DS_TRTCM_ENABLE_MARK) ? 1 : 0)

/*
        Gem port index can be 0~4095,and we only support 256 gem ports which index 
        is random between 0~4095.

        We will create an array of binding relationship which max size is 256,and use a
        globle variable to record the total num of configured gem port.

        The items in array is sort by gem_port_num,and we will use Binary Search to
        get the rule.
*/

typedef struct gpon_downstream_mapping_rule_s{
        __u16 gem_port_num;/*for 1g:0~4095:index of gem port; for 10g:0~65534:index of gem port*/
        __u16 ds_pq_enable;
	    __u16 ds_trtcm_enable;

        /*
                store interface mask here.bit0~3 mean lan1~4,bit4~7 mean ra0~3
        */
        __u32 if_mask;
        __u8 queue;//0~7 mean the queue index,0x8 mean don't specify queue
        __u8 trtcmId;
		__u8 weight;
}gpon_downstream_mapping_rule, *gpon_downstream_mapping_rule_p;

typedef struct gpon_downstream_mapping_s{
        __u16 mapping_switch;
        __u16 rule_counter;
        gpon_downstream_mapping_rule * rule;
}gpon_downstream_mapping, *gpon_downstream_mapping_p;


int gpon_ds_queue_mapping(struct sk_buff *skb, QDMA_TxQosScheduler_T *txQos);
int init_downstream_mapping_rule(void);
int clean_downstream_mapping_rule(void);

int gpon_downstream_mapping_switch_option(gpon_downstream_mapping_ioctl * data,void * arg);
int gpon_downstream_mapping_rule_option(gpon_downstream_mapping_ioctl * data,void * arg);
int get_if_mask_by_gem_port(struct sk_buff *skb, __u32 * ifmask);

#endif



/*
****************************************************************************************************
internel interface
****************************************************************************************************
*/

int startGponMapping(void);
int exitGponMapping(void);
gemPortMapping_ptr findGemPortMappingRule(gemPortMappingIoctl_ptr tableEntry);
gponQueueMapping_ptr findQueueMappingRule(u16 gemPort);

gemPortMapping_ptr findUniByMappingRule(gemPortMappingIoctl_ptr tableEntry);
int findUniByMappingRuleExt(gemPortMappingIoctl_ptr tableEntry);

/*
****************************************************************************************************
Out interface
****************************************************************************************************
*/
int addGemPortMappingRule(gemPortMappingIoctl_ptr tableEntry);
int delGemPortMappingRule(gemPortMappingIoctl_ptr tableEntry);
int displayAllGemPortMappingRule(void);
int addQueueMappingRule(gponQueueMappingIoctl_ptr tableEntry);
int delQueueMappingRule(gponQueueMappingIoctl_ptr tableEntry);
int getQueueMappingRule(gponQueueMappingIoctl_ptr tableEntry);
int reCfgQueueMappingRule(gponQueueMappingIoctl_ptr tableEntry);
int displayAllQueueMappingRule(void);
#endif /* TCSUPPORT_GPON_MAPPING */



#if defined(TCSUPPORT_EPON_MAPPING)
#define PON_QOS_DBG

#define DBG_NONE     0
#define DBG_ERROR    1
#define DBG_WARING   2
#define DBG_INFO     3
#define DBG_TRACE    4
#ifdef PON_QOS_DBG
#define QOS_PRINT(level, fmt, args...) { if (mappingDbgLevel >= level) printk("ePON map: %s:%d "fmt, __FUNCTION__, __LINE__, ##args);}
#else
#define QOS_PRINT(level, fmt, args...) {}
#endif

#define EQUAL            0
#define LESS            -1
#define GREATER          1
#define NOTEXIST         2

#define MAX_UNILLID_MAP_NUM 32

/* ------------------------------Qos classification rules structure------------------------------ */
typedef struct qosResultRule_s
{
        struct list_head list;
        struct rcu_head rcu;
        u8 pbit;                // new pbit, if equal 0xFF, don't process
        u8 queue;               // priQueue
        u8 precedence; // 0~255
        u8 matchNum; // the length of the table
        QosMatchRule_Ptr table;
}QosResultRule_t, *QosResultRule_Ptr;



typedef struct mappingMgr_s
{
        uint enable;
        u8 portResultNum[PORT_NUM]; // each port's qos rule num
        struct list_head portResultlist[PORT_NUM];
}MappingMgr_t, *MappingMgr_Ptr;

/* --------------------Packet info used for search the Qos classification list-------------------- */
typedef struct qosip4_t
{
    u32 sip;
    u32 dip;
}QosIP4_t, *QosIP4_Ptr;

typedef struct qosip6_t
{
    struct in6_addr sip;
    struct in6_addr dip;
}QosIP6_t, *QosIP6_Ptr;

typedef struct qosPacketInfo_s
{
    u8 dmac[ETH_ALEN];
    u8 smac[ETH_ALEN];

    u16 pbit: 3;   // 3bits
    u16 res: 1;    // resovle
    u16 vid: 12;   // 12bits

    u16 ethtype;

    u8 ipver;  // ip version v4/6
    u8 ipp;     // ip proto
    union QP
    {
        QosIP4_t v4;
        QosIP6_t v6;
    } L3;
        
        union {
                struct {
                        u32 ver:4,
                                dscp:8,
                                flowLbl:20;
                }bits;
                u32 value;
        }hdr;

    u16 dport;
    u16 sport;
}QosPktInfo_t, *QosPktInfo_Ptr;


/* ----------------------------------------FUNCTIONS LIST---------------------------------------- */
int compareMac(u8 mac1[ETH_ALEN], u8 mac2[ETH_ALEN]);
int compareIPv6(struct in6_addr *ip1, struct in6_addr *ip2);
int compareIP6Prex(struct in6_addr *ip1, struct in6_addr *ipPrex, int len);

u8 matchOpRes(int res, u8 op);
u8 matchRule(QosMatchRule_Ptr pMatch, u8 level, QosPktInfo_Ptr pKey);
u8 matchRules(QosResultRule_Ptr pResult, u8 level, QosPktInfo_Ptr pKey);

int equalMappingMatch(QosMatchRule_Ptr pa, QosMatchRule_Ptr pb);
int equalMappingResult(QosResultRule_Ptr pm, QosResultRule_Ptr pn);

/* ---------------------------Qos classfiction rule operator functions--------------------------- */
int   insertMappingResult(u8 port, QosResultRule_Ptr pResult);
void freeMappingResult(QosResultRule_Ptr pResult);
void printMappingResult(QosResultRule_Ptr pResult);
void printMappingMatch(QosMatchRule_Ptr pMatch);
QosResultRule_Ptr deleteMappingResult(u8 port, QosResultRule_Ptr pResult);



/* ----------------------------------------Out interface---------------------------------------- */
int    initEponMapping(void);
int    exitEponMapping(void);
int    resetEponMapping(void);

void  setMappingDbgLevel(u8 lvl);

uint getMappingEnable(u8 port);
void  setMappingEnable(u8 port, u8 enable);

void  clearMappingResult(u8 port);
void  showMappingResult(u8 port);
int    getMappingResult(u8 port, u8 index, QosResult_Ptr pRlt, u8 *pMatchNum, QosMatchRule_Ptr pMatchs);
int    addMappingResult(u8 port, QosResult_Ptr pResult, u8 matchNum, QosMatchRule_Ptr pMatchs);
int    rmMappingResult(u8 port, QosResult_Ptr pResult, u8 matchNum, QosMatchRule_Ptr pMatchs);
int	   addPortLlidMap(PortLlidMap_t *portllidmap);
u8 getMappingResultNum(u8 port);

QosResultRule_Ptr findMappingResult(u8 portid, u8 level, QosPktInfo_Ptr pKey);





/* --------------------------------Pon MAC Register Set/Get-------------------------------- */
#if 0
#define CHN_SCH_DATA                    (0x07)
#define WEIGHT_BASE_SHIFT               (3)
#define WEIGHT_BASE                     (1<<WEIGHT_BASE_SHIFT)
#define CHN_DATA_SHIFT          (4)


#define WRR_RWCMD                       (1<<31)
#define WRR_RWCMD_DONE          (1<<16)

#define CHN_IDX_SHIFT                   (3)
#define WRR_VALUE_SHIFT         (8)
#define CHN_IDX_DATA                    (0x0F)
#define QUEUE_IDX_DATA          (0x07)
#define WRR_VALUE_DATA          (0xFF)

#define TXQoS_REG_BASE          0xbfb51800
#define TXQoS_CHN0_7_CFG                (TXQoS_REG_BASE + 0x0080)
#define TXQoS_CHN8_15_CFG       (TXQoS_REG_BASE + 0x0084)
#define TXQoS_WRR_CFG           (TXQoS_REG_BASE + 0x0088) // need to change the addr

void qosSchWeightBaseWrite(u8 weightBase);
u8 qosSchWeightBaseRead(void);
int qosLlidQueueWrite(u8 llid, u8 spNum);
void qosWrrWrite(u8 llid, u8 queue, u8 weight);
u8 qosWrrRead(u8 llid, u8 queue);
#endif

/* --------------------------------Pon MAC Register Set/Get-------------------------------- */



typedef struct queueMapping_s
{
    //u8 priQueue;
    u8 enable : 1,
          llid   : 3,
          queue  : 3;
    u8 weight;
    u8 sla_eable;
}QueueMapping_t, *QueueMapping_Ptr;

void initLlidQueueMap(void );
QueueMapping_Ptr findLlidQueue(u8 priQueue);

int insertLlidQueueMap(u8 priQueue, u8 llid, u8 queue, QueueWeight_t PQweight);
int showLlidQueueMap(u8 llid);
int clearLlidQueueMap(u8 llid);


int getLlidQueueMap(u8 llid, u8 *pNum, QueueWeight_Ptr pQWeights);
int setLlidQueueMap(u8 llid, u8 num, QueueWeight_Ptr pQWeights);
#endif /* TCSUPPORT_EPON_MAPPING */

/* ---------------------------------------Hwnat interface--------------------------------------- */
void clearAllHwnatRules(void);



int xpon_mode_get(void);
int initXponMapping(void);
int exitXponMapping(void);
#endif

