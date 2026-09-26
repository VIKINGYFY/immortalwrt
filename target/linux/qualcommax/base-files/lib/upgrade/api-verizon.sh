#!/bin/sh

. /lib/functions.sh
. /lib/upgrade/common.sh

cr1000a_check_upgrade() {
	local hlos rootfs kernel_size root_size hlos_blocks root_blocks
	hlos=$(find_mmc_part '0:HLOS')
	rootfs=$(find_mmc_part rootfs)
	if [ ! -b "$hlos" ] || [ ! -b "$rootfs" ]; then
		echo 'CR1000A: 0:HLOS or rootfs partition missing' >&2
		return 1
	fi
	hlos_blocks=$(cat "/sys/class/block/${hlos##*/}/size")
	root_blocks=$(cat "/sys/class/block/${rootfs##*/}/size")
	# The APPSBL loads this many sectors at 0x44000000; bound the RAM read.
	[ "$hlos_blocks" -gt 40960 ] && hlos_blocks=40960
	tar tf "$1" sysupgrade-verizon_cr1000a/kernel >/dev/null 2>&1 &&
		tar tf "$1" sysupgrade-verizon_cr1000a/root >/dev/null 2>&1 || {
		echo 'CR1000A: kernel or root payload missing' >&2
		return 1
	}
	kernel_size=$(tar xf "$1" sysupgrade-verizon_cr1000a/kernel -O | wc -c)
	root_size=$(tar xf "$1" sysupgrade-verizon_cr1000a/root -O | wc -c)
	if [ "$kernel_size" -eq 0 ] || [ "$root_size" -eq 0 ] ||
		[ "$kernel_size" -gt "$((hlos_blocks * 512))" ] ||
		[ "$root_size" -gt "$((root_blocks * 512))" ]; then
		echo 'CR1000A: image exceeds 0:HLOS or rootfs partition' >&2
		return 1
	fi
	return 0
}

cr1000a_check_image() {
	cr1000a_check_upgrade "$1" || return 1
	if [ "$(fw_printenv -n cr1000a_recovery 2>/dev/null)" = 1 ]; then
		cr1000a-recovery verify || {
			echo 'CR1000A: enabled recovery FIT is invalid; repair or disable recovery before upgrading' >&2
			return 1
		}
	fi
}

cr1000a_setenv_if_changed() {
	[ "$(fw_printenv -n "$1" 2>/dev/null)" = "$2" ] || fw_setenv "$1" "$2"
}

cr1000a_do_upgrade() {
	local rootfs hlos hlos_start hlos_size recovery recovery_start recovery_size
	local recovery_bootcmd='if run set_custom_bootargs && run read_hlos_emmc && bootm 44000000; then true; else run boot_recovery; fi'
	cr1000a_check_upgrade "$1" || return 1
	CI_KERNPART='0:HLOS'
	CI_ROOTPART=rootfs
	hlos=$(find_mmc_part "$CI_KERNPART")
	rootfs=$(find_mmc_part "$CI_ROOTPART")
	hlos_start=$(cat "/sys/class/block/${hlos##*/}/start")
	hlos_size=$(cat "/sys/class/block/${hlos##*/}/size")
	[ "$hlos_size" -gt 40960 ] && hlos_size=40960
	if [ "$(fw_printenv -n cr1000a_recovery 2>/dev/null)" = 1 ]; then
		recovery=$(find_mmc_part recovery)
		if [ ! -b "$recovery" ]; then
			echo 'CR1000A: enabled recovery partition is missing' >&2
			return 1
		fi
		recovery_start=$(cat "/sys/class/block/${recovery##*/}/start")
		recovery_size=$(cat "/sys/class/block/${recovery##*/}/size")
		if [ "$recovery_size" -lt 40960 ]; then
			echo 'CR1000A: recovery partition is smaller than 20 MiB' >&2
			return 1
		fi
	fi
	cr1000a_setenv_if_changed set_custom_bootargs "setenv bootargs console=ttyMSM0,115200n8 root=$rootfs rootwait fstools_ignore_partname=1" || return 1
	cr1000a_setenv_if_changed read_hlos_emmc "mmc read 44000000 0x$(printf '%X' "$hlos_start") 0x$(printf '%X' "$hlos_size")" || return 1
	cr1000a_setenv_if_changed setup_and_boot 'run set_custom_bootargs;run read_hlos_emmc; bootm 44000000' || return 1
	if [ -n "$recovery_start" ]; then
		cr1000a_setenv_if_changed boot_recovery "setenv bootargs console=ttyMSM0,115200n8; mmc read 44000000 0x$(printf '%X' "$recovery_start") 0xA000 && bootm 44000000" || return 1
		cr1000a_setenv_if_changed bootcmd "$recovery_bootcmd" || return 1
	else
		cr1000a_setenv_if_changed bootcmd 'run setup_and_boot' || return 1
	fi
	emmc_do_upgrade "$1"
}
