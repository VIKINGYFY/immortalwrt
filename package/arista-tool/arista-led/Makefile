# SPDX-License-Identifier: GPL-2.0-or-later
include $(TOPDIR)/rules.mk
PKG_NAME:=arista-led
PKG_VERSION:=20261115
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
include $(INCLUDE_DIR)/package.mk

define Package/arista-led
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Arista wireless LED binding
  DEPENDS:=@(TARGET_qualcommax_ipq60xx||TARGET_qualcommax_ipq807x||TARGET_qualcommbe_ipq95xx) +iw
  PKGARCH:=all
endef

define Package/arista-led/description
 Bind wireless LEDs to their radios by hardware path on C-200, C-230,
 C-260, C-360 and C-460, independent of PHY enumeration order.
endef

define Build/Compile
endef

define Package/arista-led/install
	$(INSTALL_DIR) $(1)/usr/libexec $(1)/etc/init.d $(1)/etc/hotplug.d/ieee80211
	$(INSTALL_BIN) ./files/arista-led $(1)/usr/libexec/arista-led
	$(INSTALL_BIN) ./files/arista-led.init $(1)/etc/init.d/arista-led
	$(INSTALL_DATA) ./files/arista-led.hotplug $(1)/etc/hotplug.d/ieee80211/20-arista-led
endef

$(eval $(call BuildPackage,arista-led))
