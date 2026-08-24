#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/proc_fs.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/kthread.h>	// Jeff
#include <linux/workqueue.h>  // Iven

#include "i2c.h"
#include "en7572.h"
#include "lddla_types.h"


//extern PHY_GlbPriv_T *gpPhyPriv;
PHY_GlbPriv_T *gpPhyPriv = NULL;	// Jeff


uint  PM_load_success;
uint  DM_load_success;
uint  BOB_load_success;

UINT8 flash_pm[PM_length];
UINT8 flash_dm[DM_length];
UINT8 flash_bob[BOB_length];

//Iven 
static struct adaptive_pon_mode_data{
	  struct work_struct work;
	  int mode;
}struct_apapon;

void adaptive_pon_mode_handler(struct work_struct *work)
{
	struct adaptive_pon_mode_data *data = container_of(work, struct adaptive_pon_mode_data, work);
	AdaptivePon(data->mode);
	printk("AdaptivePon Done\n");
}
/*****************************************************************************
//function :
//		Write_data_MD32
//description : 
//		this function is used to write PM & Dm & BOB to MD32
//input :	
//		N/A
//output :
//		0 : success ; -1 : fail
//		
******************************************************************************/
void Write_data_MD32(void)
{
	uint i;
    UINT8 ptr[4];


	if(PM_load_success ==1)
	{
		BitWrite(MD32_PM_CFG_ADDR, 0, 0, 1);
		BitWrite(MD32_PM_ADDR_ADDR, 0, 31, 0);
        
		for(i=0;i<(PM_length>>2);i++)
		{
			memcpy(ptr, &flash_pm[i<<2], 4); 
			lddla_I2C_write(0, I2C_U2_CLK_DIV, MD32_DEVICE_ADDR, 2, MD32_PM_DATA_ADDR, ptr, 4); //write data to PM
		}
		printk("\r\nMD32 PM load data done! \r\n");

	}
	else
	{
		printk("\r\n NO MD32 PM DATA! \r\n");
	}
	
		
	if(DM_load_success ==1)
	{
		BitWrite(MD32_DM_CFG_ADDR, 0, 0, 1);
		BitWrite(MD32_DM_ADDR_ADDR, 0, 31, 0);
        
		for(i=0;i<(DM_length>>2);i++)
		{
			memcpy(ptr, &flash_dm[i<<2], 4); 				
			lddla_I2C_write(0, I2C_U2_CLK_DIV, MD32_DEVICE_ADDR, 2, MD32_DM_DATA_ADDR, ptr, 4);//write data to DM			
		}
		
		printk("\r\nMD32 DM load data done! \r\n");

	}
	else
	{
		printk("\r\n NO MD32 DM DATA! \r\n");
	}	

	if(BOB_load_success ==1)
	{
		//write vendorID & vendor PN==============================
		Write_Vend_data();
		
        BitWrite(MD32_DM_CFG_ADDR, 0, 0, 1);
		
		BitWrite(MD32_DM_ADDR_ADDR, 0, 31, 0x600);
        
		for(i=0;i<(BOB_length>>2);i++)
		{
			memcpy(ptr, &flash_bob[i<<2], 4); 			
			lddla_I2C_write(0, I2C_U2_CLK_DIV, MD32_DEVICE_ADDR, 2, MD32_DM_DATA_ADDR, ptr, 4);//write data to DM			
		}
		
		printk("\r\nMD32 BOB load data done! \r\n");

	}
	else
	{
		printk("\r\n NO  BOB DATA! \r\n");
	}
	
}

/*****************************************************************************
//function :
//		Write_Vend_data
//description : 
//		this function is used to write vendor ID and pn to MD32
//input :	
//		N/A
//output :
//		0 : success ; -1 : fail
//		
******************************************************************************/
void Write_Vend_data(void)
{
    UINT32 value;

// ECONET
    value = 0x4e4f4345;
    memcpy(&flash_bob[20], &value, 4);

    value = 0x20205445;
    memcpy(&flash_bob[24], &value, 4);
    
    value = 0x20202020;
    memcpy(&flash_bob[28], &value, 4);
    memcpy(&flash_bob[32], &value, 4);

//EN7572
    value = 0x35374e45;
    memcpy(&flash_bob[40], &value, 4);

    value = 0x20203237;
    memcpy(&flash_bob[44], &value, 4);

    value = 0x20202020;
    memcpy(&flash_bob[48], &value, 4);
    memcpy(&flash_bob[52], &value, 4);
/*    
	// ECONET
	flash_bob[5]=0x4e4f4345;  
	flash_bob[6]=0x20205445; 
	flash_bob[7]=0x20202020;
	flash_bob[8]=0x20202020;

	//EN7572
	flash_bob[10]=0x35374e45;
	flash_bob[11]=0x20203237;
	flash_bob[12]=0x20202020;
	flash_bob[13]=0x20202020;
*/	
}

/*****************************************************************************
//function :
//		Read_Data_From_Flash
//description : 
//		this function is used to read the Data from 7572_bob.conf
													A60993.elf.pm
													A60993.elf.dm
		to flash_matrix
//input :	
//		N/A
//output :
//		0 : success ; -1 : fail
//		
******************************************************************************/
int Read_Data_From_Flash(void)
{

	struct file 			*srcf = NULL;
	char *src = NULL;

	#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
	mm_segment_t			orgfs;
	#endif

	//====Read PM data=====================================//

	src = "/etc/lddla/A60993.elf.pm";

	#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
	orgfs = get_fs();
	set_fs(KERNEL_DS);
	#endif

	if (src && *src)
	{
		srcf = filp_open(src, O_RDONLY, 0);
		if (IS_ERR(srcf))
		{
			printk("-->PM Error opening \n");

			PM_load_success =0;
		}
		else
		{
			PM_load_success =1;
			
			LDDLA_KERNEL_FS_READ(srcf, flash_pm, sizeof(flash_pm), &srcf->f_pos);

			filp_close(srcf,NULL);

			#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
			set_fs(orgfs);
			#endif
		}
	}

	//====Read DM data=====================================//

	src = "/etc/lddla/A60993.elf.dm";

	#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
	orgfs = get_fs();
	set_fs(KERNEL_DS);
	#endif
	
	if (src && *src)
	{
		srcf = filp_open(src, O_RDONLY, 0);
		if (IS_ERR(srcf))
		{
			printk("-->DM Error opening \n");

			DM_load_success =0;
		}
		else
		{

			DM_load_success =1;

			LDDLA_KERNEL_FS_READ(srcf, flash_dm, sizeof(flash_dm), &srcf->f_pos);

			filp_close(srcf,NULL);


			#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
			set_fs(orgfs);
			#endif
		}
	}


	if ((PM_load_success == 0)&&(DM_load_success == 0))
	{
		printk("-->ERROR !PM&DM NO DATA! \n");

		goto error;
	}			

	//====Read BOB data=====================================//
	BOB_load_success =0;
	src = "/etc/lddla/en7572_bob.conf";
	#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
	orgfs = get_fs();
	set_fs(KERNEL_DS);
	#endif

	if (src && *src)
	{
		srcf = filp_open(src, O_RDONLY, 0);
		if (IS_ERR(srcf))
		{
			printk("-->bob Error opening \n");

			BOB_load_success =0;
		}
		else
		{
            printk("Load /etc/lddla/en7572_bob.conf to flash_bob[]\n");
			BOB_load_success =1;

			LDDLA_KERNEL_FS_READ(srcf, flash_bob, sizeof(flash_bob), &srcf->f_pos);

			filp_close(srcf,NULL);

			#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
			set_fs(orgfs);
			#endif
		}
	}


	if (BOB_load_success == 0)
	{
		printk("-->ERROR !BOB NO DATA! \n");

		goto error;
	}		

	return 0;

	error:
	#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
	set_fs(orgfs);	
	#endif
	return -1;		
	}

void ClearWarningAlarmFlag(void)
{
	WordWriteA2(112, 0x0000);	// Alarm
	WordWriteA2(116, 0x0000);	// Warning
}

bool isTempThld_0xFF(void)
{
    Byte values[4] = {0x00};
    UINT32 temp = 0;
    
    lddla_I2C_read(0, I2C_U2_CLK_DIV, 0x51, 2, 0x00, values, 4);

    memcpy(&temp, values, 4);
    
    printk("%s:%d\t0x%08X\n", __FUNCTION__, __LINE__, temp);
    
    if( temp == DWORD_MASK)
        return true;
    else
        return false;
}

void write_A_W_thld(void)
{
	Byte values[] = {
        0x64, 0x00, 0xce, 0x00, 0x64, 0x00, 0xce, 0x00,
        0x90, 0x88, 0x71, 0x48, 0x8e, 0x94, 0x73, 0x3c,
        0xa6, 0x05, 0x01, 0xf4, 0x9c, 0x40, 0x02, 0xee,
        0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00,
        0x31, 0x24, 0x00, 0x01, 0x27, 0x10, 0x00, 0x03
    };

	lddla_I2C_write(0, I2C_U2_CLK_DIV, 0x51, 2, 0x00, values, 40);

    printk("%s:%d\n", __FUNCTION__, __LINE__);  
}

/*********************** LDDLA APIs ******************************/
static void lddla_get_api_dispatch(lddla_api_data_t * api_data)
{
    api_data->ret = LDDLA_SUCCESS;


	switch(api_data->cmd_id)
    {			
        case LDDLA_GET_PHY_TRANS_STATUS:            
            LDDLA_PRINT("[%s: %d]\n", __FUNCTION__, __LINE__);         
            break;        

        case LDDLA_GET_PHY_EN7571_VER:                
            LDDLA_PRINT("[%s: %d]\n", __FUNCTION__, __LINE__);          
            break;            
                
        default:
            dump_stack();
            printk("unknown command id!\n");
            api_data->ret = LDDLA_NO_API;
            break;
    }
}


static void lddla_set_api_dispatch(lddla_api_data_t * api_data)
{
    api_data->ret = LDDLA_SUCCESS;


    switch(api_data->cmd_id)
    {             
		case LDDLA_SET_7572_TX_MODE:
			//by Iven 20250714
			printk("LDDLA_TX_MODE is triggered by MAC !!!\n");
			
			struct_apapon.mode = *(int *)(api_data->data);
			if (!work_pending(&struct_apapon.work))
				schedule_work(&struct_apapon.work);
			break;

        case LDDLA_SET_PHY_TRAFFIC_STATUS:    
            LDDLA_PRINT("[%s: %d]\n", __FUNCTION__, __LINE__);
            break;

        case LDDLA_SET_TRANS_SWITCH_RESET:    
            LDDLA_PRINT("[%s: %d]\n", __FUNCTION__, __LINE__);
            break;
            
        default:
            printk("unknown command id!\n");
            api_data->ret = LDDLA_NO_API;
            break;
    }
}


int lddla_api_dispatch(struct ecnt_data *in_data)
{
    lddla_api_data_t * api_data = (lddla_api_data_t *)in_data;

    switch(api_data->api_type) 
    {
        case LDDLA_API_TYPE_GET:
            lddla_get_api_dispatch(api_data);
            break;

        case LDDLA_API_TYPE_SET:
            lddla_set_api_dispatch(api_data);
            break;

        default:
            dump_stack();
            printk("unknown api_data->api_type: %d\n", api_data->api_type);
            api_data->ret = LDDLA_NO_API;
            break;
    }
    
    return ECNT_CONTINUE;
}


struct ecnt_hook_ops lddla_api_dispatch_hook_ops = {
    .name = "lddla_api_dispatch",
    .hookfn = lddla_api_dispatch,
    .is_execute = 1,
    .maintype = ECNT_LDDLA,
    .subtype = ECNT_LDDLA_API,
    .priority   = 2,
};
/****************************** LDDLA APIs End ********************************/


bool is_airoha_en7572(void)
{
	UINT16 tmp1, tmp2;

    printk("%s:%d\n", __FUNCTION__, __LINE__);
    
	tmp1 = WordReadA2(0x408);   // 0x1388
	tmp2 = WordReadA2(0x40A);   // 0x007D
	printk("0x408 = 0x%04x\n0x40A = 0x%04x\n\n", tmp1, tmp2);
	
	if((tmp1 != 0x1388))//identify 757x or other LDDLA
	{
		return false;
	}
	else
	{
		printk("EN7572A\n");
		return true;	
	}

}

void CopyTxDDMI(void)
{
    UINT32 tssi_cal_1;
    
    memcpy(&tssi_cal_1, &flash_bob[TSSI_CAL_1], 4);
 
    if(tssi_cal_1==DWORD_MASK)    // Missing 2nd TX DDMI
    {
        memcpy(&flash_bob[TSSI_CAL_1], &flash_bob[TSSI_CAL_1+256], 4);
        printk("%s:%d\n", __FUNCTION__, __LINE__);
    }
}

int mt7572_init(void)
{
	int ret=0;



// AIROHA LDDLA detection
	if( !is_airoha_en7572() ){return -1;}

	printk("\r\n MD32 INIT \r\n");

    INIT_WORK(&struct_apapon.work, adaptive_pon_mode_handler); //workqueue INIT
	printk("\r\n adaptive pon INIT_WORK \r\n");
	
    BitWrite(MD32_EN_CFG, 0, 0, 0);
	BitWrite(0x160, 30, 30, 0);	// Disable OCP
	BitWrite(0x15C, 8, 8, 0);		// Disable APD
	msleep(100);
	
	printk("\r\n EN7572 RESET \r\n");
	BitWrite(0x200, 30, 31, 0);
	BitWrite(0x200, 30, 31, 3);
	
	if(Read_Data_From_Flash() == 0)		//Read Datadata from file	
		printk("Read Data from File OK\n");										
	else											
		printk("Read Data from File ERROR!!!\n");

	Write_data_MD32(); //Write PM & DM & BOB toMD32

    if(isTempThld_0xFF()){write_A_W_thld();}

	ClearWarningAlarmFlag();

    CopyTxDDMI();   // For the 2nd TX DDMI
    
	BitWrite(MD32_EN_CFG, 0, 0, 1);
	if((ret = lddla_debug_init()) != 0) {
			printk( "lddla debug init failed.\n") ;
			return ret ;
		}
		
	
/*************************************************************************
// task_wait(timer function) start for:
// bosa_temp_extcal_task_wait, 	added by Jeff 20210225
// reduce_IavImod_task_wait,		added by Jeff 20211003
// second_cal_switch_task_wait,	added by Jeff 20211019
*************************************************************************/
	if (NULL == gpPhyPriv)
	{
		gpPhyPriv = (PHY_GlbPriv_T *)kmalloc(sizeof(PHY_GlbPriv_T), GFP_KERNEL) ;
		if( NULL == gpPhyPriv) {
			printk("Alloc data struct memory failed\n") ;

		}
	}
	memset(gpPhyPriv, 0, sizeof(PHY_GlbPriv_T)) ;
	if(BitRead(0xE8, 1, 1) == 0)	// default is disable=1, enable=0, disable=1
		gpPhyPriv->reduce_pav_task_wait= kthread_run(adaptive_pav_task_wait, NULL, "adaptive_pav_task_wait");	// start the task
	else if (BitRead(0xE8, 0, 0) == 0)
		gpPhyPriv->reduce_pav_task_wait= kthread_run(reduce_imod_task_wait, NULL, "reduce_imod_task_wait");	// start the task


	printk("\r\nMD32 INIT DONE!!! \r\n");
	
    // Register hook
    if (ECNT_REGISTER_SUCCESS != ecnt_register_hook(&lddla_api_dispatch_hook_ops) )
    {
       printk("Register hook function failed! %s:%d", __FUNCTION__, __LINE__);
    }

	return ret ;	
}



/*****************************************************************************
//function :
//		mt7572_deinit
//description : 
//		this function is used to deinit EN7572
//input :	
//		N/A
//output :
//		0 : success ; -1 : fail
//		
******************************************************************************/
void mt7572_deinit(void)
{
	
	lddla_debug_deinit();
    ecnt_unregister_hook(&lddla_api_dispatch_hook_ops);
    printk("%s:%d\n", __FUNCTION__, __LINE__);
}

module_init(mt7572_init)
module_exit(mt7572_deinit)
MODULE_LICENSE("Proprietary");

