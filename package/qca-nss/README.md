# NSS offload packages for the OpenWrt qualcommax EDMA stack

An OpenWrt package feed that runs the **Qualcomm NSS offload stack** (the UBI32
NSS cores in IPQ807x) **on top of OpenWrt main's upstream qualcommax ethernet
drivers** — `qca_edma` / `qca_ppe`, merged into OpenWrt main from
[openwrt/openwrt#22381](https://github.com/openwrt/openwrt/pull/22381) — instead
of the classic vendor pairing of `qca-nss-dp` + `qca-ssdk`.

Developed and validated on a **Xiaomi AX3600** (IPQ8071A, 512 MB) running PPPoE
at 300 Mbit/s, with ECM NAT/PPPoE acceleration and NSS SQM shaping.

The companion OpenWrt tree — kernel patches, the `kmod-qca-ppe-nss` glue module,
device-tree changes — is at
[openwrt-nss-edma](https://github.com/JuliusBairaktaris/openwrt-nss-edma),
branch `nss-edma-rework`. For what NSS offload is and how the pieces fit, see
[NSS Offload Explained](https://github.com/JuliusBairaktaris/openwrt-nss-edma/wiki/NSS-Offload-Explained)
in the wiki.

## Provenance

This feed is the **CodeLinaro QSDK `nss-host`** sources, packaged to build
standalone against OpenWrt main. The driver stack (`qca-nss-drv`, `qca-mcs`,
`qca-nss-ecm`) tracks the tip of the **`NHSS.QSDK.14.0.r8`** release — the line
Qualcomm actively develops and tests as one set, and the newest that still
carries IPQ807x support.

The packaging conversion (pinned upstream sources instead of QSDK's
`local-development.mk`), the 5.15 → 6.12 kernel-compatibility patch queues, and
the firmware tarballs come from
[qosmio/nss-packages](https://github.com/qosmio/nss-packages); the 6.18
kernel bump and the EDMA-data-plane adaptation are this tree's.

### Firmware: why a 14.0 host stack runs 12.5 firmware

The firmware is **`NSS.FW.12.5-210-HK.R`** — the newest NSS firmware Qualcomm has
ever published for IPQ807x (Hawkeye). The official
[quic/qca-sdk-nss-fw](https://github.com/quic/qca-sdk-nss-fw) stops at SPF 12.0,
qosmio's fork carries it through 12.5, and **QSDK 14.0 dropped the Hawkeye
firmware package entirely** (NSS firmware moved to IPQ95xx). A `12.5-245-HK`
build is referenced by QSDK 13.0 but has never been published, so `12.5-210-HK`
is the firmware ceiling for this SoC.

The package also offers **`NSS_FIRMWARE_VERSION_11_4`** (`NSS.HK.11.4.0.5-6-R`,
from the same tarball): the last firmware line with 802.11s mesh support, and
the required selection for the ath11k NSS mesh offload. Newer firmware rejects
mesh interfaces at the firmware level. On 11.4 the shaper statistics wire
format differs; the NSS qdisc module selects it at build time, and SQM
runs unchanged with the same `nss-edma.qos` script.

The `14.0.r8` drivers and the `12.5-210` firmware share the same wire ABI — the
same combination the community NSS builds run — and the pairing is verified at
runtime, not assumed. `qca-nss-clients` has no `14.0` line and stays at the tip
of its last release, `NHSS.QSDK.12.5.5`.

## Packages and source pins

| Package | Source | Pin | Notes |
|---|---|---|---|
| `qca-nss-drv` | [lklm/nss-drv](https://git.codelinaro.org/clo/qsdk/oss/lklm/nss-drv) | `d7ef98b1d3d3` | Tip of `NHSS.QSDK.14.0.r8`. `exports/` ABI to the EDMA glue is stable; proven against firmware 12.5-210. |
| `qca-mcs` | [lklm/qca-mcs](https://git.codelinaro.org/clo/qsdk/oss/lklm/qca-mcs) | `063a4679ed22` | Tip of `NHSS.QSDK.14.0.r8`. IGMP/MLD snooping for multicast offload. |
| `qca-nss-ecm` | [lklm/qca-nss-ecm](https://git.codelinaro.org/clo/qsdk/oss/lklm/qca-nss-ecm) | `b6af8ffbd52b` | Tip of `NHSS.QSDK.14.0.r8`. NSS front-end; SFE/PPE/SDX front-ends compiled out. |
| `qca-nss-clients` | [lklm/nss-clients](https://git.codelinaro.org/clo/qsdk/oss/lklm/nss-clients) | `51be82d` | Tip of `NHSS.QSDK.12.5.5` — the newest clients commit on any maintained branch; no 14.0 line exists. |
| `nss-firmware` | [qosmio/qca-sdk-nss-fw](https://github.com/qosmio/qca-sdk-nss-fw) | 12.5 Release 210 | `NSS.FW.12.5-210-HK.R`; the firmware the driver source pins against. |
| `sqm-scripts-nss` | local `files/` | — | NSS shaper integration; ships one queue-setup script, `nss-edma.qos`. |

`qca-nss-clients` is trimmed to the seven KernelPackages this stack uses — the
NSS qdisc, the ingress-shaping action, the PPPoE connection manager, the bridge
manager (`kmod-qca-nss-drv-bridge-mgr`, wired LAN-bridge offload), the netlink
and mirror modules (`kmod-qca-nss-drv-netlink` / `-mirror` — the `nssinfo`
stats/control interface and port mirroring), and the Wi-Fi mesh manager
(`kmod-qca-nss-drv-wifi-meshmgr`, for the 802.11s mesh offload on the 11.4
firmware). The other managers, their compatibility patches and init scripts are
not carried.

On top of the upstream sources sit a small number of commits that trim the tree
to the packages this stack uses, convert each to build standalone on OpenWrt
main, carry the 5.15 → 6.18 kernel/toolchain compatibility patch queues, and
adapt the stack to the EDMA data plane (below).

## How the EDMA data-plane integration works

The classic NSS stack replaces the ethernet driver: `qca-nss-dp` registers the
GMAC netdevs and hands the data plane to the NSS cores, with `qca-ssdk`
programming the switch. This tree keeps OpenWrt main's upstream `qca_edma`
ethernet driver and DSA switch driver, and inserts a small glue module
(`kmod-qca-ppe-nss`, in the companion OpenWrt tree) that:

- exports the six `nss_dp_*` symbols `qca-nss-drv` consumes
  (`nss_data_plane/nss_data_plane.c` is the driver's entire nss-dp surface);
- overrides the data plane per physical port: TX is redirected from `qca_edma`'s
  xmit into the NSS conduit; RX arrives via `nss_dp_receive` callbacks from the
  firmware;
- replays the port bring-up the firmware expects (vsi_assign → MAC → MTU → open
  → link state) and re-asserts the PPE VSI flood masks the firmware clears.

Consequences, all verified on hardware:

- **The IPQ807x EDMA block is shared between the host and the NSS firmware.**
  Once the firmware boots it remaps PPE queue-to-ring delivery (QID2RID) so that
  *all* wired RX lands on firmware-owned rings. There is **no mixed mode**: with
  the firmware up, every physical port must be attached to the NSS data plane or
  it loses RX.
- **Nothing autoloads.** Loading `qca-nss-drv` boots the firmware; doing that
  before the glue is armed kills all wired RX until reboot. Bring-up is an
  explicit runtime sequence (load glue → arm `fw_mask` → load `qca-nss-drv` →
  attach ports → optionally ECM, PPPoE manager, SQM). The init scripts start
  nothing at boot, and the ECM and SQM scripts refuse to load their modules
  unless `qca_nss_drv` is already up.
- `qca-nss-drv` is built without PPE virtual-port support (`nss_ppe_vp.c` is the
  only ssdk consumer in the driver; ECM does not use ppe_vp).
- The NSS qdisc is built without PPE shaper and NSS-bridge support; the firmware
  accel mode (`accel_mode 0`) that SQM uses is unaffected.
- Bonding/LAG is compiled out (its kernel hooks come from QSDK kernel patches
  this tree does not carry).

## Building

In `feeds.conf`:

```
src-link nss /path/to/this/repo
```

Minimum config for the validated AX3600 setup:

```
CONFIG_PACKAGE_kmod-qca-nss-drv=y
CONFIG_PACKAGE_kmod-qca-nss-ecm=y
CONFIG_PACKAGE_kmod-qca-nss-drv-pppoe=y     # PPPoE offload
CONFIG_PACKAGE_kmod-qca-nss-drv-qdisc=y     # NSS qdiscs for SQM
CONFIG_PACKAGE_kmod-qca-nss-drv-igs=y
CONFIG_PACKAGE_sqm-scripts-nss=y
CONFIG_NSS_FIRMWARE_VERSION_12_5=y
CONFIG_NSS_MEM_PROFILE_MEDIUM=y             # 512 MB boards (AX3600)
```

`nss-firmware` is pulled in automatically by `kmod-qca-nss-drv` on qualcommax.
**Match the memory profile to the board's RAM** — the Config.in default for
ipq807x is the 1 GB profile, which is wrong for 512 MB devices like the AX3600.

The NSS feature switches (`CONFIG_NSS_DRV_*`) follow the packages you select
(e.g. the PPPoE manager forces `NSS_DRV_PPPOE_ENABLE`); IPv4 is always on, IPv6
follows global IPv6 support.

## Known issues

Open defects in the NSS stack this feed builds, found during IPQ807x bring-up.

### Firmware coredump panic on re-arm

Re-arming the firmware after a driver reload can panic with `NSS FW coredump:
bringing system down`. Consequence: a lightweight firmware reload cannot be used
to reset the offload plane in place — only a full reboot does.

### Out-of-memory on a carrier-down with a high-rate accelerated flow

If a LAN port goes carrier-down while a near-line-rate accelerated flow is
egressing it, the flow decelerates to the host slow path and the Linux bridge
floods the packets (unknown-unicast, once the FDB entry ages out) faster than
the remaining ports drain them, exhausting memory and rebooting the router.
Distinct from the silent egress-wedge — this one crashes/reboots. Fix direction:
back-pressure or drop for a down bridge port.

## Acknowledgements

- [qosmio/nss-packages](https://github.com/qosmio/nss-packages) and the
  community NSS builds lineage — the OpenWrt packaging conversion, the 5.15 →
  6.12 kernel-compatibility patch queues, and the firmware tarballs. This tree
  would not exist without that work.
- Ansuel's [openwrt/openwrt#22381](https://github.com/openwrt/openwrt/pull/22381)
  — the EDMA driver rework this stack runs on.

## Support the project

This is an unpaid, single-maintainer effort. If this work is useful to you,
consider chipping in — it goes toward IPQ807x development and hardware to start
looking into **IPQ50xx** and **IPQ60xx** next.

- **[GitHub Sponsors](https://github.com/sponsors/JuliusBairaktaris)** — zero-fee, GitHub-native
- **[PayPal](https://paypal.me/JuliusBairaktaris)** — one-off donations

Thank you!
