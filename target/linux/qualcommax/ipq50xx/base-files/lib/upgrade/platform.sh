PART_NAME=firmware
REQUIRE_IMAGE_METADATA=1

RAMFS_COPY_BIN='dumpimage fw_printenv fw_setenv head seq'
RAMFS_COPY_DATA='/etc/fw_env.config /var/lock/fw_printenv.lock'

remove_oem_ubi_volume() {
	local oem_volume_name="$1"
	local oem_ubivol
	local mtdnum
	local ubidev

	mtdnum=$(find_mtd_index "$CI_UBIPART")
	if [ ! "$mtdnum" ]; then
		return
	fi

	ubidev=$(nand_find_ubi "$CI_UBIPART")
	if [ ! "$ubidev" ]; then
		ubiattach --mtdn="$mtdnum"
		ubidev=$(nand_find_ubi "$CI_UBIPART")
	fi

	if [ "$ubidev" ]; then
		oem_ubivol=$(nand_find_volume "$ubidev" "$oem_volume_name")
		[ "$oem_ubivol" ] && ubirmvol "/dev/$ubidev" --name="$oem_volume_name"
	fi
}

platform_check_image() {
	return 0;
}

platform_pre_upgrade() {
	case "$(board_name)" in
	xiaomi,ax6000)
		xiaomi_initramfs_prepare
		;;
	esac
}

platform_do_upgrade() {
	case "$(board_name)" in
	cmcc,mr3000d-ci|\
	cmcc,pz-l8|\
	cmcc,rax3000q|\
	elecom,wrc-x3000gs2|\
	elecom,wrc-x3000gst2|\
	iodata,wn-dax3000gr)
		local delay

		delay=$(fw_printenv bootdelay)
		[ -z "$delay" ] || [ "$delay" -eq "0" ] && \
			fw_setenv bootdelay 3

		elecom_upgrade_prepare

		remove_oem_ubi_volume bt_fw
		remove_oem_ubi_volume ubi_rootfs
		remove_oem_ubi_volume wifi_fw
		nand_do_upgrade "$1"
		;;
	glinet,gl-b3000)
		glinet_do_upgrade "$1"
		;;
	glinet,gl-x2000)
		# The stock UBI fills the whole partition (0 free LEBs) with its
		# own wifi_fw and ubi_rootfs volumes, leaving no room for the
		# OpenWrt rootfs. Drop them before upgrading.
		CI_UBIPART="rootfs"
		remove_oem_ubi_volume ubi_rootfs
		remove_oem_ubi_volume wifi_fw
		glinet_do_upgrade "$1"
		;;
	linksys,mr5500|\
	linksys,mx2000|\
	linksys,mx5500|\
	linksys,spnmx56)
		linksys_mx_pre_upgrade "$1"
		remove_oem_ubi_volume squashfs
		nand_do_upgrade "$1"
		;;
	linksys,mx6200)
		linksys_bootconfig_pre_upgrade "$1"
		remove_oem_ubi_volume ubi_rootfs
		nand_do_upgrade "$1"
		;;
	mercusys,mr80x-v2)
		# A/B: write the inactive slot, then point tp_boot_idx at it.
		# tp_boot_idx=0 boots "rootfs", 1 boots "rootfs_1". primaryboot
		# must stay 0, the stock loader and the button recovery pick the
		# slot by name from tp_boot_idx alone. tp_boot_idx only changes
		# once the new slot is fully written.
		local tp active target newtp primaryboot bcidx bcfile

		# Warn only, flipping primaryboot here would desync the running
		# and the next-boot slot names.
		bcidx=$(find_mtd_index "0:bootconfig")
		if [ -n "$bcidx" ]; then
			bcfile=/tmp/mtd"$bcidx".bin
			dd if=/dev/mtd"$bcidx" of="$bcfile" bs=1 count=336 2>/dev/null
			primaryboot=$(get_bootconfig_primaryboot "$bcfile" "rootfs")
			[ "$primaryboot" = "0" ] || \
				echo "WARNING: primaryboot=$primaryboot (expected 0); slot selection may be inverted"
		fi

		tp=$(fw_printenv -n tp_boot_idx 2>/dev/null)
		case "$tp" in
			1) active="rootfs_1"; target="rootfs";   newtp=0 ;;
			*) active="rootfs";   target="rootfs_1"; newtp=1 ;;
		esac

		if [ -n "$UPGRADE_OPT_USE_CURR_PART" ]; then
			CI_UBIPART="$active"
		else
			CI_UBIPART="$target"
		fi

		remove_oem_ubi_volume ubi_rootfs
		remove_oem_ubi_volume wifi_fw
		remove_oem_ubi_volume bt_fw
		sync
		nand_do_flash_file "$1" || nand_do_upgrade_failed
		if [ -z "$UPGRADE_OPT_USE_CURR_PART" ]; then
			fw_setenv tp_boot_idx "$newtp" || {
				echo "failed to set tp_boot_idx"
				nand_do_upgrade_failed
			}
		fi
		nand_do_upgrade_success
		;;
	tplink,archer-ax55-v1|\
	tplink,eap650-outdoor-v1|\
	tplink,re700x)
		# Dual boot: install into the inactive rootfs/rootfs_1 slot,
		# then point tp_boot_idx at it. The running slot is left
		# untouched as a fallback - if the new image fails to load,
		# TP-Link's U-Boot boots the other slot on its own (only on
		# load failure though: there is no boot counter, a kernel
		# that boots and then crashes is not detected).
		local idx=1
		CI_UBIPART="rootfs_1"
		if grep -q 'ubi.mtd=rootfs_1' /proc/cmdline; then
			idx=0
			CI_UBIPART="rootfs"
		fi
		fw_setenv tp_boot_idx $idx || {
			echo "failed to set tp_boot_idx $idx"
			return 1
		}
		# a slot last written by TP-Link firmware carries extra
		# volumes that would leave no room for ours
		remove_oem_ubi_volume ubi_rootfs
		remove_oem_ubi_volume tp_data
		nand_do_upgrade "$1"
		;;
	xiaomi,ax6000|\
	xiaomi,redmi-ax5400)
		# Make sure that UART is enabled
		fw_setenv boot_wait on
		fw_setenv uart_en 1

		# Enforce single partition.
		fw_setenv flag_boot_rootfs 0
		fw_setenv flag_last_success 0
		fw_setenv flag_boot_success 1
		fw_setenv flag_try_sys1_failed 8
		fw_setenv flag_try_sys2_failed 8

		# Kernel and rootfs are placed in 2 different UBI
		CI_KERN_UBIPART="ubi_kernel"
		CI_ROOT_UBIPART="rootfs"
		CI_DATA_UBIPART="rootfs"
		nand_do_upgrade "$1"
		;;
	yuncore,ax830|\
	yuncore,ax850|\
	zyxel,scr50axe)
		CI_UBIPART="rootfs"
		remove_oem_ubi_volume ubi_rootfs
		remove_oem_ubi_volume bt_fw
		remove_oem_ubi_volume wifi_fw
		nand_do_upgrade "$1"
		;;
	*)
		default_do_upgrade "$1"
		;;
	esac
}
