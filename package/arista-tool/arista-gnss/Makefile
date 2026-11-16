# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=arista-gnss
PKG_VERSION:=20261115
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/arista-gnss
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Arista C-460 GNSS receiver (Quectel LC79H on I2C) for gpsd
  DEPENDS:=@TARGET_qualcommbe_ipq95xx +gpsd
endef

define Package/arista-gnss/description
  Reads the NMEA output of the Arista C-460's GNSS receiver, a Quectel
  LC79H that the board connects by I2C only, and offers it on a pty,
  /var/run/lc79h-gnss, which gpsd is set up to read on first boot. The
  receiver is reset when the reader starts; only read commands go to it,
  so its settings stay as they are.
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(PKG_BUILD_DIR)/lc79h-i2c $(PKG_BUILD_DIR)/lc79h-i2c.c
endef

define Package/arista-gnss/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/uci-defaults
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/lc79h-i2c $(1)/usr/sbin/lc79h-i2c
	$(INSTALL_BIN) ./files/arista-gnss.init $(1)/etc/init.d/arista-gnss
	$(INSTALL_DATA) ./files/arista-gnss.defaults $(1)/etc/uci-defaults/95-arista-gnss
endef

$(eval $(call BuildPackage,arista-gnss))
