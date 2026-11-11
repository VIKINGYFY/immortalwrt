# Qualcomm BE patch responsibilities

Preserve explicit kernel-version backport filenames, order and provenance.
Standard Quilt context refresh is allowed with identical expanded source.
The secure WCSS PIL backport (2010-01) retains its original implementation.
Organize other patches by function, retaining the standard lexical loader.

| Area | Responsibilities |
| --- | --- |
| PHY/PCS | QCA8084 package, SerDes lifecycle, PCS modes/bring-up |
| EDMA | Controller, netdev ports, RX, TX, interrupts, statistics, tuning |
| Platform | Clocks, reset/power domains, MDIO initialization, DTS resources |
| L2 | Bridge membership, VLAN domains, FDB, multicast, flooding |
| L3 | Hardware tables, flowtable lifetime, IPv6, PPPoE, VLAN transport |
| Tunnels | Transactions, IPv4-over-IPv6, MAP-E, underlay/LWT resolution |
| VP | Allocation/events, flow delivery, ingress, redirects, uplink lifetime |
| Ownership | Indexed flow readback, topology lifetime, DS rings/producers |

Do not combine controller allocation with netdev lifetime, queue limits with
interrupt mitigation, or flow priority with shaper state merely because they
share a driver. Native PPE remains patched in the kernel source tree.
Dedicated failure handling and diagnostics belong with the relevant function.

Merged patches retain their first primary ordering number. A collapsed series
has no orphan -01 suffix; multi-member series are numbered consecutively.
Preserved version backports are exempt from renaming. Generate each unit at
its precise application point and verify both directions and final source.

The 2010 series keeps the versioned PIL base unchanged, followed by distinct
Q6 crash acknowledgements, UserPD boot metadata and auxiliary firmware units.
Shared framework changes live in generic and are not duplicated here.

The PPE/EDMA base is organized as registration (0326-01), scheduler access
(0326-02), MAC/controller contracts (0327), EDMA controller (0331), netdevice
ports (0332), RX (0333), TX (0334), diagnostics (0335), ethtool (0336) and
queue API (0342). Each contains its final base implementation; do not restore
an all-driver update patch which replaces those implementations afterwards.

EDMA lifetime repairs (0642) are split into controller resources, RX buffer
and NAPI ownership, TX ring and NAPI ownership, and netdevice stopping.
Indexed L3 transactions, VP transport identity, flow retirement/topology and
physical resource diagnostics are separate 0651 units. Tunnel RTNL admission
belongs to tunnel underlay resolution. Direct Switch producer lifetime owns
the final node-count bounds; RTL cross-tree bridging owns FDB retirement.
Path MTU, ingress VP setup, flow-family counters and other independent
features retain separate patches where combining them would blur ownership.

Use patch basenames in `Split from:` and `Merged from:` entries while retaining
original subjects and earlier provenance. Version backports and the 2010-01
secure WCSS PIL base follow the same source-equivalent standard Quilt refresh.
