# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

BOARD:=rockchip
BOARDNAME:=Rockchip
FEATURES:=ext4 audio usb usbgadget display gpio fpu pci pcie rootfs-part boot-part squashfs
SUBTARGETS:=armv8

KERNEL_PATCHVER:=6.18

define Target/Description
	Build firmware image for Rockchip SoC devices.
endef

include $(INCLUDE_DIR)/target.mk

DEFAULT_PACKAGES += \
	autocore automount cpufreq e2fsprogs f2fs-tools losetup luci uboot-envtools wpad-openssl \
	kmod-gpio-button-hotplug kmod-leds-gpio kmod-leds-pwm \
	kmod-fs-ext4 kmod-fs-f2fs kmod-usb3 kmod-usb-dwc3 \
	partx-utils kmod-phy-realtek

KERNELNAME:=Image dtbs

$(eval $(call BuildTarget))
