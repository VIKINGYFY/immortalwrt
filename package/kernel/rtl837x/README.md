# RTL837x DSA switch driver

This OpenWrt kernel package provides a Linux DSA driver for Realtek RTL837x
switch chips.

Bridge membership changes report `SVLAN bridge join/leave` with the physical
port, bridge name and standalone source SVID after successful programming.
Failed changes report their return code. Probe retries and registration do not
emit the former `RTL slot` lifecycle messages; chip, MDIO and initialization
errors remain available.

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
kmod-dsa-rtl837x
```

It builds the switch driver module:

```text
rtl837x_dsa.ko
```

The current package uses serialized SDK context selection for multiple RTL8372N
instances. DSA, PHY, GPIO, SFP and debugfs operations all use the same guard.
Mixed-family SDK context isolation remains unvalidated. The linked SDK mapper
retains callbacks referenced by all built API wrappers and SDK headers; unused
DAL modules are omitted without changing the mapper structure or PHY firmware.

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

The package installs the dedicated RTL837x 802.1ad DSA tagger:

```text
tag_rtl837x_8021ad.ko
```

The driver uses standard VLAN headers so host MAC checksum engines can parse
the encapsulated Ethernet frame. The proprietary `0x8899` CPU tag is disabled.
Port identity always uses an outer 802.1ad S-tag allocated by the Linux
tag_8021q core. The inner 802.1Q C-tag belongs to the customer VLAN. There is
one transport mode for all supported boards; the former `realtek,dsa-svlan`
property is no longer required or read. Untagged user frames remain valid.
The hardware shares one 4K table between service and customer VLANs. Customer
VIDs 3072–4095 are reserved for DSA service tags and return `-EBUSY`; membership
transactions read actual hardware rows instead of maintaining duplicate caches.

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
operational isolation matrix. The tag_8021q core allocates the reserved service-tag domain. Each user port
retains its standalone source SVID; bridge membership updates customer VLAN
membership, service-row membership and isolation without merging source-port
identities. A standalone WAN therefore remains outside the LAN forwarding
domain even when both carry untagged traffic. Leaving a bridge removes its
forwarding membership while retaining the port's source SVID.

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

The RTL837x 802.1ad DSA transport is unconditional. The CPU port is
configured as the SVLAN service port and each user port keeps a distinct
standalone SVID independent of its customer PVID. This permits VLAN-aware
bridges and routed VLAN interfaces without consuming the customer tag slot for
DSA identity. The Ethernet conduit must classify and emit independent S-tags
and C-tags; on Qualcomm PPE this requires the matching dual-slot flowtable
support. GL-BE6500 therefore uses the same transport as the Xiaomi boards.

## Network Configuration

DSA exposes switch ports as normal Linux netdevs, for example:

```text
lan1 lan2 lan3 lan4
```

OpenWrt board network setup should add these DSA user ports directly to
`br-lan`. Do not configure this driver through swconfig or `switch_vlan`.
The source SVID remains fixed and bridge membership is represented by the
customer VLAN table plus the port isolation matrix. This keeps a standalone WAN outside
the LAN forwarding domain so traffic cannot bypass routing, firewall, and NAT
processing.

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

Kernel logs identify each RTL instance by its MDIO address (`mdio=0`,
`mdio=29`, etc.) and retain probe, initialization, DSA registration, removal,
and shutdown errors. Repetitive per-slot trace lines are omitted; successful
VLAN/SVLAN bridge membership changes remain visible as `SVLAN bridge join` or
`SVLAN bridge leave` records with port, bridge, and source SVID.

The tagger validates the outer service tag and reports source-port decode
failures with rate limiting. It does not inspect PPPoE/LCP payloads or keep
per-switch tracing state. `context` identifies the `rtl837x-8021ad` transport
and its two VLAN slots on demand; setup no longer takes a separate register
snapshot for success logging.

## Notes

- RTL837x internal PHY status is handled by a small driver-specific PHY driver.
- Hardware MIB counters are exported through ethtool stats.
- Local LAN-to-LAN forwarding follows customer VLAN membership and bridge
  isolation. Source-port SVIDs remain distinct across bridge join/leave.

## Transactions and failure recovery

Bridge changes snapshot isolation, CPU tag preservation and service membership
before programming. Customer VLAN, tag VLAN and filtering changes also snapshot
the affected hardware row, customer PVID, service PVID, ingress filtering and CPU
tag preservation. Software PVID ownership is published only after readback.
Failed operations restore the complete snapshot, including a write that may have
committed before reporting failure.

A failed rollback retains one pending snapshot, marks `vlan-dirty` or
`bridge-dirty`, and attempts to block user ports. Later STP requests cannot reopen
them. A subsequent configuration transaction must restore pending VLAN state
and complete bridge recovery before forwarding is restored. A bus error can
also prevent the blocking write; this failure is explicitly logged. The
`context` file exposes both dirty flags and getter errors.

Cold PHY patch handshakes have a 30-read limit and propagate bus errors. Failure
cleanup releases patch requests/locks and restores the data-RAM access gate and
page selector. PHY firmware payloads and delays remain unchanged. VLAN table
reads and writes poll the indirect engine before and after each command.

Successful bridge membership changes retain `SVLAN bridge join/leave` logs.
Transaction failures report phase, original errno, rollback errno and dirty
state; completed recovery is logged once. There are no RTL slot lifecycle logs.

On Qualcomm PPE, `resource_stats` includes bounded `last_failure`, hardware
command failure and `last_reject` records. The latter carries monotonic time,
rejection stage/detail/source line, errno, cookie, tuple validity, interface and
port IDs, VLAN identities and PPPoE session IDs. These snapshots are overwritten
by later failures and are diagnostics, not a per-flow event history. Collect
`dmesg`, `/proc/uptime`, `/proc/sys/kernel/random/boot_id`, `resource_stats` and
RTL `context` together so timestamps can be matched to the same boot. The PPE
revision identifies the source corresponding to a rejection line number.

Host fault injection and compilation validate control paths; cold boot,
forwarding, throughput and real hardware error recovery require device testing.
