include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=dsa-rtl837x
PKG_VERSION:=20261108
PKG_RELEASE:=4

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/airjinkela/rtl837x-dsa-driver.git
PKG_SOURCE_DATE:=2026-11-04
PKG_SOURCE_VERSION:=cb3005df67a7fe5e6e8c54a66566bdbf28a6b278
PKG_SOURCE_SUBDIR:=$(PKG_NAME)-$(PKG_SOURCE_DATE)-$(PKG_SOURCE_VERSION)
PKG_SOURCE:=$(PKG_NAME)-$(PKG_SOURCE_VERSION).tar.zst
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_SOURCE_SUBDIR)
PKG_MIRROR_HASH:=6e356b5d73ae4116ef4f07dea42b70051788add585399bb5c5c73c119c858288

PKG_MAINTAINER:=StarField Xu <air_jinkela@163.com>
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_PARALLEL:=1
PKG_EXTMOD_SUBDIRS:=src

include $(INCLUDE_DIR)/package.mk

define KernelPackage/$(PKG_NAME)
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
		CONFIG_RTL8372N_DSA_DEBUG=n \
		modules
endef

$(eval $(call KernelPackage,$(PKG_NAME)))
