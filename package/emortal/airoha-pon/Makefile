# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=airoha-pon
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=ImmortalWrt
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

PON_BSP_DIR:=$(PKG_BUILD_DIR)/bsp
PON_10G_DIR:=$(PKG_BUILD_DIR)/xpon-en757x/xpon_10g
PON_PHY_10G_DIR:=$(PKG_BUILD_DIR)/xpon-en757x/xpon_phy_10g
PON_PLUGIN_DIR:=$(PKG_BUILD_DIR)/plugins

PON_INCLUDE_FLAGS:= \
	-I$(PON_BSP_DIR)/include \
	-I$(PON_BSP_DIR)/include/global_inc \
	-I$(PON_BSP_DIR)/include/global_inc/version \
	-I$(PON_BSP_DIR)/an7583/pon_pma/inc \
	-I$(PON_BSP_DIR)/qdma

PON_V2_CPU_CFLAGS:= \
	-D__IMEM= \
	-DTCSUPPORT_CPU_ARMV8=1 \
	-DTCSUPPORT_CPU_ARMV8_64=1 \
	-DTCSUPPORT_CPU_EN7581=1 \
	-DTCSUPPORT_MT7572=1 \
	-DTCSUPPORT_BOARD_SELECT=1 \
	-DCONFIG_USE_MT7520_ASIC=1 \
	$(if $(CONFIG_TARGET_airoha_an7583),-DTCSUPPORT_CPU_AN7583=1)
PON_V2_CPU_MAKE:=TCSUPPORT_CPU_EN7581=1 TCSUPPORT_MT7572=1 $(if $(CONFIG_TARGET_airoha_an7583),TCSUPPORT_CPU_AN7583=1)

define KernelPackage/airoha-pon/Default
	SUBMENU:=Network Devices
	DEPENDS:=+kmod-i2c-core
	KCONFIG:=
endef

define KernelPackage/airoha-gpon-en757x
	$(call KernelPackage/airoha-pon/Default)
	DEPENDS+=@TARGET_airoha_en7523
	TITLE:=Airoha EN757x GPON driver (legacy 1G)
	VARIANT:=gpon-en757x
	FILES:= \
		$(PKG_BUILD_DIR)/gpon-en757x/xpon_phy/phy.ko \
		$(PKG_BUILD_DIR)/gpon-en757x/xpon/xpon.ko
	AUTOLOAD:=$(call AutoProbe,phy xpon)
endef

define KernelPackage/airoha-gpon-en757x/description
	Legacy EN7570 and EN7571 1G GPON MAC and PHY modules.
endef

define KernelPackage/airoha-xpon-en757x
	$(call KernelPackage/airoha-pon/Default)
	DEPENDS+=@(TARGET_airoha_an7581||TARGET_airoha_an7583)
	TITLE:=Airoha AN7581 and AN7583 xPON MAC and PHY
	VARIANT:=xpon-en757x
	FILES:= \
		$(PON_BSP_DIR)/airoha_ecnt_hook.ko \
		$(PON_BSP_DIR)/airoha_ecnt_scu.ko \
		$(PON_BSP_DIR)/airoha_ecnt_pon_phy.ko \
		$(PON_BSP_DIR)/airoha_ecnt_olt_phy.ko \
		$(PON_BSP_DIR)/airoha_arht_board.ko \
		$(PON_BSP_DIR)/airoha_arht_serdes.ko \
		$(PON_BSP_DIR)/airoha_an7583_combo_phy.ko \
		$(PON_PHY_10G_DIR)/phy_10g.ko \
		$(PON_10G_DIR)/xpon_10g.ko
	AUTOLOAD:=$(call AutoProbe,airoha_ecnt_hook airoha_ecnt_scu airoha_ecnt_pon_phy airoha_ecnt_olt_phy airoha_arht_board airoha_arht_serdes airoha_an7583_combo_phy phy_10g xpon_10g)
endef

define KernelPackage/airoha-xpon-en757x/description
	Vendor xPON MAC/PHY modules for the Airoha AN7581 and AN7583 10G path.
endef

define KernelPackage/airoha-pon-plugins
	$(call KernelPackage/airoha-pon/Default)
	DEPENDS+=+kmod-airoha-xpon-en757x +airoha-pon-firmware
	DEPENDS+=@(TARGET_airoha_an7581||TARGET_airoha_an7583)
	TITLE:=Airoha xPON helper plugins
	VARIANT:=plugins
	FILES:= \
		$(PON_PLUGIN_DIR)/econet_bob/econet_bob.ko \
		$(PON_PLUGIN_DIR)/lddla/en7572.ko
	AUTOLOAD:=$(call AutoLoad,90,econet_bob) $(call AutoLoad,95,en7572)
endef

define KernelPackage/airoha-pon-plugins/description
	Airoha xPON helper plugins, including the EN7572AN LDDLA laser driver
	and the BoB calibration loader. Legacy VLAN, IGMP and mapping sources
	remain staged for a separate skb metadata port.
endef

define Package/airoha-pon-firmware
	SECTION:=firmware
	CATEGORY:=Firmware
	TITLE:=Airoha EN7572AN LDDLA firmware
	DEPENDS:=@(TARGET_airoha_an7581||TARGET_airoha_an7583)
	PKGARCH:=all
endef

define Package/airoha-pon-firmware/description
	PM and DM firmware required by the Airoha EN7572AN optical PHY.
endef

define Package/airoha-pon-firmware/install
	$(INSTALL_DIR) $(1)/etc/lddla
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/plugins/lddla/lddla/A60993.elf.pm $(1)/etc/lddla/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/plugins/lddla/lddla/A60993.elf.dm $(1)/etc/lddla/
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/. $(PKG_BUILD_DIR)/
	$(call Build/Patch)
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PON_BSP_DIR)" \
		AIROHA_PON_CFLAGS="$(PON_INCLUDE_FLAGS) $(PON_V2_CPU_CFLAGS)" \
		$(PON_V2_CPU_MAKE) \
		modules
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PON_PHY_10G_DIR)" \
		KCFLAGS="$(PON_INCLUDE_FLAGS) $(PON_V2_CPU_CFLAGS)" \
		KBUILD_EXTRA_SYMBOLS="$(PON_BSP_DIR)/Module.symvers" \
		MODULES_PHY_10G_DIR="$(PON_PHY_10G_DIR)" \
		MODULES_QDMA_DIR="$(PON_BSP_DIR)/qdma" \
		$(PON_V2_CPU_MAKE) \
		modules
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PON_10G_DIR)" \
		KCFLAGS="$(PON_INCLUDE_FLAGS) $(PON_V2_CPU_CFLAGS) -DTCSUPPORT_WAN_GPON=1 -DTCSUPPORT_WAN_EPON=1" \
		KBUILD_MODPOST_WARN=1 \
		KBUILD_EXTRA_SYMBOLS="$(PON_BSP_DIR)/Module.symvers $(PON_PHY_10G_DIR)/Module.symvers" \
		XPON_BSP_MODULES_XPON_10G_DRIVER="$(PON_10G_DIR)" \
		XPON_BSP_GLOBAL_INC_PRIVATE="$(PON_BSP_DIR)/include/global_inc" \
		XPON_BSP_GLOBAL_INC_PUBLIC="$(PON_BSP_DIR)/include/global_inc" \
		XPON_BSP_ECNT_HOOK_DIR="$(PON_BSP_DIR)" \
		TCSUPPORT_WAN_GPON=1 TCSUPPORT_WAN_EPON=1 \
		$(PON_V2_CPU_MAKE) \
		modules
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PON_PLUGIN_DIR)/econet_bob" \
		KCFLAGS="$(PON_V2_CPU_CFLAGS)" \
		$(PON_V2_CPU_MAKE) \
		modules
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PON_PLUGIN_DIR)/lddla" \
		KCFLAGS="$(PON_INCLUDE_FLAGS) $(PON_V2_CPU_CFLAGS)" \
		KBUILD_EXTRA_SYMBOLS="$(PON_BSP_DIR)/Module.symvers $(PON_PHY_10G_DIR)/Module.symvers $(PON_10G_DIR)/Module.symvers" \
		KBUILD_MODPOST_WARN=1 \
		$(PON_V2_CPU_MAKE) \
		modules
endef

$(eval $(call BuildPackage,airoha-pon-firmware))
$(eval $(call KernelPackage,airoha-gpon-en757x))
$(eval $(call KernelPackage,airoha-xpon-en757x))
$(eval $(call KernelPackage,airoha-pon-plugins))
