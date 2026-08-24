
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/proc_fs.h>
#include <linux/module.h>
#include <linux/delay.h>


#include "i2c.h"
#include "lddla_types.h"





extern UINT16 SIF_X_Read(UINT8 u1CHannelID, UINT16 u2ClkDiv, UINT8 u1DevAddr, UINT8 u1WordAddrNum,
                         UINT32 u4WordAddr, UINT8 *pu1Buf, UINT16 u2ByteCnt);
extern UINT16 SIF_X_Write(UINT8 u1CHannelID, UINT16 u2ClkDiv, UINT8 u1DevAddr, UINT8 u1WordAddrNum,
                          UINT32 u4WordAddr, UINT8 *pu1Buf, UINT16 u2ByteCnt);



UINT16 lddla_I2C_read(UINT8 u1CHannelID, UINT16 u2ClkDiv, UINT8 u1DevAddr, UINT8 u1WordAddrNum,
                         UINT32 u4WordAddr, UINT8 *pu1Buf, UINT16 u2ByteCnt)
{
	uint ret;

	ret = SIF_X_Read(u1CHannelID, u2ClkDiv, u1DevAddr, 
					  u1WordAddrNum, u4WordAddr, pu1Buf, 
					  u2ByteCnt);
	if(ret == 0) {
 		printk("\nError! SIF_X_Read return value = 0, u1DevAddr:0x%x, u4WordAddr:%d\n",
			u1DevAddr, u4WordAddr);
		//dump_stack();
	}

	
	return ret;
}

UINT16 lddla_I2C_write(UINT8 u1CHannelID, UINT16 u2ClkDiv, UINT8 u1DevAddr, UINT8 u1WordAddrNum,
                          UINT32 u4WordAddr, UINT8 *pu1Buf, UINT16 u2ByteCnt)
{
	uint ret;

	ret = SIF_X_Write(u1CHannelID, u2ClkDiv, u1DevAddr, 
					   u1WordAddrNum, u4WordAddr, pu1Buf, 
					   u2ByteCnt);

	if(ret == 0) {
		printk("\nError! SIF_X_Write return value = 0, u1DevAddr:0x%x, u4WordAddr:%d, pu1Buf:0x%x\n",
			u1DevAddr, u4WordAddr, *(uint *)pu1Buf);
	}
	
	return ret;
}

