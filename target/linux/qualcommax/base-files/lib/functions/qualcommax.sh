# SPDX-License-Identifier: GPL-2.0-only

# Keep the boot-time board slot when a driver unregisters and registers its
# wiphys again. A physical device path and its local radio order survive a
# controller reload; the global wiphy number does not. State is boot-local,
# so existing board MAC assignments are preserved without a topology table.
qualcommax_phy_index() {
	local path="$1" index device sibling value offset state

	index=$(cat "/sys$path/index") || return 1
	case "$index" in
		''|*[!0-9]*) return 1 ;;
	esac
	device=$(readlink -f "/sys$path/device") || return 1
	case "$device" in
		/sys/devices/*) ;;
		*) return 1 ;;
	esac

	offset=0
	for sibling in "$device"/ieee80211/*/index; do
		[ -r "$sibling" ] || continue
		read -r value < "$sibling" || return 1
		case "$value" in
			''|*[!0-9]*) return 1 ;;
		esac
		[ "$value" -ge "$index" ] || offset=$((offset + 1))
	done
	state="/var/run/qualcommax-phy-index${device#/sys/devices}/$offset"
	if [ -r "$state" ]; then
		read -r value < "$state" || return 1
		case "$value" in
			''|*[!0-9]*) return 1 ;;
		esac
		printf '%s\n' "$value"
		return 0
	fi

	(umask 077; mkdir -p "${state%/*}" &&
		printf '%s\n' "$index" > "$state.$$" &&
		mv "$state.$$" "$state") || return 1
	printf '%s\n' "$index"
}
