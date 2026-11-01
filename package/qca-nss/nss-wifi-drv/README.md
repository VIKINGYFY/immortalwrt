# NSS data-plane control

The service chooses WLAN ring ownership before ath11k probes. Change
`nss-wifi.general.offload` or firmware parameters and reboot to apply them.
Keep the service enabled even when selecting the host path with `offload=0`.
An interrupted bring-up leaves `/etc/nss-wifi.pending`; the following boot
uses host rings. Remove that file before rebooting to retry NSS.

The core-0 platform device exposes these sysfs attributes independently of
debugfs (normally `/sys/bus/platform/devices/39000000.nss`):

| Attribute | Access | Contents |
| --- | --- | --- |
| `boot` | read/write | Core running flag; write a boolean to boot or stop it. |
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
