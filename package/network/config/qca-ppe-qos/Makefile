include $(TOPDIR)/rules.mk

PKG_NAME:=qca-ppe-qos
PKG_VERSION:=20261117
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=Julius Bairaktaris <julius@bairaktaris.de>

PKG_FLAGS:=nonshared

include $(INCLUDE_DIR)/package.mk

define Package/qca-ppe-qos
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Qualcomm PPE hardware QoS
  DEPENDS:=@(TARGET_qualcommax_ipq807x||TARGET_qualcommax_ipq60xx||TARGET_qualcommbe) \
	+tc +kmod-sched-core +ucode +ucode-mod-fs +ucode-mod-uci \
	+KERNEL_DCB:dcb +TARGET_qualcommax:kmod-sched-flower \
	+TARGET_qualcommax:kmod-sched-act-pedit +TARGET_qualcommax:kmod-sched-prio \
	+TARGET_qualcommax:kmod-sched-ets +TARGET_qualcommax:kmod-sched-mqprio \
	+TARGET_qualcommax:kmod-sched-act-police
  CONFLICTS:=ppe-qos
  EXTRA_DEPENDS:=$(if $(CONFIG_USE_APK),!ppe-qos (>=0))
endef

define Package/qca-ppe-qos/description
Configures AX PPE hardware classifiers and BE physical-port TBF shapers.
BE shaping verifies hardware acceptance and preserves other root qdiscs.
AX classifiers can be used alongside a separate rate-shaping service.
endef

define Build/Compile
endef

define Package/qca-ppe-qos/conffiles
/etc/config/qca-ppe-qos
endef

define Package/qca-ppe-qos/install
	$(INSTALL_DIR) $(1)/etc/config $(1)/etc/init.d
	$(INSTALL_CONF) ./files/qca-ppe-qos.config $(1)/etc/config/qca-ppe-qos
	$(INSTALL_BIN) ./files/qca-ppe-qos.init $(1)/etc/init.d/qca-ppe-qos
	$(INSTALL_DIR) $(1)/etc/hotplug.d/net
	$(INSTALL_DATA) ./files/qca-ppe-qos.hotplug $(1)/etc/hotplug.d/net/20-qca-ppe-qos
	$(INSTALL_DIR) $(1)/etc/uci-defaults $(1)/usr/share/qca-ppe-qos
	$(INSTALL_BIN) ./files/qca-ppe-qos.migrate $(1)/etc/uci-defaults/91-qca-ppe-qos-migrate
	$(INSTALL_CONF) ./files/qca-ppe-qos.config $(1)/usr/share/qca-ppe-qos/config.default
	$(INSTALL_DIR) $(1)/usr/libexec/rpcd $(1)/etc/hotplug.d/iface
	$(INSTALL_BIN) ./files/qca-ppe-qos.uc $(1)/usr/libexec/qca-ppe-qos.uc
	$(INSTALL_BIN) ./files/qca-ppe-qos.rpcd $(1)/usr/libexec/rpcd/luci.qca-ppe-qos
	$(INSTALL_DATA) ./files/qca-ppe-qos.iface $(1)/etc/hotplug.d/iface/20-qca-ppe-qos
endef

$(eval $(call BuildPackage,qca-ppe-qos))
