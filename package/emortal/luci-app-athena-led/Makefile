include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-athena-led
PKG_DEPENDS:=
PKG_VERSION:=0.0.7
PKG_RELEASE:=20241029

include $(INCLUDE_DIR)/package.mk

# 包定义;指示我们的包将如何以及在哪里出现在整体配置菜单中 （'make menuconfig'）
define Package/$(PKG_NAME)
  TITLE:=$(PKG_NAME)
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  PKGARCH:=all
endef

define Package/luci-i18n-athena-led-zh-cn
  TITLE:=Chinese translation for $(PKG_NAME)
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  PKGARCH:=all
  DEPENDS:=$(PKG_NAME)
endef

define Build/Compile
endef

# Package description; a more verbose description on what our package does
define Package/$(PKG_NAME)/description
  LuCI support for athenaLed
endef

define Package/luci-i18n-athena-led-zh-cn/description
  Chinese translation for LuCI support for athenaLed
endef


define Package/$(PKG_NAME)/install
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci
	cp -pR ./luasrc/* $(1)/usr/lib/lua/luci
	$(INSTALL_DIR) $(1)/
	cp -pR ./root/* $(1)/
endef

define Package/$(PKG_NAME)/postinst
#!/bin/sh
	chmod +x /usr/sbin/athena-led
	chmod +x /etc/init.d/athena_led
exit 0
endef

define Package/luci-i18n-athena-led-zh-cn/install
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/i18n
	$(INSTALL_DATA) ./po/zh-cn/athena_led.po $(1)/usr/lib/lua/luci/i18n/athena_led.zh-cn.po
	$(INSTALL_DATA) ./po/zh_Hans/athena_led.po $(1)/usr/lib/lua/luci/i18n/athena_led.zh_Hans.po
endef

# This command is always the last, it uses the definitions and variables we give above in order to get the job done
$(eval $(call BuildPackage,$(PKG_NAME)))
$(eval $(call BuildPackage,luci-i18n-athena-led-zh-cn))
