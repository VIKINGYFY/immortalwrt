#!/bin/sh

. /lib/functions.sh
. /lib/upgrade/common.sh

arista_ap_ensure_fw_env_config() {
	local env_mtd

	[ -s /etc/fw_env.config ] && return 0

	env_mtd="$(find_mtd_index '0:APPSBLENV')"
	[ -n "$env_mtd" ] || return 1

	echo "/dev/mtd${env_mtd} 0x0 0x10000 0x10000" > /etc/fw_env.config
}

arista_ap_setenv_if_needed() {
	local name="$1"
	local wanted="$2"
	local current

	current="$(fw_printenv -n "$name" 2>/dev/null)"
	[ "$current" = "$wanted" ] && return 0

	if [ -n "$current" ]; then
		echo "updating U-Boot env $name: $current -> $wanted"
	else
		echo "setting U-Boot env $name: $wanted"
	fi

	fw_setenv "$name" "$wanted" || echo "warning: failed to update $name"
}

arista_c360_update_ubootenv() {
	local bootargs='console=ttyMSM0,115200n8 ubi.mtd=rootfs root=mtd:ubi_rootfs'
	local bootsys='nand read 0x44000000 0x0 0x1000000; bootm'
	local bootcmd='run bootsys'

	arista_ap_ensure_fw_env_config || {
		echo 'warning: failed to initialize fw_env.config'
		return 0
	}

	fw_printenv >/dev/null || {
		echo 'warning: U-Boot environment is not readable'
		return 0
	}

	# Only touch the boot variables needed to make AP-C260/AP-C360 images boot.
	arista_ap_setenv_if_needed bootargs "$bootargs"
	arista_ap_setenv_if_needed bootsys "$bootsys"
	arista_ap_setenv_if_needed bootcmd "$bootcmd"
}

arista_ap_do_upgrade() {
	local image="$1"
	local board_dir kernel_mtd kernel_file

	board_dir="$(tar tf "$image" | grep -m 1 '^sysupgrade-.*/$')"
	board_dir="${board_dir%/}"
	[ -n "$board_dir" ] || nand_do_upgrade_failed

	kernel_mtd="$(find_mtd_index '0:HLOS')"
	[ -n "$kernel_mtd" ] || {
		echo 'cannot find kernel mtd partition 0:HLOS'
		nand_do_upgrade_failed
	}

	kernel_file=/tmp/arista-ap-kernel.itb
	tar xOf "$image" "$board_dir/kernel" > "$kernel_file" || nand_do_upgrade_failed
	[ -s "$kernel_file" ] || nand_do_upgrade_failed

	CI_UBIPART="rootfs"
	CI_KERNPART="none"
	nand_do_flash_file "$image" || nand_do_upgrade_failed

	mtd write "$kernel_file" "/dev/mtd${kernel_mtd}" || nand_do_upgrade_failed
	rm -f "$kernel_file"

	[ "$(board_name)" = "arista,c360" ] && arista_c360_update_ubootenv

	nand_do_upgrade_success
}
