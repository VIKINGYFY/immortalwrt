REQUIRE_IMAGE_METADATA=1
RAMFS_COPY_BIN='fitblk fit_check_sign'

gl_be6500_remove_oem_rootfs() {
	local mtdnum
	local ubidev
	local ubivol

	mtdnum=$(find_mtd_index "$CI_UBIPART")
	if [ -z "$mtdnum" ]; then
		echo "Unable to find UBI MTD partition $CI_UBIPART"
		return 1
	fi

	ubidev=$(nand_find_ubi "$CI_UBIPART")
	if [ -z "$ubidev" ]; then
		ubiattach --mtdn="$mtdnum" || return 1
		ubidev=$(nand_find_ubi "$CI_UBIPART")
	fi

	[ -n "$ubidev" ] || return 1

	ubivol=$(nand_find_volume "$ubidev" ubi_rootfs)
	[ -z "$ubivol" ] || {
		echo "Removing legacy ubi_rootfs volume"
		ubirmvol "/dev/$ubidev" --name=ubi_rootfs || return 1
	}
}

platform_do_upgrade() {
	case "$(board_name)" in
	gl.inet,gl-be6500)
		CI_UBIPART="rootfs"
		gl_be6500_remove_oem_rootfs || return 1
		nand_do_upgrade "$1"
		;;
	ubnt,u7-pro-xgs)
		CI_KERNPART="kernel0"
		fit_do_upgrade "$1"
		;;
	xiaomi,be3600-pro-wired-*)
		nand_do_upgrade "$1"
		;;
	jdcloud,re-cs-08)
		CI_KERNPART="0:HLOS"
		CI_ROOTPART="rootfs"
		CI_DATAPART="rootfs_data"
		emmc_do_upgrade "$1"
		;;
	*)
		echo "Sysupgrade is not supported on your board yet."
		return 1
		;;
	esac
}

platform_check_image() {
	[ "$#" -gt 1 ] && return 1

	case "$(board_name)" in
	gl.inet,gl-be6500|\
	jdcloud,re-cs-08|\
	xiaomi,be3600-pro-wired-*)
		return 0
		;;
	ubnt,u7-pro-xgs)
		fit_check_image "$1"
		;;
	*)
		echo "Sysupgrade is not supported on your board yet."
		return 1
		;;
	esac
}

platform_copy_config() {
	case "$(board_name)" in
	jdcloud,re-cs-08|\
	ubnt,u7-pro-xgs)
		emmc_copy_config
		;;
	esac
}
