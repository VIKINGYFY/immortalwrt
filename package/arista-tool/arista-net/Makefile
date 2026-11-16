# SPDX-License-Identifier: GPL-2.0-or-later
include $(TOPDIR)/rules.mk

PKG_NAME:=arista-net
PKG_VERSION:=20261115
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/arista-net
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Arista Ethernet and VLAN configuration
  DEPENDS:=@TARGET_qualcommax_ipq807x +arista-platform +uci +jsonfilter +kmod-bonding +vxlan
  PKGARCH:=all
endef

define Package/arista-net/description
 Shared two-port VLAN, trunk, VXLAN and LACP configuration with timed rollback.
endef

define Build/Compile
endef

define Package/arista-net/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/arista-net $(1)/usr/sbin/arista-net
endef

$(eval $(call BuildPackage,arista-net))
