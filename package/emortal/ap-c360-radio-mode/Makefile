include $(TOPDIR)/rules.mk

PKG_NAME:=ap-c360-radio-mode
PKG_RELEASE:=1
PKG_FLAGS:=nonshared

include $(INCLUDE_DIR)/package.mk

define Package/ap-c360-radio-mode
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Arista AP-C360 radio mode selector
  HIDDEN:=1
  DEPENDS:=+uci
endef

define Package/ap-c360-radio-mode/description
Selects the Arista AP-C360 switchable radio mode before ath11k loads.
The default mode is 5G-H; 6G can be selected over SSH and takes effect
after reboot.
endef

define Package/ap-c360-radio-mode/conffiles
/etc/config/ap_c360
endef

define Build/Compile
endef

define Package/ap-c360-radio-mode/install
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/ap_c360.config $(1)/etc/config/ap_c360
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/ap-c360-radio-mode.init $(1)/etc/init.d/ap-c360-radio-mode
	$(INSTALL_DIR) $(1)/etc/rc.d
	$(LN) ../init.d/ap-c360-radio-mode $(1)/etc/rc.d/S05ap-c360-radio-mode
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/ap-c360-radio-mode $(1)/usr/sbin/ap-c360-radio-mode
	$(INSTALL_DIR) $(1)/usr/share/ap-c360-radio-mode
	$(CP) ./files/usr/share/ap-c360-radio-mode/. $(1)/usr/share/ap-c360-radio-mode/
endef

define Package/ap-c360-radio-mode/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && {
	mkdir -p "$${IPKG_INSTROOT}/etc/rc.d"
	ln -sf ../init.d/ap-c360-radio-mode \
		"$${IPKG_INSTROOT}/etc/rc.d/S05ap-c360-radio-mode"
	exit 0
}

/etc/init.d/ap-c360-radio-mode enable
exit 0
endef

$(eval $(call BuildPackage,ap-c360-radio-mode))
