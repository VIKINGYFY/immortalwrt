include $(TOPDIR)/rules.mk

PKG_NAME:=qca-mcs
PKG_RELEASE:=2

# Source: https://git.codelinaro.org/clo/qsdk/oss/lklm/qca-mcs.git @ 063a4679ed22b1610925d3809397ff82308d4d00 (pre-generated 'git archive' of the commit).
# Fetched from our release mirror so the download is deterministic and hash-verified;
# git-proto regenerates the zstd archive locally and its hash varies by toolchain.
PKG_SOURCE_DATE:=2026-04-29
PKG_SOURCE_VERSION:=063a4679ed22b1610925d3809397ff82308d4d00
QSDK_VERSION:=14.0
PKG_VERSION:=$(QSDK_VERSION).$(subst -,.,$(PKG_SOURCE_DATE))~$(PKG_SOURCE_VERSION)
PKG_SOURCE:=$(PKG_NAME)-$(subst ~,.,$(PKG_VERSION)).tar.zst
PKG_SOURCE_URL:=https://github.com/JuliusBairaktaris/nss-packages/releases/download/nss-source-tarballs
PKG_HASH:=c362bd1197d5709548d073dd73c3c3f97fc67bf12682a0f72dcee2a0c5551f60

PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

define KernelPackage/qca-mcs
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Support
  URL:=http://www.qca.qualcomm.com
  MAINTAINER:=Qualcomm Atheros, Inc.
  TITLE:=QCA Multicast Snooping Support
  DEPENDS:=+@KERNEL_IPV6_MROUTE +@KERNEL_IP_MROUTE
  KCONFIG:=CONFIG_NETFILTER=y
  FILES:=$(PKG_BUILD_DIR)/qca-mcs.ko
  # No AUTOLOAD: qca-mcs hooks the bridge forwarding path, so the default
  # (NSS-off) image must stay byte-identical. It is loaded by nss-up before
  # ECM, with the rest of the runtime opt-in NSS plane.
endef

define KernelPackage/qca-mcs/Description
  This package installs the IGMP/MLD Snooping Module
endef

QCA_MC_SNOOPING_HEADERS= \
            $(PKG_BUILD_DIR)/mc_api.h \
            $(PKG_BUILD_DIR)/mc_ecm.h \

define Build/InstallDev
  mkdir -p $(1)/usr/include/qca-mcs
	$(foreach header_file,$(QCA_MC_SNOOPING_HEADERS), $(CP) $(header_file) $(1)/usr/include/qca-mcs;)
endef

EXTRA_CFLAGS+=-Wno-implicit-fallthrough

QCA_MC_SNOOPING_MAKE_OPTS:= \
        $(KERNEL_MAKE_FLAGS) \
        CONFIG_SUPPORT_MLD=y \
        MDIR=$(PKG_BUILD_DIR) \
        KBUILDPATH=$(LINUX_DIR) \
        KERNELPATH=$(LINUX_SRC_DIR) \
        KERNELRELEASE=$(LINUX_RELEASE)

define Build/Compile
	+$(MAKE) -C $(LINUX_DIR) \
      $(KERNEL_MAKE_FLAGS) \
      $(PKG_JOBS) \
      KBUILDPATH=$(LINUX_DIR) \
      $(PKG_MAKE_FLAGS) \
      M=$(PKG_BUILD_DIR) \
      NOSTDINC_FLAGS="$(KERNEL_NOSTDINC_FLAGS) $(EXTRA_CFLAGS)" \
      $(strip $(QCA_MC_SNOOPING_MAKE_OPTS)) \
      modules
endef

$(eval $(call KernelPackage,qca-mcs))
