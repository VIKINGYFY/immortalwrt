include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-athena-led
PKG_VERSION:=1.0
PKG_RELEASE:=20260610

LUCI_TITLE:=LuCI Support for JDCloud Athena LED Screen Control
LUCI_DEPENDS:=+luci-base @aarch64
LUCI_PKGARCH:=

define Package/$(PKG_NAME)/conffiles
/etc/config/athena_led
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
