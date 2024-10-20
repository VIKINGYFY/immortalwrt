include $(TOPDIR)/rules.mk

ARCH:=aarch64
BOARD:=qualcommax
BOARDNAME:=Qualcomm Atheros 802.11ax WiSoC-s
FEATURES:=squashfs ramdisk fpu nand rtc emmc
KERNELNAME:=Image
CPU_TYPE:=cortex-a53
SUBTARGETS:=ipq807x ipq60xx

KERNEL_PATCHVER:=6.6

include $(INCLUDE_DIR)/target.mk
DEFAULT_PACKAGES += \
	autocore cpufreq e2fsprogs losetup luci uboot-envtools wpad-openssl \
	kmod-fs-ext4 kmod-gpio-button-hotplug kmod-leds-gpio kmod-usb3 kmod-usb-dwc3 kmod-usb-dwc3-qcom \
	kmod-ath11k kmod-ath11k-ahb kmod-ath11k-pci ath11k-firmware-qcn9074 kmod-phy-aquantia \
	kmod-qca-nss-dp \
	kmod-qca-nss-drv \
	kmod-qca-nss-drv-bridge-mgr \
	kmod-qca-nss-drv-eogremgr \
	kmod-qca-nss-drv-gre \
	kmod-qca-nss-drv-igs \
	kmod-qca-nss-drv-l2tpv2 \
	kmod-qca-nss-drv-lag-mgr \
	kmod-qca-nss-drv-map-t \
	kmod-qca-nss-drv-match \
	kmod-qca-nss-drv-mirror \
	kmod-qca-nss-drv-pppoe \
	kmod-qca-nss-drv-pptp \
	kmod-qca-nss-drv-qdisc \
	kmod-qca-nss-drv-tun6rd \
	kmod-qca-nss-drv-tunipip6 \
	kmod-qca-nss-drv-vlan-mgr \
	kmod-qca-nss-drv-vxlanmgr \
	kmod-qca-nss-drv-wifi-meshmgr \
	kmod-qca-nss-ecm \
	kmod-qca-nss-macsec \

$(eval $(call BuildTarget))
