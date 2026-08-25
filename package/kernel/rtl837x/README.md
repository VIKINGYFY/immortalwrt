# RTL837x DSA switch driver

This OpenWrt kernel package provides a Linux DSA driver for Realtek RTL837x
switch chips.

The driver was refactored from the swconfig/GSW driver at:

https://github.com/RuijieNetworksCommunity/rtl837x-gsw-driver.git

The original switch setup, Realtek SDK integration, MDIO/regmap access, GPIO
support and chip initialization code were kept where practical. The OpenWrt
integration and switch control path have been reworked to use the modern Linux
DSA model instead of swconfig.

## Licensing

The DSA integration files carrying SPDX GPL-2.0 notices are covered by the
included GNU GPL version 2 text. Files under `src/rtk-api` retain Realtek's
restrictive proprietary notice and require a separate authorization from
Realtek for use, modification or distribution. Package metadata therefore
records both `GPL-2.0-only` and `LicenseRef-Realtek-Proprietary` instead of
describing the complete source tree as GPL-only. The kernel module's
`MODULE_LICENSE("GPL")` declaration is required for the GPL-only DSA interfaces
it consumes; it does not relicense the bundled Realtek SDK sources.

## Package

The package name is:

```text
kmod-rtl837x-dsa
```

It builds the switch driver module:

```text
rtl837x_dsa.ko
```

Package version `0.1` includes serialized SDK context selection for multiple
RTL8372N instances sharing the RTL8373 mapper family, an indirect-MDIO path
aligned with the validated native `mii_bus` transaction sequence, and an optional
bootloader configuration handoff mode. Ordered RTL8372N initialization and selective
CPU SerDes SDK reinitialization, plus deferred-probe port quarantine, are available
as generic Device Tree policies. The driver also records per-instance MDIO
transport diagnostics, including the first indirect command's control and data
words, for early hardware bring-up. An opt-in RTL8372N LED policy restores the
chip vendor's parallel LED defaults when a warm handoff intentionally skips
the SDK cold-initialization path. Version `0.0.13` corrected the raw-register to
SDK-field conversion used by that LED table. Version `0.0.14` changes the
operational data path to standard tag_8021q VLAN headers, keeps VLAN-unaware
bridge PVIDs in the tagger-managed VID domain, and adds bounded forwarding-state
and first-packet diagnostics. Early deferred-probe quarantine remains CPU-only;
after DSA registration, tag VLAN membership and egress filtering isolate
standalone ports and bridge domains. Version `0.0.15` also makes routed bridge
traffic fall back to the target port's standalone VID when that target is not a
bridge member, and implements the DSA MTU callbacks needed to add the 802.1Q
tag overhead to the CPU conduit. Version `0.1` removes the legacy recovery poller,
makes VLAN/PVID updates transactional, and maps VLAN-unaware FDB entries into
their tag_8021q isolation domains. Mixed-family SDK context isolation remains
unvalidated.

The driver consumes the MDIO controller exposed by the SoC DTS. It does not
require QSDK-specific UNIPHY clocks, CMN register windows, or a board-specific
MDIO-controller compatible.

Boards whose bootloader leaves an out-of-spec MDC rate should set the standard
MDIO `clock-frequency` property. For example, `1562500` selects the IPQ4019
controller's 100 MHz / 64 divider and avoids depending on a bootloader-retained
divider. This is a bus timing property and is independent of the switch model,
MDIO address, and DSA port layout.
The IPQ4019 MDIO controller patch also emits one startup line containing the
requested rate, effective divider, and mode register for hardware validation.

The package installs the kernel's switch-agnostic 802.1Q DSA tagger:

```text
tag_vsc73xx_8021q.ko
```

The driver uses standard VLAN headers so host MAC checksum engines can parse
the encapsulated Ethernet frame. The proprietary `0x8899` CPU tag is disabled.
Port identity is encoded by the Linux tag_8021q core using standalone and
bridge VIDs derived from DSA topology; no board-specific register sequence is
used.

The driver implements `port_change_mtu` and `port_max_mtu`. DSA therefore raises
the CPU conduit MTU by the tagger's 4-byte overhead while user ports retain the
requested Layer-3 MTU. RTL837x receive limits are programmed for both low-speed
and gigabit-or-faster modes with Ethernet header, VLAN header and FCS included.
This is handled per switch instance and does not require an interface-name or
board-specific hotplug script.

## Device Tree

Use a DSA `ports` description. The CPU port must reference the SoC Ethernet MAC
through the `ethernet` property, and user ports should be described as normal
DSA user ports.

Example:

```dts
&mdio {
	rtl837x: rtl837x-dsa@29 {
		compatible = "realtek,rtl837x";
		reg = <29>;

		#address-cells = <1>;
		#size-cells = <0>;

		reset-gpios = <&tlmm 42 GPIO_ACTIVE_LOW>; /* optional */
		reset-assert-us = <10000>; /* optional, default 100000 */
		reset-deassert-us = <50000>; /* optional, default 100000 */
		realtek,preserve-boot-config; /* optional */
		realtek,rtl8372n-led-init; /* optional, RTL8372N only */

		rtl837x,sds0mode = "10g-kr";

		sds0-rx-swap; /* optional */
		sds0-tx-swap; /* optional */
		sds1-rx-swap; /* optional */
		sds1-tx-swap; /* optional */
		phy-mdi-reverse; /* optional */
		phy-tx-polarity-swap; /* optional */

		gpio-controller; /* optional */
		#gpio-cells = <2>; /* optional */

		ports {
			#address-cells = <1>;
			#size-cells = <0>;

			port@3 {
				reg = <3>;
				label = "cpu";
				ethernet = <&gmac2>;
				phy-mode = "10gbase-r";

				fixed-link {
					speed = <10000>;
					full-duplex;
				};
			};

			port@4 {
				reg = <4>;
				label = "lan1";
				phy-mode = "internal";
			};

			port@5 {
				reg = <5>;
				label = "lan2";
				phy-mode = "internal";
			};

			port@6 {
				reg = <6>;
				label = "lan3";
				phy-mode = "internal";
			};

			port@7 {
				reg = <7>;
				label = "lan4";
				phy-mode = "internal";
			};
		};
	};
};
```

Legacy swconfig properties such as `rtl837x,cpu-port` and top-level `ethernet`
are only kept as compatibility fallback. New boards should use the DSA `ports`
binding.

### SerDes selection

Use `rtl837x,sds0mode` or `rtl837x,sds1mode` according to the physical CPU
port wiring. In the RTL8372/RTL8373 SDK and RTL8372N block diagram, SDS0 is
routed to MAC port 3 and SDS1 to MAC port 8. The driver derives the CPU SerDes
from the detected chip family and the DSA CPU port; it does not match a board
compatible. Supported mode strings include `10g-kr`, `10g-usxg`, `hsgmii`,
`2500base-x`, `sgmii`, `1000base-x`, `100base-fx`, and `8221b`.

### Multiple switches

The driver supports multiple RTL837x instances on the same MDIO controller.
Each switch must have its own MDIO `reg`, DSA CPU port, and SoC Ethernet
conduit. Independent switches should also use distinct `dsa,member` tree IDs,
for example `<0 0>` and `<1 0>`.

The Realtek SDK keeps an internal global register context. The driver therefore
serializes SDK operations and selects the calling switch before every hardware
transaction. DSA operations, internal PHY access, GPIO access, SFP callbacks,
and debugfs access all use the same context guard. Device-specific compatible
strings or fixed MDIO addresses are not required by the driver.

`realtek,init-after` is an optional phandle for layouts whose switches must be
initialized in a defined order. Probe of the consumer is deferred until the
referenced MDIO switch has completed DSA registration. The property describes
the hardware dependency directly and is not tied to a board name or address:

```dts
	switch1: switch@1d {
		realtek,init-after = <&switch0>;
	};
```

`realtek,quarantine-before-conduit` applies a minimal port-isolation state
before checking the SoC Ethernet conduit and before full DSA registration. The SDK's physical
valid-port mask is used for validation, while the operational mask is derived
from the available DSA `ports` children, so an instance can expose a subset of
the chip's physical ports. Each declared user port can
reach only the detected DSA CPU port, while that CPU port can reach the user
ports. The driver then checks the referenced conduit netdev and returns
`-EPROBE_DEFER` until it is fully registered and present, so full SDK/DSA access
is not attempted against an incompletely initialized PPE provider. This closes
the bootloader-state forwarding window without resetting the switch or applying
SDK cold initialization. The physical mask comes from the
detected switch family and the configured mask
comes from the available DSA ports, both constrained by the DSA CPU port; the
property contains no board or MDIO-address policy. Full DSA setup enables VLAN
egress filtering and registers all tag_8021q VLANs before it opens the
operational isolation matrix. The tag_8021q core gives each standalone port a
distinct VID and replaces it with a
shared bridge VID only while the port belongs to a VLAN-unaware bridge. This
makes VLAN membership the sole operational forwarding gate and prevents a
standalone WAN from joining a LAN broadcast domain even when both links carry
untagged traffic. Removing a port from a bridge restores its standalone VID.

### MDIO and reset handling

RTL837x register access uses the chip's indirect Clause 22 window. Each command
uses `__mdiobus_read()`/`__mdiobus_write()` while holding the bus lock and polls
the chip busy bit every 10 microseconds with a 5 millisecond bound before and
after the command. This matches the validated Linux 6.18 DSA transaction
sequence and avoids recursive entry through the mii_bus locking wrapper.
Controller errors and a busy response are recorded per switch instance. The
transaction does not impose QSDK timing, clock, or reset policy on the SoC MDIO
controller.

`reset-gpios` is optional. When present, the driver asserts the GPIO once and
then leaves it deasserted, honoring `reset-assert-us` and `reset-deassert-us`.
When absent, switch state handed off by the bootloader is left intact before
normal SDK initialization.

`realtek,preserve-boot-config` selects a warm-handoff path for systems whose
bootloader has already initialized the switch PHYs and SerDes. The driver still
detects the chip, attaches the SDK/DAL mapper, and applies the L2/VLAN/STP state
required by DSA, but it skips reset, the SDK switch-wide cold initialization,
board polarity overrides, `extra-init`, EEE changes, and SerDes programming.
Automatic CPU SerDes reset recovery is also disabled in this mode. This is a
generic per-switch policy; it does not depend on a board compatible, CPU port,
or MDIO address. Use it only when the bootloader contract is known and stable.

`realtek,rtl8372n-led-init` applies the RTL8372N SDK parallel-LED default table
after the DAL mapper is attached. This is separate from switch reset, PHY
patching, SerDes setup, and DSA forwarding state, so a board can preserve a
known-good bootloader data path while explicitly restoring the switch LED
mux, polarity, output-enable, and link/activity selection. The driver rejects
the property on other detected switch models; it does not match a board name,
MDIO address, or CPU port. The recovered table stores already shifted raw
register values; the driver converts each value back to the unshifted field
form required by `rtl8373_setAsicRegBits()` before applying it.

`realtek,reinit-cpu-serdes` provides a selective exception for a handoff where
one CPU link still needs the SDK's complete SerDes mode sequence. The driver
derives SDS0 or SDS1 from the detected chip and DSA CPU port, uses the mode from
`rtl837x,sds0mode` or `rtl837x,sds1mode`, runs the SDK reset flow before the
mode change, and relies on the SDK mode routine for its normal post-reset. No
board compatible, MDIO address, or fixed CPU-port value is used. The property
requires `realtek,preserve-boot-config` and a non-`off` CPU SerDes mode.

## Network Configuration

DSA exposes switch ports as normal Linux netdevs, for example:

```text
lan1 lan2 lan3 lan4
```

OpenWrt board network setup should add these DSA user ports directly to
`br-lan`. Do not configure this driver through swconfig or `switch_vlan`.
The tag_8021q bridge callbacks replace a port's standalone VID with the bridge
VID on join and restore it on leave. Ports in the same VLAN-unaware bridge can
forward in hardware, while a standalone WAN remains in a different VLAN domain.
This keeps WAN and LAN ports on the same switch from bypassing routing,
firewall, and NAT processing. `tx_fwd_offload` is enabled only through the
standard tag_8021q bridge contract.

## Debugging

Each probed switch gets a separate directory below `/sys/kernel/debug`, named
after its MDIO device. The files are bound to that switch instance:

```text
context
reg
phy_mmd
sdsreg
sds_page_dump
```

`context` reports the MDIO address, detected chip, CPU port, physical and
configured port masks,
configured SerDes modes, live SerDes control register, SDK context selection
counter, indirect-MDIO read/write counts, timeouts, last transaction, reset
configuration, LED-init policy, quarantine state, boot-handoff policy,
conduit readiness, probe
attempts/last raw ID, DSA/PHY operation counters, CPU-tag enable/readback,
VLAN-1 membership, and each port's live isolation mask, PVID, tag PVID, bridge
PVID, and ingress-filter state. The other files
provide per-instance register, internal PHY, and SerDes access. Mount debugfs
before use if it is not already mounted.

The tagger logs only the first transmitted and received 802.1Q packet for each
DSA tree/switch/port. The line includes the encoded VID, packet checksum state,
and forwarding-offload mark. It also logs the first routed fallback where a
bridge-originated packet targets a standalone DSA port, plus the first pending
checksum packet; subsequent packets remain silent.

## Notes

- RTL837x internal PHY status is handled by a small driver-specific PHY driver.
- Hardware MIB counters are exported through ethtool stats.
- Local LAN-to-LAN forwarding is permitted only through a shared tag_8021q
  bridge VID. Standalone ports retain distinct VIDs behind the CPU conduit.
