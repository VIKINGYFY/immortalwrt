ARCH:=aarch64
SUBTARGET:=an7581
BOARDNAME:=AN7581 / AN7566 / AN7551
CPU_TYPE:=cortex-a53
KERNELNAME:=Image dtbs
FEATURES+=pwm

DEFAULT_PACKAGES += \
	airoha-en7581-npu-firmware kmod-nf-conntrack-bridge uboot-envtools \
	kmod-airoha-pon-frontend kmod-airoha-xpon \
	airoha-ponctl airoha-pond luci-app-pon luci-i18n-pon-zh-cn

define Target/Description
	Build firmware images for Airoha an7581 ARM based boards.
endef
