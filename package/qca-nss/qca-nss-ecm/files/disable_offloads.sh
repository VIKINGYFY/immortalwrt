#!/bin/sh
# shellcheck disable=3014,3043,2086,1091,2154
#
# Helper script which disables rx-gro-list for ECM/NSS compatibility.

log() {
	local status="$1"
	local feature="$2"
	local interface="$3"

	if [ "$status" -eq 0 ]; then
		logger "[ethtool] $feature: disabled on $interface"
	fi

	if [ "$status" -eq 1 ]; then
		logger -s "[ethtool] $feature: failed to disable on $interface"
	fi

	if [ "$status" -gt 1 ]; then
		logger "[ethtool] $feature: no changes performed on $interface"
	fi
}

disable_feature() {
	local feature="$1"
	local interface="$2"
	local current_state

	current_state=$(ethtool -k "$interface" 2>/dev/null | awk -v feature="^$feature:" '$0 ~ feature {print $2}')

	# Only disable and log if the feature is currently enabled
	if [ "$current_state" = "on" ]; then
		# Try to disable the feature
		ethtool -K "$interface" "$feature" off 1> /dev/null 2> /dev/null
		log $? "Disabling feature: $feature" "($interface)"
	fi
}

disable_offload() {
	[ -z "$1" ] && interface=$(echo /sys/class/net/*/device) || interface=$*

	for iface in $interface; do
		i=${iface%/*}
		i=${i##*/}

		# Skip Loopback and Bonding Masters
		if [ "$i" = lo ] || [ -f "$iface" ]; then
			continue
		fi

		disable_feature "rx-gro-list" "$i"
	done
}
