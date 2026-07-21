include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=qca-ppe-nss
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/qca-ppe-nss
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS data-plane glue for the qca_edma/qca_ppe stack
  DEPENDS:=@TARGET_qualcommax
  FILES:=$(PKG_BUILD_DIR)/qca-ppe-nss.ko
endef

define KernelPackage/qca-ppe-nss/description
  Implements the nss-dp data-plane override API on top of the DSA user
  ports created by the qca_ppe switch driver, so qca-nss-drv can attach
  the NSS firmware fast path to the qca_edma ethernet stack.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
	$(if $(QUILT),mkdir -p $(PKG_BUILD_DIR)/patches)
	$(if $(QUILT),touch $(PKG_BUILD_DIR)/.quilt_used)
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/qca-ppe-nss
	$(CP) $(PKG_BUILD_DIR)/exports/nss_dp_api_if.h $(1)/usr/include/qca-ppe-nss/
endef

$(eval $(call KernelPackage,qca-ppe-nss))
