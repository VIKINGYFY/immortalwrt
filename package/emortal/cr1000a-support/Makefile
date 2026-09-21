# SPDX-License-Identifier: GPL-2.0-only

# Vendored from https://github.com/kinglove1990/files at commit
# 62ebb91b9483bf712cc0aea315722289cbdc89fd (2025-12-09).

include $(TOPDIR)/rules.mk

PKG_NAME:=cr1000a-support
PKG_RELEASE:=1
PKG_FLAGS:=nonshared
PKG_LICENSE:=LicenseRef-Unknown

include $(INCLUDE_DIR)/package.mk

define Package/cr1000a-support
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Verizon CR1000A LAN and storage support
  DEPENDS:=@TARGET_qualcommax_ipq807x +@KERNEL_DEVMEM +cryptsetup \
    +kmod-spi-dev +ethtool +libubus +libubox +libgcc +kmod-fs-ext4 \
    +kmod-crypto-xts +kmod-crypto-cbc +kmod-crypto-sha256
endef

define Package/cr1000a-support/description
Userspace helpers and firmware required by the Verizon CR1000A RTL9303 LAN
switch, Aquantia WAN PHY and encrypted eMMC data partition.
endef

define Build/Compile
	$(CP) ./files/lib/rtl/usrApp $(PKG_BUILD_DIR)/usrApp
	# Match the installed ABI-qualified libraries without writing to /lib.
	$(STAGING_DIR_HOST)/bin/patchelf \
		--replace-needed libubus.so $(notdir $(realpath $(STAGING_DIR)/usr/lib/libubus.so)) \
		--replace-needed libubox.so $(notdir $(realpath $(STAGING_DIR)/usr/lib/libubox.so)) \
		$(PKG_BUILD_DIR)/usrApp
endef

define Package/cr1000a-support/install
	$(INSTALL_DIR) $(1)/lib/rtl $(1)/lib/aqr $(1)/lib/mmc $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/usrApp $(1)/lib/rtl/usrApp
	$(INSTALL_BIN) ./files/lib/rtl/init.sh $(1)/lib/rtl/init.sh
	$(INSTALL_BIN) ./files/lib/aqr/aq-fw-download $(1)/lib/aqr/aq-fw-download
	$(INSTALL_DATA) ./files/lib/aqr/aqr-v5.6.7.cld $(1)/lib/aqr/aqr-v5.6.7.cld
	$(INSTALL_BIN) ./files/lib/aqr/init.sh $(1)/lib/aqr/init.sh
	$(INSTALL_DATA) ./files/lib/mmc/data_key $(1)/lib/mmc/data_key
	chmod 0600 $(1)/lib/mmc/data_key
	$(INSTALL_BIN) ./files/lib/mmc/init.sh $(1)/lib/mmc/init.sh
	$(INSTALL_BIN) ./files/etc/init.d/cr1000a-support $(1)/etc/init.d/cr1000a-support
endef

$(eval $(call BuildPackage,cr1000a-support))
