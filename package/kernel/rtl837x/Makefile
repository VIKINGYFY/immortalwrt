include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=dsa-rtl837x
PKG_VERSION:=20261107
PKG_RELEASE:=9

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/airjinkela/rtl837x-dsa-driver.git
PKG_SOURCE_DATE:=2026-10-05
PKG_SOURCE_VERSION:=b14546dbeabf500c287a6ff7ce847919a02d4a26
PKG_MIRROR_HASH:=b59e03205446d041920119b0798f9745387de4c4a4e51f47621de5e9dff0f68b

PKG_MAINTAINER:=StarField Xu <air_jinkela@163.com>
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_PARALLEL:=1
PKG_EXTMOD_SUBDIRS:=src

include $(INCLUDE_DIR)/package.mk

define KernelPackage/dsa-rtl837x
  SUBMENU:=Network Devices
  TITLE:=Realtek RTL8372N DSA switch driver
  KCONFIG:=CONFIG_NET_DSA_TAG_RTL837X_8021AD
  FILES:= \
	$(PKG_BUILD_DIR)/src/rtl8372n_dsa.ko \
	$(LINUX_DIR)/net/dsa/tag_rtl837x_8021ad.ko
  AUTOLOAD:=$(call AutoLoad,99,rtl8372n_dsa)
  DEPENDS:=+kmod-dsa +kmod-regmap-core
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) \
		M="$(PKG_BUILD_DIR)/src" \
		EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
		CONFIG_RTL8372N_DSA=m \
		CONFIG_RTL837X_PHY_PATCH=n \
		CONFIG_RTL8372N_DSA_DEBUG=n \
		modules
endef

$(eval $(call KernelPackage,dsa-rtl837x))
