include $(TOPDIR)/rules.mk

PKG_NAME:=airscan
PKG_VERSION:=0.2
PKG_RELEASE:=19
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/airscan
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Spectrum, BSS and client survey on a dedicated scan radio
  DEPENDS:=+arista-platform +ucode +ucode-mod-fs +ucode-mod-uloop +ucode-mod-ubus +ucode-mod-uci \
	+ucode-mod-struct +ucode-mod-socket +ucode-mod-nl80211 +iw
endef

define Package/airscan/description
 Runs spectral scans (ath10k, ath11k), passive BSS scans and a monitor-mode client
 survey on a board's dedicated scan radio, never on a serving radio, plus a
 Bluetooth LE survey on the board's BLE controller (hci0), and
 publishes the results on ubus (object "airscan") and in /tmp/airscan.
 Arista C-360: the QCN9074 multifunction radio (PCI 0001:01:00.0).
 Arista C-260: the QCA988x third radio (PCI 0000:01:00.0).
endef

define Package/airscan/conffiles
/etc/config/airscan
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -o $(PKG_BUILD_DIR)/airscan-fft \
		$(PKG_BUILD_DIR)/airscan-fft.c -lm
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -o $(PKG_BUILD_DIR)/airscan-ble \
		$(PKG_BUILD_DIR)/airscan-ble.c
endef

define Package/airscan/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/libexec $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/airscan-fft $(1)/usr/libexec/airscan-fft
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/airscan-ble $(1)/usr/libexec/airscan-ble
	$(INSTALL_BIN) ./files/airscan-config $(1)/usr/libexec/airscan-config
	$(INSTALL_BIN) ./files/airscan.uc $(1)/usr/sbin/airscan
	$(INSTALL_BIN) ./files/airscan.init $(1)/etc/init.d/airscan
	$(INSTALL_CONF) ./files/airscan.config $(1)/etc/config/airscan
endef

$(eval $(call BuildPackage,airscan))
