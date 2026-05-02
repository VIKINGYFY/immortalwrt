#!/bin/sh

#指定文件路径
FILE="/usr/share/rpcd/ucode/luci"

#添加NSS状态显示 (非贪婪 sed)
[ -f "$FILE" ] && sed -i "s#const fd = popen('top -n1[^']*')#const fd = popen('/sbin/cpuusage')#" "$FILE"

exit 0
