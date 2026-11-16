#
# tpm2-tools: the command-line tools for a TPM 2.0 (tpm2_getrandom,
# tpm2_getcap, tpm2_pcrread, tpm2_createprimary, tpm2_create, tpm2_load,
# tpm2_sign, tpm2_unseal and the rest), as one multi-call binary, tpm2, with a
# link per tool. Built on libtss2, without the FAPI tools.
#
# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=tpm2-tools
PKG_VERSION:=5.8
PKG_RELEASE:=1

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/tpm2-software/tpm2-tools/releases/download/$(PKG_VERSION)
PKG_HASH:=1cb73185cae814b4e15c7c2d0b22642d640faf48775f4156a1fd92edf84bef73

PKG_LICENSE:=BSD-3-Clause
PKG_LICENSE_FILES:=docs/LICENSE

PKG_INSTALL:=1
PKG_BUILD_PARALLEL:=1
PKG_BUILD_DEPENDS:=tpm2-tss

include $(INCLUDE_DIR)/package.mk

define Package/tpm2-tools
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Command-line tools for a TPM 2.0
  URL:=https://github.com/tpm2-software/tpm2-tools
  DEPENDS:=+libtss2 +libopenssl +libcurl
endef

define Package/tpm2-tools/description
 The tpm2_* tools for a TPM 2.0: random numbers, capabilities, PCRs, keys
 (create, load, sign, verify), sealing and unsealing, NV storage. They talk to
 the kernel's resource manager, /dev/tpmrm0.
endef

CONFIGURE_ARGS += \
	--disable-fapi \
	--disable-hardening \
	--disable-unit \
	--without-efivar

define Package/tpm2-tools/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/tpm2 $(1)/usr/bin/
	# one link per tool (tpm2_getrandom, tpm2_getcap, ...), as upstream installs them
	for t in $$$$(cd $(PKG_INSTALL_DIR)/usr/bin && ls tpm2_*); do \
		ln -sf tpm2 $(1)/usr/bin/$$$$t; \
	done
endef

$(eval $(call BuildPackage,tpm2-tools))
