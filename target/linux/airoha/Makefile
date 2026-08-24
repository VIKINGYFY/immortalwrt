include $(TOPDIR)/rules.mk

ARCH:=arm
BOARD:=airoha
BOARDNAME:=Airoha ARM
SUBTARGETS:=en7523 an7581 an7583
FEATURES:=dt squashfs nand ramdisk gpio

KERNEL_PATCHVER:=6.18

include $(INCLUDE_DIR)/target.mk
DEFAULT_PACKAGES += \
	autocore automount cpufreq e2fsprogs f2fs-tools losetup luci uboot-envtools wpad-openssl \
	kmod-gpio-button-hotplug kmod-leds-gpio kmod-leds-pwm \
	kmod-fs-ext4 kmod-fs-f2fs kmod-usb3 kmod-usb-dwc3

$(eval $(call BuildTarget))
