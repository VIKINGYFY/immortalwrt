
include $(TOPDIR)/rules.mk

PKG_NAME:=ath12k-firmware
PKG_SOURCE_DATE:=2026-01-21
PKG_RELEASE:=1
PKG_LICENSE_FILES:=LICENSE.qca_firmware

include $(INCLUDE_DIR)/package.mk

RSTRIP:=:
STRIP:=:

define Package/ath12k-firmware-default
  SECTION:=firmware
  CATEGORY:=Firmware
  URL:=$(PKG_SOURCE_URL)
  DEPENDS:=
endef

define Package/ath12k-firmware-ipq5332
$(Package/ath12k-firmware-default)
  TITLE:=IPQ5332 ath12k firmware
endef

define Build/Compile

endef

define Package/ath12k-firmware-ipq5332/install
	$(INSTALL_DIR) $(1)/lib/firmware/ath12k/IPQ5332/hw1.0/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/IPQ5332/hw1.0/* $(1)/lib/firmware/ath12k/IPQ5332/hw1.0/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/QCN9274/hw2.0/regdb.bin $(1)/lib/firmware/ath12k/IPQ5332/hw1.0/regdb.bin
	$(INSTALL_DIR) $(1)/lib/firmware/ath12k/QCN9274/hw2.0/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/QCN9274/hw2.0/* $(1)/lib/firmware/ath12k/QCN9274/hw2.0/
endef

$(eval $(call BuildPackage,ath12k-firmware-ipq5332))
