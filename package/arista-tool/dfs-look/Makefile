include $(TOPDIR)/rules.mk

PKG_NAME:=dfs-look
PKG_VERSION:=0.1
PKG_RELEASE:=6
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/dfs-look
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Move a 5 GHz radio to a DFS channel only after a look
  DEPENDS:=+ucode +ucode-mod-fs +ucode-mod-uloop +ucode-mod-ubus +ucode-mod-nl80211 +ucode-mod-socket
  PKGARCH:=all
endef

define Package/dfs-look/description
 Optional automatic 5 GHz channel evaluation and DFS channel switching through
 hostapd. Disabled by default. Does not change local ACS defaults, radar
 detection, CAC or non-occupancy handling. An enabled switch may interrupt AP
 service for CAC; fixed channels and acs_exclude_dfs are left alone.
endef

define Package/dfs-look/conffiles
/etc/config/dfs-look
endef

define Build/Compile
endef

define Package/dfs-look/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) ./files/dfs-look.uc $(1)/usr/sbin/dfs-look
	$(INSTALL_CONF) ./files/dfs-look.config $(1)/etc/config/dfs-look
	$(INSTALL_BIN) ./files/dfs-look.init $(1)/etc/init.d/dfs-look
endef

$(eval $(call BuildPackage,dfs-look))
