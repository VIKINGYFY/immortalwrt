/*
* File Name: phy.c
* Description: .
*
******************************************************************/
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/proc_fs.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/kthread.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(6,6,0)
#include <linux/seq_file.h>
#endif
#include <ecnt_hook/ecnt_hook_lddla.h>

#include "i2c.h"
#include "en7572.h"
#include "lddla_types.h"


#define DATE_CODE "2025-07-17"	// bob ver control with date


bool db_msg = false;

static const UINT16 MPD_LUT[156][2] = {
	{100, 63  }, {101, 64  }, {102, 65  }, {103, 67  }, {104, 68  }, {105, 70  }, {106, 71  }, {107, 73  },
	{108, 75  }, {109, 76  }, {110, 78  }, {111, 80  }, {112, 81  }, {113, 83  }, {114, 85  }, {115, 87  },
	{116, 89  }, {117, 91  }, {118, 93  }, {119, 95  }, {120, 97  }, {121, 99  }, {122, 101 }, {123, 103 },
	{124, 105 }, {125, 108 }, {126, 110 }, {127, 112 }, {128, 115 }, {129, 117 }, {130, 120 }, {131, 123 },
	{132, 125 }, {133, 128 }, {134, 131 }, {135, 134 }, {136, 137 }, {137, 140 }, {138, 143 }, {139, 146 },
	{140, 149 }, {141, 152 }, {142, 156 }, {143, 159 }, {144, 163 }, {145, 166 }, {146, 170 }, {147, 173 },
	{148, 177 }, {149, 181 }, {150, 185 }, {151, 189 }, {152, 193 }, {153, 198 }, {154, 202 }, {155, 206 },
	{156, 211 }, {157, 215 }, {158, 220 }, {159, 225 }, {160, 230 }, {161, 235 }, {162, 240 }, {163, 245 },
	{164, 251 }, {165, 256 }, {166, 262 }, {167, 268 }, {168, 273 }, {169, 279 }, {170, 286 }, {171, 292 },
	{172, 298 }, {173, 305 }, {174, 311 }, {175, 318 }, {176, 325 }, {177, 332 }, {178, 340 }, {179, 347 },
	{180, 355 }, {181, 362 }, {182, 370 }, {183, 378 }, {184, 387 }, {185, 395 }, {186, 404 }, {187, 413 },
	{188, 422 }, {189, 431 }, {190, 440 }, {191, 450 }, {192, 460 }, {193, 470 }, {194, 480 }, {195, 491 },
	{196, 501 }, {197, 512 }, {198, 524 }, {199, 535 }, {200, 547 }, {201, 559 }, {202, 571 }, {203, 584 },
	{204, 596 }, {205, 609 }, {206, 623 }, {207, 636 }, {208, 650 }, {209, 665 }, {210, 679 }, {211, 694 },
	{212, 709 }, {213, 725 }, {214, 741 }, {215, 757 }, {216, 773 }, {217, 790 }, {218, 808 }, {219, 825 },
	{220, 843 }, {221, 862 }, {222, 881 }, {223, 900 }, {224, 920 }, {225, 940 }, {226, 960 }, {227, 981 },
	{228, 1003}, {229, 1025}, {230, 1047}, {231, 1070}, {232, 1094}, {233, 1118}, {234, 1142}, {235, 1167},
	{236, 1193}, {237, 1219}, {238, 1245}, {239, 1273}, {240, 1301}, {241, 1329}, {242, 1358}, {243, 1388},
	{244, 1418}, {245, 1449}, {246, 1481}, {247, 1514}, {248, 1547}, {249, 1581}, {250, 1615}, {251, 1651},
	{252, 1687}, {253, 1724}, {254, 1761}, {255, 1800}
};

#ifdef RXPE
static const UINT16 RX_PE_LUT[63][7] = {
	{400, 0, 20, 0, 20, 0, 1},
	{400, 1, 21, 2, 22, 0, 0},
	{400, 2, 23, 5, 25, 0, 0},
	{400, 3, 24, 8, 28, 0, 0},
	{400, 4, 26, 12, 48, 0, 0},
	{400, 5, 28, 24, 52, 0, 0},
	{400, 6, 30, 28, 56, 0, 0},
	{450, 0, 22, 0, 22, 0, 1},
	{450, 1, 24, 3, 25, 0, 0},
	{450, 2, 25, 6, 28, 0, 0},
	{450, 3, 27, 9, 48, 0, 0},
	{450, 4, 29, 13, 52, 0, 0},
	{450, 5, 31, 26, 56, 0, 0},
	{450, 6, 50, 30, 61, 1, 0},
	{500, 0, 25, 0, 25, 0, 1},
	{500, 1, 26, 3, 28, 0, 0},
	{500, 2, 28, 6, 31, 0, 0},
	{500, 3, 30, 10, 51, 0, 0},
	{500, 4, 48, 15, 56, 1, 0},
	{500, 5, 51, 27, 60, 1, 0},
	{500, 6, 51, 27, 60, 1, 0},
	{550, 0, 28, 0, 28, 0, 1},
	{550, 1, 29, 3, 31, 0, 0},
	{550, 2, 31, 7, 51, 0, 0},
	{550, 3, 49, 11, 55, 1, 0},
	{550, 4, 52, 24, 60, 1, 0},
	{550, 5, 52, 24, 60, 1, 0},
	{550, 6, 52, 24, 60, 1, 0},
	{600, 0, 30, 0, 30, 0, 1},
	{600, 1, 48, 4, 50, 1, 0},
	{600, 2, 50, 8, 54, 1, 0},
	{600, 3, 52, 12, 58, 1, 0},
	{600, 4, 52, 12, 58, 1, 0},
	{600, 5, 52, 12, 58, 1, 0},
	{600, 6, 52, 12, 58, 1, 0},
	{650, 0, 48, 0, 48, 1, 1},
	{650, 1, 50, 4, 52, 1, 0},
	{650, 2, 53, 8, 57, 1, 0},
	{650, 3, 55, 13, 62, 1, 0},
	{650, 4, 55, 13, 62, 1, 0},
	{650, 5, 55, 13, 62, 1, 0},
	{650, 6, 55, 13, 62, 1, 0},
	{700, 0, 51, 0, 51, 1, 1},
	{700, 1, 53, 4, 55, 1, 0},
	{700, 2, 56, 9, 60, 1, 0},
	{700, 3, 56, 9, 60, 1, 0},
	{700, 4, 56, 9, 60, 1, 0},
	{700, 5, 56, 9, 60, 1, 0},
	{700, 6, 56, 9, 60, 1, 0},
	{750, 0, 54, 0, 54, 1, 1},
	{750, 1, 56, 5, 58, 1, 0},
	{750, 2, 58, 10, 63, 1, 0},
	{750, 3, 58, 10, 63, 1, 0},
	{750, 4, 58, 10, 63, 1, 0},
	{750, 5, 58, 10, 63, 1, 0},
	{750, 6, 58, 10, 63, 1, 0},
	{800, 0, 56, 0, 56, 1, 1},
	{800, 1, 58, 5, 61, 1, 0},
	{800, 2, 58, 5, 61, 1, 0},
	{800, 3, 58, 5, 61, 1, 0},
	{800, 4, 58, 5, 61, 1, 0},
	{800, 5, 58, 5, 61, 1, 0},
	{800, 6, 58, 5, 61, 1, 0}
};
#endif

UINT8 ByteReadA0(UINT32 addr)
{
	UINT8 ptr[1]={0};
	lddla_I2C_read(0, I2C_U2_CLK_DIV, 0x50, 2, addr, ptr, 1);
	return ptr[0];
}
UINT8 ByteReadA2(UINT32 addr)
{
	UINT8 ptr[1]={0};
	lddla_I2C_read(0, I2C_U2_CLK_DIV, 0x51, 2, addr, ptr, 1);
	return ptr[0];
}

UINT16 WordReadA0(UINT32 addr)
{
	UINT8 ptr[2]={0};
	lddla_I2C_read(0, I2C_U2_CLK_DIV, 0x50, 2, addr, ptr, 2);
	return (ptr[1]<<8)|ptr[0];
}

UINT16 WordReadA2(UINT32 addr)
{
	UINT8 ptr[2]={0};
	lddla_I2C_read(0, I2C_U2_CLK_DIV, 0x51, 2, addr, ptr, 2);
	return (ptr[1]<<8)|ptr[0];
}

void ByteWriteA0(UINT32 addr, UINT8 value)
{
	UINT8 ptr[1];
	ptr[0] = value;
	lddla_I2C_write(0, I2C_U2_CLK_DIV, 0x50, 2, addr, ptr, 1);
}

void WordWriteA0(UINT32 addr, UINT16 value)
{
	UINT8 ptr[2];
	ptr[0] = (UINT8)(value&0xFF);
	ptr[1] = (UINT8)(value>>8);
	lddla_I2C_write(0, I2C_U2_CLK_DIV, 0x50, 2, addr, ptr, 2);
}

void ByteWriteA2(UINT32 addr, UINT8 value)
{
	UINT8 ptr[1];
	ptr[0] = value;
	lddla_I2C_write(0, I2C_U2_CLK_DIV, 0x51, 2, addr, ptr, 1);
}

void WordWriteA2(UINT32 addr, UINT16 value)
{
	UINT8 ptr[2];
	ptr[0] = (UINT8)(value&0xFF);
	ptr[1] = (UINT8)(value>>8);
	lddla_I2C_write(0, I2C_U2_CLK_DIV, 0x51, 2, addr, ptr, 2);
}

UINT32 BitRead(UINT32 addr, UINT32 bitStart, UINT32 bitEnd)
{
	UINT8 ptr[4]={0};
	UINT32 j, readValue, valueMask = 0;
		
	if(bitEnd<bitStart)
	{
		bitEnd=bitEnd^bitStart;
		bitStart=bitEnd^bitStart;
		bitEnd=bitEnd^bitStart;
	}
	
	for(j=bitStart;j<=bitEnd;j++)
		valueMask+=(1<<j);

	lddla_I2C_read(0, I2C_U2_CLK_DIV, 0x51, 2, addr, ptr, 4);
	readValue = (ptr[3]<<24) | (ptr[2]<<16) | (ptr[1]<<8) | ptr[0];

	return ((readValue&valueMask)>>(int)bitStart);	
}

void BitWrite(UINT32 addr, UINT32 bitStart, UINT32 bitEnd, UINT32 value)
{
	UINT8 ptr[4];
	UINT32 j, valueMaskInv, readValue, finalValue, valueMask = 0;
	
	if(bitEnd<bitStart)
	{
		bitEnd=bitEnd^bitStart;
		bitStart=bitEnd^bitStart;
		bitEnd=bitEnd^bitStart;
	}

	for(j=bitStart;j<=bitEnd;j++)
		valueMask+=(1<<j);
	
	value=(value<<(int)bitStart)&valueMask;

	lddla_I2C_read(0, I2C_U2_CLK_DIV, 0x51, 2, addr, ptr, 4);
	readValue = (ptr[3]<<24) | (ptr[2]<<16) | (ptr[1]<<8) | ptr[0];

	valueMaskInv=valueMask^0xFFFFFFFF;
	finalValue=(readValue&valueMaskInv)|value;

	ptr[3] = (UINT8)(finalValue>>24);
	ptr[2] = (UINT8)(finalValue>>16);
	ptr[1] = (UINT8)(finalValue>>8);
	ptr[0] = (UINT8)finalValue;	
	lddla_I2C_write(0, I2C_U2_CLK_DIV, 0x51, 2, addr, ptr, 4);	
}

static const unsigned short logtable[256] = {
	0x0000, 0x0171, 0x02e0, 0x044e, 0x05ba, 0x0725, 0x088e, 0x09f7,
	0x0b5d, 0x0cc3, 0x0e27, 0x0f8a, 0x10eb, 0x124b, 0x13aa, 0x1508,
	0x1664, 0x17bf, 0x1919, 0x1a71, 0x1bc8, 0x1d1e, 0x1e73, 0x1fc6,
	0x2119, 0x226a, 0x23ba, 0x2508, 0x2656, 0x27a2, 0x28ed, 0x2a37,
	0x2b80, 0x2cc8, 0x2e0f, 0x2f54, 0x3098, 0x31dc, 0x331e, 0x345f,
	0x359f, 0x36de, 0x381b, 0x3958, 0x3a94, 0x3bce, 0x3d08, 0x3e41,
	0x3f78, 0x40af, 0x41e4, 0x4319, 0x444c, 0x457f, 0x46b0, 0x47e1,
	0x4910, 0x4a3f, 0x4b6c, 0x4c99, 0x4dc5, 0x4eef, 0x5019, 0x5142,
	0x526a, 0x5391, 0x54b7, 0x55dc, 0x5700, 0x5824, 0x5946, 0x5a68,
	0x5b89, 0x5ca8, 0x5dc7, 0x5ee5, 0x6003, 0x611f, 0x623a, 0x6355,
	0x646f, 0x6588, 0x66a0, 0x67b7, 0x68ce, 0x69e4, 0x6af8, 0x6c0c,
	0x6d20, 0x6e32, 0x6f44, 0x7055, 0x7165, 0x7274, 0x7383, 0x7490,
	0x759d, 0x76aa, 0x77b5, 0x78c0, 0x79ca, 0x7ad3, 0x7bdb, 0x7ce3,
	0x7dea, 0x7ef0, 0x7ff6, 0x80fb, 0x81ff, 0x8302, 0x8405, 0x8507,
	0x8608, 0x8709, 0x8809, 0x8908, 0x8a06, 0x8b04, 0x8c01, 0x8cfe,
	0x8dfa, 0x8ef5, 0x8fef, 0x90e9, 0x91e2, 0x92db, 0x93d2, 0x94ca,
	0x95c0, 0x96b6, 0x97ab, 0x98a0, 0x9994, 0x9a87, 0x9b7a, 0x9c6c,
	0x9d5e, 0x9e4f, 0x9f3f, 0xa02e, 0xa11e, 0xa20c, 0xa2fa, 0xa3e7,
	0xa4d4, 0xa5c0, 0xa6ab, 0xa796, 0xa881, 0xa96a, 0xaa53, 0xab3c,
	0xac24, 0xad0c, 0xadf2, 0xaed9, 0xafbe, 0xb0a4, 0xb188, 0xb26c,
	0xb350, 0xb433, 0xb515, 0xb5f7, 0xb6d9, 0xb7ba, 0xb89a, 0xb97a,
	0xba59, 0xbb38, 0xbc16, 0xbcf4, 0xbdd1, 0xbead, 0xbf8a, 0xc065,
	0xc140, 0xc21b, 0xc2f5, 0xc3cf, 0xc4a8, 0xc580, 0xc658, 0xc730,
	0xc807, 0xc8de, 0xc9b4, 0xca8a, 0xcb5f, 0xcc34, 0xcd08, 0xcddc,
	0xceaf, 0xcf82, 0xd054, 0xd126, 0xd1f7, 0xd2c8, 0xd399, 0xd469,
	0xd538, 0xd607, 0xd6d6, 0xd7a4, 0xd872, 0xd93f, 0xda0c, 0xdad9,
	0xdba5, 0xdc70, 0xdd3b, 0xde06, 0xded0, 0xdf9a, 0xe063, 0xe12c,
	0xe1f5, 0xe2bd, 0xe385, 0xe44c, 0xe513, 0xe5d9, 0xe69f, 0xe765,
	0xe82a, 0xe8ef, 0xe9b3, 0xea77, 0xeb3b, 0xebfe, 0xecc1, 0xed83,
	0xee45, 0xef06, 0xefc8, 0xf088, 0xf149, 0xf209, 0xf2c8, 0xf387,
	0xf446, 0xf505, 0xf5c3, 0xf680, 0xf73e, 0xf7fb, 0xf8b7, 0xf973,
	0xfa2f, 0xfaea, 0xfba5, 0xfc60, 0xfd1a, 0xfdd4, 0xfe8e, 0xff47
};

unsigned int intlog2(UINT32 value)
{
	/**
	 *	returns: log2(value) * 2^24
	 *	wrong result if value = 0 (log2(0) is undefined)
	 */
	unsigned int msb;
	unsigned int logentry;
	unsigned int significand;
	unsigned int interpolation;

/*  if (unlikely(value == 0)) {
        WARN_ON(1);*/
    if (value == 0) {    
        return 0;
    }


	/* first detect the msb (count begins at 0) */
	msb = fls(value) - 1;

	/**
	 *	now we use a logtable after the following method:
	 *
	 *	log2(2^x * y) * 2^24 = x * 2^24 + log2(y) * 2^24
	 *	where x = msb and therefore 1 <= y < 2
	 *	first y is determined by shifting the value left
	 *	so that msb is bit 31
	 *		0x00231f56 -> 0x8C7D5800
	 *	the result is y * 2^31 -> "significand"
	 *	then the highest 9 bits are used for a table lookup
	 *	the highest bit is discarded because it's always set
	 *	the highest nine bits in our example are 100011000
	 *	so we would use the entry 0x18
	 */
	significand = value << (31 - msb);
	logentry = (significand >> 23) & 0xff;

	/**
	 *	last step we do is interpolation because of the
	 *	limitations of the log table the error is that part of
	 *	the significand which isn't used for lookup then we
	 *	compute the ratio between the error and the next table entry
	 *	and interpolate it between the log table entry used and the
	 *	next one the biggest error possible is 0x7fffff
	 *	(in our example it's 0x7D5800)
	 *	needed value for next table entry is 0x800000
	 *	so the interpolation is
	 *	(error / 0x800000) * (logtable_next - logtable_current)
	 *	in the implementation the division is moved to the end for
	 *	better accuracy there is also an overflow correction if
	 *	logtable_next is 256
	 */
	interpolation = ((significand & 0x7fffff) *
			((logtable[(logentry + 1) & 0xff] -
			  logtable[logentry]) & 0xffff)) >> 15;

	/* now we return the result */
	return ((msb << 24) + (logtable[logentry] << 8) + interpolation);
}

unsigned int intlog10(UINT32 value)
{
	/**
	 *	returns: log10(value) * 2^24
	 *	wrong result if value = 0 (log10(0) is undefined)
	 */
	unsigned long long log;

/*	if (unlikely(value == 0)) {
		WARN_ON(1);*/
    if (value == 0) {    
		return 0;
	}

	log = intlog2(value);

	/**
	 *	we use the following method:
	 *	log10(x) = log2(x) * log10(2)
	 */

	return (log * 646456993) >> 31;
}

void ddmi_tx(uint input_TxPwr)
{
	WordWriteA2(0xB4, WordReadA2(0xF0));
	WordWriteA2(0xB6, input_TxPwr);	

	BitWrite(0x3E0, 9, 9, 1);		// TX_DIS = 1
	msleep(500);
	WordWriteA2(0xB8, WordReadA2(0xF0));
	WordWriteA2(0xBA, 0);	// -40 dBm
	BitWrite(0x3E0, 9, 9, 0);		// TX_DIS = 0

	printk("done\n");
}

void ddmi_tx_2(uint input_TxPwr)
{
	WordWriteA0(0xB4, WordReadA2(0xF0));
	WordWriteA0(0xB6, input_TxPwr);	

	BitWrite(0x3E0, 9, 9, 1);		// TX_DIS = 1
	msleep(500);
	WordWriteA0(0xB8, WordReadA2(0xF0));
	WordWriteA0(0xBA, 0);	// -40 dBm
	BitWrite(0x3E0, 9, 9, 0);		// TX_DIS = 0

	printk("done\n");
}

void ddmi_rx(uint input_RxPwr, uint input_addr)
{	
    UINT16 adc_code; 
	
	printk("RSSI_ADC_CODE = %d\n", (adc_code = WordReadA2(0xF2)) );
    
	WordWriteA2(input_addr, adc_code);
	WordWriteA2(input_addr+0x02, input_RxPwr);	
}


void ddmi_rx_done(void)
{
	ByteWriteA2(0x553, 0x01);
	while( ByteReadA2(0x553) == 0x01 ){}
	printk("DDMI RX power done\n");
}


/*****************************************************************************
//function :
//		lddla_read_proc
//description : 
//		This function is to output EN7572 ADC codes for external DDMI usage
//		Type "cat /proc/lddla/debug" to get these codes
//input :	
//		N/A
//output :
//		EN7572 ADC codes
******************************************************************************/
#if LINUX_VERSION_CODE > KERNEL_VERSION(6,6,0)
static ssize_t lddla_read_proc(struct file *file, char __user *buf, size_t count, loff_t *data)
#else
static int lddla_read_proc(char *buf, char **start, off_t off, int count, int *eof, void *data)
#endif
{


	return 0;
}

/*****************************************************************************
//function :
//		lddla_write_proc
//description : 
//		This function is to output EN7572 ADC codes for external DDMI usage
//		Type "cat /proc/lddla/debug" to get these codes
//input :	
//		N/A
//output :
//		EN7572 ADC codes
******************************************************************************/
#if LINUX_VERSION_CODE > KERNEL_VERSION(6,6,0)
static ssize_t lddla_write_proc(struct file *file, const char __user *buffer, size_t count, loff_t *data)
#else
static int lddla_write_proc(struct file *file, const char *buffer, unsigned long count, void *data)
#endif
{
	char val_string[64], cmd[64] ,subcmd[64];
	uint dec1=0, dec2=0, dec3=0, dec4=0;
	uint hex1=0, hex2=0, hex3=0, hex4=0;	// ang_20180208
	uint input1=0, input2=0, input3=0 ,input4=0;



	memset(val_string,0,(sizeof(char)*64));
	memset(cmd,0,(sizeof(char)*64));
	memset(subcmd,0,(sizeof(char)*64));

	if (count > sizeof(val_string) - 1)
		return -EINVAL ;

	if (copy_from_user(val_string, buffer, count))
		return -EFAULT ;
		
	sscanf(val_string, "%s %d %d %d %d", 	cmd, &dec1, 	&dec2, 		&dec3,		&dec4);
	sscanf(val_string, "%s %s %d %d %d", 	cmd, subcmd, 	&dec2, 		&dec3,		&dec4);
	sscanf(val_string, "%s %x %x %x %x", 	cmd, &hex1, 	&hex2, 		&hex3,		&hex4);
	sscanf(val_string, "%s %s %x %x %x", 	cmd, subcmd, 	&hex2, 		&hex3,		&hex4);
	sscanf(val_string, "%s %x %d %d %x",	cmd, &input1, 	&input2,	&input3,	&input4);
	sscanf(val_string, "%s %x %d %d %d",	cmd, &hex1, 	&dec2,		&dec3,		&dec4);

	if (!strcmp(cmd, "save_bob")){
		save_bob_table();
	}
	else if((!strcmp(cmd, "usage"))||(!strcmp(cmd, "?")))
	{
		printk("db_msg [0|1]\t\t\t1:Debug msg on\t0:Debug msg off\n");
		printk("sink\t\t\t\tMPD sink\n");
		printk("source\t\t\t\tMPD source\n");
		printk("apc [0x00~0xFF]\t\t\tTX power calibration\n");
		printk("apc_done\t\t\tTX power calibration done\n");
		printk("erc [0x00~0xFF]\t\t\tER calibration\n");
		printk("txout_r [0x00~0x3F]\t\tTX out termination\n");
		printk("tr_boost [0x0~0xF]\t\tTX rising time\n");
        printk("cross [0x00~0x7F]\t\tTX crossing point\n");
		printk("save_tx\t\t\t\tsave the 1st TX calibration data\n");
		printk("save_tx_2\t\t\tsave the 2nd TX calibration data\n");
        printk("save_bob\t\t\tsave A0/A2 tables to en7572_bob.conf\n");
		printk("ddmi_tx\t[0~0xFFFF]\t\tDDMI TX power calibration\n");
        printk("ddmi_tx_2\t[0~0xFFFF]\t\tDDMI TX power calibration\n");
		printk("ddmi_rx\t[0~0xFFFF] [addr]\tDDMI RX power calibration\n");
        printk("ddmi_rx_done\t\t\tRX DDMI calibration done\n");
        printk("vbr\t\t\t\tauto Vbr searching\n");
        printk("apd_dac [0x00~0xFF]\t\t20~70V\n");
        printk("hys [2~6]\t\t\tLOS hysteresis\n");
        printk("los\t\t\t\tauto LOS calibration\n");
#ifdef RXPE
        printk("rx_pe [400~800(dec)] [0~6]    RX out emphasis\n");
#endif
        printk("apd_prmt [0~0xFF] [0~0xFF] [0~0xFFFF]\tslope_dn, slope_up, voltage_25'C\n");
        printk("apd_volt [0~0xFFFF] [0~0xFFFF] [0~0xFFFF] [0~0xFFFF]\n");
        printk("apd_ctrl [0|1]\t\t\t1:Enable 0:Disable\n");
        printk("auto_vapd_nt\tenable apd_ctrl & auto-determined 25'C APD voltage\n");        
        printk("temp_offset [0~0xFF]\t\tBOSA temperature offset -128~+127 'C\n");
        printk("cal_data\t\t\tshow calibration data in flash_bob\n");
        printk("table [0xA0|0xA2]\t\tshow A0/A2 table\n");
        printk("xw [addr] [start] [end] [value(hex)]\n");
        printk("xr [addr] [start] [end]\n");
        printk("bob_info\n");
        printk("bosa_info\n");
        printk("trx_cal_info\n");
        printk("re_cal\t\t\t\treset TX\n");
        printk("clr_cal_data\t\t\tclear en7572_bob.conf to all F\n");
        printk("bosa_temp [0~0xFF]\t\tset BOSA's temperature('C)\n");
        printk("curr_slope [0~0xFF]\t\tcurrent slope\n");
        printk("bob_debug\n");
        printk("GPL [0~2]\t\t\t0:default 1:-3dB 2:-6dB\n");
        printk("adapon [0|1]\t\t\t0:The 1st TX eye\t1:The 2nd TX eye\n");
        printk("cal_done\t\t\tDUT calibration done\n");    
        printk("ibias_imod [0~0xFFF] [0~0xFFF]\tset Ibias/Imod in open-loop mode\n");
        printk("adcl\t\t\t\tauto dual-closed loop\n");
        //printk("apc_lut\t\t\t\tshow APC lookup table info\n");
        //printk("erc_lut\t\t\t\tshow ERC lookup table info\n");
        //printk("showApcErcLut\tshow APC/ERC lookup table\n");
        printk("pwr_up\t\t\t\tincease TX power by 0.1dB\n");
        printk("pwr_dn\t\t\t\tdncease TX power by 0.1dB\n");
        printk("en7572_init\t\tre-initialize 7572\n");
	}
	else if (!strcmp(cmd, "idle")){
		if (hex1 == 1)		// idle
			ByteWriteA2(0x83, 0x01);
		else if (hex1 == 0)	//not idle
			ByteWriteA2(0x83, 0xFF);
		else
			printk("Command error!\n");				
	}
	else if (!strcmp(cmd, "db_msg")){
		db_msg = hex1;
		if(hex1)
			printk("Debug msg on!\n");
		else
			printk("Debug msg off!\n");
	}
	/* Calibration CMD */
	else if (!strcmp(cmd, "sink")){
		BitWrite(0x550, 16, 23, 1);
	}
    else if (!strcmp(cmd, "source")){
		BitWrite(0x550, 16, 23, 0);
	}
	else if (!strcmp(cmd, "apc")){
		BitWrite(0x124, 8, 15, hex1);
	}
	else if (!strcmp(cmd, "apc_done")){
		BitWrite(0x54c, 8, 15, 1);
	}
	else if (!strcmp(cmd, "erc")){	
		BitWrite(0x128, 20, 27, hex1);
	}
	else if (!strcmp(cmd, "tr_boost")){	
		if(hex1 > 0x7)
			hex1 = 0x7;
		BitWrite(0x104, 4, 7, hex1);
	}
	else if (!strcmp(cmd, "txout_r")){	
		if(hex1 > 0x1f)
			hex1 = 0x1f;
		BitWrite(0x104, 24, 29, hex1);
	}
	else if (!strcmp(cmd, "cross")){	
		if(hex1 > 0x7f)
			hex1 = 0x7f;
		BitWrite(0x100, 12, 18, hex1);
	}
	else if (!strcmp(cmd, "save_tx")){
		BitWrite(0x54C, 24, 31, 1);
		
	}
    else if (!strcmp(cmd, "save_tx_2")){
        if ( SaveTxData2() )
            printk("done\n");
	}
	else if (!strcmp(cmd, "ddmi_tx")){
		ddmi_tx(hex1);
	}	
    else if (!strcmp(cmd, "ddmi_tx_2")){
		ddmi_tx_2(hex1);
	}	
	else if (!strcmp(cmd, "ddmi_rx")){
		ddmi_rx(hex1, hex2);
	}
	else if (!strcmp(cmd, "ddmi_rx_done")){
		ddmi_rx_done();
	}
	else if (!strcmp(cmd, "vbr")){
		search_Vbr();
	}
    else if (!strcmp(cmd, "auto_vapd_nt")){
	    auto_Vapd();
	}
	else if (!strcmp(cmd, "apd_dac")){
		BitWrite(0x15C, 0, 7, hex1);
	}
	else if (!strcmp(cmd, "hys")){
		BitWrite(0x54C, 16, 23, hex1);
	}
	else if (!strcmp(cmd, "los")){
		search_LOS();
	}	
	else if (!strcmp(cmd, "apd_prmt")){
		ByteWriteA2(0xA0, hex1);
		ByteWriteA2(0xA1, hex2);
		WordWriteA2(0xA2, hex3);
	}
	else if (!strcmp(cmd, "apd_volt")){
		WordWriteA2(0xA4, hex1);
		WordWriteA2(0xA6, hex2);
		WordWriteA2(0xA8, hex3);
		WordWriteA2(0xAA, hex4);
	}
	else if (!strcmp(cmd, "temp_offset")){
		WordWriteA2(0xAC, hex1);
	}
#ifdef RXPE
	else if (!strcmp(cmd, "rx_pe")){
		SetRxPreEmphasis(dec1, dec2);
	}
#endif
	else if (!strcmp(cmd, "cal_data")){
		showCalData();	
	}
	else if (!strcmp(cmd, "table")){
		showA0A2Table(hex1);	
	}	
	else if (!strcmp(cmd, "xw")){
		BitWrite(hex1, dec2, dec3, hex4);	
	}
	else if (!strcmp(cmd, "xr")){
		printk("value = 0x%X\n", BitRead(hex1, dec2, dec3));	
	}	
	else if (!strcmp(cmd, "bob_info")){
		bob_info();	
	}
	else if (!strcmp(cmd, "bosa_info")){
		bosa_info();	
	}
	else if (!strcmp(cmd, "trx_cal_info")){
		TRX_cal_info();	
	}
	else if (!strcmp(cmd, "re_cal")){
		BitWrite(0x550, 8, 15, 0);
	}
	else if (!strcmp(cmd, "clr_cal_data")){
		clearA2UpperTable();
	}
	else if (!strcmp(cmd, "dump_efuse")){
		dumpEfuse(hex1, hex2);
	}
	else if (!strcmp(cmd, "bosa_temp")){
		autoBosaTempCal(hex1);
	}
	else if (!strcmp(cmd, "bob_debug")){
		bob_debug();
	}
	else if (!strcmp(cmd, "reduce_current")){
		if((hex1 == 0) | (hex1 == 1))
		{
			BitWrite(0xE8, 0, 0, !hex1);	// default is disable=1, enable=0, disable=1
			printk("Reduce current %s!\n", (hex1==1)?"enable":"disable");
		}
		else
		{
			printk("Command error!\n");
		}
	}
	else if (!strcmp(cmd, "adapav")){
		if((hex1 == 0) | (hex1 == 1))
		{
			BitWrite(0xE8, 1, 1, !hex1);	// default is disable=1, enable=0, disable=1
			printk("Adaptive Pav %s!\n", (hex1==1)?"enable":"disable");
		}
	}
	else if (!strcmp(cmd, "slope_time")){
		ByteWriteA2(0xE6, hex1);
		LDDLA_PRINT("slope_time = 0x%x\n", hex1);
	}
	else if (!strcmp(cmd, "apc_red_lmt")){
		ByteWriteA2(0xE7, hex1);
		LDDLA_PRINT("apc_red_lmt = 0x%x\n", hex1);
	}
	else if (!strcmp(cmd, "iav_max")){
		ByteWriteA2(0xE4, hex1);
		LDDLA_PRINT("Iav max code = %d\n", hex1);
	}
	else if (!strcmp(cmd, "imod_max")){
		ByteWriteA2(0xE5, hex1);
		LDDLA_PRINT("Imod max code = %d\n", hex1);
	}
	else if (!strcmp(cmd, "GPL")){
		ByteWriteA2(0x9C, hex1);
		printk("Power Level = %X\n", (UINT8)hex1);
	}
	else if (!strcmp(cmd, "adapon")){
        if (!strcmp(subcmd, "?"))
            AdaptivePon(-1);
        else if ( AdaptivePon(hex1) )
            printk("Mode %d done\n", hex1);
	}
	else if (!strcmp(cmd, "search_erc_dac")){
        search_erc_dac();
	}    
    else if (!strcmp(cmd, "ibias_imod")){
        OpenLoop_IbiasImod(hex1, hex2);
    }   
    else if (!strcmp(cmd, "adcl")){        
        AutoDcl();    
    }       
    else if (!strcmp(cmd, "apc_lut")){        
        printk("APC cal=0x%X\t", ByteReadA2(APC_CAL));
        printk("APC now=0x%X\n", BitRead(RG_APC_ANA_VMON_SEL,8,15));
    }
    else if (!strcmp(cmd, "erc_lut")){        
        printk("ERC cal=0x%X\t", WordReadA2(ERC_DAC)>>4);
        printk("ERC now=0x%X\n", BitRead(RG_IMPD_SINK,20,27));
    }
    else if (!strcmp(cmd, "showApcErcLut")){        
        showApcErcLut();
    }
    else if (!strcmp(cmd, "pwr_up")){ 
        ByteWriteA2(0x9C, 0x11);
    }
    else if (!strcmp(cmd, "pwr_dn")){ 
        ByteWriteA2(0x9C, 0x1F);
    }
    else if (!strcmp(cmd, "en7572_init")){ 
        mt7572_init();
    }
    else if (!strcmp(cmd, "w_a_w_thld")){
        write_A_W_thld();
	}
	/* End of Calibration CMD */
	else
	{
		printk("%s => invalid /proc/lddla/debug cmd option\n",cmd);
	}

	return count ;
}



static struct proc_dir_entry *lddla_proc_dir=NULL;
static struct proc_dir_entry *lddla_proc=NULL; 

#if LINUX_VERSION_CODE > KERNEL_VERSION(6,6,0)

static const struct proc_ops lddla_debug_proc_ops =
{		.proc_read		= lddla_read_proc,
		.proc_write		= lddla_write_proc,
};
#endif

/*****************************************************************************
******************************************************************************/
int lddla_debug_init(void) 
{
	#if LINUX_VERSION_CODE > KERNEL_VERSION(6,6,0)	
	lddla_proc_dir = proc_mkdir("lddla", NULL);
	if ( !lddla_proc_dir )	
	{		
		printk("create proc/lddla failed!\n");		
		return -1;	
	}
	
	lddla_proc = proc_create("debug", 0, lddla_proc_dir, &lddla_debug_proc_ops);
	if(NULL == lddla_proc) 
	{
        printk("ERR: create lddla debug proc Fail\n");
	    return -1;
	}
	#else
    if(lddla_proc){
        return 0;
    }
    
	/* create proc node */
	lddla_proc_dir = proc_mkdir("lddla", NULL);
	if(lddla_proc_dir){
		lddla_proc = create_proc_entry("debug", 0, lddla_proc_dir);
	    if(lddla_proc) {
		    lddla_proc->write_proc = lddla_write_proc;
		    lddla_proc->read_proc = lddla_read_proc;
	    }
	}
	#endif
	
	return 0 ;
}

int lddla_debug_deinit(void){

	if(lddla_proc){
		remove_proc_entry("debug", lddla_proc_dir);
	}

    remove_proc_entry("lddla",NULL);
	return 0 ;
}

void showCalData(void)
{
	UINT16 i;

    for( i=128; i<256; i++ )				
		printk("A0\t0x%X\t0x%02X\n", (0x80+(i-128)), flash_bob[i]);

printk("\n\n");
    
	for( i=384; i<512; i++ )				
		printk("A2\t0x%X\t0x%02X\n", (0x80+(i-384)), flash_bob[i]);			
}

void showA0A2Table(UINT8 table)
{
	UINT8 ptr[1];
	
	UINT16 i;

	for( i=0; i<=0xFF; i++ )	
	{		
		if(table==0xA0)			// A0
		{
			lddla_I2C_read(0, I2C_U2_CLK_DIV, 0x50, 2, i, ptr, 1);
			printk("A0 0x%02X = 0x%02X\n", i, ptr[0]);			
		}
		else if(table==0xA2)	// A2
		{
			lddla_I2C_read(0, I2C_U2_CLK_DIV, 0x51, 2, i, ptr, 1);
			printk("A2 0x%02X = 0x%02X\n", i, ptr[0]);			
		}
	}
	
}

void showApcErcLut(void)
{
	UINT8 ptr[1];
	
	UINT8 i;

    printk("Temp\tAPC\tERC\n");
	for( i=0; i<32; i++ )	
	{		
		{
			lddla_I2C_read(0, I2C_U2_CLK_DIV, 0x50, 2, (0xE0+i), ptr, 1);   // APC
			printk("%d\t%d\t", (-40+i*5), (signed char)ptr[0]);
			lddla_I2C_read(0, I2C_U2_CLK_DIV, 0x50, 2, (0xC0+i), ptr, 1);   // ERC
			printk("%d\n", (signed char)ptr[0]);			
		}
	}
	
}

void bob_info(void)
{
    UINT8 ptr[12];
	UINT16 output_8472;
	int temp;
	printk("Date code = %s\n", DATE_CODE);
	printk("Driver Ver = %d\n", ByteReadA2(0x82));
	printk("LOS = %d\tTX_DIS = %d\tBEN = %d\n", ByteReadA2(0xFA), BitRead(0x3E0,8,8), BitRead(0x488, 0, 0));

    lddla_I2C_read(0, I2C_U2_CLK_DIV, 0x51, 2, 96, ptr, 12);
// Temperature  
    output_8472 = (ptr[0]<<8) | ptr[1];
    temp = ( (signed short)output_8472*10 )>>8;    // Resolution = 0.1'C
    printk("Temp = %d.%01d'C\n", (temp/10), abs(temp%10) );
// VccT 
    output_8472 = (ptr[2]<<8) | ptr[3];
    printk("VccT = %d.%02dV\n", (int)output_8472/10000, (output_8472%10000)/100); 
// Ibias
    output_8472 = (ptr[4]<<8) | ptr[5];
    output_8472 /= 50;                                                      // Resolution = 0.1mA
    printk("Ibias = %d.%01d mA\n", (output_8472/10), (output_8472%10) );
// Imod
    output_8472 = (ptr[10]<<8) | ptr[11];
    output_8472 /= 50;                                                      // Resolution = 0.1mA
    printk("Imod = %d.%01d mA\n", (output_8472/10), (output_8472%10) );
//TX power
    output_8472 = (ptr[6]<<8) | ptr[7];
    temp = intlog10(output_8472)>>14;
    temp -= (4<<10);    
    temp = (temp*1000)>>10;
    printk("TxPwr = %d.%02d dBm\n", (temp/100), abs(temp%100) ); 
// RX power 
    output_8472 = (ptr[8]<<8) | ptr[9];
    temp = intlog10(output_8472)>>14;
    temp -= (4<<10);        
    temp = (temp*1000)>>10; 
    printk("RxPwr = %d.%02d dBm\n", (temp/100), abs(temp%100) ); 
}

void bosa_info(void)
{
	int a, b;

	UINT8 regAddr;



// Vapd at 25'C	
	a = WordReadA2(0xA2)>>3;
	b = ( WordReadA2(0xA2)*10/8 )%10;
	printk("Vapd@25'C = %d.%01dV\t", a, b);
// slope
	a = ByteReadA2(0xA1)*1000/1024;
	b = ByteReadA2(0xA0)*1000/1024;
	printk("slope@HT = 0.%03dV/C\tslope@LT = 0.%03dV/C\n", a, b);
// Four Vapd points
	for( regAddr=(0xA4>>1); regAddr<=(0xAA>>1); regAddr++)
	{
		a = WordReadA2(regAddr<<1)>>3;
		b = ( WordReadA2(regAddr<<1)*10/8 )%10;		
		printk("Vapd@0x%X0 = %d.%01dV\n", (((regAddr<<1)&0x0F)-0x04)<<1, a, b);
	}
// Temperature of BOSA	
	printk("BOSA Temp = %d'C\t", (signed char)ByteReadA2(0xCD));
// Current Vapd
	a = WordReadA2(0xAE)>>3;
	b = ( WordReadA2(0xAE)*10/8 )%10;
	printk("Vapd = %d.%01dV\t", a, b);
// RSSI current	
	printk("RSSI current = %d uA\n", WordReadA2(0xF6)>>5);
// OCP
	if( BitRead(0x160, 30, 30) == 1 )
	{
		printk("OCP enabled ");
		if( BitRead(0x3E4, 8, 8) == 1 )
			printk("(detected)\n");
		else
			printk("\n");
	}
}

void TRX_cal_info(void)
{
	int temp;
    UINT16 txpwr_cal;
    
printk("TX eye 0:\n");
// TX current	
	temp = WordReadA2(0x084)*1000/4095;
	printk("Ibias_cal = %d.%01d mA\t", (temp/10), (temp%10) );
	temp = WordReadA2(0x086)*1000/4095;
	printk("Imod_cal = %d.%01d mA\n", (temp/10), (temp%10) );
	//temp = WordReadA2(0x088)*1000/4095;
	//printk("Iav_cal = %d.%01d mA\n", (temp/10), (temp%10) );
// Dual-closed loop
	printk("APC_org = 0x%X\t", ByteReadA2(0x095));
	printk("APC_cal = 0x%X\t", ByteReadA2(0x08A));
	printk("ERC_cal = 0x%X\n", WordReadA2(0x08E));
// DDMI TX
    memcpy(&txpwr_cal, &flash_bob[TXPWR_CAL_1+256], 2);
	temp = intlog10(txpwr_cal)>>14;
	temp -= (4<<10);		
	temp = (temp*1000)>>10;	
	printk("TxPwr_cal = %d.%02d dBm\n", (temp/100), abs(temp%100) ); 

// TXOUT_R
	printk("TxOut_R = 0x%X\t", ByteReadA2(0x98));
// TX Crossing
	printk("TxCross = 0x%X\t", ByteReadA2(0x99));
// TR Boost
	printk("TR_Boost = 0x%x\n\n", ByteReadA2(0x9D));

printk("TX eye 1:\n");    

// TX current	
	temp = WordReadA0(0x084)*1000/4095;
	printk("Ibias_cal = %d.%01d mA\t", (temp/10), (temp%10) );
	temp = WordReadA0(0x086)*1000/4095;
	printk("Imod_cal = %d.%01d mA\n", (temp/10), (temp%10) );
	//temp = WordReadA0(0x088)*1000/4095;
	//printk("Iav_cal = %d.%01d mA\n", (temp/10), (temp%10) );
// Dual-closed loop
	printk("APC_org = 0x%X\t", ByteReadA0(0x095));
	printk("APC_cal = 0x%X\t", ByteReadA0(0x08A));
	printk("ERC_cal = 0x%X\n", WordReadA0(0x08E));
// DDMI TX
    memcpy(&txpwr_cal, &flash_bob[TXPWR_CAL_1], 2);
	temp = intlog10(txpwr_cal)>>14;
	temp -= (4<<10);		
	temp = (temp*1000)>>10;	
	printk("TxPwr_cal = %d.%02d dBm\n", (temp/100), abs(temp%100) ); 

// TXOUT_R
	printk("TxOut_R = 0x%X\t", ByteReadA0(0x98));
// TX Crossing
	printk("TxCross = 0x%X\t", ByteReadA0(0x99));
// TR Boost
	printk("TR_Boost = 0x%x\n\n", ByteReadA0(0x9D));


// DDMI RX	
	temp = intlog10(WordReadA2(0x0BE))>>14;
	temp -= (4<<10);		
	temp = (temp*1000)>>10;	
	printk("RxPwr1_cal = %d.%02d dBm\n", (temp/100), abs(temp%100) ); 

	temp = intlog10(WordReadA2(0x0C2))>>14;
	temp -= (4<<10);		
	temp = (temp*1000)>>10;	
	printk("RxPwr2_cal = %d.%02d dBm\n", (temp/100), abs(temp%100) ); 

	temp = intlog10(WordReadA2(0x0C6))>>14;
	temp -= (4<<10);		
	temp = (temp*1000)>>10;	
	printk("RxPwr3_cal = %d.%02d dBm\n", (temp/100), abs(temp%100) ); 

// RX LOS
	printk("LOS_cal = 0x%X\tSD_cal = 0x%X\n", ByteReadA2(0xDE), ByteReadA2(0xDF));

#ifdef RXPE
// RX Pre-emphasis
	GetRxPreEmphasis(data);
	printk("RxPE_Swing = %dmV\tRxPE_Boost = %ddB\n", data[0], data[1]);	
#endif
}

void search_LOS(void)
{
	UINT8 los_dac;

	BitWrite(0x43C, 24, 31, 0);			// Set los_dac = 0
	msleep(100);
	if( BitRead(0x424, 24, 24) == 1 )		// LOS status should be 0
	{
		printk("Error, LOS = 1\n");
		return;
	}
	printk("Searching...\n");
	for( los_dac=1; los_dac<=0xFE; los_dac++ )
	{
		BitWrite(0x43C, 24, 31, los_dac);
		msleep(100);
		if( BitRead(0x424, 24, 24) == 1 )
			break;
	}
	printk("LOS_DAC = 0x%x\n", los_dac);
	printk("SD_DAC = 0x%x\n", BitRead(0x43C, 16, 23));
}

UINT8 search_Vbr(void)
{
	UINT8 Vbr_dac = 0x0;

	BitWrite(0x550, 0, 7, 1);
	printk("Searching...\n");
	while( BitRead(0x550, 0, 7) == 1 ){}
	
	Vbr_dac = (UINT8)BitRead(0x550, 0, 7);
	printk("Vbr DAC = 0x%x\n", Vbr_dac);

	return Vbr_dac;
}

void auto_Vapd(void)
{
    UINT8 vbr_dac, apd_dac_target, apd_dac, bosa_temp_offset;
    UINT16 volt_nt;

    bosa_temp_offset = ByteReadA2(BOSA_TEMP_OFFSET);
    if(bosa_temp_offset==0xFF)
    {
        printk("Please do 'bosa_temp' first\n");
        return;
    }
    
    ByteWriteA2(BOSA_TEMP_OFFSET, 0xFF);
    
    vbr_dac = search_Vbr();

    apd_dac_target = (vbr_dac>15) ? (vbr_dac-15) : 0x00;   // Vbr-3v
    printk("apd_dac_target(Vbr-3v)=0x%x\n", apd_dac_target);
        

	ByteWriteA2(BOSA_TEMP_OFFSET, bosa_temp_offset);
    volt_nt = WordReadA2(VOLT_NT);
    

    apd_dac = BitRead(0x15C, 0, 7);
        
    while( apd_dac != apd_dac_target )
    {
        msleep(50);
        
        if(apd_dac>apd_dac_target){
            volt_nt--;}
        else{
            volt_nt++;}

        printk("volt_nt=%d\n", volt_nt);
        if( (volt_nt==0) || (volt_nt==WORD_MASK) )
        {
            printk("volt_nt error\n");
            return;
        }
        

        WordWriteA2(VOLT_NT, volt_nt);

  
        apd_dac = BitRead(0x15C, 0, 7);
        printk("apd_dac=0x%x\n", apd_dac);
    }
}

int save_bob_table(void)
{
	struct file				*srcf = NULL;
	uint i;	

	UINT8 ptr[4];
	char *src = NULL;

	#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
	mm_segment_t			orgfs;	
	#endif

    printk("Save flash_bob[] to /etc/lddla/en7572_bob.conf\n");
	
	for(i=0;i<128;i++)
	{
        BitWrite(MD32_DM_ADDR_ADDR, 0, 31, (i*4)+0x600);
        
		lddla_I2C_read(0, I2C_U2_CLK_DIV, MD32_DEVICE_ADDR, 2,MD32_DM_DATA_ADDR, ptr, 4);
		
        memcpy(&flash_bob[i<<2], ptr, 4);
	}


	//save conf to flash
	src = "/etc/lddla/en7572_bob.conf";

	#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
	orgfs = get_fs();//memory file
	set_fs(KERNEL_DS);
	#endif
	
	if (src && *src)
	{
		srcf = filp_open(src, O_RDWR|O_CREAT, 0);
		if (IS_ERR(srcf))
		{
			printk("--> Error opening \n");
			goto error;
		}
		else
		{
			srcf->f_pos = 0;
			
			if(LDDLA_KERNEL_FS_WRITE(srcf, flash_bob, sizeof(flash_bob), &srcf->f_pos) >0)
			{
				printk("--> write flash_matrix success \n");
			}
			else
			{
				printk("--> Error write \n");
				filp_close(srcf,NULL);						
				goto error;
			}
				
			filp_close(srcf,NULL);
		}
	}
	#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
	set_fs(orgfs);	
	#endif
	return 0;

	error:
	#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
	set_fs(orgfs);	
	#endif
	return -1;	
	
}


/*****************************************************************************
//function :
//		clearA2UpperTable
//description : 
//		this function is used to clear the A2 upper table in DM 
//input :	
//		N/A
//output :
//		N/A
//		
******************************************************************************/
void clearA2UpperTable(void)
{	
	UINT32 addr;
	
	for(addr=0x80;addr<0xFF;addr++)	
		ByteWriteA2(addr, 0xFF);
}


void dumpEfuse(UINT32 key1, UINT32 key2)
{
	UINT32 addr;

	printk("Address\tValue\n");
	
	for(addr=0;addr<=0x23;addr++)			
	{
		BitWrite(0x3334, 0, 31, key1);	
		BitWrite(0x338C, 0, 31, key2);	

		BitWrite(0x3304, 0, 10, addr);	
		BitWrite(0x3300, 0, 3, 1);		

		BitWrite(0x3334, 0, 31, 0);		
		BitWrite(0x338C, 0, 31, 0);		

		if (BitRead(0x3308, 0, 0) == 1)	   
			printk("0x%02X\t0x%02X\n", addr, BitRead(0x3314, 0, 7));		
	}	
}

void autoBosaTempCal(UINT8 bosaTemp)
{
	UINT16 	output_8472;
	UINT8	offset;
	int 	temp, temp_int_part, temp_float_part;

	output_8472 = (UINT16)((ByteReadA2(96)<<8)|ByteReadA2(97));

	temp = ( (int16)output_8472 ) >>5;	// Resolution = 0.125'C
	temp *= 125; 						// Temperature('C) x1000

	temp_int_part = temp/1000;	// int part
	temp_float_part = abs(temp%1000/100);	// float part
	
	if(temp_float_part > 4)	// rounding
	{
		offset = (temp_int_part>bosaTemp) ? (UINT8)(temp_int_part-bosaTemp+1) : 0;	
	}
	else
	{
		offset = (temp_int_part>bosaTemp) ? (UINT8)(temp_int_part-bosaTemp) : 0;
	}
	
	ByteWriteA2(0xAC, offset);
	printk("Temperature offset = %d'C\n", offset);	
}

int bosa_temp_extcal_task_wait(void)
{
	int ret=0;
	int16 BosaOffset_x256;
	UINT16 output_8472, BosaOffset_slope, BosaOffset_offset;
	
	while(!kthread_should_stop())
	{			
		if(ByteReadA2(0xAD) == 1)	//external calibration for BOSA temp offset
		{
			output_8472 = (UINT16)((ByteReadA2(96)<<8)|ByteReadA2(97));
			BosaOffset_slope = ((ByteReadA2(0xB0)<<8)|ByteReadA2(0xB1));	//external calibration T_slope for temperature	
			BosaOffset_offset = ((ByteReadA2(0xB2)<<8)|ByteReadA2(0xB3));	//external calibration T_offset for temperature

			BosaOffset_x256 = (int16)(((BosaOffset_slope * (int16)(output_8472))>>8) + (int16)BosaOffset_offset);
			ByteWriteA2(0xAC, (UINT8)(((BosaOffset_x256 >> 8)>0)?(BosaOffset_x256 >> 8):0));
		}

		msleep(10000);
	}
	return ret;
}

UINT8 search_apc_dac(void)
{
    UINT32 ro_apc_labcal_adc_add;
	UINT16 apc_dac=0;
	UINT8 tiaGain,dcl_mode;

	tiaGain = BitRead(0x130, 8, 13);
	dcl_mode = BitRead(0x208,4,6);

	BitWrite(0x208,4,6,4);	// rg_loop_sel = debug mode
	BitWrite(0x130,8,13,36);	// 0.8K
	msleep(50);

	BitWrite(0x120,26,26,1);	// RG_APC_ADC_EN=1
	msleep(5);

	ro_apc_labcal_adc_add = BitRead(0x33c,0,31);
	apc_dac = 256 - (ro_apc_labcal_adc_add >> 7);

	BitWrite(0x130,8,13,tiaGain);
	BitWrite(0x120,26,26,0);
	BitWrite(0x208,4,6,dcl_mode);

	return (UINT8)apc_dac;
}

UINT32 mpd_current(void)
{  
	UINT8 i, apc_dac;

    apc_dac = search_apc_dac();

	for(i=0; i<156; i++)
	{
		if(MPD_LUT[i][0] == apc_dac)
		{
			return MPD_LUT[i][1];	
		}
	}
	return 0xFFFF;
}

UINT16 search_erc_dac(void)
{
    UINT16 stepSize=0x800, ercDac=0x800, mpd, rep;
    UINT32 ercResult;

    UINT32 
        rg_loop_sel = BitRead(0x208, 4, 6),
        rg_erc_im_dac = BitRead(RG_IMPD_SINK, 16, 27);
    
    BitWrite(0x208, 4, 6, 4);
    BitWrite(RG_IMPD_SINK, 16, 27, ercDac);
    
    while (stepSize>=4)
	{          
        msleep(100);
        BitWrite(0x214, 0, 0, 0);  
        ercResult = BitRead(0x234, 0, 31);
        BitWrite(0x214, 0, 0, 1); 
        
        rep = ercResult&WORD_MASK;
        mpd = (ercResult>>16)&WORD_MASK;
        printk("ercDac=0x%x\trep:mpd=%d:%d\tstepSize=0x%x\n", ercDac, rep, mpd, stepSize);
        stepSize/=2;

        if(mpd>rep)
            ercDac+=stepSize;        
        else if(rep>mpd)
            ercDac-=stepSize;        
        else
            break;
        
        BitWrite(RG_IMPD_SINK, 16, 27, ercDac);        
	}

    printk("\nercDac=0x%x\n", ercDac);

    BitWrite(RG_IMPD_SINK, 16, 27, rg_erc_im_dac);
    BitWrite(0x208, 4, 6, rg_loop_sel);

    return ercDac;
}

void OpenLoop_IbiasImod(UINT16 ibias, UINT16 imod)
{    
    ibias &= 0xFFF;
    imod  &= 0xFFF;
    
    BitWrite(DCL_CTRL_2, 0, 31, (imod<<16) | (ibias+(imod>>1)) );
    BitWrite(0x208, 4, 6, 0);
}

void AutoDcl(void)
{
    BitWrite(RG_APC_ANA_VMON_SEL, 8, 15, search_apc_dac());
    BitWrite(0x54c, 8, 15, 1);
    msleep(100);
    BitWrite(0x208, 4, 6, 0);
    BitWrite(RG_IMPD_SINK, 16, 27, search_erc_dac());
    BitWrite(0x208, 4, 6, 2);
}

void custom_func_print(UINT8 func, UINT8 disable)
{
	if(!disable)
	{
		switch(func)
		{
			case 0:
				printk("\treduce_current\n");
				break;
			case 1:
				printk("\tadaptive_pav\n");
				break;			
		}
	}
}
void bob_debug(void)
{
	int tmp;
	UINT32 tmp2;
	UINT8 i=0;
	// MPD current
	printk("MPD current = %d uA\n", mpd_current());
	// Current limit
	tmp2 = BitRead(0x214,16,28)*150;
	printk("Iav MAX = %d.%.2d mA\n",tmp2/6142, ((tmp2%6142*100)/6142));
	tmp2 = BitRead(0x248,0,11)*100;
	printk("Imod MAX = %d.%.2d mA\n",tmp2/4095, ((tmp2%4095*100)/4095));
	// Custom function
	tmp = ByteReadA2(0xE8);
	printk("Custom function enabled:\n");
	if(ByteReadA2(0xAD) == 1)
		printk("\tbosa_temp_extcal\n");
	for(i=0; i<8; i++)
	{
		custom_func_print(i, (tmp >> i) & 0x1);
	}
    printk("BOSA temp=%d\tAPC offset=%d\n", (signed char)ByteReadA2(0xCD), ByteReadA2(0x125)-ByteReadA2(0x08A));
	printk("\n");	
}


/*****************************************************************************
//function :
//		reduce_Imod_task_wait
//description : 
//		Solution 1. This function is used to reduce Imod at high temperature
//input :	
//		N/A
//output :
//		N/A
//		
******************************************************************************/

int reduce_imod_task_wait(void *data)
{

	UINT16	delay_ms = 5000, Imod_max_tune, Imod_max_new = 0, Iav_code, Imod_code;	
	UINT8 	High_Temp_th, temp_offset;
	int16 	temperature;	

	static bool assert = false, first_boot=true;
	static int16 temperature_org;
	static UINT8 cnt1=0, cnt2=0;
	static UINT16 Iav_max, Imod_max;
	
	while(!kthread_should_stop())
	{	
		msleep(delay_ms<5000?5000:delay_ms);
		if(BitRead(0xE8, 0, 0) == 1)	// enable=0, disable=1
		{
			continue;
		}
	
		LDDLA_PRINT("delay_ms = %d ms\n", delay_ms);
		msleep(delay_ms<5000?5000:delay_ms);
		LDDLA_PRINT("Reduce_current_assert = %d\n", assert);

		Iav_code = BitRead(0x3C4, 0, 12);
		Imod_code = BitRead(0x3C8, 16, 27);
		LDDLA_PRINT("Iav_code = %d\n", Iav_code);
		LDDLA_PRINT("Imod_code = %d\n", Imod_code);
		Iav_max = (((UINT16)ByteReadA2(0xE4))<<5) | 0x1E;
		Imod_max = ((UINT16)ByteReadA2(0xE5))<<4 | 0xF;
		LDDLA_PRINT("Iav_code_max = %d\n", Iav_max);
		LDDLA_PRINT("Imod_code_max = %d\n", Imod_max);
		LDDLA_PRINT("first_boot = %d\n", first_boot);
		
		temp_offset = ByteReadA2(0xAC);
		temperature	= ((int16)((UINT16)((ByteReadA2(96)<<8)|ByteReadA2(97))))>>8;	
		temperature = temperature - temp_offset;	// BOSA temperature

		if(first_boot)	// first boot at high temperature
		{
			if(BitRead(0x488, 0, 0) == 0)
				continue;
			first_boot = false;
			
			High_Temp_th = ByteReadA2(0xE7);
				
			if(Iav_max < 0x17FE && Imod_max < 0xFFF && High_Temp_th == 0xFF)
			{
				LDDLA_PRINT("First boot db = 1\n");
				temperature_org = 80;
			}
			else if(High_Temp_th == 0xFF)
			{
				LDDLA_PRINT("First boot db = 2\n");
				continue;	
			}
			else
			{
				temperature_org = (signed char)(High_Temp_th)-temp_offset;	// BOSA temperature
				LDDLA_PRINT("First boot db = 3\n");
			}
				
			LDDLA_PRINT("Temp = %d 'C\n", temperature);
			LDDLA_PRINT("Temp_org = %d 'C\n", temperature_org);
			if((temperature > temperature_org) && ((Iav_code==Iav_max) | (Imod_code==Imod_max)))
			{
				assert = true;
				
				if(Imod_max>Imod_code)		// Iav touch the Iav_max first
					Imod_max_new = Imod_code;
				else
					Imod_max_new = Imod_max;
				if(Iav_max>Iav_code)	// Imod touch the Imod_max first
					BitWrite(0x214, 16, 28, Iav_code);
				LDDLA_PRINT("high temperature boot asserted\n");
			}
			delay_ms=20000;
		}
		
		if(assert)
		{
			LDDLA_PRINT("temperature = %d 'C\n", temperature);
			LDDLA_PRINT("temperature_org = %d 'C\n", temperature_org);

			Imod_max_tune = Imod_max_new-ByteReadA2(0xE6)*(temperature-temperature_org);
			if(Imod_max_tune > Imod_max_new)
				Imod_max_tune = Imod_max_new;
			else if(Imod_max_tune < Imod_max_new/2)
				Imod_max_tune = (UINT16)(Imod_max_new/2);
			
			LDDLA_PRINT("Imod_max_tune = %d\n", Imod_max_tune);
			BitWrite(0x248,0,11, Imod_max_tune);
		}
		
		if((temperature<temperature_org) && assert)
		{
			cnt2++;
			delay_ms=10000;
		}
		else if((temperature>=temperature_org) && assert)
		{
			cnt2=0;
			delay_ms=20000;
		}
		else if(Iav_code<Iav_max && Imod_code<Imod_max && (!assert))
		{
			cnt1=0;
			delay_ms=((Iav_max-Iav_code < 50) | (Imod_max-Imod_code < 50))?5000:30000;
		}
		else if(((Iav_code==Iav_max) | (Imod_code==Imod_max)) && !assert)		//Iav or Imod are fixed by max_org setting
		{
			cnt1++;
			delay_ms=5000;
		}
		LDDLA_PRINT("cnt1 = %d\ncnt2 = %d\n", cnt1,cnt2);

		if(cnt1 >= 4 && (!assert))	// assert
		{
			assert = true;
			LDDLA_PRINT("asserted\n");
			cnt1=0;
			cnt2=0;
			
			temperature_org = temperature;

			if(Imod_max>Imod_code)		// Iav touch the Iav_max first
				Imod_max_new = Imod_code;
			else
				Imod_max_new = Imod_max;
			if(Iav_max>Iav_code)	// Imod touch the Imod_max first
				BitWrite(0x214, 16, 28, Iav_code);
			continue;	
		}
		if(cnt2 >= 4 && assert)	// de-assert
		{
			assert = false;
			LDDLA_PRINT("deassert\n");
			cnt1=0;
			cnt2=0;	
			BitWrite(0x214, 16, 28, (((UINT16)ByteReadA2(0xE4))<<5) | 0x1E);
			BitWrite(0x248, 0, 11, ((UINT16)ByteReadA2(0xE5))<<4 | 0xF);
			continue;
		}				
	}
	
	return 0;	
}

int adaptive_pav_task_wait(void *data)
{
	static bool isAssert = false;
	UINT8 apc, apc_cal, apc_reduction_lmt;
	static UINT8 cntIncPav = 0;
	UINT16	Imod_code, Imod_max;
	static UINT16 delay_ms = 1000;

	while(!kthread_should_stop())
	{
		msleep(delay_ms);
		
		if(BitRead(0xE8, 1, 1) == 1)
		{
			delay_ms = 1000 * 10;
			continue;
		}

		apc_cal = ByteReadA2(0x8A);
		LDDLA_PRINT("apc_cal = %d\n", apc_cal);

		apc = (UINT8)BitRead(0x124, 8, 15);
		LDDLA_PRINT("apc = %d\n", apc);

		apc_reduction_lmt = ByteReadA2(0xE7);
		LDDLA_PRINT("apc_reduction_lmt = %d\n", apc_reduction_lmt);
		
		Imod_code = BitRead(0x3C8, 16, 27);
		LDDLA_PRINT("Imod_code = %d\n", Imod_code);
		
		Imod_max = ((UINT16)ByteReadA2(0xE5))<<4 | 0xF;
		LDDLA_PRINT("Imod_code_max = %d\n", Imod_max);

		LDDLA_PRINT("Adaptive Pav assert = %d\n", isAssert);
		LDDLA_PRINT("Cycle = %ds\n", delay_ms/1000);

		
		if(apc == apc_cal) { isAssert = false; }
		
		if(BitRead(0x488, 0, 0) == 0)
		{
			LDDLA_PRINT("BEN = %d\n", BitRead(0x488, 0, 0));
			delay_ms = 1000;
			continue;
		}
		else { delay_ms = 1000 * ByteReadA2(0xE6); }

		
		if(Imod_code == Imod_max) 	// Reduce Pav
		{
			LDDLA_PRINT("Try to reduce Pav...\n");
			isAssert = true;
			cntIncPav = 0;
			if( apc > (apc_cal-apc_reduction_lmt) )
			{
				ByteWriteA2(0x9C, 0x1F);		// minus
				LDDLA_PRINT("Pav reduced\n");
			}
			else { LDDLA_PRINT("Pav limited\n") };
		}
		else if( (Imod_code < Imod_max) && isAssert )	// Increase Pav
		{
			LDDLA_PRINT("cntIncPav = %d\n", cntIncPav);
			if(++cntIncPav >= 5)
			{
				cntIncPav = 5;
				LDDLA_PRINT("Try to increase Pav...\n");
				if(apc < apc_cal)
				{
					ByteWriteA2(0x9C, 0x11); 	// add
					LDDLA_PRINT("Pav increased\n");
				}
			}
		}
	}
	
	return 0;
}


#ifdef SECOND_CAL
void save_tx_sec(void)
{
	writeByBitA0(0x8A, 0, 7,	(UINT8)BitRead(0x124, 8, 15));		// APC_cal
		
	writeByBitA0(0x8C, 0, 7, 	(UINT8)BitRead(0x130, 0, 0));			// TIA_CUR
	writeByBitA0(0x8D, 0, 7, 	(UINT8)BitRead(0x128, 8, 15));		// ERC_CDAC
	writeByBitA0(0x8E, 0, 15, 	(UINT16)BitRead(0x128, 16, 27));		// ERC_DAC

	writeByBitA0(0x90, 0, 7,	(UINT8)BitRead(0x130, 8, 10));		// TIA_GAIN
	writeByBitA0(0x91, 0, 7,	(UINT8)BitRead(0x130, 11, 13));		// TIA_BW 
	writeByBitA0(0x92, 0, 7, 	(UINT8)BitRead(0x13C, 16, 18));		// PGA_GAIN
	writeByBitA0(0x93, 0, 7,	(UINT8)BitRead(0x13C, 5, 6));			// PGA_CAP

	writeByBitA0(0x98, 0, 7,  	(UINT8)BitRead(0x104, 24, 29));		// RG_TXOUTP_R
	writeByBitA0(0x99, 0, 7,  	(UINT8)BitRead(0x100, 12, 18));		// RG_TX_CROSSING
}
void cal_value_load(UINT8 cal_value)
{
	static UINT16 first_TSSI_1, first_TXPWR_1, first_TSSI_2, first_TXPWR_2;
	
	switch(cal_value)
	{
		case 1:		// load first calibration value
			BitWrite(0x124, 8, 15,	(UINT8)BitRead(0x8A, 0, 7));			// APC_cal
			
			BitWrite(0x130, 0, 0, 	(UINT8)BitRead(0x8C, 0, 7));			// TIA_CUR
			BitWrite(0x128, 8, 15, 	(UINT8)BitRead(0x8D, 0, 7));			// ERC_CDAC
			BitWrite(0x128, 16, 27, 	(UINT16)BitRead(0x8E, 0, 15));		// ERC_DAC

			BitWrite(0x130, 8, 10,	(UINT8)BitRead(0x90, 0, 7));			// TIA_GAIN
			BitWrite(0x130, 11, 13,	(UINT8)BitRead(0x91, 0, 7));			// TIA_BW 
			BitWrite(0x13C, 16, 18, 	(UINT8)BitRead(0x92, 0, 7));			// PGA_GAIN
			
			BitWrite(0x13C, 5, 6,		(UINT8)BitRead(0x93, 0, 7));			// PGA_CAP

			BitWrite(0x104, 24, 29,  	(UINT8)BitRead(0x98, 0, 7));			// RG_TXOUTP_R
			BitWrite(0x100, 12, 18,  	(UINT8)BitRead(0x99, 0, 7));			// RG_TX_CROSSING

			// TX DDMI
			#if 1	// reduce code size
			BitWrite(0xB4, 0, 31, (UINT32)((first_TXPWR_1<<16) | first_TSSI_1));
			BitWrite(0xB8, 0, 31, (UINT32)((first_TXPWR_2<<16) | first_TSSI_2));
			#else
			writeByBit(0xB4, 0, 15, first_TSSI_1);
			writeByBit(0xB6, 0, 15, first_TXPWR_1);	
			writeByBit(0xB8, 0, 15, first_TSSI_2);
			writeByBit(0xBA, 0, 15, first_TXPWR_2);	
			#endif
			
			break;

		case 2:		// load second calibration value
			BitWrite(0x124, 8, 15,	(UINT8)readByBitA0(0x8A, 0, 7));		// APC_cal
			
			BitWrite(0x130, 0, 0, 	(UINT8)readByBitA0(0x8C, 0, 7));		// TIA_CUR
			BitWrite(0x128, 8, 15, 	(UINT8)readByBitA0(0x8D, 0, 7));		// ERC_CDAC
			BitWrite(0x128, 16, 27, 	(UINT16)readByBitA0(0x8E, 0, 15));		// ERC_DAC

			BitWrite(0x130, 8, 10,	(UINT8)readByBitA0(0x90, 0, 7));		// TIA_GAIN
			BitWrite(0x130, 11, 13,	(UINT8)readByBitA0(0x91, 0, 7));		// TIA_BW 
			BitWrite(0x13C, 16, 18, 	(UINT8)readByBitA0(0x92, 0, 7));		// PGA_GAIN
			
			BitWrite(0x13C, 5, 6,		(UINT8)readByBitA0(0x93, 0, 7));		// PGA_CAP

			BitWrite(0x104, 24, 29,  	(UINT8)readByBitA0(0x98, 0, 7));		// RG_TXOUTP_R
			BitWrite(0x100, 12, 18,  	(UINT8)readByBitA0(0x99, 0, 7));		// RG_TX_CROSSING

			// TX DDMI
			first_TSSI_1 = WordReadA2(0xB4);
			first_TXPWR_1 = WordReadA2(0xB6);
			first_TSSI_2 = WordReadA2(0xB8);
			first_TXPWR_2 = WordReadA2(0xBA);
			#if 1	//reduce code size
			BitWrite(0xB4, 0, 31, readByBitA0(0xB4, 0, 31));
			BitWrite(0xB8, 0, 31, readByBitA0(0xB8, 0, 31));
			#else
			writeByBit(0xB4, 0, 15, readByBitA0(0xB4));
			writeByBit(0xB6, 0, 15, readByBitA0(0xB6));	
			writeByBit(0xB8, 0, 15, read2Bytes(0xB8));
			writeByBit(0xBA, 0, 15, read2Bytes(0xBA));
			#endif
						
			break;	
	}
}

/*****************************************************************************
//function :
//		second_cal_switch_task_wait
//description : 
//		Solution 2. This function is used to switch to second calibration value and back to first calibration automaticlly
//input :	
//		N/A
//output :
//		N/A
//		
******************************************************************************/

int second_cal_switch_task_wait(void)
{
	UINT16 delay_ms=5000, Iav_code, Imod_code, Iav_max, Imod_max;
	UINT8 cnt1=0, cnt2=0, cnt_sw=0, temp_offset, High_Temp_th;
	int16 temperature;
	static int16 temperature_org;
	static bool assert=false, inSwitch=false, first_boot=true;
	
	while(!kthread_should_stop())
	{
		msleep(delay_ms);
		if(BitRead(0xE8, 1, 1) == 1)	// default is disable=1, for user: enable=1, disable=0
		{
			continue;
		}
		LDDLA_PRINT("delay_ms = %d\n", delay_ms);
		LDDLA_PRINT("first_boot = %d\n", first_boot);
		LDDLA_PRINT("inSwitch = %d\n", inSwitch);
		LDDLA_PRINT("Assert = %d\n", assert);
		LDDLA_PRINT("cnt1 = %d\n", cnt1);
		LDDLA_PRINT("cnt2 = %d\n", cnt2);
	
		if(inSwitch)
		{
			inSwitch=false;
			delay_ms=30000;
			//reset apc_cnt and erc_cnt
			BitWrite(0x304, 16, 31, 256);
			BitWrite(0x23c, 0, 3, 5);

			ByteWriteA2(0x83, 0xFF);
			continue;
		}
		
		Iav_code = BitRead(0x3C4, 0, 12);
		Imod_code = BitRead(0x3C8, 16, 27);
		LDDLA_PRINT("Iav_code = %d\n", Iav_code);
		LDDLA_PRINT("Imod_code = %d\n", Imod_code);
		Iav_max = (((UINT16)ByteReadA2(0xE4))<<5) | 0x1E;
		Imod_max = ((UINT16)ByteReadA2(0xE5))<<4 | 0xF;

		temp_offset = ByteReadA2(0xAC);
		temperature	= ((int16)((UINT16)((ByteReadA2(96)<<8)|ByteReadA2(97))))>>8;	
		temperature = temperature - temp_offset;	// BOSA temperature

		if(first_boot)	// First boot at high temperature
		{
			if(BitRead(0x488, 0, 0) == 0)
				continue;
			first_boot=false;
			LDDLA_PRINT("in first_boot function\n");
			
			High_Temp_th = ByteReadA2(0xE7);
			if(High_Temp_th == 0xFF)
				temperature_org = 80;
			else
				temperature_org = (signed char)High_Temp_th - temp_offset;
			
			LDDLA_PRINT("temperature = %d\n", temperature);
			LDDLA_PRINT("temperature_org = %d\n", temperature_org);
			if((Iav_code==Iav_max) | (Imod_code==Imod_max))
			{
				assert = true;
				cnt1=0;
				cnt2=0;
					
				delay_ms=5000;
				
				BitWrite(0x208, 4, 6, 4);		// debug mode
				ByteWriteA2(0x83, 0x1);

				cal_value_load(2);

				//set apc and erc
				BitWrite(0x304, 16, 31, 16384);
				BitWrite(0x23c, 0, 3, 15);
				inSwitch=true;
				
				BitWrite(0x208, 4, 6, 2);		// DCL mode
				LDDLA_PRINT("first_boot_assert\n");
				continue;
			}
		}
		
		LDDLA_PRINT("temperature = %d\n", temperature);
		LDDLA_PRINT("temperature_org = %d\n", temperature_org);
		delay_ms = ((Iav_max-Iav_code < 5) | (Imod_max-Imod_code < 5))?5000:30000;
		
		if(((Iav_code==Iav_max) | (Imod_code==Imod_max)) && !assert)	// prepare to assert
		{
			cnt1++;
			cnt2=0;
			delay_ms=2000;
		}
		else if ((temperature_org-temperature) > ByteReadA2(0xE9) && assert)	// prepare to de-assert
		{
			cnt2++;
			cnt1=0;
			delay_ms=5000;
		}

		if(cnt1 == 5)	// assert
		{
			assert = true;
			cnt1=0;
			cnt2=0;
				
			temperature_org = temperature;
			delay_ms=10000;
			
			BitWrite(0x208, 4, 6, 4);		// debug mode
			ByteWriteA2(0x83, 0x1);
			
			cal_value_load(2);

			//set apc and erc
			BitWrite(0x304, 16, 31, 16384);
			BitWrite(0x23c, 0, 3, 15);
			inSwitch=true;
			
			BitWrite(0x208, 4, 6, 2);		// DCL mode
			continue;
		}
		else if(cnt2 == 4)	// de-assert
		{
			assert = false;
			cnt1=0;
			cnt2=0;
			
			delay_ms=30000;
			BitWrite(0x208, 4, 6, 4);		// debug mode
			ByteWriteA2(0x83, 0x1);
			
			cal_value_load(1);

			//set apc and erc
			BitWrite(0x304, 16, 31, 16384);
			BitWrite(0x23c, 0, 3, 15);
			inSwitch=true;
			
			BitWrite(0x208, 4, 6, 2);		// DCL mode
			continue;
		}
		
	}
	return 1;
}
#endif


bool AdaptivePon(int mode)
{
    UINT8 ptr[4], apc_dac;
    UINT16 erc_dac;
    int temp;

    printk("[%s: %d]mode=%d\n", __FUNCTION__, __LINE__,mode);
    
    if ( mode == -1 )
    {
        apc_dac = BitRead(RG_APC_ANA_VMON_SEL, 8, 15);
        erc_dac = BitRead(RG_IMPD_SINK, 16, 27); 

        if( (apc_dac == FLASH_BOB_A2(APC_CAL)) &&
            (erc_dac == ((FLASH_BOB_A2((ERC_DAC+0x01))<<8)|FLASH_BOB_A2(ERC_DAC))) )
            printk("Using TX eye 0\n");
        else if( (apc_dac == FLASH_BOB_A0(APC_CAL)) &&
            (erc_dac == ((FLASH_BOB_A0((ERC_DAC+0x01))<<8)|FLASH_BOB_A0(ERC_DAC))) )
            printk("Using TX eye 1\n");
    }
    else if ( mode == 0 )    // Mode 0 is default mode
    {
        BitWrite(0x100, 2, 3, 2);		// BEN off
        
    // Load calibration data
        BitWrite(DCL_CTRL_2, 16, 27,      (FLASH_BOB_A2((IMOD_CAL+0x01))<<8)|FLASH_BOB_A2(IMOD_CAL));
        BitWrite(DCL_CTRL_2, 0, 12,       (FLASH_BOB_A2((IAV_CAL+0x01)) <<8)|FLASH_BOB_A2(IAV_CAL) );

        BitWrite(RG_APC_ANA_VMON_SEL, 8, 15,  FLASH_BOB_A2(APC_CAL));            // RG_APC_DAC

        BitWrite(RG_RESERVE_TIA, 0, 0,        FLASH_BOB_A2(TIA_CUR));           // TIA_CUR        
		
		BitWrite(RG_IMPD_SINK, 8, 15, 	FLASH_BOB_A2(ERC_CDAC));  		// ERC_CDAC
		BitWrite(RG_IMPD_SINK, 16, 27, 	(FLASH_BOB_A2((ERC_DAC+0x01))<<8)|FLASH_BOB_A2(ERC_DAC));		// ERC_DAC
		
        BitWrite(RG_RESERVE_TIA, 8, 13,       ((FLASH_BOB_A2(TIA_BW)<<3)|FLASH_BOB_A2(TIA_GAIN)));
        BitWrite(RG_REP_PH_CAP_SEL, 16, 18,   FLASH_BOB_A2(PGA_GAIN));   // PGA_GAIN
        BitWrite(RG_REP_PH_CAP_SEL, 5, 6,     FLASH_BOB_A2(PGA_CAP));    // PGA_CAP      

    // DDMI
        memcpy(ptr, &flash_bob[TSSI_CAL_1+256], 4);
        lddla_I2C_write(0, I2C_U2_CLK_DIV, 0x51, 2, TSSI_CAL_1, ptr, 4);
    
    // TX_SD
        //writeByBit(0x154, 12, 20, FLASH_BOB_A2(TX_SD_DAC));       // Set RG_TXSD_DAC     

        BitWrite(0x208, 0, 0, 0);     // rg_loop_en   
        BitWrite(0x208, 0, 0, 1);     // rg_loop_en   
        BitWrite(0x100, 2, 3, 0);     // BEN normal
    }
    else if ( mode == 1 )
    {       
        BitWrite(0x100, 2, 3, 2);		// BEN off
        
    // Load calibration data
		BitWrite(DCL_CTRL_2, 16, 27,		(FLASH_BOB_A0((IMOD_CAL+0x01))<<8)|FLASH_BOB_A0(IMOD_CAL));
		BitWrite(DCL_CTRL_2, 0, 12,		(FLASH_BOB_A0((IAV_CAL+0x01)) <<8)|FLASH_BOB_A0(IAV_CAL) );


        BitWrite(RG_APC_ANA_VMON_SEL, 8, 15,  FLASH_BOB_A0(APC_CAL));            // RG_APC_DAC

        BitWrite(RG_RESERVE_TIA, 0, 0,        FLASH_BOB_A0(TIA_CUR));           // TIA_CUR        

		BitWrite(RG_IMPD_SINK, 8, 15, 	FLASH_BOB_A0(ERC_CDAC));  		// ERC_CDAC
		BitWrite(RG_IMPD_SINK, 16, 27, 	(FLASH_BOB_A0((ERC_DAC+0x01))<<8)|FLASH_BOB_A0(ERC_DAC));		// ERC_DAC

        BitWrite(RG_RESERVE_TIA, 8, 13,       ((FLASH_BOB_A0(TIA_BW)<<3)|FLASH_BOB_A0(TIA_GAIN)));
        BitWrite(RG_REP_PH_CAP_SEL, 16, 18,   FLASH_BOB_A0(PGA_GAIN));   // PGA_GAIN
        BitWrite(RG_REP_PH_CAP_SEL, 5, 6,     FLASH_BOB_A0(PGA_CAP));    // PGA_CAP      

    // DDMI
        memcpy(ptr, &flash_bob[TSSI_CAL_1], 4);
        lddla_I2C_write(0, I2C_U2_CLK_DIV, 0x51, 2, TSSI_CAL_1, ptr, 4);

    // TX_SD
        //writeByBit(0x154, 12, 20, FLASH_BOB_A0(TX_SD_DAC));       // Set RG_TXSD_DAC 

        BitWrite(0x208, 0, 0, 0);     // rg_loop_en   
        BitWrite(0x208, 0, 0, 1);     // rg_loop_en   
        BitWrite(0x100, 2, 3, 0);     // BEN normal
    }


    


    
    printk("APC_DAC = 0x%X\n", BitRead(RG_APC_ANA_VMON_SEL, 8, 15));
    printk("ERC_DAC = 0x%X\n", BitRead(RG_IMPD_SINK, 16, 27));

    // DDMI TX
	temp = intlog10(WordReadA2(0x0B6))>>14;
	temp -= (4<<10);		
	temp = (temp*1000)>>10;	
	printk("TxPwr_cal = %d.%02d dBm\n", (temp/100), abs(temp%100) ); 
    
    return TRUE;
}


bool SaveTxData2(void)
{
    WordWriteA0(IBIAS_CAL, 	(UINT16)BitRead(CSR_IBIAS_IMOD, 0, 11));
    WordWriteA0(IMOD_CAL, 	(UINT16)BitRead(CSR_IBIAS_IMOD, 16, 27));
    WordWriteA0(IAV_CAL, 		(UINT16)BitRead(CSR_IAV, 0, 12));
	
    ByteWriteA0(APC_CAL, 	(UINT8)BitRead(RG_APC_ANA_VMON_SEL,8,15));// RG_APC_DAC
    ByteWriteA0(TIA_CUR,	(UINT8)BitRead(RG_RESERVE_TIA, 0, 0));		// TIA_CUR
    ByteWriteA0(ERC_CDAC,	(UINT8)BitRead(RG_IMPD_SINK, 8, 15));			// ERC_CDAC
    WordWriteA0(ERC_DAC,	(UINT16)BitRead(RG_IMPD_SINK, 16, 27));		// ERC_DAC
    ByteWriteA0(TIA_GAIN,	(UINT8)BitRead(RG_RESERVE_TIA, 8, 10));		// TIA_GAIN
    ByteWriteA0(TIA_BW,	(UINT8)BitRead(RG_RESERVE_TIA, 11, 13));		// TIA_BW 
    ByteWriteA0(PGA_GAIN,	(UINT8)BitRead(RG_REP_PH_CAP_SEL, 16, 18));	// PGA_GAIN
    ByteWriteA0(PGA_CAP, 	(UINT8)BitRead(RG_REP_PH_CAP_SEL, 5, 6));		// PGA_CAP
    
    return TRUE;
}

#ifdef RXPE
void SetRxPreEmphasis(UINT16 dc_swing, UINT16 boost)
{
	int i=0;
	for (i=0; i<64; i++)
	{
		if(	(RX_PE_LUT[i][0] == dc_swing) &&
 			(RX_PE_LUT[i][1] == boost) )
		{
			BitWrite(0x114, 8, 	13, 	RX_PE_LUT[i][2]);
			BitWrite(0x114, 16, 	20, 	RX_PE_LUT[i][3]);
			BitWrite(0x114, 24, 	29, 	RX_PE_LUT[i][4]);
			BitWrite(0x114, 3, 	3, 		RX_PE_LUT[i][5]);
			BitWrite(0x110, 6, 	6, 		RX_PE_LUT[i][6]);
			printk("DcSwing=%dmV\tBoost=%ddB\n", dc_swing, boost);
			break;
		}
	}
}

void GetRxPreEmphasis(UINT16* data)	
{
	int i =0;

	UINT16
		ctrl1 = (UINT16)BitRead(0x114, 8, 13),
		ctrl2 = (UINT16)BitRead(0x114, 16, 20),
		ctrl3 = (UINT16)BitRead(0x114, 24, 29),
		ctrl4 = (UINT16)BitRead(0x114, 3, 3),
		ctrl5 = (UINT16)BitRead(0x110, 6, 6);
	for (i=0; i<64; i++)
	{
		if(	(RX_PE_LUT[i][2] == ctrl1) &&
 			(RX_PE_LUT[i][3] == ctrl2) &&
 			(RX_PE_LUT[i][4] == ctrl3) &&
 			(RX_PE_LUT[i][5] == ctrl4) &&
 			(RX_PE_LUT[i][6] == ctrl5) )
		{
				*(data+0)=RX_PE_LUT[i][0];	// dc swing
				*(data+1)=RX_PE_LUT[i][1];	// boost
		}
	}
}
#endif


