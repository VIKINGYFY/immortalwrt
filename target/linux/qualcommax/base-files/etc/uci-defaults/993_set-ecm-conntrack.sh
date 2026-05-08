#!/bin/sh

FILE="/etc/sysctl.d/qca-nss-ecm.conf"
[ -f "$FILE" ] || exit 0

sed -i "s/^nf_conntrack_max=.*/nf_conntrack_max=65536/" "$FILE"

exit 0
