include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=nss-wifi-drv
PKG_VERSION:=20261101
PKG_RELEASE:=2
PKG_CONFIG_DEPENDS:=CONFIG_ATH11K_NSS_WIFI CONFIG_NSS_WIFI_DEBUG
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/nss-wifi-drv
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS firmware host driver
  DEPENDS:=@(TARGET_qualcommax_ipq807x||TARGET_qualcommax_ipq60xx) \
	+nss-firmware +kmod-nft-bridge
  FILES:=$(PKG_BUILD_DIR)/nss-wifi-drv.ko
endef

define KernelPackage/nss-wifi-drv/config
  config NSS_WIFI_DEBUG
    bool "Enable active NSS/WLAN diagnostic probes"
    depends on PACKAGE_kmod-nss-wifi-drv
    default n
    help
      Exposes probes and experimental ring ownership parameters. Leave
      disabled for normal firmware; passive status remains available.
endef

define KernelPackage/nss-wifi-drv/conffiles
/etc/config/nss-wifi
endef

define KernelPackage/nss-wifi-drv/description
  Boots the NSS firmware for Wi-Fi and generic redirect offload. Raw-IP
  and USB Ethernet devices use their host drivers with NSS forwarding,
  including on devices without ath11k. The wired switch ports belong to
  qca_edma and qca_ppe.

  The module does not autoload: the nss-wifi service loads it and boots a
  core when nss-wifi.general.offload is 1, the default. The wired ports
  never depend on it. nss-wifi.general.firmware picks the firmware line,
  12.5 by default or 11.4 for 802.11s mesh.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" CONFIG_NSS_WIFI_DEBUG=$(if $(CONFIG_NSS_WIFI_DEBUG),y,n) modules
endef

# The WLAN driver describes its rings through this, and is built by another
# package, so the description has to be reachable from the staging tree.
define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/nss-wifi-drv/linux/soc/qcom
	$(CP) ./src/include/linux/soc/qcom/nss_wifi.h \
		$(1)/usr/include/nss-wifi-drv/linux/soc/qcom/
endef

define KernelPackage/nss-wifi-drv/install
	$(INSTALL_DIR) $(1)/etc/config $(1)/etc/init.d
	$(INSTALL_CONF) ./files/nss-wifi.config $(1)/etc/config/nss-wifi
	$(INSTALL_BIN) ./files/nss-wifi.init $(1)/etc/init.d/nss-wifi
	$(INSTALL_DIR) $(1)/etc/uci-defaults $(1)/usr/share/nss-wifi
	echo $(if $(CONFIG_ATH11K_NSS_WIFI),1,0) > $(1)/usr/share/nss-wifi/wlan
	$(INSTALL_BIN) ./files/nss-wifi.uci-defaults $(1)/etc/uci-defaults/90-nss-wifi-flowtable
	$(INSTALL_BIN) ./files/nss-wifi.flowtable $(1)/usr/share/nss-wifi/flowtable.sh
endef

# The firewall include the uci-defaults script adds goes with the package,
# or every reload would warn about a missing script.
define Package/kmod-nss-wifi-drv/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
uci -q delete firewall.nss_wifi_flowtable && uci -q commit firewall
exit 0
endef

$(eval $(call KernelPackage,nss-wifi-drv))
