ARCH:=aarch64
SUBTARGET:=filogic
BOARDNAME:=Filogic 8x0 (MT798x)
CPU_TYPE:=cortex-a53
KERNELNAME:=Image dtbs
DEFAULT_PROFILE:=openwrt_one
DEFAULT_PACKAGES += \
	autocore automount cpufreq e2fsprogs f2fs-tools losetup luci uboot-envtools wpad-openssl \
	kmod-gpio-button-hotplug kmod-leds-gpio kmod-leds-pwm \
	kmod-fs-ext4 kmod-fs-f2fs kmod-usb3 kmod-usb-dwc3 \
	bridger fitblk kmod-crypto-hw-safexcel kmod-phy-realtek

define Target/Description
	Build firmware images for MediaTek Filogic ARM based boards.
endef
