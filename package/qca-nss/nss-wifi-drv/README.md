# NSS data-plane control

The service chooses WLAN ring ownership before ath11k probes. Change
`nss-wifi.general.offload` or firmware parameters and reboot to apply them.
Keep the service enabled even when selecting the host path with `offload=0`.
An interrupted bring-up leaves `/etc/nss-wifi.pending`; the following boot
uses host rings. Remove that file before rebooting to retry NSS.

The early-probe guard checks each PHY's bound driver and blocks only
ath11k/ath11k_pci. A separately autoloaded ath10k AIoT PHY, such as the
AX3600's QCA9887, does not prevent the regular ath11k radios from loading.

The core-0 platform device exposes these sysfs attributes independently of
debugfs (normally `/sys/bus/platform/devices/39000000.nss`):

| Attribute | Access | Contents |
| --- | --- | --- |
| `boot` | read/write | Core running flag; write a boolean to boot or stop it. Manual stop returns `-EBUSY` while WLAN targets are configured. |
| `wifi_start` | write | Write `1` to initialize all fully described WLAN instances. Errors are returned to the writer. |
| `state` | read | Current physical target count, registered/described/started instances, readiness, VDEV/peer counts and per-instance errors. |
| `flow_stats` | read | IPv4, IPv6, total installed bidirectional rules, total firmware capacity. |

`ready=1` requires a running core and every expected on-chip/eligible PCI
instance on the selected buses. Disabled device-tree WLAN nodes and absent
bus modules are excluded. VDEVs are created later by netifd and are not a
boot prerequisite. `ready=1` does not establish AP availability or client
forwarding. `vdev_errors` counts registered instances whose latest VDEV
creation failed; each instance reports the corresponding `vdev_error`.
A later successful creation or a new ring registration clears that error.

After a WLAN reset, the final radio ring registration waits for the NSS
restart worker outside the WiFiLi mutex and before mac80211 restores VDEVs
under RTNL. VDEV creation never waits for that worker. A WLAN-only restart
does not reset shared EDMA while another instance keeps using it.
Port arming runs in separate work, so its RTNL acquisition cannot delay
completion of WLAN recovery.

A core fault stops the processor before its buffers can be released. Pending
buffer interrupts cannot mark a halted core as newly booted. All WLAN
instances must release the old rings before the core is loaded again;
mac80211 PHY restarts follow that handover so they do not hold RTNL while
another instance still needs to stop. A PCIe WLAN instance still in mission
mode skips waiting for an RDDM dump; actual RDDM faults retain dump collection.

IPQ6018 also asserts its separate GCC processor reset on halt and releases it
only after the boot address and fetch window have been programmed. The shared
clamp resets alone do not reliably restart this processor. IPQ8074 retains
its existing reset bits.

Core boot allows five seconds for the firmware response. If an attempt times
out, the driver waits for the shared EDMA reset and retries once before
returning the failure to WiFiLi.

For the selected IPQ6018/IPQ8074 AHB remoteproc, the service changes an
`inline` crash dump to `enabled` before probing WLAN. The kernel retains
the dump in `/sys/class/devcoredump/` without waiting up to five minutes
for userspace to release it before restarting the firmware. This uses a
copy of the dump while it is retained; non-WLAN targets are unaffected.
The persistent guard is cleared only after 120 seconds and a fresh healthy
state check. Repeated service actions preserve a healthy running instance.

The package is selected independently of ath11k on IPQ60xx and IPQ807x.
RE-CS-07 and NO WIFI images can run the NSS core without loading ath11k,
waiting for WLAN rings, or issuing WiFiLi initialization messages. The
service derives the read-only `wlan` module parameter from the build marker,
installed modules and enabled hardware: bit 0 selects AHB, bit 1 PCI, and
zero selects non-WLAN offload only. Apply a change by rebooting.

With no WLAN targets the automatic payload pool uses 8192 buffers with a
4096-buffer refill threshold, instead of 16384/4352. Explicit nonzero
`pool_size` and `pool_low` parameters override these defaults. The shared
firmware memory requests and descriptor pool are retained for forwarding.

## Optional connection tables

The default is 1024 bidirectional connections for each of IPv4 and IPv6.
Unused capacity in one family is not available to the other. NSS12.5 can
negotiate host-backed tables with the `connections` module parameter: 1024,
2048, 3072 or 4096 connections per family. Values above 1024 are opt-in and
are rejected with other firmware lines. Change parameters before booting
NSS; changing `connections` while the core is loaded returns `-EBUSY`.

The connection tables and packet payload pool are separate resources.
For example, this WLAN configuration requests 4096 connections per family
and a 24576-buffer payload ceiling:

```sh
uci set nss-wifi.general.param='connections=4096 pool_size=24576'
uci commit nss-wifi
reboot
```

Relative to the automatic WLAN pool of 16384 buffers, that ceiling allows
8192 additional 4 KiB buffer objects, or 32 MiB, plus connection-table RAM.
The firmware supplies the required connection-entry and match-entry table
sizes. The driver allocates DMA memory and updates admission limits only
after both families acknowledge the configuration. The firmware profile
and available memory still constrain which requests can succeed. No PPE
SRAM tables are enlarged by these parameters.

While WLAN targets are configured, a manual `boot=0` request returns
`-EBUSY`: stopping NSS alone would leave automatic WLAN recovery without
its offload core. Use a full WLAN/NSS module teardown or reboot to change
startup parameters. Coordinated fault recovery and driver removal keep
their existing paths.

A failed or timed-out table handover stops the core before its DMA memory
is freed; correct the parameters before retrying NSS. The persistent
bring-up guard described above still applies. Default table configuration
does not allocate these optional host tables.

Read `flow_stats` for the installed IPv4, IPv6 and total counts followed by
the configured combined capacity. Its final value is 8192 for the example;
each family is still limited to 4096. Passive `flows` diagnostics also
show per-family limits, resource retries and firmware buffer availability.
Statistics scan budgets grow with the configured table size so active
connections are refreshed before the kernel flow timeout. Concurrent
per-flow statistics work reads the RCU cache while one caller polls NSS.
At capacity, new connections continue in software and may retry hardware
offload after entries are released. Firmware buffer-allocation refusals
use bounded backoff independently of table admission.

Non-WLAN functions include supported IPv4/IPv6 TCP/UDP routing and IPv4 NAT
through USB Ethernet/raw-IP redirect devices, plus the routed-multicast
back end and its VLAN/PPPoE handling. Ordinary wired unicast remains on PPE.
This driver provides no general crypto, IPsec or arbitrary tunnel offload
API. Firmware capabilities alone do not make an unimplemented protocol
available. Unsupported flows remain in software.

An authorization transport or firmware error stops the NSS processor before
returning the error. Shared DMA is retained until WLAN reset has stopped the
radios. A reboot is required to re-enable offload after this failure.

Normal builds omit action-on-read debugfs probes and experimental ath11k
ring ownership parameters. `CONFIG_NSS_WIFI_DEBUG=y` explicitly enables
them; passive counters and error diagnostics remain available by default.
`log_poll_us` accepts 100 through 1000000 microseconds; its default remains
200 until fault-log retention and CPU measurements justify a change.
