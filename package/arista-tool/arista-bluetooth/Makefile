# SPDX-License-Identifier: GPL-2.0-or-later
include $(TOPDIR)/rules.mk

PKG_NAME:=arista-bluetooth
PKG_VERSION:=20261115
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/arista-bluetooth
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Arista Bluetooth initialization
  DEPENDS:=@TARGET_qualcommax_ipq807x +bluez-utils +bluez-utils-extra +kmod-hci-uart +bluetooth-uart +squashfs-tools-unsquashfs +ubi-utils
  PKGARCH:=all
endef

define Package/arista-bluetooth/description
 Shared CSR8811 and Nordic controller initialization with board-specific ART addresses.
endef

define Build/Compile
endef

define Package/arista-bluetooth/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/lib/upgrade/keep.d $(1)/etc/uci-defaults
	$(INSTALL_DATA) ./files/arista-bluetooth.defaults $(1)/etc/uci-defaults/95-arista-bluetooth
	$(INSTALL_BIN) ./files/arista-bluetooth $(1)/usr/sbin/arista-bluetooth
	$(INSTALL_BIN) ./files/arista-bluetooth.init $(1)/etc/init.d/arista-bluetooth
	$(INSTALL_DATA) ./files/arista-bluetooth.keep $(1)/lib/upgrade/keep.d/arista-bluetooth
endef

$(eval $(call BuildPackage,arista-bluetooth))
