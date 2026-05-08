#!/bin/sh

FILE="/usr/share/rpcd/ucode/luci"
# 替换 top -n1 调用为 cpuusage (行级替换: ucode 中 \' 转义导致 [^']* 无法匹配)
[ -f "$FILE" ] && sed -i "s|const fd = popen('top.*|const fd = popen('/sbin/cpuusage');|" "$FILE"

exit 0
