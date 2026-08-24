/*****************************************************************************
 * Airoha (HK) Limited  Airoha. ALL RIGHTS RESERVED.
 * 
 * BY OPENING OR USING THIS FILE, RECEIVER HEREBY UNEQUIVOCALLY 
 * ACKNOWLEDGES AND AGREES THAT THE SOFTWARE/FIRMWARE AND ITS 
 * DOCUMENTATIONS (��Airoha SOFTWARE��) RECEIVED FROM Airoha 
 * AND/OR ITS REPRESENTATIVES ARE PROVIDED TO RECEIVER ON AN ��AS IS�� 
 * BASIS ONLY. Airoha EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES, 
 * WHETHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED 
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, 
 * OR NON-INFRINGEMENT. NOR DOES Airoha PROVIDE ANY WARRANTY 
 * WHATSOEVER WITH RESPECT TO THE SOFTWARE OF ANY THIRD PARTIES WHICH 
 * MAY BE USED BY, INCORPORATED IN, OR SUPPLIED WITH THE Airoha SOFTWARE. 
 * RECEIVER AGREES TO LOOK ONLY TO SUCH THIRD PARTIES FOR ANY AND ALL 
 * WARRANTY CLAIMS RELATING THERETO. RECEIVER EXPRESSLY ACKNOWLEDGES 
 * THAT IT IS RECEIVER��S SOLE RESPONSIBILITY TO OBTAIN FROM ANY THIRD 
 * PARTY ALL PROPER LICENSES CONTAINED IN Airoha SOFTWARE.
 * 
 * Airoha SHALL NOT BE RESPONSIBLE FOR ANY Airoha SOFTWARE RELEASES 
 * MADE TO RECEIVER��S SPECIFICATION OR CONFORMING TO A PARTICULAR 
 * STANDARD OR OPEN FORUM. RECEIVER'S SOLE AND EXCLUSIVE REMEDY AND 
 * Airoha'S ENTIRE AND CUMULATIVE LIABILITY WITH RESPECT TO THE Airoha 
 * SOFTWARE RELEASED HEREUNDER SHALL BE, AT Airoha'S SOLE OPTION, TO 
 * REVISE OR REPLACE THE Airoha SOFTWARE AT ISSUE OR REFUND ANY SOFTWARE 
 * LICENSE FEES OR SERVICE CHARGES PAID BY RECEIVER TO Airoha FOR SUCH 
 * Airoha SOFTWARE.
 *
 *****************************************************************************/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>

#if defined(TCSUPPORT_MT7570)
#define BOB_INFO_FILE_NAME_PATH "/tmp/7570_bob.conf"
#elif defined(TCSUPPORT_MT7572)
#define BOB_INFO_FILE_NAME_PATH "/etc/lddla/en7572_bob.conf"
#endif
#define ECNT_MTD_CMD_PATH 			"/sbin/ecnt_sys"
#define OP_READ						"get"
#define OP_BOB_FLAG					"bob"
#define OP_DEL						"del"

#define CHMOD_CMD_PATH			  	"/bin/chmod"
#define BOB_INFO_FILE_ATTRIBUTE		"644"

#define RM_CMD_PATH					"/bin/rm"
#define RM_CMD_F					"-f"
static char *envp[] = {"HOME=/", "PATH=/bin:/sbin:/usr/bin:/usr/sbin", NULL}; 
char *dualbob  = "nodual";

static int get_bob_info_from_flash(void)
{
	int ret = 0;
	char* argv[] = {ECNT_MTD_CMD_PATH, OP_READ, OP_BOB_FLAG, dualbob, NULL};
	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
	if(0 != ret)
	{
		printk("error invoking bob_info %s %s %s %s: ret = %d.\n", argv[0], argv[1], argv[2],argv[3], ret);
	}
	else
	{
		printk("get bob info from flash,dualbob=%s\n",dualbob);
	}

	return ret;
}

static int chmod_bob_info_file_attribute(void)
{
	int ret = 0;
	char* argv[] = {CHMOD_CMD_PATH, BOB_INFO_FILE_ATTRIBUTE, BOB_INFO_FILE_NAME_PATH, NULL};
	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
	if(0 != ret)
	{
		printk("error invoking chmod bob_info attribute %s %s %s: ret = %d.\n", argv[0], argv[1], argv[2], ret);
	}
	else
	{
		printk("chmod bob_info attribute.\n");
	}

	return ret;
}

static int remove_bob_info(void)
{
	int ret = 0;
	char* argv[] = {ECNT_MTD_CMD_PATH, OP_DEL, OP_BOB_FLAG, dualbob, NULL};
	
	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
	if(0 != ret)
	{
		printk("error invoking remove bob_info %s %s %s: ret = %d.\n", argv[0], argv[1], argv[2], ret);
	}
	else
	{
		printk("remove bob_info,dualbob=%s.\n",dualbob);
	}
	
	return ret;
}
static int __init ready_bob_info(void)
{
	int ret = 0;	
	ret = get_bob_info_from_flash();
	if(0 != ret)
	{
		printk("get bob info from flash faile \n");
		return ret;
	}
/*move to ecnt_sy*/
/*	ret = chmod_bob_info_file_attribute();
	if(0 != ret)
	{
		printk("chmod bob_info attribute faile.\n");
		goto exit;
	}
	*/
	return 0;
	
exit:
	remove_bob_info();
	return ret;
	
}

static void __exit exit_bob_info(void)
{
	remove_bob_info();
}

module_init(ready_bob_info);
module_exit(exit_bob_info);
module_param(dualbob, charp, S_IRUGO);
MODULE_LICENSE("Proprietary");
MODULE_AUTHOR("junwei.ren <renxufeng2003@163.com>");


