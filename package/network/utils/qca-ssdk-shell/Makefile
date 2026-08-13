include $(TOPDIR)/rules.mk

PKG_NAME:=qca-ssdk-shell
PKG_RELEASE:=1

PKG_SOURCE_URL:=https://git.codelinaro.org/clo/qsdk/oss/ssdk-shell.git
PKG_SOURCE_PROTO:=git
PKG_SOURCE_DATE:=2022-09-12
PKG_SOURCE_VERSION:=b43fba2d2c6805d54b70d3d43363b689f729db54
PKG_MIRROR_HASH:=9bfc36a8db4f14959d79d34f8f18e5b1b1611b6eea340d7735600a6fce8a8bfb

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

define Package/qca-ssdk-shell
  SECTION:=QCA
  CATEGORY:=Utilities
  TITLE:=Shell application for QCA SSDK
endef

define Package/qca-ssdk-shell/description
  This package contains a qca-ssdk shell application for QCA chipset.
endef

ifndef CONFIG_TOOLCHAIN_BIN_PATH
CONFIG_TOOLCHAIN_BIN_PATH=$(TOOLCHAIN_DIR)/bin
endif

QCASSDK_CONFIG_OPTS += \
	TOOL_PATH=$(CONFIG_TOOLCHAIN_BIN_PATH) \
	SYS_PATH=$(LINUX_DIR) \
	TOOLPREFIX=$(TARGET_CROSS) \
	KVER=$(LINUX_VERSION) \
	CFLAGS="$(TARGET_CFLAGS)" \
	LDFLAGS="$(TARGET_LDFLAGS)" \
	ARCH=$(LINUX_KARCH)

define Build/Compile
	$(MAKE) -j1 -C $(PKG_BUILD_DIR) $(strip $(QCASSDK_CONFIG_OPTS))
endef

define Package/qca-ssdk-shell/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/bin/ssdk_sh $(1)/usr/sbin/
endef

$(eval $(call BuildPackage,qca-ssdk-shell))
