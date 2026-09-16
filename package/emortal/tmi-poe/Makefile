include $(TOPDIR)/rules.mk

PKG_NAME:=tmi-poe
PKG_VERSION:=20260917
PKG_RELEASE:=4
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/tmi-poe
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Xiaomi TMI7604R/TMI7608R PoE control
  DEPENDS:=@TARGET_qualcommbe +libuci
  KCONFIG:=CONFIG_I2C_CHARDEV=y CONFIG_GPIO_CDEV=y
endef

define Package/tmi-poe/description
 Native PoE control and status monitoring for the Xiaomi BE3600 Pro wired
 P5 and P8. Uses the board-specific factory initialization and power budget.
endef

define Package/tmi-poe/conffiles
/etc/config/tmi-poe
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -Wall -Wextra -Werror \
		-o $(PKG_BUILD_DIR)/tmi-poe $(PKG_BUILD_DIR)/main.c \
		$(PKG_BUILD_DIR)/tmi.c $(PKG_BUILD_DIR)/log.c $(TARGET_LDFLAGS) -luci
endef

define Package/tmi-poe/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/tmi-poe $(1)/usr/sbin/tmi-poe
	$(INSTALL_BIN) ./files/tmi-poe.init $(1)/etc/init.d/tmi-poe
	$(INSTALL_CONF) ./files/tmi-poe.config $(1)/etc/config/tmi-poe
endef

$(eval $(call BuildPackage,tmi-poe))
