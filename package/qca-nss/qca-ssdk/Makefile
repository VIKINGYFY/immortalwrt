include $(TOPDIR)/rules.mk

PKG_NAME:=qca-ssdk
PKG_RELEASE:=1

PKG_SOURCE_URL:=https://git.codelinaro.org/clo/qsdk/oss/lklm/qca-ssdk.git
PKG_SOURCE_PROTO:=git
PKG_SOURCE_DATE:=2025-11-14
PKG_SOURCE_VERSION:=d9a196497ecee2530722d906e0efe1b7408b6ef6
PKG_MIRROR_HASH:=4f7126ee44f5793081845137d8699c8b548d9833169f19487eceb6c4e3ef6d1e

PKG_FLAGS:=nonshared
PKG_BUILD_PARALLEL:=1
PKG_BUILD_FLAGS:=no-lto
PKG_EXTMOD_SUBDIRS:=mk

PKG_CONFIG_DEPENDS:= \
	CONFIG_TARGET_SUBTARGET

PKG_BUILD_DEPENDS:=qca-nss-phy

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

define KernelPackage/qca-ssdk
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=Qualcomm SSDK switch driver
  DEPENDS:=@TARGET_qualcommax \
	+kmod-dsa
  FILES:=$(PKG_BUILD_DIR)/build/bin/qca-ssdk.ko \
  $(LINUX_DIR)/drivers/net/mdio/mdio-bitbang.ko \
  $(LINUX_DIR)/drivers/net/mdio/mdio-i2c.ko

  AUTOLOAD:=$(call AutoLoad,30,qca-ssdk)
endef

define KernelPackage/qca-ssdk/description
  Driver for Qualcomm Atheros switches.
endef

GCC_VERSION:=$(shell echo "$(CONFIG_GCC_VERSION)" | sed 's/[^0-9.]*\([0-9.]*\).*/\1/')

LNX_CONFIG_OPTS:=LNX_MAKEOPTS='$(KERNEL_MAKEOPTS)' PRJ_PATH=$(PKG_BUILD_DIR) MODULE_TYPE=KSLIB modules

QCA_SSDK_SYMVERS:=$(PKG_BUILD_DIR)/mk/Module.symvers

SSDK_SUBTARGET:=$(subst ",,$(CONFIG_TARGET_SUBTARGET))

EXTRA_CFLAGS:= \
	-Wno-missing-prototypes \
	-Wno-missing-declarations \
	-I$(LINUX_DIR)/drivers/net/phy \
	-I$(STAGING_DIR)/usr/include

SSDK_MAKE_FLAGS:=$(MAKE_FLAGS) \
	TARGET_NAME=$(CONFIG_TARGET_NAME) \
	MK_PATH=$(PKG_BUILD_DIR)/mk \
	TOOL_PATH=$(firstword $(TOOLCHAIN_BIN_DIRS)) \
	SYS_PATH=$(LINUX_DIR) \
	TOOLPREFIX=$(TARGET_CROSS) \
	KVER=$(LINUX_VERSION) \
	ARCH=$(LINUX_KARCH) \
	TARGET_SUFFIX=$(CONFIG_TARGET_SUFFIX) \
	GCC_VERSION=$(GCC_VERSION) \
	EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
	SoC=$(SSDK_SUBTARGET) \
	SHELL="$(BASH)" \
	PTP_FEATURE=disable \
	SWCONFIG_FEATURE=disable \
	ISISC_ENABLE=disable \
	IN_QCA803X_PHY=FALSE \
	IN_QCA808X_PHY=FALSE \
	IN_MALIBU_PHY=FALSE \
	IN_MP_PHY=FALSE

ifeq ($(SSDK_SUBTARGET),ipq807x)
SSDK_MAKE_FLAGS += CHIP_TYPE=HPPE
else ifeq ($(SSDK_SUBTARGET),ipq60xx)
SSDK_MAKE_FLAGS += CHIP_TYPE=CPPE IN_AQUANTIA_PHY=TRUE
else ifeq ($(SSDK_SUBTARGET),ipq50xx)
SSDK_MAKE_FLAGS += CHIP_TYPE=MP
endif

define Build/Compile
	+$(MAKE) $(PKG_JOBS) -C $(PKG_BUILD_DIR)/mk $(SSDK_MAKE_FLAGS) $(LNX_CONFIG_OPTS)
	$(INSTALL_DIR) $(PKG_SYMVERS_DIR)
	$(CP) $(QCA_SSDK_SYMVERS) $(PKG_SYMVERS_DIR)/$(PKG_NAME).symvers
endef

define Build/InstallDev
	$(INSTALL_DIR) \
		$(1)/usr/include/qca-ssdk \
		$(1)/usr/include/qca-ssdk/api \
		$(1)/usr/include/qca-ssdk/ref \
		$(1)/usr/include/qca-ssdk/fal \
		$(1)/usr/include/qca-ssdk/sal \
		$(1)/usr/include/qca-ssdk/init
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/include/api/sw_ioctl.h $(1)/usr/include/qca-ssdk/api/
	if [ -f $(PKG_BUILD_DIR)/include/ref/ref_vsi.h ]; then \
		$(INSTALL_DATA) $(PKG_BUILD_DIR)/include/ref/ref_vsi.h $(1)/usr/include/qca-ssdk/ref/; \
	fi
	if [ -f $(PKG_BUILD_DIR)/include/ref/ref_fdb.h ]; then \
		$(INSTALL_DATA) $(PKG_BUILD_DIR)/include/ref/ref_fdb.h $(1)/usr/include/qca-ssdk/ref/; \
	fi
	if [ -f $(PKG_BUILD_DIR)/include/ref/ref_port_ctrl.h ]; then \
		$(INSTALL_DATA) $(PKG_BUILD_DIR)/include/ref/ref_port_ctrl.h $(1)/usr/include/qca-ssdk/ref/; \
	fi
	if [ -f $(PKG_BUILD_DIR)/include/init/ssdk_init.h ]; then \
		$(INSTALL_DATA) $(PKG_BUILD_DIR)/include/init/ssdk_init.h $(1)/usr/include/qca-ssdk/init/; \
	fi
	if [ -f $(PKG_BUILD_DIR)/include/init/ssdk_netlink.h ]; then \
		$(INSTALL_DATA) $(PKG_BUILD_DIR)/include/init/ssdk_netlink.h $(1)/usr/include/qca-ssdk/init/; \
	fi
	$(CP) $(PKG_BUILD_DIR)/include/fal $(1)/usr/include/qca-ssdk/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/include/common/*.h $(1)/usr/include/qca-ssdk/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/include/sal/os/linux/*.h $(1)/usr/include/qca-ssdk/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/include/sal/os/*.h $(1)/usr/include/qca-ssdk/
endef

$(eval $(call KernelPackage,qca-ssdk))
