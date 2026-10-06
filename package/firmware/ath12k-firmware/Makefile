
include $(TOPDIR)/rules.mk

PKG_NAME:=ath12k-firmware
PKG_SOURCE_DATE:=2026-08-21
PKG_SOURCE_VERSION:=bfa20b54c49903eb656d14880a98661655017684
PKG_MIRROR_HASH:=c3ffa402199c8834b7df4348cecc3f81e0215adba79b4cfaaa368893f60fc981
PKG_RELEASE:=2

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/VIKINGYFY/ath12k-firmware-ddwrt.git

PKG_LICENSE_FILES:=LICENSE.qca_firmware

PKG_MAINTAINER:=VIKING <viking.yfy@qq.com>

include $(INCLUDE_DIR)/package.mk

# WSI groups require the tested IPQ5332/QCN9274 WLAN.WBE.1.6 pair.
IPQ5332_FW_VERSION:=2365932733ca8ec3b346621d9cec2eb3df3b2cf3
IPQ5332_FW_SOURCE:=ath12k-ipq5332-flint3-$(IPQ5332_FW_VERSION).tar.gz
IPQ5332_FW_PATH:=openwrt-flint3-$(IPQ5332_FW_VERSION)/package/firmware/ath12k-firmware/src/IPQ5332/hw1.0

define Download/ipq5332-wbe16
  FILE:=$(IPQ5332_FW_SOURCE)
  URL:=https://codeload.github.com/perceival/openwrt-flint3/tar.gz
  URL_FILE:=$(IPQ5332_FW_VERSION)
  HASH:=8574a516dcbdf9ff9ca055b41951ae116627907d129f5833f9ca8a913660b287
endef
$(eval $(call Download,ipq5332-wbe16))

RSTRIP:=:
STRIP:=:

define Package/ath12k-firmware-default
  SECTION:=firmware
  CATEGORY:=Firmware
  URL:=$(PKG_SOURCE_URL)
  DEPENDS:=
endef

define Package/ath12k-firmware-ipq5332-ddwrt
$(Package/ath12k-firmware-default)
  TITLE:=IPQ5332 ath12k firmware ddwrt
endef

define Package/ath12k-firmware-qcn9274-ddwrt
$(Package/ath12k-firmware-default)
  TITLE:=QCN9274 ath12k firmware ddwrt
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	rm -f $(PKG_BUILD_DIR)/IPQ5332/hw1.0/q6_fw[01].* $(PKG_BUILD_DIR)/IPQ5332/hw1.0/iu_fw.*
	$(TAR) -xzf $(DL_DIR)/$(IPQ5332_FW_SOURCE) --strip-components=7 \
		-C $(PKG_BUILD_DIR)/IPQ5332/hw1.0 --wildcards \
		'$(IPQ5332_FW_PATH)/q6_fw[01].b??' '$(IPQ5332_FW_PATH)/q6_fw[01].mdt' \
		'$(IPQ5332_FW_PATH)/iu_fw.b??' '$(IPQ5332_FW_PATH)/iu_fw.mdt'
endef

define Build/Compile

endef

define Package/ath12k-firmware-ipq5332-ddwrt/install
	$(INSTALL_DIR) $(1)/lib/firmware/ath12k/IPQ5332/hw1.0/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/IPQ5332/hw1.0/* $(1)/lib/firmware/ath12k/IPQ5332/hw1.0/
endef

define Package/ath12k-firmware-qcn9274-ddwrt/install
	$(INSTALL_DIR) $(1)/lib/firmware/ath12k/QCN9274/hw2.0/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/QCN9274/hw2.0/* $(1)/lib/firmware/ath12k/QCN9274/hw2.0/
endef

$(eval $(call BuildPackage,ath12k-firmware-ipq5332-ddwrt))
$(eval $(call BuildPackage,ath12k-firmware-qcn9274-ddwrt))
