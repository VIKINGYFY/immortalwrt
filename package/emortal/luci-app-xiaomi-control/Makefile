# SPDX-License-Identifier: GPL-2.0-or-later

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-xiaomi-control
PKG_VERSION:=20261112
PKG_RELEASE:=1
PKG_PO_VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)
PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=ViKing <viking.yfy@qq.com>

LUCI_TITLE:=LuCI Xiaomi fan and RGB lighting control
LUCI_DEPENDS:=@TARGET_qualcommbe_ipq95xx +luci-base +rpcd-mod-file \
	+ucode +ucode-mod-fs +ucode-mod-uci +ucode-mod-uloop +ucode-mod-ubus \
	+kmod-hwmon-emc2305 +kmod-leds-pwm
LUCI_PKGARCH:=all

define Package/$(PKG_NAME)/conffiles
/etc/config/xiaomi-control
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
