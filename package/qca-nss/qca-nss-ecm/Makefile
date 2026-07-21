include $(TOPDIR)/rules.mk

PKG_NAME:=qca-nss-ecm
PKG_RELEASE:=9

# Source: https://git.codelinaro.org/clo/qsdk/oss/lklm/qca-nss-ecm.git @ b6af8ffbd52b0dedc68f3fb4c04a714d1df77b96 (pre-generated 'git archive' of the commit).
# Fetched from our release mirror so the download is deterministic and hash-verified;
# git-proto regenerates the zstd archive locally and its hash varies by toolchain.
PKG_SOURCE_DATE:=2026-06-24
PKG_SOURCE_VERSION:=b6af8ffbd52b0dedc68f3fb4c04a714d1df77b96
QSDK_VERSION:=14.0
PKG_VERSION:=$(QSDK_VERSION).$(subst -,.,$(PKG_SOURCE_DATE))~$(PKG_SOURCE_VERSION)
PKG_SOURCE:=$(PKG_NAME)-$(subst ~,.,$(PKG_VERSION)).tar.zst
PKG_SOURCE_URL:=https://github.com/JuliusBairaktaris/nss-packages/releases/download/nss-source-tarballs
PKG_HASH:=fb84fb0f04634d87161c2d4ca4e72fb4454ce8ba1c1bbc28818584224ad3a663

PKG_BUILD_PARALLEL:=1
PKG_FLAGS:=nonshared
PKG_BUILD_FLAGS:=gc-sections lto

PKG_CONFIG_DEPENDS:= \
	CONFIG_NSS_DRV_PPPOE_ENABLE \
	CONFIG_NSS_FIRMWARE_VERSION_11_4 \
	CONFIG_NSS_FIRMWARE_VERSION_12_5 \
	CONFIG_PACKAGE_kmod-gre \
	CONFIG_PACKAGE_kmod-gre6 \
	CONFIG_PACKAGE_kmod-ip6-tunnel \
	CONFIG_PACKAGE_kmod-ipsec \
	CONFIG_PACKAGE_kmod-macvlan \
	CONFIG_PACKAGE_kmod-nat46 \
	CONFIG_PACKAGE_kmod-pppoe \
	CONFIG_PACKAGE_kmod-qca-mcs \
	CONFIG_PACKAGE_kmod-qca-nss-drv \
	CONFIG_PACKAGE_kmod-qca-nss-drv-igs \
	CONFIG_PACKAGE_kmod-sit \
	CONFIG_PACKAGE_kmod-vxlan

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

define KernelPackage/qca-nss-ecm
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Support
  DEPENDS:=@(TARGET_qualcommbe||TARGET_qualcommax||TARGET_ipq806x) \
		   +ethtool \
		   +kmod-nf-conntrack \
		   +@(TARGET_qualcommax||TARGET_ipq806x):NSS_DRV_IPV6_ENABLE \
		   +@(TARGET_qualcommax||TARGET_ipq806x):NSS_DRV_VIRT_IF_ENABLE \
		   +@(TARGET_qualcommax||TARGET_ipq806x):NSS_DRV_PPPOE_ENABLE \
		   +PACKAGE_kmod-qca-nss-drv:kmod-qca-nss-drv \
		   +PACKAGE_kmod-qca-mcs:kmod-qca-mcs \
		   +PACKAGE_kmod-vxlan:kmod-vxlan \
		   +PACKAGE_kmod-nat46:kmod-nat46 \
		   +kmod-ppp \
		   +kmod-pppoe
  TITLE:=QCA NSS Enhanced Connection Manager (ECM)
  FILES:=$(PKG_BUILD_DIR)/ecm.ko
  KCONFIG:= \
	CONFIG_NF_CONNTRACK_DSCPREMARK_EXT=y \
	CONFIG_NF_CONNTRACK_EVENTS=y
endef

define KernelPackage/qca-nss-ecm/Description
This package contains the QCA NSS Enhanced Connection Manager
endef

define KernelPackage/qca-nss-ecm/conffiles
/etc/config/ecm
endef

define KernelPackage/qca-nss-ecm/install
	$(INSTALL_DIR) $(1)/etc/firewall.d $(1)/etc/init.d $(1)/usr/bin $(1)/lib/netifd/offload $(1)/etc/config $(1)/etc/uci-defaults $(1)/etc/sysctl.d $(1)/etc/hotplug.d/net
	$(INSTALL_DATA) ./files/qca-nss-ecm.firewall $(1)/etc/firewall.d/qca-nss-ecm
	$(INSTALL_BIN) ./files/qca-nss-ecm.init $(1)/etc/init.d/qca-nss-ecm
	$(INSTALL_BIN) ./files/ecm_dump.sh $(1)/usr/bin/
	$(INSTALL_DATA) ./files/disable_offloads.sh $(1)/lib/netifd/offload
	$(INSTALL_DATA) ./files/on-demand-down $(1)/lib/netifd/offload/on-demand-down
	$(INSTALL_DATA) ./files/qca-nss-ecm.uci $(1)/etc/config/ecm
	$(INSTALL_DATA) ./files/qca-nss-ecm.defaults $(1)/etc/uci-defaults/99-qca-nss-ecm
	$(INSTALL_DATA) ./files/qca-nss-ecm.sysctl $(1)/etc/sysctl.d/qca-nss-ecm.conf
	$(INSTALL_DATA) ./files/disable_offloads.hotplug $(1)/etc/hotplug.d/net/99-disable_offloads
endef

EXTRA_CFLAGS+= \
		 -I$(STAGING_DIR)/usr/include/qca-nss-drv \
		 -I$(STAGING_DIR)/usr/include/qca-mcs \
		 -I$(STAGING_DIR)/usr/include/nat46

ifneq (, $(findstring $(CONFIG_TARGET_BOARD), "qualcommax" "ipq60xx"))
ECM_MAKE_OPTS+= ECM_NON_PORTED_SUPPORT_ENABLE=y \
			   ECM_INTERFACE_VLAN_ENABLE=y \
			   ECM_CLASSIFIER_MARK_ENABLE=y \
			   ECM_CLASSIFIER_DSCP_ENABLE=y \
			   ECM_CLASSIFIER_PCC_ENABLE=n
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv),)
ECM_MAKE_OPTS+=ECM_FRONT_END_NSS_ENABLE=y
endif

# Disable ECM IPv6 support when global IPv6 support is disabled.
ifneq ($(CONFIG_IPV6),)
ECM_MAKE_OPTS+=ECM_IPV6_ENABLE=y
endif

# Disable ECM Bridge VLAN Filtering support when global kernel config is disabled
ifneq ($(CONFIG_BRIDGE_VLAN_FILTERING),)
ECM_MAKE_OPTS+=ECM_BRIDGE_VLAN_FILTERING_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-igs),)
ECM_MAKE_OPTS+=ECM_CLASSIFIER_DSCP_IGS=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-mcs),)
ECM_MAKE_OPTS+=ECM_MULTICAST_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-nat46),)
ECM_MAKE_OPTS+=ECM_INTERFACE_MAP_T_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-ipsec),)
ECM_MAKE_OPTS+=ECM_INTERFACE_IPSEC_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-pppoe),)
ECM_MAKE_OPTS+=ECM_INTERFACE_PPPOE_ENABLE=y \
			   ECM_INTERFACE_PPP_ENABLE=y
else
ECM_MAKE_OPTS+=ECM_INTERFACE_PPPOE_ENABLE=n \
			   ECM_INTERFACE_PPP_ENABLE=n
endif

# The EDMA-rework kernel carries only the PPPoE ppp hooks (0961); the
# PPTP/L2TP kernel hooks (qosmio 0603-2/0603-3) are not ported, so
# these interface types must stay off regardless of selected kmods.
ECM_MAKE_OPTS+=ECM_INTERFACE_PPTP_ENABLE=n \
			   ECM_INTERFACE_L2TPV2_ENABLE=n

ifneq ($(CONFIG_PACKAGE_kmod-gre)$(CONFIG_PACKAGE_kmod-gre6),)
ECM_MAKE_OPTS+=ECM_INTERFACE_GRE_TAP_ENABLE=y \
			   ECM_INTERFACE_GRE_TUN_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-sit),)
ECM_MAKE_OPTS+=ECM_INTERFACE_SIT_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-ip6-tunnel),)
ECM_MAKE_OPTS+=ECM_INTERFACE_TUNIPIP6_ENABLE=y
endif

# The EDMA-rework kernel does not carry the QCA bonding hooks
# (bond_get_tx_dev), so LAG offload must stay off regardless of
# kmod-bonding.
ECM_MAKE_OPTS+=ECM_INTERFACE_BOND_ENABLE=n

ifneq ($(CONFIG_NSS_FIRMWARE_VERSION_12_5),)
EXTRA_CFLAGS+=-DNSS_FIRMWARE_VERSION_12_5
endif

define Build/InstallDev
	mkdir -p $(1)/usr/include/qca-nss-ecm
	$(CP) $(PKG_BUILD_DIR)/exports/* $(1)/usr/include/qca-nss-ecm
endef

ifeq ($(CONFIG_TARGET_BOARD), "qualcommax")
    SOC:=$(CONFIG_TARGET_SUBTARGET)
endif

define Build/Compile
	+$(MAKE) -C "$(LINUX_DIR)" $(strip $(ECM_MAKE_OPTS)) \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		ARCH="$(LINUX_KARCH)" \
		M="$(PKG_BUILD_DIR)" \
		NOSTDINC_FLAGS="$(KERNEL_NOSTDINC_FLAGS) $(EXTRA_CFLAGS)" \
		SoC=$(SOC) \
		$(KERNEL_MAKE_FLAGS) \
		$(PKG_JOBS) \
		modules
endef

$(eval $(call KernelPackage,qca-nss-ecm))
