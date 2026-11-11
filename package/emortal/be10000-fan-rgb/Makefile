include $(TOPDIR)/rules.mk

PKG_NAME:=be10000-fan-rgb
PKG_VERSION:=2026.09.02
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=zh8416
PKGARCH:=all

include $(INCLUDE_DIR)/package.mk

define Package/be10000-fan-rgb
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Xiaomi BE10000 smart fan and RGB ambient LED controls
  DEPENDS:=+ucode +ucode-mod-fs +ucode-mod-uci +ucode-mod-uloop \
	+coreutils-timeout +luci-base +rpcd +rpcd-mod-file
  CONFFILES:=/etc/config/be10000_rgb /etc/config/fancontrol
endef

define Package/be10000-fan-rgb/description
  Temperature-based fan control (PWM + RPM readout, 4 presets, failsafe) and
  a seven-mode RGB ambient light bar controller for the Xiaomi BE10000.
  Ported from the ImmortalWrt v26 BE10000 deep-adaptation firmware.
  Requires the board DTS to expose pwm-leds as red/green/blue:ambient and
  the EMC230x fan controller on I2C address 0x2f.
endef

define Build/Prepare
endef

define Build/Compile
endef

define Package/be10000-fan-rgb/install
	$(INSTALL_DIR) $(1)/etc/config $(1)/etc/init.d $(1)/etc/uci-defaults \
		$(1)/sbin $(1)/usr/libexec $(1)/usr/sbin \
		$(1)/usr/share/luci/menu.d $(1)/usr/share/rpcd/acl.d \
		$(1)/usr/share/ucitrack \
		$(1)/www/luci-static/resources/view/system
	$(CP) ./files/* $(1)/
	chmod 0755 $(1)/etc/init.d/be10000-* \
		$(1)/etc/uci-defaults/99-be10000-fan-rgb \
		$(1)/usr/libexec/be10000-*.uc \
		$(1)/usr/sbin/be10000-rgb \
		$(1)/sbin/faninfo
endef

$(eval $(call BuildPackage,be10000-fan-rgb))
