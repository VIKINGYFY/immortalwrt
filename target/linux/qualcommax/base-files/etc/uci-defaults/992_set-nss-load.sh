#!/bin/sh
# 修正后的 NSS 启动调优
# - sed 正则非贪婪,避免误伤多行 popen
# - sysctl 改 sysctl.d 持久化,procd-sysctl 在合适时机应用
FILE="/usr/share/rpcd/ucode/luci"
[ -f "$FILE" ] && sed -i "s#popen('top -n1[^']*')#popen('/sbin/cpuusage')#" "$FILE"

mkdir -p /etc/sysctl.d
echo 'dev.nss.clock.auto_scale = 0' > /etc/sysctl.d/97-nss-lock-clock.conf

exit 0
