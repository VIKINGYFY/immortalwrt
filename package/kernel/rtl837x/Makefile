include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=dsa-rtl837x
PKG_VERSION:=1.0
PKG_RELEASE:=2
PKG_MAINTAINER:=air jinkela (air_jinkela@163.com)
PKG_LICENSE:=GPL-2.0-only AND LicenseRef-Realtek-Proprietary
PKG_LICENSE_FILES:=LICENSE LICENSE.realtek

include $(INCLUDE_DIR)/package.mk

define KernelPackage/dsa-rtl837x
  SUBMENU:=Network Devices
  TITLE:=Realtek RTL8372N DSA switch driver
  KCONFIG:=CONFIG_NET_DSA_TAG_VSC73XX_8021Q
  FILES:= \
	$(PKG_BUILD_DIR)/rtl837x_dsa.ko \
	$(LINUX_DIR)/net/dsa/tag_vsc73xx_8021q.ko
  AUTOLOAD:=$(call AutoLoad,99,rtl837x_dsa)
  DEPENDS:=+kmod-dsa +kmod-regmap-core
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) \
		M="$(PKG_BUILD_DIR)" \
		EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
		CONFIG_RTL837X_DSA=m \
		modules
endef

$(eval $(call KernelPackage,dsa-rtl837x))
