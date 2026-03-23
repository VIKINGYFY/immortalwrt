#
# Copyright (C) 2022 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=ath11k-firmware
PKG_SOURCE_DATE:=2026-03-21
PKG_SOURCE_VERSION:=e853ab3fedec869ca666e871ef4bab41237b09e6
PKG_MIRROR_HASH:=4d37ebfec65fd5d1585c38bd6925fbc46bb6ad2a7eacc5ea93685f2873c1f291
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/VIKINGYFY/ath11k-firmware-ddwrt.git

PKG_LICENSE_FILES:=LICENSE.qca_firmware

PKG_MAINTAINER:=Robert Marko <robimarko@gmail.com>

include $(INCLUDE_DIR)/package.mk

RSTRIP:=:
STRIP:=:

define Package/ath11k-firmware-default
  SECTION:=firmware
  CATEGORY:=Firmware
  URL:=$(PKG_SOURCE_URL)
  DEPENDS:=
endef

define Package/ath11k-firmware-ipq5018-qcn6122
$(Package/ath11k-firmware-default)
  TITLE:=IPQ5018/QCN6122 ath11k firmware
endef

define Package/ath11k-firmware-ipq5018-ddwrt
$(Package/ath11k-firmware-default)
  TITLE:=IPQ5018 ath11k firmware ddwrt
endef

define Package/ath11k-firmware-ipq6018-ddwrt
$(Package/ath11k-firmware-default)
  TITLE:=IPQ6018 ath11k firmware ddwrt
endef

define Package/ath11k-firmware-ipq8074-ddwrt
$(Package/ath11k-firmware-default)
  TITLE:=IPQ8074 ath11k firmware ddwrt
endef

define Package/ath11k-firmware-qcn9074-ddwrt
$(Package/ath11k-firmware-default)
  TITLE:=QCN9074 ath11k firmware ddwrt
endef

define Build/Compile

endef

define Package/ath11k-firmware-ipq5018-qcn6122/install
	$(INSTALL_DIR) $(1)/lib/firmware/ath11k/IPQ5018/hw1.0
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/IPQ5018/hw1.0/* $(1)/lib/firmware/ath11k/IPQ5018/hw1.0/
	$(INSTALL_DIR) $(1)/lib/firmware/ath11k/QCN6122/hw1.0
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/QCN6122/hw1.0/* $(1)/lib/firmware/ath11k/QCN6122/hw1.0/
endef

define Package/ath11k-firmware-ipq5018-ddwrt/install
	$(INSTALL_DIR) $(1)/lib/firmware/ath11k/IPQ5018/hw1.0
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/IPQ5018/hw1.0/* $(1)/lib/firmware/ath11k/IPQ5018/hw1.0/
endef

define Package/ath11k-firmware-ipq6018-ddwrt/install
	$(INSTALL_DIR) $(1)/lib/firmware/IPQ6018
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/IPQ6018/hw1.0/* $(1)/lib/firmware/IPQ6018/
endef

define Package/ath11k-firmware-ipq8074-ddwrt/install
	$(INSTALL_DIR) $(1)/lib/firmware/IPQ8074
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/IPQ8074/hw2.0/* $(1)/lib/firmware/IPQ8074/
endef

define Package/ath11k-firmware-qcn9074-ddwrt/install
	$(INSTALL_DIR) $(1)/lib/firmware/ath11k/QCN9074/hw1.0
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/QCN9074/hw1.0/* $(1)/lib/firmware/ath11k/QCN9074/hw1.0/
endef

$(eval $(call BuildPackage,ath11k-firmware-ipq5018-qcn6122))
$(eval $(call BuildPackage,ath11k-firmware-ipq5018-ddwrt))
$(eval $(call BuildPackage,ath11k-firmware-ipq6018-ddwrt))
$(eval $(call BuildPackage,ath11k-firmware-ipq8074-ddwrt))
$(eval $(call BuildPackage,ath11k-firmware-qcn9074-ddwrt))
