#!/bin/sh

. /lib/functions.sh
[ "$(board_name)" = "verizon,cr1000a" ] || exit 0

# Do not mount over an existing mount or assume a particular eMMC number.
awk '$2 == "/mnt/data" { found = 1 } END { exit !found }' /proc/mounts && exit 0
data_device=
for uevent in /sys/class/block/mmcblk*p*/uevent; do
	[ -r "$uevent" ] || continue
	grep -qx 'PARTNAME=data' "$uevent" || continue
	data_device="${uevent%/uevent}"
	data_device="/dev/${data_device##*/}"
	break
done
[ -b "$data_device" ] || {
	echo 'cr1000a: optional eMMC data partition is absent; skipping mount'
	exit 0
}

mkdir -p /mnt/data || exit 1
# Use a private mapping name to avoid another service's /dev/mapper/data.
opened=0
if [ ! -b /dev/mapper/cr1000a-data ]; then
	cryptsetup --batch-mode --key-file=/lib/mmc/data_key \
		luksOpen "$data_device" cr1000a-data || exit 1
	opened=1
fi
mount -t ext4 /dev/mapper/cr1000a-data /mnt/data && exit 0
[ "$opened" = 0 ] || cryptsetup close cr1000a-data
exit 1
