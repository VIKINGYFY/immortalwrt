#ifndef _DRV_TYPES_H_
#define _DRV_TYPES_H_

#include <linux/types.h>

#include <linux/uaccess.h>		// support kernel 5.4, Jeff 20220826
#include <linux/version.h>		// support kernel 5.4, Jeff 20220826

#if !( LINUX_VERSION_CODE > KERNEL_VERSION(6,6,0) )
#include <linux/ecnt_utility.h>	// support kernel 5.4, Jeff 20220826
#endif


#ifdef UINT32
#undef UINT32
#endif
#ifdef UINT16
#undef UINT16
#endif
#ifdef UINT8
#undef UINT8
#endif

#define BYTE_MASK	0xFF
#define WORD_MASK  	0xFFFF
#define DWORD_MASK  0xFFFFFFFF

typedef signed short        int16;

typedef unsigned int		UINT32 ;
typedef unsigned short		UINT16 ;
typedef unsigned char   	UINT8 ;

typedef unsigned int		UInt32;
typedef unsigned short		UInt16;
typedef unsigned char   	Byte;

#define PACKING
typedef unsigned int FIELD;

#ifndef VPint
#define VPint			*(volatile unsigned int *)
#endif /* VPint */

#ifndef     TRUE
#define     TRUE    1
#endif
#ifndef     FALSE
#define     FALSE   0
#endif

#ifndef NACK
#define NACK 	0
#endif
#ifndef ACK
#define ACK 1
#endif

#ifndef LDDLA_KERNEL_FS_READ	// support kernel 5.4, Jeff 20220826
#if LINUX_VERSION_CODE > KERNEL_VERSION(6,6,0)
    #define LDDLA_KERNEL_FS_READ(Osfd, PdataPtr, ReadLen, Fpos) \
	kernel_read(Osfd, PdataPtr, ReadLen, Fpos)
#elif LINUX_VERSION_CODE > KERNEL_VERSION(4,4,90)
    #define LDDLA_KERNEL_FS_READ(Osfd, PdataPtr, ReadLen, Fpos) \
	ecnt_kernel_fs_read(Osfd, PdataPtr, ReadLen, Fpos)
	#define KERNEL_READ_MSG 1
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3, 19, 0)
    #define LDDLA_KERNEL_FS_READ(Osfd, PdataPtr, ReadLen, Fpos) \
	srcf->f_op->read(Osfd, PdataPtr, ReadLen, Fpos)
	#define KERNEL_READ_MSG 2
#else
#define LDDLA_KERNEL_FS_READ(Osfd, PdataPtr, ReadLen, Fpos) \
	__vfs_read(Osfd, PdataPtr, ReadLen, Fpos)
	#define KERNEL_READ_MSG 3
#endif
#endif

#ifndef LDDLA_KERNEL_FS_WRITE	// support kernel 5.4, Jeff 20220826
#if LINUX_VERSION_CODE > KERNEL_VERSION(6,6,0)
    #define LDDLA_KERNEL_FS_WRITE(Osfd, PdataPtr, WriteLen, Fpos) \
    kernel_write(Osfd, PdataPtr, WriteLen, Fpos)
#elif LINUX_VERSION_CODE > KERNEL_VERSION(4,4,90)
    #define LDDLA_KERNEL_FS_WRITE(Osfd, PdataPtr, WriteLen, Fpos) \
	ecnt_kernel_fs_write(Osfd, PdataPtr, WriteLen, Fpos)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3, 19, 0)
    #define LDDLA_KERNEL_FS_WRITE(Osfd, PdataPtr, WriteLen, Fpos) \
	srcf->f_op->write(Osfd, PdataPtr, WriteLen, Fpos)
#else
#define LDDLA_KERNEL_FS_WRITE(Osfd, PdataPtr, WriteLen, Fpos) \
	__vfs_write(Osfd, PdataPtr, WriteLen, Fpos)
#endif
#endif

#endif /* _DRV_TYPES_H_ */

