# SPDX-License-Identifier: GPL-2.0-or-later
. /lib/functions/arista-transaction.sh

# UCI -t adds a search path; it does not remove /tmp/.uci. Give copied
# packages unique names as well, so another session's deltas cannot be read.
arista_config_begin() {
	local package
	ARISTA_CONFIG_LOCKED=0
	ARISTA_CONFIG_PACKAGES="$*"
	ARISTA_CONFIG_ROOT=/etc/config
	ARISTA_TX_DEST=$ARISTA_CONFIG_ROOT
	ARISTA_TX_JOURNAL=$ARISTA_CONFIG_ROOT/.arista-transaction
	exec 9>/var/lock/arista-config.lock
	flock -n 9 || return 1
	ARISTA_CONFIG_LOCKED=1
	arista_tx_recover || return 1
	ARISTA_CONFIG_WORK=$(mktemp -d /tmp/arista-config.XXXXXX) || return 1
	ARISTA_CONFIG_ID=arista_$(basename "$ARISTA_CONFIG_WORK" | tr -cd 'a-zA-Z0-9_')
	mkdir -p "$ARISTA_CONFIG_WORK/config" "$ARISTA_CONFIG_WORK/delta" \
		"$ARISTA_CONFIG_WORK/before" "$ARISTA_CONFIG_WORK/result" || return 1
	for package in $ARISTA_CONFIG_PACKAGES; do
		[ -z "$(uci -q changes "$package")" ] || {
			echo "arista: uncommitted changes in $package; commit or revert them first" >&2
			return 1
		}
		if [ -f "$ARISTA_CONFIG_ROOT/$package" ]; then
			cp -p "$ARISTA_CONFIG_ROOT/$package" "$ARISTA_CONFIG_WORK/before/$package" || return 1
			cp -p "$ARISTA_CONFIG_ROOT/$package" "$ARISTA_CONFIG_WORK/config/${ARISTA_CONFIG_ID}_$package" || return 1
		else
			: > "$ARISTA_CONFIG_WORK/config/${ARISTA_CONFIG_ID}_$package" || return 1
		fi
	done
}

arista_uci() {
	local arg package output operation=
	for arg in "$@"; do
		case "$arg" in -*) ;; *) [ -n "$operation" ] || operation=$arg ;; esac
		shift
		for package in $ARISTA_CONFIG_PACKAGES; do
			case "$arg" in
			"$package"|"$package".*) arg=${ARISTA_CONFIG_ID}_$arg; break ;;
			esac
		done
		set -- "$@" "$arg"
	done
	output=$(uci -c "$ARISTA_CONFIG_WORK/config" -t "$ARISTA_CONFIG_WORK/delta" "$@") || {
		case "$operation" in
		set|delete|add|add_list|del_list|rename|reorder|commit) : > "$ARISTA_CONFIG_WORK/error" ;;
		esac
		return 1
	}
	[ -z "$output" ] || printf '%s\n' "$output" | sed "s/^${ARISTA_CONFIG_ID}_//"
	return 0
}

arista_uci_delete() {
	arista_uci -q get "$1" >/dev/null || return 0
	arista_uci -q delete "$1"
}

arista_config_unchanged() {
	local package
	for package in $ARISTA_CONFIG_PACKAGES; do
		[ -z "$(uci -q changes "$package")" ] || return 1
		if [ -e "$ARISTA_CONFIG_WORK/before/$package" ]; then
			cmp -s "$ARISTA_CONFIG_WORK/before/$package" "$ARISTA_CONFIG_ROOT/$package" || return 1
		else
			[ ! -e "$ARISTA_CONFIG_ROOT/$package" ] || return 1
		fi
	done
}

arista_config_commit() {
	local package
	[ ! -e "$ARISTA_CONFIG_WORK/error" ] || return 1
	for package in $ARISTA_CONFIG_PACKAGES; do
		arista_uci commit "$package" || return 1
		cp -p "$ARISTA_CONFIG_WORK/config/${ARISTA_CONFIG_ID}_$package" \
			"$ARISTA_CONFIG_WORK/result/$package" || return 1
	done
	arista_tx_stage "$ARISTA_CONFIG_WORK/result" || return 1
	arista_config_unchanged || {
		echo 'arista: configuration changed concurrently; retry after reviewing it' >&2
		return 1
	}
	arista_tx_install && arista_tx_finish
}

arista_config_end() {
	local ret=0
	[ "$ARISTA_CONFIG_LOCKED" = 1 ] || return 0
	arista_tx_recover || ret=1
	[ -z "$ARISTA_CONFIG_WORK" ] || rm -rf "$ARISTA_CONFIG_WORK"
	ARISTA_CONFIG_WORK=
	ARISTA_CONFIG_LOCKED=0
	exec 9>&-
	return "$ret"
}
