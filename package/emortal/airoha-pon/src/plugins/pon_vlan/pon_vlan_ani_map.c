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
	pon_vlan_ani_map.c

	Abstract:

	Revision History:
	Who			When			What
	--------	----------		----------------------------------------------
	Name		Date			Modification logs
	Xi.Wang		2020/1/7	Create
*/

#include <linux/slab.h>
#include "pon_vlan_ani_map.h"
#include "pon_vlan.h"
#include "xpon_ioctl_if.h"

__u32 gponAniMapDbgLevel = GPON_ANI_MAP_MSG_NONE;
int gponAniMapEnableFlag = 1;
Gpon_Ani_Map_T gponAniMapInfo;

static int gpon_ani_map_get_free_column(__u16* column_ptr)
{
	__u16 i = 0;

	if(NULL == column_ptr){
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"parameter is NULL\n");
		return -1;
	}
	for(i=0; i<GPON_GEMPORT_MAX_NUM; i++){
		if(INVALID_GEM_ID == gponAniMapInfo.map[i].gemId)
		{
			*column_ptr = i;
			GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_TRACE,"column(%d) is free,success!\n",i);
			return 0;
		}
	}
	GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"map is full\n");
	return -1;
}

static int gpon_ani_map_get_free_row(__u16 column_id, __u16* row_ptr)
{
	__u16 i = 0;

	if((column_id >= GPON_GEMPORT_MAX_NUM)||(NULL == row_ptr)){
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"parameter error, column_id=%d or row_ptr is NULL\n",column_id);
		return -1;
	}
	for(i=0; i<GPON_GEM_MAX_ANI_NUM; i++){
		if(INVALID_ANI_ID == gponAniMapInfo.map[column_id].ani[i])
		{
			*row_ptr = i;
			GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_TRACE,"column(%d),row(%d) is free,success!\n",column_id,i);
			return 0;
		}
	}
	GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"column(%d) is full\n",i);
	return -1;
}
static int gpon_ani_map_check_exist(__u16 gem_id,__u16 ani_id)
{
	__u16 i = 0;
	__u16 column_id = 0;

	if((gem_id > GPON_10G_MAX_VALID_GEM_ID)||(ani_id >= GPON_MAX_ANI_INTERFACE)){
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"parameter error, gem_id=%d,ani_id=%d\n",gem_id,ani_id);
		return -1;
	}
	column_id = gponAniMapInfo.gemIdToIndex[gem_id];
	if(column_id >= GPON_GEMPORT_MAX_NUM){
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"column_id error: gem_id=%d,column_id=%d !\n",gem_id,column_id);
		return -1;
	}
	for(i=0; i<GPON_GEM_MAX_ANI_NUM; i++){
		if(ani_id == gponAniMapInfo.map[column_id].ani[i]){
			GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_TRACE,"Exist: gem_id=%d,ani_id=%d !\n",gem_id,ani_id);
			return 0;
		}
	}
	GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_TRACE,"Not Exist: gem_id=%d,ani_id=%d !\n",gem_id,ani_id);
	return -1;
}
static int gpon_ani_map_add_by_gem_ani(__u16 gem_id,__u16 ani_id)
{
	__u16 column_id = 0;
	__u16 row_id = 0;

	if((gem_id > GPON_10G_MAX_VALID_GEM_ID)||(ani_id >= GPON_MAX_ANI_INTERFACE)){
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"parameter error, gem_id=%d,ani_id=%d\n",gem_id,ani_id);
		return -1;
	}
	column_id = gponAniMapInfo.gemIdToIndex[gem_id];
	if((column_id >= GPON_GEMPORT_MAX_NUM) && (INVALID_GEM_ID_TO_INDEX != column_id)){
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"column_id error: gem_id=%d,column_id=%d !\n",gem_id,column_id);
		return -1;
	}
	/*This gemport without ani mapping, then alloc column id for it.*/
	if(INVALID_GEM_ID_TO_INDEX == column_id){
		if(gpon_ani_map_get_free_column(&column_id)){
			GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"get column id fail\n");
			return -1;
		}
		if(column_id >= GPON_GEMPORT_MAX_NUM){
			GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"column_id error: gem_id=%d,column_id=%d !\n",gem_id,column_id);
			return -1;
		}
		gponAniMapInfo.gemIdToIndex[gem_id] = column_id;
		gponAniMapInfo.map[column_id].gemId = gem_id;
		gponAniMapInfo.map[column_id].ani[0] = ani_id;
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_TRACE,"column(%d),row(0) mapping done,succss!\n",column_id);
		return 0;
	}
	/*The gemport has recorded current ani index.*/
	if(0 == gpon_ani_map_check_exist(gem_id,ani_id)){
		return 0;
	}
	/*This gemport has ani mapping, then add one more ani id.*/
	if(gpon_ani_map_get_free_row(column_id, &row_id)){
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"column(%d),get row id fail\n",column_id);
		return -1;
	}
	if(ani_id >= GPON_GEM_MAX_ANI_NUM){
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"ani_id error: gem_id=%d,ani_id=%d !\n",gem_id,ani_id);
		return -1;
	}
	gponAniMapInfo.map[column_id].ani[row_id] = ani_id;
	GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_TRACE,"column(%d),row(%d) mapping done,succss!\n",column_id,row_id);
	return 0;
}

static int gpon_ani_map_delete_by_gem_ani(__u16 gem_id, __u16 ani_id)
{
	__u16 column_id = 0;
	__u16 i = 0;
	int has_other_ani = 0;

	if((gem_id > GPON_10G_MAX_VALID_GEM_ID)||(ani_id >= GPON_MAX_ANI_INTERFACE)){
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"parameter error, gem_id=%d,ani_id=%d\n",gem_id,ani_id);
		return -1;
	}
	column_id = gponAniMapInfo.gemIdToIndex[gem_id];
	if(column_id >= GPON_GEMPORT_MAX_NUM){
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"column_id error: gem_id=%d,column_id=%d !\n",gem_id,column_id);
		return -1;
	}
	for(i=0; i<GPON_GEM_MAX_ANI_NUM; i++){
		if(ani_id == gponAniMapInfo.map[column_id].ani[i]){
			gponAniMapInfo.map[column_id].ani[i] = INVALID_ANI_ID;
		}else if(INVALID_ANI_ID != gponAniMapInfo.map[column_id].ani[i]){
			has_other_ani = 1;
		}
	}
	/*This gemport without other ani id, clear mapping record.*/
	if(0 == has_other_ani){
		gponAniMapInfo.gemIdToIndex[gem_id] = INVALID_GEM_ID_TO_INDEX;
		gponAniMapInfo.map[column_id].gemId = INVALID_GEM_ID;
	}
	return 0;
}

int gpon_ani_map_get_by_gem(__u16 gem_id,__u16* ani_ptr)
{
	__u16 i = 0;
	__u16 column_id = 0;
	int ani_num = 0;

	if((gem_id > GPON_10G_MAX_VALID_GEM_ID)||(NULL == ani_ptr)){
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"parameter error, gem_id=%d or ani_id is NULL\n",gem_id);
		return -1;
	}
	column_id = gponAniMapInfo.gemIdToIndex[gem_id];
	if(column_id >= GPON_GEMPORT_MAX_NUM){
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"column_id error: gem_id=%d,column_id=%d !\n",gem_id,column_id);
		return -1;
	}
	for(i=0; i<GPON_GEM_MAX_ANI_NUM; i++){
		if(INVALID_ANI_ID != gponAniMapInfo.map[column_id].ani[i]){
			*ani_ptr = gponAniMapInfo.map[column_id].ani[i];
			GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_TRACE,"gem_id=%d or ani_id=%d\n",gem_id,*ani_ptr);
			ani_ptr ++;
			ani_num ++;
		}
	}
	GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_TRACE,"gem_id=%d, ani_num=%d\n",gem_id,ani_num);
	return ani_num;
}

static int gpon_ani_opt_modify(unsigned int cmd, void* arg)
{
	__u16 i = 0;
	struct XMCS_GemPortAssign_S *gemInfo;
	int ret = 0;

	if(NULL == arg)
	{
		printk("[%s][%d]parameter is NULL\n",__FUNCTION__,__LINE__);
		return -1;
	}
	if(((PONVLAN_IOC_ADD_GPON_ANI_MAP_OPT) != cmd)&&((PONVLAN_IOC_DEL_GPON_ANI_MAP_OPT) != cmd)){
		printk("[%s][%d]parameter error,cmd =%d\n",__FUNCTION__,__LINE__,cmd);
		return -1;
	}

	gemInfo = kzalloc(sizeof(struct XMCS_GemPortAssign_S), GFP_ATOMIC);
	if(NULL == gemInfo){
		printk("[%s][%d]kzalloc fail\n",__FUNCTION__,__LINE__);
		return -1;
	}

	if (copy_from_user(gemInfo, (struct XMCS_GemPortAssign_S*)arg, sizeof(struct XMCS_GemPortAssign_S)))
	{
		printk("[%s][%d]copy_from_user fail\n",__FUNCTION__,__LINE__);
		ret = -EFAULT;
		goto END;
	}
	if(gemInfo->entryNum > GPON_GEMPORT_MAX_NUM) {
		GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"parameter error, gemInfo.entryNum=%d\n",gemInfo->entryNum);
		ret = -EFAULT;
		goto END;
	}
	for(i=0; i<gemInfo->entryNum ; i++) {
		if(gemInfo->gemPort[i].ani >= GPON_MAX_ANI_INTERFACE) {
			GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_WARN,"parameter error, ani_id=%d\n",gemInfo->gemPort[i].ani);
			continue;
		}
		if(gemInfo->gemPort[i].id <= GPON_10G_MAX_VALID_GEM_ID) {
			if((PONVLAN_IOC_ADD_GPON_ANI_MAP_OPT) == cmd){
				if(gpon_ani_map_add_by_gem_ani(gemInfo->gemPort[i].id, gemInfo->gemPort[i].ani)) {
					GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"Add gem ani map fail\n");
					ret = -1;
					goto END;
				}
			}else {
				if(gpon_ani_map_delete_by_gem_ani(gemInfo->gemPort[i].id, gemInfo->gemPort[i].ani)) {
					GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_ERR,"Delete gem ani map fail\n");
					ret = -1;
					goto END;
				}
			}
		}
	}
	GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_TRACE,"Modify ani id(%d) mapping done,succss!\n",gemInfo->gemPort[i].ani);
	ret = 0;

END:
	if(NULL != gemInfo)
		kfree(gemInfo);
	return ret;
}

int gpon_ani_opt_clean(void)
{
	memset(&gponAniMapInfo, 0xFF, sizeof(Gpon_Ani_Map_T));
	GPON_ANI_MAP_MSG(GPON_ANI_MAP_MSG_TRACE,"Clean all rule done!\n");
	return 0;
}

static int gpon_ani_opt_get(void* arg)
{
	__u16 gem_id = 0;
	int ani_num = 0, i = 0;
	__u16 ani_id[GPON_GEM_MAX_ANI_NUM] = {0};
	
	if(NULL == arg)
	{
		printk("[%s][%d]arg is NULL\n",__FUNCTION__,__LINE__);
		return -1;
	}
	memset(&gem_id, 0, sizeof(gem_id));
	if (copy_from_user(&gem_id, (__u16*)arg, sizeof(gem_id)))
	{
		printk("[%s][%d]copy_from_user fail\n",__FUNCTION__,__LINE__);
		return -EFAULT;
	}
	ani_num = gpon_ani_map_get_by_gem(gem_id,ani_id);
	if(ani_num <= 0){
		printk("\r\nGEM Port(%d), Without ANI\n",gem_id);
		return 0;
	}
	ani_num = (ani_num <= GPON_GEM_MAX_ANI_NUM)? ani_num:GPON_GEM_MAX_ANI_NUM;

	printk("\r\nGEM Port(%d)",gem_id);
	for(i=0; i<ani_num; i++){
		printk(", ANI ID[%d]:%d", i, ani_id[i]);
	}
	printk("\r\n");
	return 0;
}

static int gpon_ani_opt_display(void)
{
	__u16 i = 0, j = 0;
	int has_gem_ani = 0;

	printk("\r\nGEM Port & ANI Index mapping:");
	for(i=0; i<GPON_GEMPORT_MAX_NUM; i++){
		if(INVALID_GEM_ID == gponAniMapInfo.map[i].gemId){
			continue;
		}
		has_gem_ani = 1;
		printk("\r\nGEM ID:%d",gponAniMapInfo.map[i].gemId);
		for(j=0; j<GPON_GEM_MAX_ANI_NUM; j++){
			if(INVALID_ANI_ID == gponAniMapInfo.map[i].ani[j]){
				continue;
			}
			printk(", ANI ID[%d]:%d",j,gponAniMapInfo.map[i].ani[j]);
		}
	}
	if(0 == has_gem_ani){
		printk("\r\nWithout Gemport ANI mapping rule!");
	}
	printk("\r\n");
	return 0;
}

static int gpon_ani_opt_dbg_level(void* arg)
{
	__u32 dbg_level = 0;
	
	if(NULL == arg)
	{
		printk("[%s][%d]arg is NULL\n",__FUNCTION__,__LINE__);
		return -1;
	}
	memset(&dbg_level, 0, sizeof(dbg_level));
	if (copy_from_user(&dbg_level, (__u32*)arg, sizeof(dbg_level)))
	{
		printk("[%s][%d]copy_from_user fail\n",__FUNCTION__,__LINE__);
		return -EFAULT;
	}
	gponAniMapDbgLevel = dbg_level;
	printk("set dbg level success,now value is %x\n",gponAniMapDbgLevel);

	return 0;
}

static int gpon_ani_opt_enable(void* arg)
{
	int enable = 0;
	
	if(NULL == arg)
	{
		printk("[%s][%d]arg is NULL\n",__FUNCTION__,__LINE__);
		return -1;
	}
	memset(&enable, 0, sizeof(enable));
	if (copy_from_user(&enable, (int*)arg, sizeof(enable)))
	{
		printk("[%s][%d]copy_from_user fail\n",__FUNCTION__,__LINE__);
		return -EFAULT;
	}
	gponAniMapEnableFlag = enable;
	printk("set enable flag success,now value is %s\n",((enable == 0) ? "disable":"ensable"));

	return 0;
}

int gpon_ani_opt_dispatch(unsigned int cmd, void* arg)
{
	int ret = 0;

	if(NULL == arg)
	{
		printk("[%s][%d]arg is NULL\n",__FUNCTION__,__LINE__);
		return -1;
	}

	switch(cmd)
	{
		case PONVLAN_IOC_ADD_GPON_ANI_MAP_OPT:
		case PONVLAN_IOC_DEL_GPON_ANI_MAP_OPT:
			ret = gpon_ani_opt_modify(cmd,arg);
			break;
		case PONVLAN_IOC_CLEAN_GPON_ANI_MAP_OPT:
			ret = gpon_ani_opt_clean();
			break;
		case PONVLAN_IOC_GET_GPON_ANI_MAP_OPT:
			ret = gpon_ani_opt_get(arg);
			break;
		case PONVLAN_IOC_DISP_GPON_ANI_MAP_OPT:
			ret = gpon_ani_opt_display();
			break;
		case PONVLAN_IOC_GPON_ANI_MAP_DBG_LEVEL:
			ret = gpon_ani_opt_dbg_level(arg);
			break;
		case PONVLAN_IOC_ENABLE_GPON_ANI_MAP:
			ret = gpon_ani_opt_enable(arg);
			break;
		default:
			ret = -1;
			printk("[%s][%d]cmd error\n",__FUNCTION__,__LINE__);
			break;
	}

	return ret;
}

int gpon_init_ani_map(void)
{
	gponAniMapDbgLevel = GPON_ANI_MAP_MSG_NONE;
	gponAniMapEnableFlag = 1;
	memset(&gponAniMapInfo, 0xFF, sizeof(Gpon_Ani_Map_T));

	return 0;
}

