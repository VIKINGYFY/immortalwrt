include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=rtl9303-spi
PKG_VERSION:=20260921
PKG_RELEASE:=3
PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_PARALLEL:=1
PKG_FILE_DEPENDS:=$(TOPDIR)/target/linux/realtek/files-6.18/drivers/net/mdio/mdio-realtek-otto.c \
	$(TOPDIR)/target/linux/realtek/files-6.18/drivers/net/pcs/pcs-rtl-otto.c \
	$(TOPDIR)/target/linux/realtek/files-6.18/drivers/net/mdio/mdio-realtek-otto-serdes.c

include $(INCLUDE_DIR)/package.mk

define KernelPackage/dsa-rtl9303-spi
  SUBMENU:=Network Devices
  TITLE:=Realtek RTL9303 external SPI switch
  DEPENDS:=@TARGET_qualcommax +kmod-dsa +kmod-regmap-core +kmod-phy-realtek +kmod-phy-aquantia
  KCONFIG:=CONFIG_NET_DSA_TAG_RTL9303_8021AD CONFIG_PHY_COMMON_PROPS=y
  FILES:= \
	$(PKG_BUILD_DIR)/rtl9303-spi.ko \
	$(PKG_BUILD_DIR)/rtl9303-dsa.ko \
	$(PKG_BUILD_DIR)/mdio-rtl9303.ko \
	$(PKG_BUILD_DIR)/mdio-rtl9303-serdes.ko \
	$(PKG_BUILD_DIR)/pcs-rtl9303.ko \
	$(LINUX_DIR)/net/dsa/tag_rtl9303_8021ad.ko
  AUTOLOAD:=$(call AutoLoad,50,mdio-rtl9303 mdio-rtl9303-serdes pcs-rtl9303 rtl9303-dsa rtl9303-spi,1)
endef

define KernelPackage/dsa-rtl9303-spi/install
	$(call ModuleAutoLoad,dsa-rtl9303-tag,$(1),49-,1,tag_rtl9303_8021ad)
	$(INSTALL_DIR) $(1)/etc
	echo 'cr1000a-$(PKG_VERSION)-r$(PKG_RELEASE)' > $(1)/etc/cr1000a-build
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) $(TOPDIR)/target/linux/realtek/files-6.18/drivers/net/mdio/mdio-realtek-otto.c $(PKG_BUILD_DIR)/mdio-rtl9303.c
	$(CP) $(TOPDIR)/target/linux/realtek/files-6.18/drivers/net/mdio/mdio-realtek-otto-serdes.c $(PKG_BUILD_DIR)/mdio-rtl9303-serdes.c
	$(CP) $(TOPDIR)/target/linux/realtek/files-6.18/drivers/net/pcs/pcs-rtl-otto.c $(PKG_BUILD_DIR)/pcs-rtl9303.c
	$(PYTHON) ./prepare.py $(PKG_BUILD_DIR)
	echo '#define CR1000A_BUILD_TAG "cr1000a-$(PKG_VERSION)-r$(PKG_RELEASE)"' > $(PKG_BUILD_DIR)/rtl9303-version.h
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,dsa-rtl9303-spi))
