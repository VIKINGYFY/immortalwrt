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
- `package/network/config/firewall4/patches/003-match-nat-leakage-address-family.patch`:
  only emit NAT leakage protection for masqueraded address families, so turning
  off one family's masquerading cannot conflict with a zone's subnet matches.

Kernel patches use the generic kernel stack. LuCI integration patches apply only
to the copied view in the UI package build directory; no global metadata hook
or writable Git feed checkout is required.

The separate `package/emortal/luci-app-fullconenat-sonic` package owns the LuCI
integration, ACL and translations for both firewall generations, using the
standard `htdocs/`, `root/` and `po/` layout and the feed's `luci.mk`.
The optional `luci-app-fullconenat-sonic` package is selected by default
when `luci-app-firewall` is enabled. The settings appear in **Network →
Firewall → General Settings**: the global switch follows **Drop invalid
packets**, and the zone switch follows **IPv4 Masquerading** in both the
zone table and its add/edit dialog. No separate tab is added. At build time,
the package copies the current feed zone form and applies a small integration
patch, keeping the standard LuCI zone controls. Its menu entry selects that
extended view at the existing `firewall/zones` route. The original firewall
package files remain owned by `luci-app-firewall`. Headless builds do not
require the UI package.
The standard LuCI build generates `luci-i18n-fullconenat-sonic-zh-cn` and
selects it when Simplified Chinese is enabled in the LuCI language options.
Translation compilation, installation and post-install cache refresh use
the common LuCI rules; no custom translation or refresh commands are needed.
The copied view removes legacy fullcone controls and uses the SONiC helper.
The original feed and its feature probe remain unchanged.

Both legacy `fullconenat-nft` and `fullconenat` source packages are removed.
The package rejects installation alongside their old binary packages.
For APK builds this is also encoded as negative dependencies, since this
tree's APK packer does not translate `CONFLICTS` into package metadata.
`CONFIG_NF_NAT_FULLCONE` enables the SONiC core and frontends through normal
kernel package metadata. When disabled, the extra endpoint table, conntrack
fields, locks and reservation scans are compiled out.

Build the kernel together with the packages. Installing the small
userspace package alone cannot add this feature to a different kernel.

## Configuration

Fullcone requires `defaults.fullcone=1`, `zone.fullcone=1`, and `zone.masq=1`
for IPv4 or (with fw4) `zone.masq6=1` for IPv6. Fresh configurations enable the global
switch and WAN zone, with software/hardware firewall flow offloading disabled.
The LuCI package also resets both offload flags to `0` on installation and
saving the form, and hides the Routing/NAT Offloading section. This does not
change the separate Qualcomm NSS/ECM configuration. The base firewall
configuration enables IPv4 masquerading; installing the LuCI package never
changes IPv6 masquerading. Enable it independently when IPv6 NAT is needed.

```uci
config defaults
        option fullcone '1'

config zone
        option name 'wan'
        list network 'wan'
        list network 'wan6'
        option masq '1'
        option fullcone '1'
```

Fullcone always uses all supported protocols: TCP, UDP, UDP-Lite and SCTP.
There is no protocol selector in LuCI or UCI. The install migration removes
obsolete `fullcone_proto` lists. Other protocols and connections managed by a
NAT helper retain ordinary NAT semantics. fw4 groups the four protocols into
one set per direction and address family; fw3 emits one rule per protocol.

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
zone switch. The LuCI package carries a one-time uci-defaults migration that
maps an enabled legacy `fullcone6` value to `fullcone` when needed and then
removes the obsolete option; the normal settings page has no legacy fallback.
The integrated form and its add/edit dialogs use the standard staged save/apply
flow. Enabling a zone's Fullcone NAT enables IPv4 masquerading. Turning off
IPv4 masquerading also disables the
corresponding zone's Fullcone NAT when saving, preserving that mask-off choice.
IPv6 masquerading and other zones are unaffected. Enabling IPv4 masquerading alone does not enable
Fullcone NAT. Disabling a zone's Fullcone NAT or the global switch leaves the
current masquerading settings unchanged; no restore snapshots are kept.
New zones include the switch, initially off. The same one-time migration script
enables the global and WAN switches only if absent, enables IPv4 masquerading for
active zones, and removes the obsolete fullconenat_sonic snapshot configuration.
The normal form only reads and writes the firewall configuration.

## Mapping and offload behavior

- Reuse is serialized by internal endpoint. Randomized port allocation
  still reuses that endpoint's existing fullcone binding.
- Public endpoints are checked and published under their own bucket lock;
  lookup, tuple copying and deletion use the same lock to prevent reads of
  recycled conntracks. Source mapping reuse holds the source list lock.
  A matching owner ends the reservation scan, including when many connections
  share the endpoint. Ordinary SNAT also participates in the
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

The kernel implementation keeps established supported TCP/UDP flows eligible
for normal flowtable and PPE offload; this build disables the firewall flowtable
policy by default as described above. A new remote tuple misses those entries and reaches the
software fullcone lookup. Lack of hardware wildcard fullcone entries is
not, by itself, a reason to disable ordinary connection offload. Existing
MTK/QCA/QCB driver limits (for example NAT66 or double NAT) still apply.

Compile, rule-rendering and source-level concurrency checks do not replace
router packet tests, Linux RCU/lockdep testing, or MTK/QCA/QCB PPE tests.

## Regression checks

The source-extraction, firewall-rule, LuCI staging and migration test tools are
kept in the external validation archive rather than shipped in this package.
They use prepared kernel/firewall sources and isolated host fixtures; they do
not contact a router. The recorded checks cover endpoint isolation, bounded
allocation, mixed NAT policies, concurrent retirement/reuse, compilation
without fullcone state, all supported protocols, address restrictions, DNAT
precedence, unavailable-kernel fallback, IPv4 masquerading coupling, IPv6
independence and idempotent migration.

Compile the affected target modules with the kernel option both on and off,
and rebuild the firmware before deploying kernel changes. The validation
archive contains the exact tool copies, source hashes and run logs for each
release check.
