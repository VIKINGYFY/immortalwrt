# SPDX-License-Identifier: GPL-2.0-or-later
include $(TOPDIR)/rules.mk
PKG_NAME:=arista-platform
PKG_VERSION:=20261115
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
include $(INCLUDE_DIR)/package.mk

define Package/arista-platform
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Arista board data and calibration
  DEPENDS:=@(TARGET_qualcommax_ipq60xx||TARGET_qualcommax_ipq807x||TARGET_qualcommbe_ipq95xx) +ubi-utils +squashfs-tools-unsquashfs +uci +flock
  PKGARCH:=all
endef

define Build/Compile
endef

define Package/arista-platform/install
	$(INSTALL_DIR) $(1)/lib/preinit $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/board.d $(1)/lib/functions $(1)/usr/libexec
	$(INSTALL_DATA) ./files/arista-transaction.sh ./files/arista-config.sh $(1)/lib/functions/
	$(INSTALL_BIN) ./files/arista-wifi-prepare $(1)/usr/libexec/arista-wifi-prepare
	$(INSTALL_BIN) ./files/arista-boarddata $(1)/usr/sbin/arista-boarddata
	$(INSTALL_BIN) ./files/arista-boarddata.init $(1)/etc/init.d/arista-boarddata
	$(INSTALL_BIN) ./files/arista-wlan-country $(1)/etc/board.d/05_arista_wlan_country
	$(INSTALL_DATA) ./files/arista-platform.preinit $(1)/lib/preinit/81_arista_platform
endef

$(eval $(call BuildPackage,arista-platform))
