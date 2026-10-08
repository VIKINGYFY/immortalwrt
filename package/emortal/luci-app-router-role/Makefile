# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-router-role
PKG_VERSION:=20261007
PKG_RELEASE:=5
PKG_PO_VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)
PKG_LICENSE:=GPL-2.0-only

LUCI_TITLE:=LuCI Router Role presets
LUCI_DEPENDS:=luci-mod-network +odhcp6c +odhcpd-ipv6only +firewall4 +ucode-mod-fs +ucode-mod-log +ucode-mod-uci +ucode-mod-ubus
LUCI_DEFAULT:=y if PACKAGE_luci-mod-network
LUCI_PKGARCH:=all

# Copy the feed view and extend it in this package.  The feed checkout stays
# untouched, so a later feed update cannot remove the integration.
PKG_FILE_DEPENDS:=$(TOPDIR)/feeds/luci/modules/luci-mod-network/htdocs/luci-static/resources/view/network/interfaces.js

define Build/Prepare/luci-app-router-role
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/htdocs/luci-static/resources/view/network
	$(CP) $(PKG_FILE_DEPENDS) $(PKG_BUILD_DIR)/htdocs/luci-static/resources/view/network/interfaces-router-role.js
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
