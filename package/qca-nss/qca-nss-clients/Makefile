include $(TOPDIR)/rules.mk

PKG_NAME:=qca-nss-clients
PKG_RELEASE:=8

# Source: https://git.codelinaro.org/clo/qsdk/oss/lklm/nss-clients.git @ 51be82d (pre-generated 'git archive' of the commit).
# Fetched from our release mirror so the download is deterministic and hash-verified;
# git-proto regenerates the zstd archive locally and its hash varies by toolchain.
PKG_SOURCE_DATE:=2024-09-11
PKG_SOURCE_VERSION:=51be82d
QSDK_VERSION:=12.5
PKG_VERSION:=$(QSDK_VERSION).$(subst -,.,$(PKG_SOURCE_DATE))~$(PKG_SOURCE_VERSION)
PKG_SOURCE:=$(PKG_NAME)-$(subst ~,.,$(PKG_VERSION)).tar.zst
PKG_SOURCE_URL:=https://github.com/JuliusBairaktaris/nss-packages/releases/download/nss-source-tarballs
PKG_HASH:=876c3898cb4723fccd4a1c870324724dd7c4061ac89ec88db2226fd749c18b38

PKG_BUILD_PARALLEL:=1

PKG_CONFIG_DEPENDS:=\
	CONFIG_NSS_DRV_IGS_ENABLE \
	CONFIG_NSS_DRV_PPPOE_ENABLE \
	CONFIG_NSS_DRV_SHAPER_ENABLE \
	CONFIG_NSS_FIRMWARE_VERSION_11_4 \
	CONFIG_NSS_FIRMWARE_VERSION_12_5

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

ifeq ($(CONFIG_TARGET_SUBTARGET), "ipq807x")
  SOC="ipq807x_64"
  subtarget:=$(CONFIG_TARGET_SUBTARGET)
else ifeq ($(CONFIG_TARGET_SUBTARGET), "ipq60xx")
  SOC="ipq60xx_64"
  subtarget:=$(CONFIG_TARGET_SUBTARGET)
else ifeq ($(CONFIG_TARGET_SUBTARGET), "ipq50xx")
  SOC="ipq50xx_64"
  subtarget:=$(CONFIG_TARGET_SUBTARGET)
else
  SOC=$(CONFIG_TARGET_BOARD)
endif

define KernelPackage/qca-nss-drv-pppoe
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS connection manager for PPPoE protocol
  DEPENDS:=@(TARGET_qualcommax||TARGET_ipq806x) \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_PPPOE_ENABLE \
	   +kmod-ppp \
	   +kmod-pppoe
  FILES:=$(PKG_BUILD_DIR)/pppoe/qca-nss-pppoe.ko
endef

define KernelPackage/qca-nss-drv-pppoe/Description
Kernel modules for NSS connection manager - Support for PPPoE
endef

define KernelPackage/qca-nss-drv-vlan-mgr
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS VLAN interface manager
  DEPENDS:=@(TARGET_qualcommax||TARGET_ipq806x) \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_VLAN_ENABLE
  FILES:=$(PKG_BUILD_DIR)/vlan/qca-nss-vlan.ko
endef

define KernelPackage/qca-nss-drv-vlan-mgr/Description
Registers 802.1Q upper devices of NSS physical interfaces as firmware
VLAN interfaces, so the firmware delivers tagged traffic to the VLAN
net device instead of untagging it onto the parent.
endef

define KernelPackage/qca-nss-drv-qdisc
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=Qdisc for configuring shapers in NSS
  DEPENDS:=@(TARGET_qualcommax||TARGET_ipq806x) \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_SHAPER_ENABLE \
	   +@NSS_DRV_IGS_ENABLE
  FILES:=$(PKG_BUILD_DIR)/nss_qdisc/qca-nss-qdisc.ko
  KCONFIG:=CONFIG_NET_CLS_ACT=y
endef

define KernelPackage/qca-nss-drv-qdisc/Description
Linux qdisc that aids in configuring shapers in the NSS
endef

define KernelPackage/qca-nss-drv-igs
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS connection manager to perform ingress shaping
  DEPENDS:=@(TARGET_qualcommax||TARGET_ipq806x) \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_IGS_ENABLE \
	   +kmod-qca-nss-drv-qdisc \
	   +kmod-ifb \
	   +kmod-nf-conntrack \
	   +kmod-sched-core
  FILES:=$(PKG_BUILD_DIR)/nss_qdisc/igs/act_nssmirred.ko
endef

define KernelPackage/qca-nss-drv-igs/description
Linux action that helps in offloading traffic to an IFB interface to perform ingress shaping.
endef

define KernelPackage/qca-nss-drv-bridge-mgr
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS bridge manager (hardware bridge offload)
  DEPENDS:=@(TARGET_qualcommax_ipq807x||TARGET_qualcommax_ipq60xx) \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_BRIDGE_ENABLE \
	   +kmod-qca-ppe-nss
  FILES:=$(PKG_BUILD_DIR)/bridge/qca-nss-bridge-mgr.ko
endef

define KernelPackage/qca-nss-drv-bridge-mgr/Description
Offloads Linux-bridge L2 forwarding into the NSS firmware by binding the
bridge's qca_ppe member ports to a firmware bridge interface. No AUTOLOAD:
loaded at runtime after the NSS data plane is up (see nss-up).
endef

define KernelPackage/qca-nss-drv-netlink
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS netlink manager (userspace control and stats interface)
  DEPENDS:=@TARGET_qualcommax \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_IPV6_ENABLE \
	   +@NSS_DRV_WIFIOFFLOAD_ENABLE
  FILES:=$(PKG_BUILD_DIR)/netlink/qca-nss-netlink.ko
endef

define KernelPackage/qca-nss-drv-netlink/Description
Generic-netlink families exposing NSS firmware state (per-interface and
per-subsystem statistics, dynamic interfaces, rule injection) to
userspace tools such as nssinfo.
endef

define KernelPackage/qca-nss-drv-netlink/config
	source "$(SOURCE)/Config.in"
endef

define KernelPackage/qca-nss-drv-mirror
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS interface mirroring (copies firmware-path packets to a host netdev for capture)
  DEPENDS:=@TARGET_qualcommax \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_MIRROR_ENABLE
  FILES:=$(PKG_BUILD_DIR)/mirror/qca-nss-mirror.ko
endef

define KernelPackage/qca-nss-drv-mirror/Description
Creates mirror netdevs that receive copies of packets flowing through
the NSS firmware, making offloaded traffic visible to host capture
tools (tcpdump). Configured via /proc/sys/dev/nss/mirror/config.
endef

define KernelPackage/qca-nss-drv-wifi-meshmgr
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS Wi-Fi mesh manager (802.11s mesh offload)
  DEPENDS:=@TARGET_qualcommax \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_WIFI_MESH_ENABLE
  FILES:=$(PKG_BUILD_DIR)/wifi_meshmgr/qca-nss-wifi-meshmgr.ko
endef

define KernelPackage/qca-nss-drv-wifi-meshmgr/Description
Manages NSS firmware Wi-Fi mesh interfaces (mesh path and proxy path
tables) for 802.11s mesh offload. No AUTOLOAD of its own: ath11k links
against it when built with ATH11K_NSS_MESH_SUPPORT, so it loads as a
module dependency; its init is inert until a mesh device is created.
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/qca-nss-clients
	$(CP) $(PKG_BUILD_DIR)/netlink/include/* $(1)/usr/include/qca-nss-clients/
	$(CP) $(PKG_BUILD_DIR)/exports/* $(1)/usr/include/qca-nss-clients/
endef

# Vendor code is not warning-clean; keep warnings non-fatal. The accumulated
# EXTRA_CFLAGS are handed to kbuild via NOSTDINC_FLAGS: kbuild dropped
# EXTRA_CFLAGS support in kernel 6.15.
EXTRA_CFLAGS+= -Wno-error

EXTRA_CFLAGS+= \
	-I$(STAGING_DIR)/usr/include/qca-nss-drv

# Build individual packages if selected
ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-qdisc),)
NSS_CLIENTS_MAKE_OPTS+=qdisc=y
# PPE shaper mode and NSS-bridge qdisc attach need qca-ssdk and
# bridge-mgr, neither of which exists on the EDMA stack. The NSS-fw
# accel mode (accel_mode 0) is unaffected.
NSS_CLIENTS_MAKE_OPTS+=NSS_QDISC_PPE_ENABLE=n
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-igs),)
NSS_CLIENTS_MAKE_OPTS+=igs=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-pppoe),)
NSS_CLIENTS_MAKE_OPTS+=pppoe=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-vlan-mgr),)
NSS_CLIENTS_MAKE_OPTS+=vlan-mgr=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-bridge-mgr),)
NSS_CLIENTS_MAKE_OPTS+=bridge-mgr=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-netlink),)
NSS_CLIENTS_MAKE_OPTS+=netlink=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-mirror),)
NSS_CLIENTS_MAKE_OPTS+=mirror=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-wifi-meshmgr),)
NSS_CLIENTS_MAKE_OPTS+=wifi-meshmgr=y
endif

# The 11.4 firmware line uses the per-node shaper statistics wire format
# (see the nss_qdisc 11.4 stats patch); select it to match the firmware.
ifdef CONFIG_NSS_FIRMWARE_VERSION_11_4
EXTRA_CFLAGS+=-DNSS_FIRMWARE_VERSION_11_4
endif

EXTRA_CFLAGS+= \
	-Wno-missing-prototypes \
	-Wno-missing-declarations \
	-Wno-empty-body

define Build/Compile
	+$(MAKE) -C "$(LINUX_DIR)" $(strip $(NSS_CLIENTS_MAKE_OPTS)) \
	  CROSS_COMPILE="$(TARGET_CROSS)" \
	  ARCH="$(LINUX_KARCH)" \
	  M="$(PKG_BUILD_DIR)" \
	  NOSTDINC_FLAGS="$(KERNEL_NOSTDINC_FLAGS) $(EXTRA_CFLAGS)" \
	  CFLAGS_MODULE="-include $(PKG_BUILD_DIR)/compat.h -Wno-error" \
	  SoC=$(SOC) \
	  $(KERNEL_MAKE_FLAGS) \
	  $(PKG_JOBS) \
	  modules
endef

$(eval $(call KernelPackage,qca-nss-drv-pppoe))
$(eval $(call KernelPackage,qca-nss-drv-vlan-mgr))
$(eval $(call KernelPackage,qca-nss-drv-qdisc))
$(eval $(call KernelPackage,qca-nss-drv-igs))
$(eval $(call KernelPackage,qca-nss-drv-bridge-mgr))
$(eval $(call KernelPackage,qca-nss-drv-netlink))
$(eval $(call KernelPackage,qca-nss-drv-mirror))
$(eval $(call KernelPackage,qca-nss-drv-wifi-meshmgr))
