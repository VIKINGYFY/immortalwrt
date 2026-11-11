# Qualcomm AX patch responsibilities

Keep kernel-version-labelled backport filenames, order and upstream provenance.
Standard Quilt context refresh is allowed when expanded source stays identical.
Other changes are organized by functional purpose,
not by collecting every modification to one source file or SoC.

| Area | Responsibilities |
| --- | --- |
| Platform resources | Firmware memory, secure WLAN, CPU frequency/cooling |
| USB and PHY | DWC3 reference clocks/layout, IPQ5018 combo USB3 PHY |
| Clocking | Ethernet parents, GMAC/UNIPHY reset, essential fabric clocks |
| Firmware | SCM services, MDT loading, secure WCSS startup, multi-PD ownership |
| Peripherals | Pin control, NAND IDs/ECC, APM, CPR and boot command line |
| Networking | Driver registration, QCA ethertype, 8021Q domains, VLAN uppers |

USB controller layout is separate from Ethernet parent clocks. Secure WCSS,
multi-PD contexts and firmware-loading APIs retain distinct ownership.
Existing complete APM/CPR implementations remain individual driver units.

AX PPE/EDMA implementation and its dedicated matrix snapshot live in the
standard target overlay. Kernel patches provide driver registration and
network framework integration. No additional patch loader is used.

Only consecutive changes with a coherent function are combined, generated
from their exact application point. Keep kernel-version backports outside
those units. Remove a redundant series suffix when only one member remains;
remaining series members use consecutive suffixes. Preserve the primary
numeric application prefix and validate the full replay result.

PPE (0952) and EDMA (0953) registration remain separate to follow upstream
driver updates. Common flowtable/FDB/VLAN interfaces are owned by generic;
platform patches provide only their hardware consumers.

IPQ6018 and IPQ8074 ownership follows the same boundary: SoC-common clock
providers and GCC inputs live in the kernel DTS patches, while the target
ESS/NSS overlays contain the local UNIPHY, PPE, EDMA and NSS consumers. Do not
reintroduce a second `&gcc` or CMN PLL definition in an ESS overlay when the
corresponding SoC patch already supplies it. Local NSS node unit-addresses
match the first `reg` address; firmware load addresses remain explicit properties.

Keep upstream patch filenames and application positions when extending their
scope. Refresh later local contexts from exact source pairs. Preserve the
separate upstream IPQ8074 USB conversion rather than folding it into the older
IPQ5018/IPQ6018 conversion. Board USB mode, GPIO wiring, PHY reset delays,
MAC/calibration cells, CPU corners and memory layouts stay in their applicable
board or family include. Differences in register cell widths, UNIPHY counts,
NSS cores, clock names/rates, IRQ numbers and regulator data describe hardware;
they must not be flattened into a common include for cosmetic consistency.

IPQ5018 multi-PD boot owns its internal-radio MSA exception from the first
implementation. CPU frequency resources describe the final supported OPPs
directly. IPQ6018 secure WLAN device-tree wiring and SMP2P crash acknowledgement
remain separate units. DSA flowtable block ownership (0943), software VLAN
insertion for frame taggers (0944), generic VLAN-upper filtering (0963-01)
and the QCA transport consumer (0963-02) have independent responsibilities.

Use patch basenames in `Split from:` and `Merged from:` entries. Retain the
original subjects and existing provenance chains. Version-labelled patches
follow the same standard Quilt refresh while retaining their source references.
