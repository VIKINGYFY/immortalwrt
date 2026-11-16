# SPDX-License-Identifier: GPL-2.0-or-later

include $(TOPDIR)/rules.mk

PKG_NAME:=arista-radio
PKG_VERSION:=20261115
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/arista-radio
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Arista C-360 radio plan (6 GHz or dual 5 GHz)
  DEPENDS:=@TARGET_qualcommax_ipq807x +arista-platform
  PKGARCH:=all
endef

define Package/arista-radio/description
  Switches the Arista C-360 between Arista's two radio plans: 6 GHz (the
  IPQ8074 on 2.4 GHz and all of 5 GHz, the serving QCN9074 on 6 GHz) and
  dual 5 GHz (the IPQ8074 on lower 5 GHz, the serving QCN9074 on upper
  5 GHz), using GPIO9, ART calibration and the upstream country/plan BDF
  selection supplied by arista-platform. Runtime firmware stays DD-WRT.
endef

define Package/arista-radio/conffiles
/etc/config/arista-radio
endef

define Build/Compile
endef

define Package/arista-radio/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/config
	$(INSTALL_BIN) ./files/arista-radio-mode $(1)/usr/sbin/arista-radio-mode
	$(INSTALL_CONF) ./files/arista-radio.config $(1)/etc/config/arista-radio
endef

$(eval $(call BuildPackage,arista-radio))
