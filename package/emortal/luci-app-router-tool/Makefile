# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-router-tool
PKG_VERSION:=20261127
PKG_RELEASE:=6
PKG_PO_VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)
PKG_LICENSE:=GPL-2.0-only

LUCI_TITLE:=LuCI Router Tool presets
LUCI_DEPENDS:=luci-mod-network +luci-mod-status +odhcp6c +odhcpd-ipv6only +firewall4 +ucode-mod-fs +ucode-mod-log +ucode-mod-uci +ucode-mod-ubus
LUCI_DEFAULT:=y if PACKAGE_luci-mod-network
LUCI_PKGARCH:=all

define Package/$(PKG_NAME)/conffiles
/etc/config/router-tool
/etc/router-tool/
endef

# Extend private copies of the feed views and the overview template.
LUCI_NETWORK_SOURCE:=$(TOPDIR)/feeds/luci/modules/luci-mod-network
LUCI_STATUS_SOURCE:=$(TOPDIR)/feeds/luci/modules/luci-mod-status
LUCI_VIEW_DIR:=htdocs/luci-static/resources/view
LUCI_NETWORK_VIEW:=$(LUCI_VIEW_DIR)/network/interfaces.js
LUCI_WIRELESS_VIEW:=$(LUCI_VIEW_DIR)/network/wireless.js
LUCI_STATUS_VIEW:=$(LUCI_VIEW_DIR)/status/index.js
LUCI_PORTS_VIEW:=$(LUCI_VIEW_DIR)/status/include/29_ports.js
LUCI_STATUS_TEMPLATE:=ucode/template/admin_status/index.ut
ROUTER_TOOL_STATUS_TEMPLATE:=root/usr/share/ucode/luci/template/admin_status/overview-router-tool.ut
PKG_FILE_DEPENDS:=$(LUCI_NETWORK_SOURCE)/$(LUCI_NETWORK_VIEW) \
	$(LUCI_NETWORK_SOURCE)/$(LUCI_WIRELESS_VIEW) \
	$(LUCI_STATUS_SOURCE)/$(LUCI_PORTS_VIEW) $(LUCI_STATUS_SOURCE)/$(LUCI_STATUS_VIEW) \
	$(LUCI_STATUS_SOURCE)/$(LUCI_STATUS_TEMPLATE)

define Build/Prepare/luci-app-router-tool
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/$(LUCI_VIEW_DIR)/network
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/$(LUCI_VIEW_DIR)/status
	$(INSTALL_DIR) $(dir $(PKG_BUILD_DIR)/$(ROUTER_TOOL_STATUS_TEMPLATE))
	$(CP) $(LUCI_NETWORK_SOURCE)/$(LUCI_NETWORK_VIEW) $(PKG_BUILD_DIR)/$(LUCI_VIEW_DIR)/network/interfaces-router-tool.js
	$(CP) $(LUCI_NETWORK_SOURCE)/$(LUCI_WIRELESS_VIEW) $(PKG_BUILD_DIR)/$(LUCI_VIEW_DIR)/network/wireless-router-tool.js
	$(CP) $(LUCI_STATUS_SOURCE)/$(LUCI_STATUS_VIEW) $(PKG_BUILD_DIR)/$(LUCI_VIEW_DIR)/status/index-router-tool.js
	$(CP) $(LUCI_STATUS_SOURCE)/$(LUCI_PORTS_VIEW) $(PKG_BUILD_DIR)/$(LUCI_VIEW_DIR)/status/ports-router-tool.js
	$(CP) $(LUCI_STATUS_SOURCE)/$(LUCI_STATUS_TEMPLATE) $(PKG_BUILD_DIR)/$(ROUTER_TOOL_STATUS_TEMPLATE)
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
