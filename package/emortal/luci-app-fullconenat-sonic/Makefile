# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-fullconenat-sonic
PKG_VERSION:=20260917
PKG_RELEASE:=13
PKG_PO_VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)
PKG_LICENSE:=GPL-2.0-only

LUCI_TITLE:=LuCI support for SONiC full cone NAT
LUCI_DEPENDS:=@LINUX_6_18 @(PACKAGE_firewall4||PACKAGE_firewall) +fullconenat-sonic luci-app-firewall
LUCI_DEFAULT:=y if PACKAGE_luci-app-firewall && (PACKAGE_firewall4 || PACKAGE_firewall)
LUCI_PKGARCH:=all

# Extend the feed's current zone form without maintaining a second copy of it.
PKG_FILE_DEPENDS:=$(TOPDIR)/feeds/luci/applications/luci-app-firewall/htdocs/luci-static/resources/view/firewall/zones.js

define Build/Prepare/luci-app-fullconenat-sonic
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/htdocs/luci-static/resources/view/firewall
	$(CP) $(PKG_FILE_DEPENDS) $(PKG_BUILD_DIR)/htdocs/luci-static/resources/view/firewall/zones-sonic.js
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
