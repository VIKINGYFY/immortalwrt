#!/bin/sh

# Keep generic RPS settings, then restore QCA IRQ/NAPI placement after
# netifd's network/interface/firewall events. No polling worker is needed.
flows="$(uci -q get network.@globals[0].steering_flows)"
case "$flows" in
	''|*[!0-9]*) ;;
	*) [ "$flows" -eq 0 ] || set -- -l "$flows" "$@" ;;
esac
/usr/libexec/network/packet-steering.uc "$@"
result=$?
/etc/init.d/smp_affinity start || exit $?
exit "$result"
