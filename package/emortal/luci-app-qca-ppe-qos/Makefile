# Copyright 2026 Julius Bairaktaris <julius@bairaktaris.de>
# Licensed to the public under the Apache License 2.0.

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-qca-ppe-qos
PKG_VERSION:=20261116
PKG_RELEASE:=5
PKG_PO_VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)

PKG_MAINTAINER:=Julius Bairaktaris <julius@bairaktaris.de>
PKG_LICENSE:=Apache-2.0

LUCI_TITLE:=LuCI support for Qualcomm PPE hardware QoS
LUCI_DEPENDS:=+luci-base +qca-ppe-qos
LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
