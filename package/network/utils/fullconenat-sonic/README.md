# fullconenat-sonic

firewall3/iptables and fw4/nftables integration based on [openwrt-sonic-fullcone](https://github.com/mufeng05/openwrt-sonic-fullcone),
revision `4de0643f6b48e7e7d57d768144204ce5d3078c8f` (2026-09-17), with local
mapping reservation and policy isolation fixes. The fw3/iptables target and
the fw4/nftables expression are compiled into the existing NAT modules.

## Build and UI

On Linux 6.18, either `firewall` (fw3) or `firewall4` (fw4) selects
`fullconenat-sonic`. The backend and its UI are restricted to Linux 6.18.
The kernel code is part of `nf_nat.ko`, `nft_masq.ko` and
`xt_MASQUERADE.ko`, with no separate fullcone kernel module. The package
enables UDP-Lite and SCTP conntrack support through its own `KCONFIG` metadata.

Cross-component patches use their standard build locations:

- `target/linux/generic/hack-6.18/984-*-sonic-*.patch`: NAT core.
- `target/linux/generic/hack-6.18/985-*-sonic-*.patch`: iptables target.
- `target/linux/generic/hack-6.18/986-*-sonic-*.patch`: nft expression.
- `package/network/utils/iptables/patches/901-*.patch`: `libxt_NAT` target.
- `package/network/config/firewall/patches/001-sonic-fullcone.patch`: fw3 rules.
- `package/libs/libnftnl/patches/001-*.patch`: expression encoding.
- `package/network/utils/nftables/patches/002-*.patch`: nft syntax.
- `package/network/config/firewall4/patches/001-sonic-fullcone.patch` and
  `002-default-enable-fullcone.patch`: rule generation and defaults.

The LuCI cleanup patches are kept in this package's `patches/luci/` directory.
The top-level `prepare-tmpinfo` step invokes its small idempotent bridge before
package metadata is scanned, applying those patches to the separate LuCI feed
Git checkout when it is present. Both patches are checked before either is
applied. An already applied patch is accepted; an upstream context mismatch
stops the build instead of silently changing the feed.
`include/quilt.mk` is unmodified and no patch files need to be copied into the
feed repository. Kernel patches are applied through the generic kernel stack,
independently of userspace package preparation.
The package's own `Build/Patch` is empty because these patches target the feed.

The separate `package/emortal/luci-app-fullconenat-sonic` package owns the LuCI
page, menu, ACL and translations for both firewall generations, using the
standard `htdocs/`, `root/` and `po/` layout and the feed's `luci.mk`.
The optional `luci-app-fullconenat-sonic` package is selected by default
when `luci-app-firewall` is enabled. The settings appear under **Network →
Firewall → Fullcone NAT**. Headless builds do not require the UI package.
The standard LuCI build generates `luci-i18n-fullconenat-sonic-zh-cn` and
selects it when Simplified Chinese is enabled in the LuCI language options.
Translation compilation, installation and post-install cache refresh use
the common LuCI rules; no custom translation or refresh commands are needed.
These LuCI patches remove obsolete UI code; the SONiC page does not depend on
the old feature probe. The feed checkout remains disposable and can be
refreshed; the bridge reapplies the tracked patches on the next preparation.

Both legacy `fullconenat-nft` and `fullconenat` source packages are removed.
The package rejects installation alongside their old binary packages.
For APK builds this is also encoded as negative dependencies, since this
tree's APK packer does not translate `CONFLICTS` into package metadata.
`CONFIG_NF_NAT_FULLCONE` enables the SONiC frontends through normal kernel
package metadata; the generic kernel configuration defaults it to off.

Build the kernel together with the packages. Installing the small
userspace package alone cannot add this feature to a different kernel.

## Configuration

Fullcone requires `defaults.fullcone=1`, `zone.fullcone=1`, and `zone.masq=1`
for IPv4 or (with fw4) `zone.masq6=1` for IPv6. Fresh configurations enable the global
switch and WAN zone, preserving this tree's software/hardware offload
defaults. IPv6 masquerading is not enabled by default.

```uci
config defaults
        option fullcone '1'

config zone
        option name 'wan'
        list network 'wan'
        list network 'wan6'
        option masq '1'
        option fullcone '1'
        list fullcone_proto 'udp'
```

Supported fullcone protocols are TCP, UDP, UDP-Lite and SCTP. An empty
`fullcone_proto` list enables all four. Other protocols, and connections
managed by a NAT helper, use ordinary NAT. An explicitly unsupported
protocol list does not enable fullcone for all traffic.
Negated protocol entries are ignored; they never enable the excluded
protocol. The `tcpudp` alias and protocol numbers 6, 17, 132 and 136 are
accepted by both firewall generations.

Mapping creation respects `masq_src` and `masq_dest`. New remote peers can
reach an existing fullcone mapping without a destination restriction;
ordinary NAT bindings are not exposed by that lookup. Explicit port
forwards run first. The default translated port range is `1024-65535`.
The kernel also checks endpoint reservations when no range is supplied.
If the nft expression or range attributes are unavailable, fw4 disables
fullcone and generates ordinary masquerade rules. fw3 uses the `FULLCONE`
iptables target in both PREROUTING and POSTROUTING, and probes its kernel
availability before generating rules. fw3 itself generates IPv4 masquerade
only. Its UI reflects that scope. The iptables target supports both families;
IPv6 rules can be added with `ip6tables` and `kmod-ipt-nat6`:

```sh
ip6tables -t nat -A POSTROUTING -o wan -p udp -j FULLCONE --to-ports 1024-65535
ip6tables -t nat -A PREROUTING -i wan -p udp -j FULLCONE
```

Replace `wan` with the actual interface and place explicit DNAT rules first.
On fw3, the `FULLCONE` target is included in the existing `libiptext` library;
no separate iptables extension package is needed.
The nft expression supports `ip`, `ip6` and `inet` NAT tables, including
JSON rule import/export through the patched nftables package.

Existing firewall configurations remain under package-manager conffile
handling. Upgrades from the old global-only switch require enabling the
zone switch. The obsolete global `fullcone6` option is unused.

## Mapping and offload behavior

- Reuse is serialized by internal endpoint. Randomized port allocation
  still reuses that endpoint's existing fullcone binding.
- Public endpoints are checked and published under their own bucket lock;
  deletion uses the same lock. Ordinary SNAT also participates in the
  reservation check, while retaining normal five-tuple sharing with other
  ordinary SNAT flows.
- Matching includes network namespace, both conntrack zone directions,
  address family and transport protocol. A private conntrack flag marks
  fullcone source bindings. DNAT only consults confirmed, non-dying marked
  bindings. No userspace conntrack status bit is repurposed.
- Allocation uses bounded attempts and drops on fullcone allocation
  failure; it never evicts another fullcone binding to reuse its port.
- Mappings remain tied to outbound conntracks. Once all outbound bindings
  have been destroyed, new peers need a new outbound mapping; already
  established inbound connections retain their own conntrack NAT state.

Established supported TCP/UDP flows remain eligible for normal flowtable
and PPE offload. A new remote tuple misses those entries and reaches the
software fullcone lookup. Lack of hardware wildcard fullcone entries is
not, by itself, a reason to disable ordinary connection offload. Existing
MTK/QCA/QCB driver limits (for example NAT66 or double NAT) still apply.

Compile, rule-rendering and source-level concurrency checks do not replace
router packet tests, Linux RCU/lockdep testing, or MTK/QCA/QCB PPE tests.
