# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-fullconenat-sonic
PKG_VERSION:=20260917
PKG_RELEASE:=5
PKG_PO_VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)
PKG_LICENSE:=GPL-2.0-only

LUCI_TITLE:=LuCI support for SONiC full cone NAT
LUCI_DEPENDS:=@LINUX_6_18 @(PACKAGE_firewall4||PACKAGE_firewall) +fullconenat-sonic luci-app-firewall
LUCI_DEFAULT:=y if PACKAGE_luci-app-firewall && (PACKAGE_firewall4 || PACKAGE_firewall)
LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
