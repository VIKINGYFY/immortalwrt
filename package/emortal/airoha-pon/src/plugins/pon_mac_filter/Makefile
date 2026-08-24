KERNELDIR=$(KERNEL_DIR)

ifneq ($(strip $(TCSUPPORT_OPENWRT)),)
include $(OPENWRT_BUILD_ROOT)/BSP/xpon_bsp/xpon_dir.mak
else 
ifneq ($(strip $(RDKB_BUILD)),)
ifneq ($(strip $(ECNT_SDK_ROOT)),)
include $(ECNT_SDK_ROOT)/BSP/xpon_bsp/xpon_dir.mak
endif
endif
endif
EXTRA_CFLAGS += -I$(XPON_BSP_GLOBAL_INC_PRIVATE) -I$(XPON_BSP_GLOBAL_INC_PUBLIC)

####-Warning for error MUST at last of EXTRA_CFLAGS
EXTRA_CFLAGS+=-Werror

obj-m += ponmacfilter.o

ponmacfilter-objs := pon_mac_filter.o

all:
	$(MAKE) -C $(KERNELDIR) M=`pwd` modules
	$(STRIP) --strip-unneeded *.ko
	
clean:
	$(MAKE) -C $(KERNELDIR) M=`pwd` clean

