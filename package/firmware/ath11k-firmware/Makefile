#
# Copyright (C) 2022 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=ath11k-firmware
PKG_SOURCE_DATE:=2026-11-07
PKG_SOURCE_VERSION:=0ad99e44afa4c0dc1fb8f231427082700e562098
PKG_MIRROR_HASH:=6fdd617ca513d49aa1546c6432e0a8206ca92d8522105d26ebb870301da5a453
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
  TITLE:=IPQ5018 ath11k firmware DD-WRT
endef

define Package/ath11k-firmware-ipq6018-ddwrt
$(Package/ath11k-firmware-default)
  TITLE:=IPQ6018 ath11k firmware DD-WRT
endef

define Package/ath11k-firmware-ipq8074-ddwrt
$(Package/ath11k-firmware-default)
  TITLE:=IPQ8074 ath11k firmware DD-WRT
endef

define Package/ath11k-firmware-qcn9074-ddwrt
$(Package/ath11k-firmware-default)
  TITLE:=QCN9074 ath11k firmware DD-WRT
endef

define Package/ath11k-firmware-ipq9574-ddwrt
$(Package/ath11k-firmware-default)
  TITLE:=IPQ9574 ath11k firmware DD-WRT
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

# IPQ9574 uses the Q6 and M3 split images through the secure WCSS loader.
define Package/ath11k-firmware-ipq9574-ddwrt/install
	$(INSTALL_DIR) $(1)/lib/firmware/ath11k/IPQ9574/hw1.0
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/IPQ9574/hw1.0/* $(1)/lib/firmware/ath11k/IPQ9574/hw1.0/
endef

define Package/ath11k-firmware-qcn9074-ddwrt/install
	$(INSTALL_DIR) $(1)/lib/firmware/ath11k/QCN9074/hw1.0
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/QCN9074/hw1.0/* $(1)/lib/firmware/ath11k/QCN9074/hw1.0/
endef

$(eval $(call BuildPackage,ath11k-firmware-ipq5018-qcn6122))
$(eval $(call BuildPackage,ath11k-firmware-ipq5018-ddwrt))
$(eval $(call BuildPackage,ath11k-firmware-ipq6018-ddwrt))
$(eval $(call BuildPackage,ath11k-firmware-ipq8074-ddwrt))
$(eval $(call BuildPackage,ath11k-firmware-ipq9574-ddwrt))
$(eval $(call BuildPackage,ath11k-firmware-qcn9074-ddwrt))
