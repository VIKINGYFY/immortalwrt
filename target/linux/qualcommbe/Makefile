include $(TOPDIR)/rules.mk

ARCH:=aarch64
BOARD:=qualcommbe
BOARDNAME:=Qualcomm Atheros 802.11be WiSoC-s
FEATURES:=squashfs ramdisk fpu nand rtc emmc
KERNELNAME:=Image
CPU_TYPE:=cortex-a53
SUBTARGETS:=ipq53xx ipq95xx

KERNEL_PATCHVER:=6.18

include $(INCLUDE_DIR)/target.mk
DEFAULT_PACKAGES += \
	autocore automount cpufreq e2fsprogs f2fs-tools losetup luci uboot-envtools wpad-openssl \
	kmod-qcom-ppe kmod-qrtr-mhi kmod-qrtr-smd \
	kmod-ath12k kmod-dsa kmod-dsa-qca8k kmod-phy-aquantia kmod-phy-qca83xx \
	kmod-fs-ext4 kmod-fs-f2fs kmod-gpio-button-hotplug kmod-leds-gpio kmod-leds-pwm \
	kmod-usb3 kmod-usb-dwc3 kmod-usb-dwc3-qcom kmod-usb-serial-qualcomm

$(eval $(call BuildTarget))
