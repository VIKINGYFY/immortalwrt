# SPDX-License-Identifier: GPL-2.0-only
#
# Runtime integration for Qualcomm NSS offloading on the qca_edma/qca_ppe
# (EDMA-rework) driver stack.

include $(TOPDIR)/rules.mk

PKG_NAME:=nss-tools
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/nss-tools
  SECTION:=net
  CATEGORY:=Network
  TITLE:=NSS offload runtime tools (nss-up, nss-status)
  DEPENDS:=@(TARGET_qualcommax) +kmod-qca-ppe-nss +kmod-qca-nss-drv \
	+kmod-qca-nss-ecm +kmod-qca-nss-drv-pppoe +kmod-qca-nss-drv-vlan-mgr \
	+kmod-qca-nss-drv-bridge-mgr \
	+kmod-qca-mcs +kmod-qca-nss-drv-qdisc +kmod-qca-nss-drv-igs \
	+sqm-scripts-nss +ethtool +ip-bridge
  PKGARCH:=all
endef

define Package/nss-tools/description
 Brings the opt-in NSS data plane up in the required order at boot (the
 'nss' service running /usr/sbin/nss-up: firmware arm, bridge/multicast/
 ECM/PPPoE managers, Wi-Fi offload flip, SQM), reports its health via
 /usr/sbin/nss-status (-j for JSON) and captures EDMA/PPE registers with
 edma-dump (needs a devmem-enabled busybox). Nothing autoloads: disable
 with "uci set nss.general.enabled='0'" to stay on the plain host stack.
 Pulls in the complete NSS runtime package set.
endef

define Package/nssqos
  SECTION:=net
  CATEGORY:=Network
  TITLE:=DSCP marking and NSS fast-lane prioritization
  DEPENDS:=@(TARGET_qualcommax) +nftables-json
  PKGARCH:=all
endef

define Package/nssqos/description
 Renders /etc/config/nssqos into a standalone nftables table that
 DSCP-marks matching connections and steers them into the strict-priority
 fast lane of the NSS SQM script (nss-edma.qos). Marking and steering
 apply to NSS-accelerated flows in both directions.
endef

define Package/nssqos/conffiles
/etc/config/nssqos
endef

define Package/luci-app-nss
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI status page for NSS offloading
  DEPENDS:=+nss-tools +luci-base
  PKGARCH:=all
endef

define Package/luci-app-nss/description
 Status -> NSS Offload: renders 'nss-status -j' (firmware state, cores,
 ports, ECM acceleration counts, SQM/fast-lane health) in LuCI.
endef

define Package/luci-app-nssqos
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI editor for NSS QoS marking rules
  DEPENDS:=+nssqos +luci-base
  PKGARCH:=all
endef

define Package/luci-app-nssqos/description
 Network -> QoS Marking (NSS): edit the DSCP marking and fast-lane
 prioritization rules of /etc/config/nssqos.
endef

define Build/Compile
endef

define Package/nss-tools/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/nss-up $(1)/usr/sbin/nss-up
	$(INSTALL_BIN) ./files/nss-status $(1)/usr/sbin/nss-status
	$(INSTALL_BIN) ./files/edma-dump $(1)/usr/sbin/edma-dump
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/nss.init $(1)/etc/init.d/nss
	$(INSTALL_BIN) ./files/nss-recovery $(1)/usr/sbin/nss-recovery
	$(INSTALL_BIN) ./files/nss-recovery.init $(1)/etc/init.d/nss-recovery
	$(INSTALL_DIR) $(1)/etc/hotplug.d/net
	$(INSTALL_DATA) ./files/nss-rps.hotplug $(1)/etc/hotplug.d/net/30-nss-rps
	$(INSTALL_DIR) $(1)/lib/nss
	$(INSTALL_DATA) ./files/functions.sh $(1)/lib/nss/functions.sh
	$(INSTALL_DIR) $(1)/etc/sysctl.d
	$(INSTALL_DATA) ./files/nss.sysctl $(1)/etc/sysctl.d/12-nss-networking.conf
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_DATA) ./files/nss-offload.defaults $(1)/etc/uci-defaults/98-nss-offload
endef

define Package/nssqos/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/nssqos.init $(1)/etc/init.d/nssqos
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/nssqos.config $(1)/etc/config/nssqos
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/nssqos-conns $(1)/usr/sbin/nssqos-conns
	$(INSTALL_BIN) ./files/nssqos-resolve $(1)/usr/sbin/nssqos-resolve
endef

define Package/luci-app-nss/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/nss
	$(INSTALL_DATA) ./files/luci-nss-status.js $(1)/www/luci-static/resources/view/nss/status.js
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ./files/luci-app-nss.menu.json $(1)/usr/share/luci/menu.d/luci-app-nss.json
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./files/luci-app-nss.acl.json $(1)/usr/share/rpcd/acl.d/luci-app-nss.json
endef

define Package/luci-app-nssqos/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/nss
	$(INSTALL_DATA) ./files/luci-nss-qos.js $(1)/www/luci-static/resources/view/nss/qos.js
	$(INSTALL_DATA) ./files/luci-nss-connections.js $(1)/www/luci-static/resources/view/nss/connections.js
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ./files/luci-app-nssqos.menu.json $(1)/usr/share/luci/menu.d/luci-app-nssqos.json
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./files/luci-app-nssqos.acl.json $(1)/usr/share/rpcd/acl.d/luci-app-nssqos.json
endef

$(eval $(call BuildPackage,nss-tools))
$(eval $(call BuildPackage,nssqos))
$(eval $(call BuildPackage,luci-app-nss))
$(eval $(call BuildPackage,luci-app-nssqos))
