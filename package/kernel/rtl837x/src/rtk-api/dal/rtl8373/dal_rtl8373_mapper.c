
/*
 * Copyright (C) 2012 Realtek Semiconductor Corp.
 * All Rights Reserved.
 *
 * This program is the proprietary software of Realtek Semiconductor
 * Corporation and/or its licensors, and only be used, duplicated,
 * modified or distributed under the authorized license from Realtek.
 *
 * ANY USE OF THE SOFTWARE OTHER THAN AS AUTHORIZED UNDER
 * THIS LICENSE OR COPYRIGHT LAW IS PROHIBITED.
 */

/*
 * Include Files
 */
#include <dal/dal_mapper.h>
#include <dal/rtl8373/dal_rtl8373_mapper.h>
#include <dal/rtl8373/dal_rtl8373_switch.h>
#include <dal/rtl8373/dal_rtl8373_vlan.h>
#include <dal/rtl8373/dal_rtl8373_svlan.h>
#include <dal/rtl8373/dal_rtl8373_acl.h>
#include <dal/rtl8373/dal_rtl8373_mirror.h>
#include <dal/rtl8373/dal_rtl8373_cpuTag.h>
#include <dal/rtl8373/dal_rtl8373_isolation.h>
#include <dal/rtl8373/dal_rtl8373_qos.h>
#include <dal/rtl8373/dal_rtl8373_sharemeter.h>
#include <dal/rtl8373/dal_rtl8373_storm.h>
#include <dal/rtl8373/dal_rtl8373_dot1x.h>
#include <dal/rtl8373/dal_rtl8373_nic.h>
#include <dal_rtl8373_igmp.h>
#include <dal/rtl8373/dal_rtl8373_lut.h>
#include <dal/rtl8373/dal_rtl8373_macsec.h>
#include <dal/rtl8373/dal_rtl8373_trunk.h>
#include <dal/rtl8373/dal_rtl8373_rtkpp.h>
#include <dal/rtl8373/dal_rtl8373_dos.h>
#include <dal/rtl8373/dal_rtl8373_gpio.h>
#include <dal/rtl8373/dal_rtl8373_ptp.h>
#include <dal/rtl8373/dal_rtl8373_drv.h>
#include <dal/rtl8373/dal_rtl8373_i2c.h>
#include <dal/rtl8373/dal_rtl8373_rate.h>
#include <dal/rtl8373/dal_rtl8373_eee.h>
#include <dal/rtl8373/dal_rtl8373_interrupt.h>
#include <dal/rtl8373/dal_rtl8373_mib.h>
#include <dal/rtl8373/dal_rtl8373_rma.h>
#include <dal/rtl8373/dal_rtl8373_led.h>
#include <dal/rtl8373/dal_rtl8373_port.h>
#include <dal/rtl8373/dal_rtl8373_phy.h>

#if 0
#include <dal/rtl8373/dal_rtl8373_leaky.h>
#include <dal/rtl8373/dal_rtl8373_rldp.h>
#include <dal/rtl8373/dal_rtl8373_led.h>
#include <dal/rtl8373/dal_rtl8373_stat.h>
#include <dal/rtl8373/dal_rtl8373_interrupt.h>
#include <dal/rtl8373/dal_rtl8373_oam.h>
#endif
/*
 * Symbol Definition
 */

/*
 * Data Declaration
 */
static dal_mapper_t dal_rtl8373_mapper = {
	/* Switch */
	.switch_init = dal_rtl8373_switch_init,
	//.switch_portMaxPktLen_set = dal_rtl8373_portMaxLen_set,
	//.switch_portMaxPktLen_get = dal_rtl8373_portMaxLen_get,
	//.switch_maxPktLenCfg_set = dal_rtl8373_switch_maxPktLenCfg_set,
	//.switch_maxPktLenCfg_get = dal_rtl8373_switch_maxPktLenCfg_get,
	.switch_greenEthernet_set = NULL, //dal_rtl8373_switch_greenEthernet_set,
	.switch_greenEthernet_get = NULL, //dal_rtl8373_switch_greenEthernet_get,

	.fMdrv_miim_mmd_read = dal_rtl8373_phy_read,
	.fMdrv_miim_mmd_write = dal_rtl8373_phy_write,
	.fMdrv_miim_mmd_readbits = dal_rtl8373_phy_readBits,
	.fMdrv_miim_mmd_writebits = dal_rtl8373_phy_writeBits,

	.fw_reset_flow_tgr_tgx = fw_reset_flow_tgr_tgx,
	.fw_reset_flow_tgr_8224 = fw_reset_flow_tgr_8224,
	.fw_reset_flow_8221B = dal_rtl8373_fw_reset_flow_8221B,
	.rtl8224_top_reg_write = dal_rtl8224_top_reg_write,
	.rtl8224_top_reg_read = dal_rtl8224_top_reg_read,
	.rtl8224_top_regbit_write = dal_rtl8224_top_regbit_write,
	.rtl8224_top_regbit_read = dal_rtl8224_top_regbit_read,
	.rtl8224_top_regbits_write = dal_rtl8224_top_regbits_write,
	.rtl8224_top_regbits_read = dal_rtl8224_top_regbits_read,
	.rtl8373_sds_reg_write = dal_rtl8373_sds_reg_write,
	.rtl8373_sds_reg_read = dal_rtl8373_sds_reg_read,
	.rtl8373_sds_regbits_write = dal_rtl8373_sds_regbits_write,
	.rtl8373_sds_regbits_read = dal_rtl8373_sds_regbits_read,
	.rtl8224_sds_reg_write = dal_rtl8224_sds_reg_write,
	.rtl8224_sds_reg_read = dal_rtl8224_sds_reg_read,
	.rtl8224_sds_regbits_write = dal_rtl8224_sds_regbits_write,
	.rtl8224_sds_regbits_read = dal_rtl8224_sds_regbits_read,
	.rtl8373_phy_regbits_write = dal_rtl8373_phy_regbits_write,
	.rtl8373_phy_regbits_read = dal_rtl8373_phy_regbits_read,

	.rtl8373_setAsicRegBit = rtl8373_setAsicRegBit,
	.rtl8373_getAsicRegBit = rtl8373_getAsicRegBit,
	.rtl8373_setAsicRegBits = rtl8373_setAsicRegBits,
	.rtl8373_getAsicRegBits = rtl8373_getAsicRegBits,
	.rtl8373_setAsicReg = rtl8373_setAsicReg,
	.rtl8373_getAsicReg = rtl8373_getAsicReg,

	/* cpu */
	.cpuTag_externalCpuPort_set = dal_rtl8373_cpuTag_externalCpuPort_set,
	.cpuTag_externalCpuPort_get = dal_rtl8373_cpuTag_externalCpuPort_get,
	.cpuTag_tpid_set = dal_rtl8373_cpuTag_tpid_set,
	.cpuTag_tpid_get = dal_rtl8373_cpuTag_tpid_get,
	.cpuTag_enable_set = dal_rtl8373_cpuTag_enable_set,
	.cpuTag_enable_get = dal_rtl8373_cpuTag_enable_get,
	.cpuTag_insertMode_set = dal_rtl8373_cpuTag_insertMode_set,
	.cpuTag_insertMode_get = dal_rtl8373_cpuTag_insertMode_get,
	.cpuTag_awarePort_set = dal_rtl8373_cpuTag_awarePort_set,
	.cpuTag_awarePort_get = dal_rtl8373_cpuTag_awarePort_get,
	.cpuTag_priRemap_set = dal_rtl8373_cpuTag_priRemap_set,
	.cpuTag_priRemap_get = dal_rtl8373_cpuTag_priRemap_get,

	/* acl */

	/* mirror */

	.port_isolation_set = dal_rtl8373_port_isolation_set,
	.port_isolation_get = dal_rtl8373_port_isolation_get,

	/* Storm */

	/* Rate */
	/*QoS*/

	/*VLAN*/
	.vlan_init = dal_rtl8373_vlan_init,
	.vlan_set = dal_rtl8373_vlan_set,
	.vlan_get = dal_rtl8373_vlan_get,
	.vlan_egrFilterEnable_set = dal_rtl8373_vlan_egrFilterEnable_set,
	.vlan_egrFilterEnable_get = dal_rtl8373_vlan_egrFilterEnable_get,
	.vlan_portPvid_set = dal_rtl8373_vlan_portPvid_set,
	.vlan_portPvid_get = dal_rtl8373_vlan_portPvid_get,
	.vlan_portIgrFilterEnable_set = dal_rtl8373_vlan_portIgrFilterEnable_set,
	.vlan_portIgrFilterEnable_get = dal_rtl8373_vlan_portIgrFilterEnable_get,
	.vlan_portAcceptFrameType_set = dal_rtl8373_vlan_portAcceptFrameType_set,
	.vlan_portAcceptFrameType_get = dal_rtl8373_vlan_portAcceptFrameType_get,
	.vlan_tagMode_set = dal_rtl8373_vlan_tagMode_set,
	.vlan_tagMode_get = dal_rtl8373_vlan_tagMode_get,
	.vlan_transparent_set = dal_rtl8373_vlan_portTransparent_set,
	.vlan_transparent_get = dal_rtl8373_vlan_portTransparent_get,
	.vlan_keep_set = dal_rtl8373_vlan_keep_set,
	.vlan_keep_get = dal_rtl8373_vlan_keep_get,
	.vlan_stg_set = dal_rtl8373_vlan_stg_set,
	.vlan_stg_get = dal_rtl8373_vlan_stg_get,
	.vlan_portFid_set = dal_rtl8373_vlan_portFid_set,
	.vlan_portFid_get = dal_rtl8373_vlan_portFid_get,
	.vlan_reservedVidAction_set = dal_rtl8373_vlan_reservedVidAction_set,
	.vlan_reservedVidAction_get = dal_rtl8373_vlan_reservedVidAction_get,
	.vlan_realKeepRemarkEnable_set = dal_rtl8373_vlan_realKeepRemarkEnable_set,
	.vlan_realKeepRemarkEnable_get = dal_rtl8373_vlan_realKeepRemarkEnable_get,
	.vlan_disL2Learn_entry_set = dal_rtl8373_vlan_disL2Learn_entry_set,
	.vlan_disL2Learn_entry_get = dal_rtl8373_vlan_disL2Learn_entry_get,
	.vlan_reset = dal_rtl8373_vlan_reset,

	/*dot1x*/
	//.dot1x_eapolFrame2CpuEnable_set = dal_rtl8373_dot1x_eapolFrame2CpuEnable_set,
	//.dot1x_eapolFrame2CpuEnable_get = dal_rtl8373_dot1x_eapolFrame2CpuEnable_get,

	/*MACsec*/

	/*SVLAN*/
	// .svlan_memberPortEntry_set = dal_rtl8373_svlanMbrrPortEntry_set,
	// .svlan_memberPortEntry_get = dal_rtl8373_svlanmemberPortEntry_get,
	//.svlan_memberPortEntry_adv_set = NULL,
	//.svlan_memberPortEntry_adv_get = NULL,

	/*NIC*/

	/* IGMP */
	.igmp_init = dal_rtl8373_igmp_init,
	.igmp_state_set = dal_rtl8373_igmp_state_set,
	.igmp_state_get = dal_rtl8373_igmp_state_get,
	.igmp_static_router_port_set = dal_rtl8373_igmp_static_router_port_set,
	.igmp_static_router_port_get = dal_rtl8373_igmp_static_router_port_get,
	.igmp_protocol_set = dal_rtl8373_igmp_protocol_set,
	.igmp_protocol_get = dal_rtl8373_igmp_protocol_get,
	.igmp_fastLeave_set = dal_rtl8373_igmp_fastLeave_set,
	.igmp_fastLeave_get = dal_rtl8373_igmp_fastLeave_get,
	.igmp_maxGroup_set = dal_rtl8373_igmp_maxGroup_set,
	.igmp_maxGroup_get = dal_rtl8373_igmp_maxGroup_get,
	.igmp_currentGroup_get = dal_rtl8373_igmp_currentGroup_get,
	.igmp_tableFullAction_set = dal_rtl8373_igmp_tableFullAction_set,
	.igmp_tableFullAction_get = dal_rtl8373_igmp_tableFullAction_get,
	.igmp_checksumErrorAction_set = dal_rtl8373_igmp_checksumErrorAction_set,
	.igmp_checksumErrorAction_get = dal_rtl8373_igmp_checksumErrorAction_get,
	.igmp_leaveTimer_set = dal_rtl8373_igmp_leaveTimer_set,
	.igmp_leaveTimer_get = dal_rtl8373_igmp_leaveTimer_get,
	.igmp_queryInterval_set = dal_rtl8373_igmp_queryInterval_set,
	.igmp_queryInterval_get = dal_rtl8373_igmp_queryInterval_get,
	.igmp_robustness_set = dal_rtl8373_igmp_robustness_set,
	.igmp_robustness_get = dal_rtl8373_igmp_robustness_get,
	.igmp_dynamicRouterPortAllow_set = dal_rtl8373_igmp_dynamicRouterPortAllow_set,
	.igmp_dynamicRouterPortAllow_get = dal_rtl8373_igmp_dynamicRouterPortAllow_get,
	.igmp_dynamicRouterPort_get = dal_rtl8373_igmp_dynamicRouterPort_get,
	.igmp_suppressionEnable_set = dal_rtl8373_igmp_suppressionEnable_set,
	.igmp_suppressionEnable_get = dal_rtl8373_igmp_suppressionEnable_get,
	.igmp_portRxPktEnable_set = dal_rtl8373_igmp_portRxPktEnable_set,
	.igmp_portRxPktEnable_get = dal_rtl8373_igmp_portRxPktEnable_get,
	.igmp_groupInfo_get = dal_rtl8373_igmp_groupInfo_get,
	.igmp_ReportLeaveFwdAction_set = dal_rtl8373_igmp_ReportLeaveFwdAction_set,
	.igmp_ReportLeaveFwdAction_get = dal_rtl8373_igmp_ReportLeaveFwdAction_get,
	.igmp_dropLeaveZeroEnable_set = dal_rtl8373_igmp_dropLeaveZeroEnable_set,
	.igmp_dropLeaveZeroEnable_get = dal_rtl8373_igmp_dropLeaveZeroEnable_get,
	.igmp_bypassGroupRange_set = dal_rtl8373_igmp_bypassGroupRange_set,
	.igmp_bypassGroupRange_get = dal_rtl8373_igmp_bypassGroupRange_get,
#if 1
	/*RLDP*/
#endif

#if 1
	/*trunk*/
#endif

	/* l2 */
	.l2_init = dal_rtl8373_l2_init,
	.l2_addr_add = dal_rtl8373_l2_addr_add,
	.l2_addr_get = dal_rtl8373_l2_addr_get,
	.l2_addr_next_get = dal_rtl8373_l2_addr_next_get,
	.l2_addr_del = dal_rtl8373_l2_addr_del,
	.l2_mcastAddr_add = dal_rtl8373_l2_mcastAddr_add,
	.l2_mcastAddr_get = dal_rtl8373_l2_mcastAddr_get,
	.l2_mcastAddr_next_get = dal_rtl8373_l2_mcastAddr_next_get,
	.l2_mcastAddr_del = dal_rtl8373_l2_mcastAddr_del,
	.l2_ipMcastAddr_add = dal_rtl8373_l2_ipMcastAddr_add,
	.l2_ipMcastAddr_get = dal_rtl8373_l2_ipMcastAddr_get,
	.l2_ipMcastAddr_next_get = dal_rtl8373_l2_ipMcastAddr_next_get,
	.l2_ipMcastAddr_del = dal_rtl8373_l2_ipMcastAddr_del,
	.l2_ucastAddr_flush = dal_rtl8373_l2_ucastAddr_flush,
	.l2_table_clear = dal_rtl8373_l2_table_clear,
	.l2_table_clearStatus_get = dal_rtl8373_l2_table_clearStatus_get,
	.l2_flushLinkDownPortAddrEnable_set = dal_rtl8373_l2_flushLinkDownPortAddrEnable_set,
	.l2_flushLinkDownPortAddrEnable_get = dal_rtl8373_l2_flushLinkDownPortAddrEnable_get,
	.l2_agingEnable_set = dal_rtl8373_l2_agingEnable_set,
	.l2_agingEnable_get = dal_rtl8373_l2_agingEnable_get,
	.l2_limitLearningCnt_set = dal_rtl8373_l2_limitLearningCnt_set,
	.l2_limitLearningCnt_get = dal_rtl8373_l2_limitLearningCnt_get,
	.l2_limitSystemLearningCnt_set = dal_rtl8373_l2_limitSystemLearningCnt_set,
	.l2_limitSystemLearningCnt_get = dal_rtl8373_l2_limitSystemLearningCnt_get,
	.l2_limitLearningCntAction_set = dal_rtl8373_l2_limitLearningCntAction_set,
	.l2_limitLearningCntAction_get = dal_rtl8373_l2_limitLearningCntAction_get,
	.l2_limitSystemLearningCntAction_set = dal_rtl8373_l2_limitSystemLearningCntAction_set,
	.l2_limitSystemLearningCntAction_get = dal_rtl8373_l2_limitSystemLearningCntAction_get,
	.l2_limitSystemLearningCntPortMask_set = dal_rtl8373_l2_limitSystemLearningCntPortMask_set,
	.l2_limitSystemLearningCntPortMask_get = dal_rtl8373_l2_limitSystemLearningCntPortMask_get,
	.l2_learningCnt_get = dal_rtl8373_l2_learningCnt_get,
	.l2_floodPortMask_set = dal_rtl8373_l2_floodPortMsk_set,
	.l2_floodPortMask_get = dal_rtl8373_l2_floodPortMsk_get,
	.l2_localPktPermit_set = dal_rtl8373_srcPortPermit_set,
	.l2_localPktPermit_get = dal_rtl8373_srcPortPermit_get,
	.l2_aging_set = dal_rtl8373_l2_ageout_timer_set,
	.l2_aging_get = dal_rtl8373_l2_ageout_timer_get,
	.l2_ipMcastAddrLookup_set = dal_rtl8373_l2_ipMcastAddrLookup_set,
	.l2_ipMcastAddrLookup_get = dal_rtl8373_l2_ipMcastAddrLookup_get,
	.l2_ipMcastForwardRouterPort_set = NULL,
	.l2_ipMcastForwardRouterPort_get = NULL,
	.l2_ipMcastGroupEntry_add = dal_rtl8373_l2_ipMcastGroupEntry_add,
	.l2_ipMcastGroupEntry_del = dal_rtl8373_l2_ipMcastGroupEntry_del,
	.l2_ipMcastGroupEntry_get = dal_rtl8373_l2_ipMcastGroupEntry_get,
	.l2_entry_get = dal_rtl8373_l2_entry_get,
	.l2_lookupHitIsolationAction_set = dal_rtl8373_l2_lookupHitIsolationAction_set,
	.l2_lookupHitIsolationAction_get = dal_rtl8373_l2_lookupHitIsolationAction_get,
	.l2_unknownUnicastPktAction_set = dal_rtl8373_l2_unknUc_action_set,
	.l2_unknownUnicastPktAction_get = dal_rtl8373_l2_unknUc_action_get,
	.l2_unknownMulticastPktAction_set = dal_rtl8373_l2_unknMc_action_set,
	.l2_unknownMulticastPktAction_get = dal_rtl8373_l2_unknMc_action_get,

	/*GPIO*/
	.gpio_muxSel_set = dal_rtl8373_gpio_muxSel_set,
	.gpio_pinVal_write = dal_rtl8373_gpio_pinVal_write,
	.gpio_pinVal_read = dal_rtl8373_gpio_pinVal_read,
	.gpio_pinDir_set = dal_rtl8373_gpio_pinDir_set,

	/*I2C*/

	/*Rate : ingress BW & egress queue BW*/

	/* eee */
	.eee_init = dal_rtl8373_eee_init,
	.eee_macForceSpeedEn_set = dal_rtl8373_eee_macForceSpeedEn_set,
	.eee_macForceSpeedEn_get = dal_rtl8373_eee_macForceSpeedEn_get,
	.eee_macForceAllSpeedEn_get = dal_rtl8373_eee_macForceAllSpeedEn_get,
	.eee_portTxRxEn_set = dal_rtl8373_eee_portTxRxEn_set,
	.eee_portTxRxEn_get = dal_rtl8373_eee_portTxRxEn_get,

	/* interrupt */
	/* mib */
	.stat_global_reset = dal_rtl8373_globalMib_rst,
	.stat_port_reset = dal_rtl8373_portMib_rst,
	.stat_port_get = dal_rtl8373_portMib_read,
	.stat_lengthMode_set = dal_rtl8373_mibLength_set,
	.stat_lengthMode_get = dal_rtl8373_mibLength_get,

	/*RMA*/

	.rma_set = dal_rtl8373_asicRma_set,
	.rma_get = dal_rtl8373_asicRma_get,

	/*LED*/

	/*port*/
	.port_macForceLink_set = dal_rtl8373_portFrcAbility_set,
	.port_macForceLink_get = dal_rtl8373_portFrcAbility_get,
	.port_macStatus_get = dal_rtl8373_portStatus_get,
	.port_macLocalLoopbackEnable_set = dal_rtl8373_portLoopbackEn_set,
	.port_macLocalLoopbackEnable_get = dal_rtl8373_portLoopbackEn_get,
	.port_backpressureEnable_set = dal_rtl8373_portBackpressureEn_set,
	.port_backpressureEnable_get = dal_rtl8373_portBackpressureEn_get,
	.port_rtct_init = dal_rtl8373_rtct_init,
	.port_rtct_start = dal_rtl8373_rtct_start,
	.port_rtctResult_get = dal_rtl8373_rtct_status_get,
	.port_sdsMode_set = dal_rtl8373_sdsMode_set,
	.port_sdsMode_get = dal_rtl8373_sdsMode_get,
	.port_sdsNway_get = dal_rtl8373_port_sdsNway_get,
	.port_sdsNway_set = dal_rtl8373_port_sdsNway_set,
	.port_extphyid_set = dal_rtl8373_port_extphyid_set,
	.port_extphyid_get = dal_rtl8373_port_extphyid_get,

	/*phy*/
	.phy_common_c45_an_restart = dal_rlt8373_phy_common_c45_an_restart,
	.phy_common_c45_autoNegoEnable_get = dal_rlt8373_phy_common_c45_autoNegoEnable_get,
	.phy_common_c45_autoNegoEnable_set = dal_rlt8373_phy_common_c45_autoNegoEnable_set,
	.phy_autoNegoAbility_set = dal_rlt8373_phy_autoNegoAbility_set,
	.phy_common_c45_autoSpeed_set = dal_rlt8373_phy_common_c45_autoSpeed_set,

	.phy_common_c45_speed_set = dal_rlt8373_phy_common_c45_speed_set,
	.phy_common_c45_speed_get = dal_rlt8373_phy_common_c45_speed_get,
	.phy_common_c45_enable_set = dal_rlt8373_phy_common_c45_enable_set,
	.phy_common_c45_enable_get = dal_rlt8373_phy_common_c45_enable_get,
	.phy_common_c45_duplex_set = dal_rlt8373_phy_common_c45_duplex_set,
	.phy_common_c45_duplex_get = dal_rlt8373_phy_common_c45_duplex_get,
	.phy_common_c45_speedDuplexStatusResReg_get = dal_rlt8373_phy_common_c45_speedDuplexStatusResReg_get,

#if 0

	/*leaky*/

	/* led */

	/* oam */

	/* stat */
	.stat_global_reset = dal_rtl8373_stat_global_reset,
	.stat_port_reset = dal_rtl8373_stat_port_reset,
	.stat_port_get = dal_rtl8373_stat_port_get,
	.stat_lengthMode_set = dal_rtl8373_stat_lengthMode_set,
	.stat_lengthMode_get = dal_rtl8373_stat_lengthMode_get,

	/* interrupt */

	/* port */

	.port_adminEnable_get = dal_rtl8373_port_adminEnable_get.port_rgmiiDelayExt_set = dal_rtl8373_port_rgmiiDelayExt_set,
	.port_rtctResult_get = NULL,

#endif

	/*PTP*/

};

/*
 * Macro Declaration
 */

/*
 * Function Declaration
 */

/* Module Name    :  */

/* Function Name:
 *      dal_rtl8373_mapper_get
 * Description:
 *      Get DAL mapper function
 * Input:
 *      None
 * Output:
 *      None
 * Return:
 *      dal_mapper_t *     - mapper pointer
 * Note:
 */
dal_mapper_t *dal_rtl8373_mapper_get(void)
{
	return &dal_rtl8373_mapper;
} /* end of dal_rtl8373_mapper_get */
