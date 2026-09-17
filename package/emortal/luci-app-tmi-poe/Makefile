include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-tmi-poe
PKG_VERSION:=20260918
PKG_RELEASE:=12
PKG_PO_VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)
PKG_LICENSE:=GPL-2.0-only

LUCI_TITLE:=LuCI control for Xiaomi TMI7604R/TMI7608R PoE
LUCI_DEPENDS:=@TARGET_qualcommbe +libuci +luci-base
LUCI_DESCRIPTION:=LuCI interface and native PoE control for the Xiaomi BE3600 Pro wired P5 and P8.

define Package/luci-app-tmi-poe/conffiles
/etc/config/tmi-poe
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
