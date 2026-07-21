include $(TOPDIR)/rules.mk

PKG_NAME:=nss-userspace-oss
PKG_RELEASE:=2

# Source: https://git.codelinaro.org/clo/qsdk/oss/nss-userspace.git @ 7233e22 (pre-generated 'git archive' of the commit).
# Fetched from our release mirror so the download is deterministic and hash-verified;
# git-proto regenerates the zstd archive locally and its hash varies by toolchain.
PKG_SOURCE_DATE:=2024-11-27
PKG_SOURCE_VERSION:=7233e22
QSDK_VERSION:=13.0
PKG_VERSION:=$(QSDK_VERSION).$(subst -,.,$(PKG_SOURCE_DATE))~$(PKG_SOURCE_VERSION)
PKG_SOURCE:=$(PKG_NAME)-$(subst ~,.,$(PKG_VERSION)).tar.zst
PKG_SOURCE_URL:=https://github.com/JuliusBairaktaris/nss-packages/releases/download/nss-source-tarballs
PKG_HASH:=03dea072feb18916f32798f7bfd8c8811733e0681c4e9e1d77dd895f121de734

PKG_BUILD_PARALLEL:=1
PKG_FLAGS:=nonshared
PKG_BUILD_FLAGS:=gc-sections lto

PKG_CONFIG_DEPENDS:= \
	CONFIG_NSS_NLCAPWAP_ENABLE \
	CONFIG_NSS_NLDTLS_ENABLE \
	CONFIG_NSS_NLGRE_REDIR_ENABLE \
	CONFIG_NSS_NLIPSEC_ENABLE \
	CONFIG_NSS_NLQRFS_ENABLE \
	CONFIG_NSS_NLUDP_ST_ENABLE
	
include $(INCLUDE_DIR)/package.mk

define Package/nss-userspace
  TITLE:=Userspace utilities for NSS
endef

define Package/libnl-nss
$(call Package/nss-userspace)
  SECTION:=Libs
  CATEGORY:=Libraries
  TITLE += (libnl-nss)
  DEPENDS:=+libpthread +libnl-tiny +kmod-qca-nss-drv-netlink
endef

define Package/libnl-nss/description
 A framework in the userspace that establishes communication between userspace applications and the kernel.
endef

define Package/nssinfo
$(call Package/nss-userspace)
  SECTION:=Utils
  CATEGORY:=Utilities
  TITLE += (nssinfo)
	DEPENDS:=+libncurses +libnl-nss
endef

define Package/nssinfo/description
 A userspace utility for fetching stats from NSS.
endef

TARGET_CFLAGS += $(FPIC) -D_GNU_SOURCE=1

TARGET_CPPFLAGS:= \
		-I$(STAGING_DIR)/usr/include/qca-nss-clients \
		-I$(STAGING_DIR)/usr/include/qca-nss-drv \
		-I$(STAGING_DIR)/usr/include/libnl-tiny \
		-I$(STAGING_DIR)/usr/include/libnl-nss

ifneq ($(CONFIG_PACKAGE_libnl-nss),)
MAKE_FLAGS+=BUILD_LIBNSS=y

ifdef CONFIG_NSS_NLUDP_ST_ENABLE
MAKE_FLAGS+=udp_st=y
endif

ifdef CONFIG_NSS_NLQRFS_ENABLE
MAKE_FLAGS+=qrfs=y
endif

ifdef CONFIG_NSS_NLGRE_REDIR_ENABLE
MAKE_FLAGS+=gre_redir=y
endif

ifdef CONFIG_NSS_NLIPSEC_ENABLE
MAKE_FLAGS+=ipsec=y
endif

ifdef CONFIG_NSS_NLDTLS_ENABLE
MAKE_FLAGS+=dtls=y
endif

ifdef CONFIG_NSS_NLCAPWAP_ENABLE
MAKE_FLAGS+=capwap=y
endif
endif

ifneq ($(CONFIG_PACKAGE_nssinfo),)
MAKE_FLAGS+=BUILD_NSSINFO=y
endif

define Build/libnl-nss/InstallDev
	$(INSTALL_DIR) $(STAGING_DIR)/usr/include/libnl-nss
	$(CP) $(PKG_BUILD_DIR)/lib/include/* $(STAGING_DIR)/usr/include/libnl-nss
endef

define Package/libnl-nss/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/lib/obj/libnl-nss.so $(1)/usr/lib
endef

define Package/nssinfo/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/nssinfo/obj/nssinfo $(1)/usr/sbin/
endef

$(eval $(call BuildPackage,nssinfo))
$(eval $(call BuildPackage,libnl-nss))
