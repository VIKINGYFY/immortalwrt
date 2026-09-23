ARCH:=aarch64
SUBTARGET:=an7583
BOARDNAME:=AN7583
CPU_TYPE:=cortex-a53
KERNELNAME:=Image dtbs
FEATURES+=pwm

DEFAULT_PACKAGES += \
	airoha-an7583-npu-firmware kmod-leds-gpio \
	kmod-gpio-button-hotplug uboot-envtools \
	kmod-airoha-pon-frontend kmod-airoha-xpon \
	airoha-ponctl airoha-pond luci-app-pon luci-i18n-pon-zh-cn

define Target/Description
	Build firmware images for Airoha an7583 ARM based boards.
endef

