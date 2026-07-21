# Shared helpers for the NSS runtime scripts (nss-up, nss-status).

# Firmware blob and version string. qca-nss0.bin is a symlink the firmware
# hotplug creates on the first arm; on a freshly flashed image it does not
# exist yet - fall back to the packaged retail blob name. Matches both
# version-string shapes: NSS.FW.12.5-210-HK.R (12.x line) and
# NSS.HK.11.4.0.5-6-R (11.4 line, the mesh-capable firmware).
nss_fw_version() {
	local blob=/lib/firmware/qca-nss0.bin
	[ -e "$blob" ] || blob=/lib/firmware/qca-nss0-retail.bin
	grep -aom1 'NSS\.[A-Z][A-Z]*\.[0-9][0-9A-Za-z.-]*' "$blob" 2>/dev/null
}

# 802.11s mesh state. Sets NSS_MESH_CFG=1 when any wireless vif is
# configured in mesh mode, and NSS_MESH_CAPABLE=1 when the firmware line
# supports mesh interfaces (11.4 only - every newer firmware rejects the
# mesh dynamic-interface allocation) AND the image was built with the mesh
# offload (the Wi-Fi mesh manager module is present).
nss_mesh_state() {
	NSS_MESH_CFG=0
	uci -q show wireless | grep -q "\.mode='mesh'" && NSS_MESH_CFG=1
	NSS_MESH_CAPABLE=0
	case "$(nss_fw_version)" in
	*.11.4.*) [ -e "/lib/modules/$(uname -r)/qca-nss-wifi-meshmgr.ko" ] && NSS_MESH_CAPABLE=1 ;;
	esac
}

# The SQM queue section the NSS shaper serves. Resolved by TYPE, not by
# name (the section name is user-chosen): first enabled queue section,
# else the first one. Sets NSS_SQM_SEC (section) and NSS_SQM_DEV (the
# shaped netdev, from the section's 'interface' option).
nss_sqm_section() {
	local s
	NSS_SQM_SEC=""
	NSS_SQM_DEV=""
	for s in $(uci -q show sqm | sed -n "s/^sqm\.\([^.=]*\)=queue$/\1/p"); do
		[ -n "$NSS_SQM_SEC" ] || NSS_SQM_SEC=$s
		[ "$(uci -q get "sqm.$s.enabled")" = "1" ] && { NSS_SQM_SEC=$s; break; }
	done
	[ -n "$NSS_SQM_SEC" ] && NSS_SQM_DEV=$(uci -q get "sqm.${NSS_SQM_SEC}.interface")
}
