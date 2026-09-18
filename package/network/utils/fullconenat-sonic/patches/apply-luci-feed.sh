#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# Make passes TOPDIR explicitly; standalone runs locate the repository root.
TOPDIR=${1:-$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)}
FEED_DIR="$TOPDIR/feeds/luci"
PATCH_DIR="$SCRIPT_DIR/luci"

# A feed is optional during source-only operations. A real package build will
# fail later if its selected LuCI feed is missing.
[ -d "$FEED_DIR" ] || exit 0
FEED_DIR=$(CDPATH='' cd -- "$FEED_DIR" && pwd -P)
if [ "$(git -C "$FEED_DIR" rev-parse --show-toplevel 2>/dev/null)" != "$FEED_DIR" ]; then
	echo "fullconenat-sonic: LuCI feed is present but is not a Git checkout" >&2
	exit 1
fi

apply_patch() {
	patch_file=$1
	package_dir=$2
	mode=$3

	if git -C "$FEED_DIR/$package_dir" apply --reverse --check "$PATCH_DIR/$patch_file" >/dev/null 2>&1; then
		return 0
	fi

	if ! git -C "$FEED_DIR/$package_dir" apply --check "$PATCH_DIR/$patch_file"; then
		echo "fullconenat-sonic: cannot apply LuCI patch $patch_file" >&2
		exit 1
	fi

	if [ "$mode" = apply ]; then
		echo "fullconenat-sonic: applying $patch_file"
		git -C "$FEED_DIR/$package_dir" apply "$PATCH_DIR/$patch_file"
	fi
}

# Validate both patches before changing either package.
for mode in check apply; do
	apply_patch 100-remove-legacy-fullcone-probe.patch modules/luci-base "$mode"
	apply_patch 101-remove-legacy-fullcone-options.patch applications/luci-app-firewall "$mode"
done
