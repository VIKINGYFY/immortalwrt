include $(TOPDIR)/rules.mk

PKG_NAME:=qca-nss-drv
PKG_RELEASE:=18

# Source: https://git.codelinaro.org/clo/qsdk/oss/lklm/nss-drv.git @ d7ef98b1d3d31346981d3a1abff5152a206374af (pre-generated 'git archive' of the commit).
# Fetched from our release mirror so the download is deterministic and hash-verified;
# git-proto regenerates the zstd archive locally and its hash varies by toolchain.
PKG_SOURCE_DATE:=2026-04-10
PKG_SOURCE_VERSION:=d7ef98b1d3d31346981d3a1abff5152a206374af
QSDK_VERSION:=14.0
PKG_VERSION:=$(QSDK_VERSION).$(subst -,.,$(PKG_SOURCE_DATE))~$(PKG_SOURCE_VERSION)
PKG_SOURCE:=$(PKG_NAME)-$(subst ~,.,$(PKG_VERSION)).tar.zst
PKG_SOURCE_URL:=https://github.com/JuliusBairaktaris/nss-packages/releases/download/nss-source-tarballs
PKG_HASH:=fe700bb37bb205e22b4ccc218117bebc813bee4b17059fbc75a979f8b1087069

PKG_BUILD_PARALLEL:=1
PKG_FLAGS:=nonshared

PKG_CONFIG_DEPENDS:= \
	CONFIG_NSS_FIRMWARE_VERSION_11_4 \
	CONFIG_NSS_FIRMWARE_VERSION_12_5 \
	CONFIG_PACKAGE_kmod-gre \
	CONFIG_PACKAGE_kmod-ipsec \
	CONFIG_PACKAGE_kmod-l2tp \
	CONFIG_PACKAGE_kmod-nat46 \
	CONFIG_PACKAGE_kmod-pppoe \
	CONFIG_PACKAGE_kmod-pptp \
	CONFIG_PACKAGE_kmod-sit \
	CONFIG_NSS_DRV_BRIDGE_ENABLE \
	CONFIG_NSS_DRV_CAPWAP_ENABLE \
	CONFIG_NSS_DRV_C2C_ENABLE \
	CONFIG_NSS_DRV_CLMAP_ENABLE \
	CONFIG_NSS_DRV_CRYPTO_ENABLE \
	CONFIG_NSS_DRV_DTLS_ENABLE \
	CONFIG_NSS_DRV_GRE_ENABLE \
	CONFIG_NSS_DRV_GRE_REDIR_ENABLE \
	CONFIG_NSS_DRV_GRE_TUNNEL_ENABLE \
	CONFIG_NSS_DRV_IGS_ENABLE \
	CONFIG_NSS_DRV_IPSEC_ENABLE \
	CONFIG_NSS_DRV_IPV4_REASM_ENABLE \
	CONFIG_NSS_DRV_IPV6_ENABLE \
	CONFIG_NSS_DRV_IPV6_REASM_ENABLE \
	CONFIG_NSS_DRV_LSO_RX_ENABLE \
	CONFIG_NSS_DRV_L2TP_ENABLE \
	CONFIG_NSS_DRV_LAG_ENABLE \
	CONFIG_NSS_DRV_MAPT_ENABLE \
	CONFIG_NSS_DRV_MATCH_ENABLE \
	CONFIG_NSS_DRV_MIRROR_ENABLE \
	CONFIG_NSS_DRV_OAM_ENABLE \
	CONFIG_NSS_DRV_PORTID_ENABLE \
	CONFIG_NSS_DRV_PPPOE_ENABLE \
	CONFIG_NSS_DRV_PPTP_ENABLE \
	CONFIG_NSS_DRV_PVXLAN_ENABLE \
	CONFIG_NSS_DRV_QRFS_ENABLE \
	CONFIG_NSS_DRV_QVPN_ENABLE \
	CONFIG_NSS_DRV_RMNET_ENABLE \
	CONFIG_NSS_DRV_SHAPER_ENABLE \
	CONFIG_NSS_DRV_SJACK_ENABLE \
	CONFIG_NSS_DRV_UDP_ST_ENABLE \
	CONFIG_NSS_DRV_TLS_ENABLE \
	CONFIG_NSS_DRV_TRUSTSEC_ENABLE \
	CONFIG_NSS_DRV_TRUSTSEC_RX_ENABLE \
	CONFIG_NSS_DRV_TSTAMP_ENABLE \
	CONFIG_NSS_DRV_TUN6RD_ENABLE \
	CONFIG_NSS_DRV_TUNIPIP6_ENABLE \
	CONFIG_NSS_DRV_VIRT_IF_ENABLE \
	CONFIG_NSS_DRV_VLAN_ENABLE \
	CONFIG_NSS_DRV_VXLAN_ENABLE \
	CONFIG_NSS_DRV_WIFIOFFLOAD_ENABLE \
	CONFIG_NSS_DRV_WIFI_EXT_VDEV_ENABLE \
	CONFIG_NSS_DRV_WIFI_MESH_ENABLE \
	CONFIG_NSS_DRV_WIFI_LEGACY_ENABLE

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk


define KernelPackage/qca-nss-drv
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  DEPENDS:=@(TARGET_ipq806x||TARGET_qualcommax) \
  				 +TARGET_qualcommax:kmod-qca-ppe-nss \
  				 +TARGET_qualcommax:nss-firmware
  TITLE:=Kernel driver for NSS (core driver)
  FILES:=$(PKG_BUILD_DIR)/qca-nss-drv.ko
  # No AUTOLOAD: NSS is strictly opt-in at runtime during bring-up
endef

define KernelPackage/qca-nss-drv/config
	source "$(SOURCE)/Config.in"
endef

define KernelPackage/qca-nss-drv/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_DIR) $(1)/etc/hotplug.d/firmware
	$(INSTALL_DIR) $(1)/usr/bin

	$(INSTALL_BIN) ./files/skb_recycler.init $(1)/etc/init.d/skb_recycler
	$(INSTALL_BIN) ./files/qca-nss-drv.init $(1)/etc/init.d/qca-nss-drv
	$(INSTALL_DATA) ./files/qca-nss-drv.uci $(1)/etc/config/nss
	$(INSTALL_DATA) ./files/qca-nss-drv.hotplug $(1)/etc/hotplug.d/firmware/10-qca-nss-fw
	$(INSTALL_BIN) ./files/qca-nss-drv.debug $(1)/usr/bin/nss_stats
endef

define KernelPackage/qca-nss-drv/conffiles
/etc/config/nss
/etc/config/skb_recycler
endef

define KernelPackage/qca-nss-drv/Description
This package contains a NSS driver for QCA chipset
endef

ifeq ($(CONFIG_TARGET_SUBTARGET), "ipq807x")
  SOC="ipq807x_64"
  subtarget:=$(CONFIG_TARGET_SUBTARGET)
else ifeq ($(CONFIG_TARGET_SUBTARGET), "ipq60xx")
  SOC="ipq60xx_64"
  subtarget:=$(CONFIG_TARGET_SUBTARGET)
else ifeq ($(CONFIG_TARGET_SUBTARGET), "ipq50xx")
  SOC="ipq50xx_64"
  subtarget:=$(CONFIG_TARGET_SUBTARGET)
endif

define Build/InstallDev
	mkdir -p $(1)/usr/include/qca-nss-drv
	$(CP) $(PKG_BUILD_DIR)/exports/* $(1)/usr/include/qca-nss-drv/
ifneq (, $(findstring $(subtarget), "ipq807x" "ipq807x_64" "ipq60xx" "ipq60xx_64" "ipq50xx" "ipq50xx_64"))
	$(RM) $(1)/usr/include/qca-nss-drv/nss_ipsecmgr.h
endif
endef

EXTRA_CFLAGS+= -I$(STAGING_DIR)/usr/include/qca-ppe-nss \
			   -Wno-missing-declarations -Wno-missing-prototypes \
			   -Wno-empty-body -Wno-unused-variable

ifdef CONFIG_NSS_MEM_PROFILE_MEDIUM
  EXTRA_CFLAGS += -DNSS_MEM_PROFILE_MEDIUM
else ifdef CONFIG_NSS_MEM_PROFILE_LOW
  EXTRA_CFLAGS += -DNSS_MEM_PROFILE_LOW
endif

ifdef CONFIG_NSS_FIRMWARE_VERSION_12_5
  EXTRA_CFLAGS += -DNSS_FIRMWARE_VERSION_12_5
endif

ifdef CONFIG_NSS_FIRMWARE_VERSION_11_4
  EXTRA_CFLAGS += -DNSS_FIRMWARE_VERSION_11_4
endif

DRV_MAKE_OPTS:=

# qca_edma/qca_ppe stack: no qca-ssdk, so PPE virtual-port support
# (the only ssdk header consumer in nss-drv) is compiled out.
DRV_MAKE_OPTS += NSS_DRV_PPE_VP_ENABLE=n

ifndef CONFIG_NSS_DRV_BRIDGE_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_BRIDGE_ENABLE=n
endif

ifneq ($(CONFIG_TARGET_BOARD), "ipq806x")
ifndef CONFIG_NSS_DRV_C2C_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_C2C_ENABLE=n
endif
endif

ifndef CONFIG_NSS_DRV_CLMAP_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_CLMAP_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_CRYPTO_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_CRYPTO_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_DTLS_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_DTLS_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_GRE_ENABLE
ifndef CONFIG_PACKAGE_kmod-gre
   DRV_MAKE_OPTS += NSS_DRV_GRE_ENABLE=n
endif
endif

ifndef CONFIG_NSS_DRV_GRE_REDIR_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_GRE_REDIR_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_GRE_TUNNEL_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_GRE_TUNNEL_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_IGS_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_IGS_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_IPSEC_ENABLE
ifndef CONFIG_PACKAGE_kmod-ipsec
   DRV_MAKE_OPTS += NSS_DRV_IPSEC_ENABLE=n
endif
endif

ifndef CONFIG_NSS_DRV_IPV4_REASM_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_IPV4_REASM_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_IPV6_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_IPV6_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_IPV6_REASM_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_IPV6_REASM_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_L2TP_ENABLE
ifndef CONFIG_PACKAGE_kmod-l2tp
   DRV_MAKE_OPTS += NSS_DRV_L2TP_ENABLE=n
endif
endif

ifndef CONFIG_NSS_DRV_LAG_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_LAG_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_MAPT_ENABLE
ifndef CONFIG_PACKAGE_kmod-nat46
   DRV_MAKE_OPTS += NSS_DRV_MAPT_ENABLE=n
endif
endif

ifndef CONFIG_NSS_DRV_MATCH_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_MATCH_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_MIRROR_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_MIRROR_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_OAM_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_OAM_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_PORTID_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_PORTID_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_PPPOE_ENABLE
ifndef CONFIG_PACKAGE_kmod-pppoe
   DRV_MAKE_OPTS += NSS_DRV_PPPOE_ENABLE=n
endif
endif

ifndef CONFIG_NSS_DRV_PPTP_ENABLE
ifndef CONFIG_PACKAGE_kmod-pptp
   DRV_MAKE_OPTS += NSS_DRV_PPTP_ENABLE=n
endif
endif

ifndef CONFIG_NSS_DRV_PVXLAN_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_PVXLAN_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_QRFS_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_QRFS_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_QVPN_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_QVPN_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_RMNET_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_RMNET_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_SHAPER_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_SHAPER_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_SJACK_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_SJACK_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_TLS_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_TLS_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_TRUSTSEC_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_TRUSTSEC_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_TRUSTSEC_RX_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_TRUSTSEC_RX_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_TSTAMP_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_TSTAMP_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_TUN6RD_ENABLE
ifndef CONFIG_PACKAGE_kmod-sit
   DRV_MAKE_OPTS += NSS_DRV_TUN6RD_ENABLE=n
endif
endif

ifndef CONFIG_NSS_DRV_TUNIPIP6_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_TUNIPIP6_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_VIRT_IF_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_VIRT_IF_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_VLAN_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_VLAN_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_VXLAN_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_VXLAN_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_WIFIOFFLOAD_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_WIFIOFFLOAD_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_WIFI_EXT_VDEV_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_WIFI_EXT_VDEV_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_WIFI_MESH_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_WIFI_MESH_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_UDP_ST_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_UDP_ST_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_CAPWAP_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_CAPWAP_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_LSO_RX_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_LSO_RX_ENABLE=n
endif

ifndef CONFIG_NSS_DRV_WIFI_LEGACY_ENABLE
   DRV_MAKE_OPTS += NSS_DRV_WIFI_LEGACY_ENABLE=n
endif

define Build/Configure
	$(LN) arch/nss_$(SOC).h $(PKG_BUILD_DIR)/exports/nss_arch.h
endef

define Build/Compile
	+$(MAKE) -C "$(LINUX_DIR)" $(strip $(DRV_MAKE_OPTS)) \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		ARCH="$(LINUX_KARCH)" \
		M="$(PKG_BUILD_DIR)" \
		NOSTDINC_FLAGS="$(KERNEL_NOSTDINC_FLAGS) $(EXTRA_CFLAGS)" \
		SoC=$(SOC) \
		$(KERNEL_MAKE_FLAGS) \
		$(PKG_JOBS) \
		modules
endef

$(eval $(call KernelPackage,qca-nss-drv))
