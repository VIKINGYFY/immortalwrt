#ifndef _EN7572_REG_H_
#define _EN7572_REG_H_

#include <ecnt_hook/ecnt_hook_pon_phy.h>
#include <ecnt_hook/ecnt_hook_lddla.h>
#include "lddla_types.h"


// Debug
#define LDDLA_PRINT(F, B...)	{ \
										if(db_msg)	\
											printk(F,##B);\
									}

// Variable
extern bool db_msg;


// Structure
typedef struct
{
	struct task_struct		*bosa_temp_extcal_task_wait;
	struct task_struct		*reduce_pav_task_wait;
}	PHY_GlbPriv_T;

// EN7572 CSR
#define RG_TXOUTPK_LOAD			0x00000108
#define RG_APC_ANA_VMON_SEL		0x00000124
#define RG_IMPD_SINK			0x00000128
#define RG_ERC_MDAC_REV			0x0000012C
#define RG_RESERVE_TIA			0x00000130
#define RG_REP_PH_CAP_SEL		0x0000013C
#define RG_APD_DAC_CODE			0x0000015C
#define DCL_CTRL_2				0x00000210
#define CSR_VOLT_SUM 			0x0000039C
#define CSR_TEMP_SUM 			0x000003A0
#define CSR_MPD_SUM 			0x000003A4
#define CSR_RSSI_SUM 			0x000003A8
#define CSR_IAV 				0x000003C4
#define CSR_IBIAS_IMOD 			0x000003C8
#define IAV_IMOD_SAFE_LMT		0x000003CC
#define MPD_SAFE_LMT			0x000003D0
#define VOLT_SAFE_LMT			0x000003D4
#define TX_FAULT_DIS_SEL		0x000003DC
#define LOS_CTRL_1 				0x0000043C
#define SYSTEM_STS_0			0x00000488
#define CSR_INTERFACE_CTRL_2	0x00000508
#define DUMMY_0_REG_0			0x0000054C
#define DUMMY_1_REG_0			0x00000550

// MD32
#define MD32_DEVICE_ADDR  		0x50

#define MD32_PM_CFG_ADDR		0x3000
#define MD32_PM_ADDR_ADDR		0x3004
#define MD32_PM_DATA_ADDR		0x3008

#define MD32_DM_CFG_ADDR		0x300c
#define MD32_DM_ADDR_ADDR		0x3010
#define MD32_DM_DATA_ADDR		0x3014

#define MD32_EN_CFG				0x3018

#define I2C_U2_CLK_DIV			(0xc7)

// A0 A2
#define FLASH_BOB_A0(addr)         ((UINT8)flash_bob[addr])
#define FLASH_BOB_A2(addr)         ((UINT8)flash_bob[addr+256])

#define MAGIC_NUM				0x80
#define FW_VER					0x82
#define MCU_IDLE				0x83
#define IBIAS_CAL				0x84
#define IMOD_CAL				0x86
#define IAV_CAL					0x88
#define APC_CAL					0x8A
#define BOSA_TYPE				0x8B
#define TIA_CUR					0x8C
#define ERC_CDAC				0x8D
#define ERC_DAC					0x8E
#define TIA_GAIN				0x90
#define TIA_BW					0x91
#define PGA_GAIN				0x92
#define PGA_CAP					0x93
#define FAST_INST_CTRL			0x94
#define ORG_APC_DAC				0x95
#define RG_TXOUTP_R				0x98
#define RG_TX_CROSSING			0x99
#define SCALE_INST_IAV			0x9A
#define SCALE_INST_IMOD			0x9B
#define GPL						0x9C
#define RG_TR_BOOST				0x9D
#define RSVD_9E				    0x9E
#define RSVD_9F				    0x9F
#define SLOPE_DN 				0xA0		
#define SLOPE_UP 				0xA1
#define VOLT_NT			        0xA2
#define VOLT_0x00				0xA4
#define VOLT_0x40				0xA6
#define VOLT_0x80				0xA8
#define VOLT_0xC0				0xAA
#define BOSA_TEMP_OFFSET 		0xAC
#define EFUSE_TEMP_OFFSET 		0xAD
#define VAPD					0xAE
#define TSSI_CAL_1				0xB4
#define TXPWR_CAL_1				0xB6
#define TSSI_CAL_2				0xB8
#define TXPWR_CAL_2				0xBA
#define RSSI_CAL_1				0xBC
#define RXPWR_CAL_1				0xBE
#define RSSI_CAL_2				0xC0
#define RXPWR_CAL_2				0xC2
#define RSSI_CAL_3				0xC4
#define RXPWR_CAL_3				0xC6
#define RSSI_OFFSET_CAL			0xC8
#define RSSI_OFFSET				0xCA

#define TEMP_FINAL_OFFSET		0xCC
#define BOSA_TEMP               0xCD
#define APC_LUT_EN              0xCE
#define ERC_LUT_EN              0xCF

#define TX_SD_DAC				0xD4
#define TX_SD_SEL				0xD6
#define ROGUE_ONU_SEL			0xD7

#define ROGUE_ONU_CNT_ADDR		0xD8

#define RX_OCP					0xDC
#define RX_LOS_ALG_SEL		    0xDD
#define RX_LOS_DAC				0xDE
#define RX_SD_DAC				0xDF

#define IAV_SAFETY_LIMIT		0xE0
#define IMOD_SAFETY_LIMIT		0xE1
#define ROGUE_TXF_SEL			0xE2
#define LATCH_CTRL				0xE3

#define IAV_MAX_LIMIT			0xE4
#define IMOD_MAX_LIMIT			0xE5
#define SLOPE_TIME				0xE6
#define APC_RED_LMT				0xE7	// RESERVED_TEMP
#define CUSTOM_FUNC				0xE8

#define TSSI_ADC				0xF0
#define RSSI_ADC				0xF2

#define LA_IN_IMP				0xF4
#define LA_DRV_IMP 				0xF5
#define RSSI_CURRENT			0xF6
#define AGC_DBG					0xF8
#define CHKSUM_ERR				0xF9
#define LOS_STA 				0xFA
#define PATCH_SELECT            0xFB
#define APD_HW_DBG 				0xFC
#define APD_LUT_DBG 			0xFD
#define TXP_DBG					0xFE
#define RXP_DBG					0xFF


// Constant
#define PM_length   (16<<10)	//4byte*4096 = 16K

//DM =DM1+A0/A2 table+DM2  //4byte*(384+128+512) = 4K
#define DM_length   (4<<10)	//4byte*1024 = 4K


#define BOB_length   (512)	//4byte*128 = 512

#define LN10		2.302585093

extern UINT8 flash_pm[PM_length];
extern UINT8 flash_dm[DM_length];
extern UINT8 flash_bob[BOB_length];


// Fucntion
int lddla_debug_init(void) ;

int lddla_debug_deinit(void) ;
int save_bob_table(void);
extern int mt7572_init(void);
int bosa_temp_extcal_task_wait(void);
extern void Write_Vend_data(void);
int reduce_imod_task_wait(void *data);
int adaptive_pav_task_wait(void *data);
#ifdef RXPE
void SetRxPreEmphasis(UINT16 dc_swing, UINT16 boost);
void GetRxPreEmphasis(UINT16* data);
#endif
void clearA2UpperTable(void);
bool AdaptivePon(int mode);
bool SaveTxData2(void);
UINT8 search_Vbr(void);
void auto_Vapd(void);
UINT16 search_erc_dac(void);
void bob_debug(void);
void AutoDcl(void);
void OpenLoop_IbiasImod(UINT16 ibias, UINT16 imod);
void autoBosaTempCal(UINT8 bosaTemp);
void GponPowerLevelling(UINT8 PwrLvl);
void dumpEfuse(UINT32 key1, UINT32 key2);
void search_LOS(void);
void TRX_cal_info(void);
void bosa_info(void);
void bob_info(void);
void showA0A2Table(UINT8 table);
void showApcErcLut(void);
void showCalData(void);
extern void write_A_W_thld(void);
extern UINT16 lddla_I2C_read(UINT8 u1CHannelID, UINT16 u2ClkDiv, UINT8 u1DevAddr, UINT8 u1WordAddrNum,
                         UINT32 u4WordAddr, UINT8 *pu1Buf, UINT16 u2ByteCnt);
extern UINT32 BitRead(UINT32 addr, UINT32 bitStart, UINT32 bitEnd);
extern UINT8 ByteReadA2(UINT32 addr);
extern UINT16 WordReadA2(UINT32 addr);
extern void BitWrite(UINT32 addr, UINT32 bitStart, UINT32 bitEnd, UINT32 value);
extern void ByteWriteA2(UINT32 addr, UINT8 value);
extern void WordWriteA2(UINT32 addr, UINT16 value);

extern void ddmi_rx_done(void);
extern void ddmi_rx(uint input_RxPwr, uint input_addr);
extern void ddmi_tx(uint input_TxPwr);
#endif /* _EN7571_REG_H_ */

