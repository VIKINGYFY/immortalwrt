#!/bin/sh

. /lib/functions.sh
[ "$(board_name)" = "verizon,cr1000a" ] || exit 0
[ -f /var/run/cr1000a-rtl.done ] && exit 0

[ -c /dev/mem ] || {
	echo 'cr1000a: /dev/mem is required for RTL9303 initialization' >&2
	exit 1
}

# The vendor binary uses a fixed SPI bus number. Find the controller by DT
# compatibility, since Linux bus numbering can change when probing devices.
spi_device=
for attempt in 1 2 3 4 5 6 7 8 9 10; do
	for device in /sys/class/spidev/spidev*; do
		[ -r "$device/device/of_node/compatible" ] || continue
		tr '\000' '\n' < "$device/device/of_node/compatible" |
			grep -qx 'cisco,spi-petra' || continue
		[ -c "/dev/${device##*/}" ] || continue
		spi_device="/dev/${device##*/}"
		break
	done
	[ -n "$spi_device" ] && break
	sleep 1
done
[ -n "$spi_device" ] || {
	echo 'cr1000a: RTL9303 SPI device is unavailable' >&2
	exit 1
}
if [ "$spi_device" != /dev/spidev32765.0 ]; then
	[ ! -e /dev/spidev32765.0 ] || [ -L /dev/spidev32765.0 ] || exit 1
	ln -sf "$spi_device" /dev/spidev32765.0 || exit 1
fi

# Keep stdin open for the vendor initialization interval, then exit its CLI.
# A finite pipe avoids the original persistent FIFO and orphaned tail process.
(sleep 30; printf 'exit\n') | /lib/rtl/usrApp || exit 1
touch /var/run/cr1000a-rtl.done
