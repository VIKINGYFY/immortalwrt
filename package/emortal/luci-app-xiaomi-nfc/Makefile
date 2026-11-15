include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-xiaomi-nfc
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_PO_VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)
PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=zh8416
LUCI_TITLE:=Xiaomi NFC tag configuration
LUCI_DEPENDS:=+luci-base +rpcd +bash +flock +jshn +i2c-tools +coreutils-sleep
LUCI_DEFAULT:=y
LUCI_PKGARCH:=all

define Package/$(PKG_NAME)/conffiles
/etc/config/xiaomi_nfc
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
