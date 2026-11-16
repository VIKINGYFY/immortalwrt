#
# tpm2-tss: the TPM 2.0 software stack (TSS) from the Trusted Computing Group's
# specifications, for using a TPM from userspace: ESYS, SYS, MU, RC, the TCTI
# loader, and the TCTIs that talk to the kernel's /dev/tpmrm0 (device) or a
# simulator (mssim, swtpm). FAPI and the policy library, which pull in json-c,
# libcurl and libuuid, are not built.
#
# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=tpm2-tss
PKG_VERSION:=4.2.0
PKG_RELEASE:=1

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/tpm2-software/tpm2-tss/releases/download/$(PKG_VERSION)
PKG_HASH:=b53f0c5c8c4ce17f05701a410ca9688f725ca380c9bc4640eacd0eadb1fea124

PKG_LICENSE:=BSD-2-Clause
PKG_LICENSE_FILES:=LICENSE

PKG_INSTALL:=1
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/libtss2
  SECTION:=libs
  CATEGORY:=Libraries
  TITLE:=TPM 2.0 software stack (ESYS, SYS, MU, RC, TCTI loader)
  URL:=https://github.com/tpm2-software/tpm2-tss
  DEPENDS:=+libopenssl
  ABI_VERSION:=0
endef

define Package/libtss2/description
 The TPM 2.0 software stack: the Enhanced System API (ESYS), the System API
 (SYS), marshalling (MU), response codes (RC), the TCTI loader, and the TCTIs
 for the kernel's TPM device (/dev/tpmrm0) and for a TPM simulator.
endef

CONFIGURE_ARGS += \
	--disable-fapi \
	--disable-policy \
	--with-crypto=ossl \
	--disable-tcti-pcap \
	--disable-tcti-libtpms \
	--disable-tcti-cmd \
	--disable-tcti-spi-helper \
	--disable-tcti-spi-ltt2go \
	--disable-tcti-spidev \
	--disable-tcti-spi-ftdi \
	--disable-tcti-i2c-helper \
	--disable-tcti-i2c-ftdi \
	--disable-unit \
	--disable-integration \
	--disable-doxygen-doc \
	--disable-defaultflags \
	--disable-log-file \
	--with-maxloglevel=warning \
	--with-runstatedir=/var/run \
	--with-udevrulesdir=/lib/udev/rules.d

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include $(1)/usr/lib/pkgconfig
	$(CP) $(PKG_INSTALL_DIR)/usr/include/tss2 $(1)/usr/include/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libtss2-*.so* $(1)/usr/lib/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/pkgconfig/tss2-*.pc $(1)/usr/lib/pkgconfig/
endef

define Package/libtss2/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libtss2-*.so.* $(1)/usr/lib/
	# the TCTI loader opens TCTIs by their unversioned names
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libtss2-tcti-*.so $(1)/usr/lib/
endef

$(eval $(call BuildPackage,libtss2))
