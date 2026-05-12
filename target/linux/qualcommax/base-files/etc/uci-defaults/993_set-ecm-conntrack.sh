#!/bin/sh

FILE="/etc/sysctl.d/qca-nss-ecm.conf"
[ -f "$FILE" ] || exit 0

sed -i "s/^nf_conntrack_max=.*/nf_conntrack_max=65536/" "$FILE"

# qca-nss-ecm.conf 无数字前缀, procd-sysctl 最后处理, 优先级最高
# 确保 timeout 不被 11-nf-conntrack.conf (7440) 覆盖
if grep -q "nf_conntrack_tcp_timeout_established" "$FILE"; then
    sed -i "s/^.*nf_conntrack_tcp_timeout_established=.*/net.netfilter.nf_conntrack_tcp_timeout_established=86400/" "$FILE"
else
    echo "net.netfilter.nf_conntrack_tcp_timeout_established=86400" >> "$FILE"
fi

exit 0
