# SPDX-License-Identifier: GPL-2.0-or-later
include $(TOPDIR)/rules.mk

PKG_NAME:=arista-power
PKG_VERSION:=20261115
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/arista-power
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Arista PoE power policy
  DEPENDS:=@TARGET_qualcommax_ipq807x +iw +iwinfo +jsonfilter +lldpd
  PKGARCH:=all
endef

define Package/arista-power/description
 Shared PoE, radio-chain and peripheral power policy for C-230, C-260 and C-360.
endef

define Package/arista-power/conffiles
/etc/config/arista-power
endef

define Build/Compile
endef

define Package/arista-power/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/arista-power $(1)/usr/sbin/arista-power
	$(INSTALL_BIN) ./files/arista-power.init $(1)/etc/init.d/arista-power
	$(INSTALL_CONF) ./files/arista-power.config $(1)/etc/config/arista-power
	$(INSTALL_DATA) ./files/arista-power.lldpd $(1)/etc/uci-defaults/95-arista-lldpd
endef

$(eval $(call BuildPackage,arista-power))
