#!/bin/sh

. /lib/functions.sh
[ "$(board_name)" = "verizon,cr1000a" ] || exit 0

for attempt in 1 2 3 4 5 6 7 8 9 10; do
	[ -e /sys/class/net/wan ] && exec ethtool -s wan advertise 0x18000000E102C
	sleep 1
done
echo 'cr1000a: WAN interface is unavailable' >&2
exit 1
