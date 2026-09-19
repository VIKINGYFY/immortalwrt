include $(TOPDIR)/rules.mk

PKG_NAME:=qca-nss-ecm
PKG_RELEASE:=9

PKG_SOURCE_URL:=https://git.codelinaro.org/clo/qsdk/oss/lklm/qca-nss-ecm.git
PKG_SOURCE_PROTO:=git
PKG_SOURCE_DATE:=2026-04-03
PKG_SOURCE_VERSION:=8c7355b
PKG_MIRROR_HASH:=bf382afee7c007a2f6a436f59c9a5e7928a5df89921a3ff1003e49b949b90815
QSDK_VERSION:=13.1
PKG_VERSION:=$(QSDK_VERSION).$(subst -,.,$(PKG_SOURCE_DATE))~$(PKG_SOURCE_VERSION)

PKG_BUILD_PARALLEL:=1
PKG_FLAGS:=nonshared
PKG_BUILD_FLAGS:=gc-sections lto

PKG_CONFIG_DEPENDS:= \
	CONFIG_TARGET_SUBTARGET \
	CONFIG_BRIDGE_VLAN_FILTERING \
	CONFIG_IPV6 \
	CONFIG_NSS_DRV_PPPOE_ENABLE \
	CONFIG_NSS_DRV_RMNET_ENABLE \
	CONFIG_PACKAGE_kmod-bonding \
	CONFIG_PACKAGE_kmod-gre \
	CONFIG_PACKAGE_kmod-gre6 \
	CONFIG_PACKAGE_kmod-ip6-tunnel \
	CONFIG_PACKAGE_kmod-macvlan \
	CONFIG_PACKAGE_kmod-nat46 \
	CONFIG_PACKAGE_kmod-pppoe \
	CONFIG_PACKAGE_kmod-pppol2tp \
	CONFIG_PACKAGE_kmod-pptp \
	CONFIG_PACKAGE_kmod-qca-mcs \
	CONFIG_PACKAGE_kmod-qca-nss-drv \
	CONFIG_PACKAGE_kmod-qca-nss-drv-igs \
	CONFIG_PACKAGE_kmod-qca-ovsmgr \
	CONFIG_PACKAGE_kmod-qmi_wwan_q \
	CONFIG_PACKAGE_kmod-sit \
	CONFIG_PACKAGE_kmod-vxlan

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

define KernelPackage/qca-nss-ecm
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Support
  DEPENDS:=@TARGET_qualcommax \
	+ethtool \
	+kmod-nf-conntrack \
	+kmod-qca-nss-drv \
	+PACKAGE_kmod-bonding:kmod-bonding \
	+PACKAGE_kmod-nat46:kmod-nat46 \
	+PACKAGE_kmod-ppp:kmod-ppp \
	+PACKAGE_kmod-pppoe:kmod-pppoe \
	+PACKAGE_kmod-pppol2tp:kmod-pppol2tp \
	+PACKAGE_kmod-pptp:kmod-pptp \
	+PACKAGE_kmod-qca-mcs:kmod-qca-mcs \
	+PACKAGE_kmod-vxlan:kmod-vxlan
  TITLE:=QCA NSS Enhanced Connection Manager (ECM)
  FILES:=$(PKG_BUILD_DIR)/ecm.ko
  KCONFIG:=CONFIG_NF_CONNTRACK_DSCPREMARK_EXT=y
endef

define KernelPackage/qca-nss-ecm/description
This package contains the QCA NSS Enhanced Connection Manager
endef

define KernelPackage/qca-nss-ecm/install
	$(INSTALL_DIR) \
		$(1)/etc/firewall.d \
		$(1)/etc/hotplug.d/net \
		$(1)/etc/init.d \
		$(1)/etc/sysctl.d \
		$(1)/etc/uci-defaults \
		$(1)/lib/netifd/offload \
		$(1)/usr/bin
	$(INSTALL_BIN) ./files/ecm_dump.sh $(1)/usr/bin/
	$(INSTALL_BIN) ./files/qca-nss-ecm.init $(1)/etc/init.d/qca-nss-ecm
	$(INSTALL_DATA) ./files/disable_offloads.hotplug $(1)/etc/hotplug.d/net/99-disable_offloads
	$(INSTALL_DATA) ./files/disable_offloads.sh $(1)/lib/netifd/offload
	$(INSTALL_DATA) ./files/on-demand-down $(1)/lib/netifd/offload/on-demand-down
	$(INSTALL_DATA) ./files/qca-nss-ecm.defaults $(1)/etc/uci-defaults/99-qca-nss-ecm
	$(INSTALL_DATA) ./files/qca-nss-ecm.firewall $(1)/etc/firewall.d/qca-nss-ecm
	$(INSTALL_DATA) ./files/qca-nss-ecm.sysctl $(1)/etc/sysctl.d/qca-nss-ecm.conf
endef

EXTRA_CFLAGS += \
	-I$(STAGING_DIR)/usr/include/qca-nss-drv \
	-I$(STAGING_DIR)/usr/include/qca-mcs \
	-I$(STAGING_DIR)/usr/include/nat46 \
	-DNSS_FIRMWARE_VERSION_12_5

ECM_SUBTARGET:=$(subst ",,$(CONFIG_TARGET_SUBTARGET))
ECM_MAKE_OPTS:=

ifeq ($(CONFIG_TARGET_qualcommax),y)
ECM_MAKE_OPTS += \
	ECM_NON_PORTED_SUPPORT_ENABLE=y \
	ECM_INTERFACE_VLAN_ENABLE=y \
	ECM_CLASSIFIER_MARK_ENABLE=y \
	ECM_CLASSIFIER_DSCP_ENABLE=y \
	ECM_CLASSIFIER_PCC_ENABLE=n
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv),)
ECM_MAKE_OPTS += ECM_FRONT_END_NSS_ENABLE=y
endif

# Disable ECM IPv6 support when global IPv6 support is disabled.
ifneq ($(CONFIG_IPV6),)
ECM_MAKE_OPTS += ECM_IPV6_ENABLE=y
endif

# Disable ECM Bridge VLAN Filtering support when global kernel config is disabled
ifneq ($(CONFIG_BRIDGE_VLAN_FILTERING),)
ECM_MAKE_OPTS += ECM_BRIDGE_VLAN_FILTERING_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-igs),)
ECM_MAKE_OPTS += ECM_CLASSIFIER_DSCP_IGS=y
endif

ifeq ($(CONFIG_PACKAGE_kmod-vxlan),y)
ECM_MAKE_OPTS += ECM_INTERFACE_VXLAN_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-ovsmgr),)
ECM_MAKE_OPTS += \
	ECM_INTERFACE_OVS_BRIDGE_ENABLE=y \
	ECM_CLASSIFIER_OVS_ENABLE=y
EXTRA_CFLAGS += -I$(STAGING_DIR)/usr/include/qca-ovsmgr
endif

ifneq ($(CONFIG_PACKAGE_kmod-macvlan),)
ECM_MAKE_OPTS += ECM_INTERFACE_MACVLAN_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-mcs),)
ECM_MAKE_OPTS += ECM_MULTICAST_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-nat46),)
ECM_MAKE_OPTS += ECM_INTERFACE_MAP_T_ENABLE=y
endif

# NSS IPsec is intentionally disabled until the matching qualcommax client
# package is available.
ECM_PPPOE_ENABLE:=$(if $(CONFIG_PACKAGE_kmod-pppoe),y,n)

ECM_MAKE_OPTS += \
	ECM_INTERFACE_IPSEC_ENABLE=n \
	ECM_INTERFACE_L2TPV2_ENABLE=$(if $(CONFIG_PACKAGE_kmod-pppol2tp),y,n) \
	ECM_INTERFACE_PPPOE_ENABLE=$(ECM_PPPOE_ENABLE) \
	ECM_INTERFACE_PPP_ENABLE=$(ECM_PPPOE_ENABLE) \
	ECM_INTERFACE_PPTP_ENABLE=$(if $(CONFIG_PACKAGE_kmod-pptp),y,n)

ifneq ($(CONFIG_PACKAGE_kmod-gre)$(CONFIG_PACKAGE_kmod-gre6),)
ECM_MAKE_OPTS += \
	ECM_INTERFACE_GRE_TAP_ENABLE=y \
	ECM_INTERFACE_GRE_TUN_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-sit),)
ECM_MAKE_OPTS += ECM_INTERFACE_SIT_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-ip6-tunnel),)
ECM_MAKE_OPTS += ECM_INTERFACE_TUNIPIP6_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-bonding),)
ECM_MAKE_OPTS += ECM_INTERFACE_BOND_ENABLE=y
endif

# RAWIP calls nss_rmnet_rx_get_ifnum(), which is only exported when the
# NSS core driver builds its RMNET support.
ECM_MAKE_OPTS += ECM_INTERFACE_RAWIP_ENABLE=$(if $(and $(CONFIG_PACKAGE_kmod-qmi_wwan_q),$(CONFIG_NSS_DRV_RMNET_ENABLE)),y,n)

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/qca-nss-ecm
	$(CP) $(PKG_BUILD_DIR)/exports/* $(1)/usr/include/qca-nss-ecm
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) $(strip $(ECM_MAKE_OPTS)) \
		M="$(PKG_BUILD_DIR)" \
		EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
		SoC=$(ECM_SUBTARGET) \
		modules
endef

$(eval $(call KernelPackage,qca-nss-ecm))
