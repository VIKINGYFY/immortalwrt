include $(TOPDIR)/rules.mk

PKG_NAME:=cr1000a-recovery
PKG_RELEASE:=1
PKG_FLAGS:=nonshared

include $(INCLUDE_DIR)/package.mk

define Package/cr1000a-recovery
	SECTION:=utils
	CATEGORY:=Base system
	TITLE:=CR1000A recovery FIT utility
	DEPENDS:=@TARGET_qualcommax_ipq807x +dumpimage +fdt-utils +coreutils-sha1sum +uboot-envtools
endef

define Package/cr1000a-recovery/description
	Command-line utility for installing and managing the CR1000A recovery FIT.
endef

define Build/Prepare
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/cr1000a-recovery/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/cr1000a-recovery $(1)/usr/sbin/
endef

$(eval $(call BuildPackage,cr1000a-recovery))
