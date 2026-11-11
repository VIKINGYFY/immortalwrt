# Wireless patch responsibilities

Keep the standard PatchDir order: build, subsys, common ath, individual ath
drivers and the remaining vendors. Kernel-version-labelled backports retain
their filenames, order and provenance, including the Raspberry Pi 6.12 series.
Standard Quilt context refresh is allowed with identical expanded source. The build
unaligned-header backport also retains its Linux 6.12 source reference.

| Area | Responsibilities |
| --- | --- |
| cfg80211/mac80211 | MLO station dumps, rates, DFS, AQL, four-address links |
| AP/MLD state | Reconfiguration, active-link lifetime, per-link statistics |
| Common ath | Debug/build interfaces remain separate from regulatory policy |
| ath11k platform | QCN6122 capabilities/bus support and firmware recovery |
| ath11k performance | Ring selection, TXQ/AQL, RX ownership, TX batching |
| ath11k resources | BH scheduling and indexed TX buffer ownership |
| ath12k platform | HW1 compatibility, DT identity and memory resources |
| ath12k MLO | Peer links/IDs, firmware ABI, MAC restart, UserPD/rootPD recovery |
| ath12k datapath | WMI bounds, monitor decoding, statistics, TX status lifetime |
| ath12k Direct Switch | Complete native DMA/transport and buffer ownership |

Do not merge unrelated MAC operations, firmware memory and peer recovery into
one whole-file patch. Preserve complete functional units and combine their
consecutive repairs without intermediate enable/revert implementations.
Other vendor or compatibility patches remain independent where there is no
coherent function to combine.

ath11k 992 retains six consecutive responsibilities: ring selection, TXQ/AQL,
RX buffers, TX/TCL batching, BH scheduling and TX index lookup. Collapsed
single-member series such as ath11k 955 and ath12k 100/906 drop subindices.
Version-labelled series stay unchanged. Shared driver contracts precede their
consumers; native Direct Switch requires the matching kernel PPE interfaces.
Always validate forward/reverse replay and the final expanded source.

Use the upstream 007 IEEE80211 forward-path discovery and 108 compact DMA
capability parser once each. Bridge and routed flowtables request the upstream
IEEE80211 terminal path; ath11k/ath12k do not duplicate it with AP-only callbacks.
PPDU limits remain separate. The sole PPDU-limit and 394 management-link units
have no series subindex.

SCS/MSCS uses final per-client state and learning (439), software classification
hooks (440), driver offload operations (443), and diagnostics (444). The
classifier engine is introduced once, without a temporary single-MSCS store
or debugfs iterator that a later patch replaces. cfg80211 parsing, validation
and netlink protocol remain separate from mac80211's implementation.

ath12k firmware-memory contracts (315-01), allocation ownership (315-02), and
root-PD recovery (315-03) are separate units. Named reservations, bounded AHB
host-DDR views, optional DMA allocation and shared MLO pools retain their final
ownership rules directly. Host RX profiles remain independent. HW1 descriptor
compatibility (100) is separate from firmware MLO group admission (101).

Direct Switch ring ownership contains the bounded TCL halt/restore sequence;
peer/VAP handling contains four-address CPU fallback; lifecycle integration
contains control-port delivery. ath11k management TX IDR cleanup remains a
separate lifetime unit. NSS ring descriptions, host-ring handover, VDEV data,
authorization and interface unwind own their associated repairs, including
QCN9074 bus addressing and host TX software allocation. Recovery producer
ordering and experimental NSS controls retain separate responsibilities.

Use basenames, original subjects and preserved provenance chains in
`Split from:` and `Merged from:`. Standard Quilt refresh also covers version-
labelled backports while retaining their original source provenance.
