# RTL9303 external SPI switch

This package provides an SPI register transport, MDIO and PCS providers, and
DSA support for an RTL9303 with an external CPU connected to a front-panel MAC.
The device tree supplies the CPU and user port topology, PHY modes, PHYs, and
PCS phandles. The driver programs only those declared ports.

The private 802.1ad service tag uses five port bits. RX and TX service VLANs
are separate so frames sent by Linux reach one selected user port. A
VLAN-unaware bridge can offload unicast and broadcast; VLAN-aware bridges
remain in software. Multicast is handled by Linux.

The package sources live in `src/`. Their module names use the `rtl9303-`
prefix. Device-specific wiring and build labels belong in the board target,
outside this package.
