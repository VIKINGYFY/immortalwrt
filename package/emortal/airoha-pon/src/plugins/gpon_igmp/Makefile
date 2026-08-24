
KERNELDIR=$(KERNEL_DIR)
ifneq ($(strip $(TCSUPPORT_OPENWRT)),)
include $(OPENWRT_BUILD_ROOT)/BSP/xpon_bsp/xpon_dir.mak
endif
EXTRA_CFLAGS += -I$(XPON_BSP_GLOBAL_INC_PRIVATE) -I$(XPON_BSP_GLOBAL_INC_PUBLIC) -I$(XPON_BSP_ECNT_HOOK_DIR)/include

EXTRA_CFLAGS += \
	-I$(GLOBAL_INC_DIR)/lan_port/  \
#	-I$(GLOBAL_INC_DIR)/modules/  \

EXTRA_CFLAGS+=-Werror

obj-m += gpon_igmp.o
gpon_igmp-objs := xpon_igmp_itf.o xpon_igmp_core.o

ifneq ($(TC_CFLAGS),)
EXTRA_CFLAGS+=$(TC_CFLAGS)
endif

all:
	$(MAKE) -C $(KERNELDIR) M=`pwd` modules
	$(STRIP) --strip-unneeded gpon_igmp.ko

clean:
	$(MAKE) -C $(KERNELDIR) M=`pwd` clean
