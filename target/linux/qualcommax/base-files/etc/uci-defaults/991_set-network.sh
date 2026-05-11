#!/bin/sh

# --- NSS 运行时防护: 首次启动强制禁用 offload (qosmio 规范) ---
# uci-defaults 阶段 NSS 模块尚未加载,lsmod 不可靠。
# 此构建专为 NSS 设备,无条件禁用所有软件/硬件 offload。
# 98-nss-tune (AX6 overlay) 互补: 内核模块黑名单 (每次启动生效)。

# globals 级别 — 首次启动
uci set network.globals.packet_steering='0'

# device 级别 — 首次启动,防止 sysupgrade dirty config 泄漏
for d in $(uci -q show network 2>/dev/null | awk -F'[.=]' '/=device$/{print $2}'); do
	uci -q set "network.${d}.packet_steering=0"
	uci -q set "network.${d}.flow_offloading=0"
	uci -q set "network.${d}.flow_offloading_hw=0"
done

# --- IPv6 首次启动配置 (ula_prefix 存在时) ---
ula_prefix=$(uci get network.globals.ula_prefix 2>/dev/null)

if [ -n "$ula_prefix" ]; then
	uci set dhcp.wan6=dhcp
	uci set dhcp.wan6.interface='wan6'
	uci set dhcp.wan6.ignore='1'

	uci set dhcp.lan.force='1'
	uci set dhcp.lan.ra='hybrid'
	uci set dhcp.lan.ra_default='1'
	uci set dhcp.lan.max_preferred_lifetime='1800'
	uci set dhcp.lan.max_valid_lifetime='3600'

	uci del dhcp.lan.dhcpv6
	uci del dhcp.lan.ra_flags
	uci del dhcp.lan.ra_slaac
	uci add_list dhcp.lan.ra_flags='none'

	uci commit dhcp

	uci set network.wan6.reqaddress='try'
	uci set network.wan6.reqprefix='auto'
	uci set network.lan.ip6assign='64'
	uci set network.lan.ip6ifaceid='eui64'
	uci del network.globals.ula_prefix

	uci commit network
fi

exit 0
