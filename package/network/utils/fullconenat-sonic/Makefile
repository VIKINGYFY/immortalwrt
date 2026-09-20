# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=fullconenat-sonic
PKG_VERSION:=20260917
PKG_RELEASE:=6

PKG_URL:=https://github.com/mufeng05/openwrt-sonic-fullcone
SONIC_SOURCE_VERSION:=4de0643f6b48e7e7d57d768144204ce5d3078c8f
SONIC_LEGACY_PACKAGES:=kmod-nft-fullcone kmod-ipt-fullconenat iptables-mod-fullconenat ip6tables-mod-fullconenat
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/fullconenat-sonic
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Firewall
  TITLE:=SONiC full cone NAT support for firewall3/firewall4
  URL:=$(PKG_URL)
  DEPENDS:=@LINUX_6_18 @(PACKAGE_firewall4||PACKAGE_firewall) +PACKAGE_firewall4:kmod-nft-nat +PACKAGE_firewall:kmod-ipt-nat
  DEFAULT:=y if PACKAGE_firewall4 || PACKAGE_firewall
  CONFLICTS:=$(SONIC_LEGACY_PACKAGES)
  # APK packaging does not emit CONFLICTS; reject all legacy package versions.
  EXTRA_DEPENDS:=$(if $(CONFIG_USE_APK),$(foreach pkg,$(SONIC_LEGACY_PACKAGES),!$(pkg) (>=0)$(comma)))
  KCONFIG:=CONFIG_NF_NAT_FULLCONE=y CONFIG_NF_CT_PROTO_UDPLITE=y CONFIG_NF_CT_PROTO_SCTP=y
  PKGARCH:=all
endef

define Package/fullconenat-sonic/description
 SONiC full cone NAT integrated into nf_nat, nft_masq and xt_MASQUERADE,
 with global, per-zone and per-protocol firewall3/firewall4 settings.
 Requires a kernel built with the SONiC kernel patches.
endef

# These patches target the LuCI feed and are applied before package scanning.
define Build/Compile
endef

define Package/fullconenat-sonic/install
	$(INSTALL_DIR) $(1)/usr/share/fullconenat-sonic
	echo '$(SONIC_SOURCE_VERSION)' > $(1)/usr/share/fullconenat-sonic/version
endef

$(eval $(call BuildPackage,fullconenat-sonic))
