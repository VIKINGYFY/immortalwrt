include $(TOPDIR)/rules.mk

PKG_NAME:=apc360-devmem
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/apc360-devmem
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Small /dev/mem helper for Arista AP-C360 RF strap registers
endef

define Package/apc360-devmem/description
 Minimal mmap-based /dev/mem helper used by AP-C360 radio mode setup.
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) \
		-o $(PKG_BUILD_DIR)/apc360-devmem $(PKG_BUILD_DIR)/apc360-devmem.c $(TARGET_LDFLAGS)
endef

define Package/apc360-devmem/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/apc360-devmem $(1)/usr/sbin/apc360-devmem
endef

$(eval $(call BuildPackage,apc360-devmem))
