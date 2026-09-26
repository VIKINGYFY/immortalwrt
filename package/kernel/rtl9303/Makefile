include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=rtl9303-spi
PKG_VERSION:=20260926
PKG_RELEASE:=6
PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_PARALLEL:=1
include $(INCLUDE_DIR)/package.mk

define KernelPackage/dsa-rtl9303-spi
  SUBMENU:=Network Devices
  TITLE:=Realtek RTL9303 external SPI switch
  DEPENDS:=@TARGET_qualcommax +kmod-dsa +kmod-regmap-core +kmod-phy-realtek +kmod-phy-aquantia
  KCONFIG:=CONFIG_NET_DSA_TAG_RTL9303_8021AD CONFIG_PHY_COMMON_PROPS=y
  FILES:= \
	$(PKG_BUILD_DIR)/rtl9303-spi.ko \
	$(PKG_BUILD_DIR)/rtl9303-dsa.ko \
	$(PKG_BUILD_DIR)/rtl9303-mdio.ko \
	$(PKG_BUILD_DIR)/rtl9303-mdio-serdes.ko \
	$(PKG_BUILD_DIR)/rtl9303-pcs.ko \
	$(LINUX_DIR)/net/dsa/tag_rtl9303_8021ad.ko
  AUTOLOAD:=$(call AutoLoad,50,rtl9303-mdio rtl9303-mdio-serdes rtl9303-pcs rtl9303-dsa rtl9303-spi,1)
endef

define KernelPackage/dsa-rtl9303-spi/install
	$(call ModuleAutoLoad,dsa-rtl9303-tag,$(1),49-,1,tag_rtl9303_8021ad)
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,dsa-rtl9303-spi))
