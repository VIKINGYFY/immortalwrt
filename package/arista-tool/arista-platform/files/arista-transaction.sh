# SPDX-License-Identifier: GPL-2.0-or-later
# A complete rollback set is made before any destination is replaced. The
# journal survives interruption; the next caller recovers it before staging.
arista_tx_rollback() {
	local present path failed=0
	[ -e "$ARISTA_TX_JOURNAL/installing" ] || return 0
	while read -r present path; do
		if [ "$present" = 1 ]; then
			# A missing backup has already been restored by an earlier attempt.
			[ ! -e "$ARISTA_TX_JOURNAL/old/$path" ] &&
				[ ! -L "$ARISTA_TX_JOURNAL/old/$path" ] && continue
			mv -f "$ARISTA_TX_JOURNAL/old/$path" "$ARISTA_TX_DEST/$path" || failed=1
		else
			rm -f "$ARISTA_TX_DEST/$path" || failed=1
		fi
	done < "$ARISTA_TX_JOURNAL/files"
	[ "$failed" = 0 ] || return 1
	rm -f "$ARISTA_TX_JOURNAL/installing"
}

arista_tx_recover() {
	arista_tx_rollback || return 1
	rm -rf "$ARISTA_TX_JOURNAL"
}

arista_tx_stage() {
	local stage=$1 file path present dir
	arista_tx_recover || return 1
	mkdir -p "$ARISTA_TX_JOURNAL/new" "$ARISTA_TX_JOURNAL/old" || return 1
	: > "$ARISTA_TX_JOURNAL/files" || return 1
	# Source markers are published after every data file.
	{ find "$stage" -type f ! -name '*.source' | sort
	  find "$stage" -type f -name '*.source' | sort; } > "$ARISTA_TX_JOURNAL/list" || return 1
	while read -r file; do
		path=${file#$stage/}
		cmp -s "$file" "$ARISTA_TX_DEST/$path" && continue
		dir=${path%/*}
		[ "$dir" != "$path" ] || dir=.
		mkdir -p "$ARISTA_TX_JOURNAL/new/$dir" \
			"$ARISTA_TX_JOURNAL/old/$dir" "$ARISTA_TX_DEST/$dir" || return 1
		cp -p "$file" "$ARISTA_TX_JOURNAL/new/$path" &&
			cmp -s "$file" "$ARISTA_TX_JOURNAL/new/$path" || return 1
		present=0
		if [ -e "$ARISTA_TX_DEST/$path" ] || [ -L "$ARISTA_TX_DEST/$path" ]; then
			cp -a "$ARISTA_TX_DEST/$path" "$ARISTA_TX_JOURNAL/old/$path" || return 1
			present=1
		fi
		printf '%s %s\n' "$present" "$path" >> "$ARISTA_TX_JOURNAL/files" || return 1
	done < "$ARISTA_TX_JOURNAL/list"
}

arista_tx_install() {
	local present path
	# Persist the rollback set before publishing the first replacement.
	sync
	: > "$ARISTA_TX_JOURNAL/installing" || return 1
	sync
	while read -r present path; do
		mv -f "$ARISTA_TX_JOURNAL/new/$path" "$ARISTA_TX_DEST/$path" || return 1
	done < "$ARISTA_TX_JOURNAL/files"
}

arista_tx_finish() {
	sync
	rm -f "$ARISTA_TX_JOURNAL/installing" || return 1
	sync
	rm -rf "$ARISTA_TX_JOURNAL"
}
