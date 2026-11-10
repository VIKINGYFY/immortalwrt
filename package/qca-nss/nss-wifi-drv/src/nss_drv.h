/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared state and the firmware's wire format for the NSS host driver.
 *
 * Everything named here that the firmware also reads or writes is fixed by the
 * firmware: the two descriptor layouts, the index page they are described in,
 * the message header, the meminfo request record and the log ring. The rest is
 * this driver's own.
 */

#ifndef __NSS_DRV_H
#define __NSS_DRV_H

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/list.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "nss_fw.h"

/* A NO WIFI kernel can omit the wireless pointer from struct net_device. */
static inline bool nss_dev_is_wifi(const struct net_device *dev)
{
#if IS_ENABLED(CONFIG_CFG80211_HEADERS)
	return dev->ieee80211_ptr;
#else
	return false;
#endif
}

/* Ring geometry. The index page always describes 16 host-to-firmware and 15
 * firmware-to-host slots whatever the SoC populates, so the arrays are sized
 * by the page and the counts say how many are real.
 */
#define NSS_H2N_RING_SLOTS	16
#define NSS_N2H_RING_SLOTS	15
#define NSS_H2N_RINGS		11
#define NSS_N2H_RINGS		5
#define NSS_RING_ENTRIES	128

/* Each ring is allocated with two descriptors of slack that neither side ever
 * addresses. They set the stride between one ring's base and the next.
 */
#define NSS_RING_STRIDE_ENTRIES	(NSS_RING_ENTRIES + 2)

#define NSS_H2N_RING_EMPTY_BUF	0
#define NSS_H2N_RING_COMMAND	1
#define NSS_H2N_RING_DATA	3
#define NSS_N2H_RING_EMPTY_BUF	0

/* Buffer types, in the descriptors. */
#define NSS_H2N_BUFFER_EMPTY	0
#define NSS_H2N_BUFFER_PACKET	2
#define NSS_H2N_BUFFER_CTRL	4

#define NSS_N2H_BUFFER_EMPTY	1
#define NSS_N2H_BUFFER_PACKET	3
#define NSS_N2H_BUFFER_STATUS	6
/* A frame the firmware transmits through a host-side node, for the host to
 * send on that node's device.
 */
#define NSS_N2H_BUFFER_PACKET_VIRTUAL	10
#define NSS_N2H_BUFFER_PACKET_EXT	13

/* The hardware fills the named checksum on the way out; without these a
 * frame the stack left for the device to finish leaves carrying only the
 * pseudo-header sum.
 */
#define NSS_H2N_FLAG_GEN_IP_CHECKSUM	0x0001
#define NSS_H2N_FLAG_GEN_L4_CHECKSUM	0x0002
#define NSS_H2N_FLAG_FIRST_SEGMENT	0x0004
#define NSS_H2N_FLAG_LAST_SEGMENT	0x0008
/* A frame handed to a host-side node, to be routed as if it had arrived on
 * that node's device.
 */
#define NSS_H2N_FLAG_VIRTUAL_BUFFER	0x2000
/* The buffer may be kept rather than handed straight back. */
#define NSS_H2N_FLAG_BUFFER_REUSABLE	0x8000

unsigned int nss_pool_ceiling(void);
unsigned int nss_wifili_expected(void);

/* The firmware chooses its own offset inside a receive buffer, so the buffer
 * it is given is the head of the allocation and it reports where the payload
 * landed.
 */
#define NSS_EMPTY_BUFFER_SIZE	1984
#define NSS_EMPTY_BUFFER_ALLOC	(NSS_EMPTY_BUFFER_SIZE + NET_SKB_PAD)

/* Interface numbers the firmware dispatches on. */
#define NSS_INTERFACE_N2H	156
#define NSS_INTERFACE_ETH_RX	158
#define NSS_INTERFACE_IPV4	161
#define NSS_INTERFACE_IPV6	163
#define NSS_INTERFACE_DYNAMIC	176
/* The base virtual-device node, where the firmware keeps settings that
 * belong to every virtual device rather than to one of them.
 */
#define NSS_INTERFACE_VAP_BASE	200

#define NSS_INTERFACE_WIFILI	203
/* The instances for radios on their own chip. */
#define NSS_INTERFACE_WIFILI_EXT0	215
#define NSS_INTERFACE_WIFILI_EXT1	216
#define NSS_INTERFACE_MAX	228

/* Interrupt causes, one GIC line each: there are no mask or status registers
 * on this SoC, so the cause is fixed when the line is claimed.
 */
/* The order is the firmware's, not a choice: each entry is the index of the
 * interrupt line in the device tree, and the firmware decides what it raises
 * on which.
 */
enum nss_cause {
	NSS_CAUSE_EMPTY_BUFFER_SOS = 0,
	NSS_CAUSE_EMPTY_BUFFER_QUEUE = 1,
	NSS_CAUSE_TX_UNBLOCKED = 2,
	NSS_CAUSE_DATA_QUEUE_0 = 3,
	NSS_CAUSE_DATA_QUEUE_1 = 4,
	NSS_CAUSE_DATA_QUEUE_2 = 5,
	NSS_CAUSE_DATA_QUEUE_3 = 6,
	NSS_CAUSE_COREDUMP_COMPLETE = 7,
	NSS_CAUSE_PAGED_EMPTY_BUFFER_SOS = 8,
	NSS_CAUSE_PROFILE_DMA = 9,
	NSS_CAUSE_MAX,
};

/* Doorbell: the interrupt-cause bit for a host-to-firmware queue, written to
 * the shared QGIC register. Core 0's causes start at bit 13.
 */
#define NSS_QGIC_IPC_REG	0x8
#define NSS_H2N_INTR_BASE(id)	(13 + (id) * 6)
#define NSS_H2N_INTR_EMPTY_BUF	0
#define NSS_H2N_INTR_DATA_CMD	1

struct h2n_descriptor {
	u32 interface_num;
	u32 buffer;
	u32 qos_tag;
	u16 buffer_len;
	u16 payload_len;
	u16 mss;
	u16 payload_offs;
	u16 bit_flags;
	u8 buffer_type;
	u8 reserved;
	u64 opaque;
};

struct n2h_descriptor {
	u32 interface_num;
	u32 buffer;
	u16 buffer_len;
	u16 payload_len;
	u16 payload_offs;
	u16 bit_flags;
	u8 buffer_type;
	u8 response_type;
	u8 pri;
	u8 service_code;
	u32 reserved;
	u64 opaque;
};

static_assert(sizeof(struct h2n_descriptor) == 32);
static_assert(sizeof(struct n2h_descriptor) == 32);

struct h2n_desc_if_meta {
	u32 desc_addr;
	u16 size;
	u16 pad;
};

struct n2h_desc_if_meta {
	u32 desc_addr;
	u16 size;
	u16 pad;
};

/* The page both sides use to find the rings and to publish how far each has
 * got through them. The firmware owns the nss_index arrays and the host owns
 * the hlos_index arrays; neither writes the other's. The page is coherent
 * memory, so the only thing a read of the other side's index needs is for the
 * compiler not to invent or cache it - READ_ONCE and WRITE_ONCE, not volatile
 * on the type.
 */
struct nss_if_mem_map {
	struct h2n_desc_if_meta h2n_desc_if[NSS_H2N_RING_SLOTS];
	struct n2h_desc_if_meta n2h_desc_if[NSS_N2H_RING_SLOTS];
	u32 magic;
	u16 if_version;
	u8 h2n_rings;
	u8 n2h_rings;
	u32 h2n_nss_index[NSS_H2N_RING_SLOTS];
	u32 n2h_nss_index[NSS_N2H_RING_SLOTS];
	u8 num_phys_ports;
	u8 reserved1[3];
	u32 h2n_hlos_index[NSS_H2N_RING_SLOTS];
	u32 n2h_hlos_index[NSS_N2H_RING_SLOTS];
	u32 reserved;
};

static_assert(sizeof(struct nss_if_mem_map) == 512);

#define NSS_IF_MEM_MAP_MAGIC	0x4e52522e
#define NSS_IF_MEM_MAP_VERSION	1

/* Meminfo. The image declares what it needs in a table the host walks,
 * writing back the address it allocated for each entry.
 */
#define NSS_MEMINFO_OFFSET		8
#define NSS_MEMINFO_MAGIC_RESERVE	0x9526
#define NSS_MEMINFO_MAGIC_MAP		0x9527
#define NSS_MEMINFO_MAGIC_REQUEST	0x9528
#define NSS_MEMINFO_MAGIC_END		0x9529

#define NSS_MEMINFO_BLOCK_NAME_LEN	48

enum nss_meminfo_memtype {
	NSS_MEMINFO_MEMTYPE_IMEM,
	NSS_MEMINFO_MEMTYPE_SDRAM,
	NSS_MEMINFO_MEMTYPE_UTCM_SHARED,
	NSS_MEMINFO_MEMTYPE_INFO,
};

/* Log ring. The firmware writes entries and advances its own index; the host
 * only ever reads.
 */
#define NSS_LOG_COOKIE		0xff785634
#define NSS_LOG_LINE_WIDTH	132

struct nss_log_entry {
	u64 sequence_num;
	u32 cookie;
	u32 thread_num;
	u32 timestamp;
	char message[NSS_LOG_LINE_WIDTH];
} __aligned(32);

struct nss_log_descriptor {
	u32 cookie;
	u32 log_nentries;
	u32 current_entry;
	u8 pad[20];
	struct nss_log_entry log_ring_buffer[];
} __aligned(32);

static_assert(sizeof(struct nss_log_entry) == 160);
static_assert(sizeof(struct nss_log_descriptor) == 32);

/* The message header is the exported ABI, shared with every wifili and vdev
 * message that embeds it. The sixteen bytes at cb and app_data come back
 * exactly as they were sent, which is the only thing tying a reply to its
 * request - there is no sequence number anywhere in the protocol.
 */
#define NSS_HLOS_MESSAGE_VERSION	1

static_assert(sizeof(struct nss_cmn_msg) == 40);
static_assert(offsetof(struct nss_cmn_msg, cb) == 24);

/* What the host knows about a buffer it lent the firmware. The descriptor a
 * buffer comes back in is written by the other side, so its address and
 * length describe what the firmware did with the buffer, not what the host
 * mapped - and unmapping by those is a partial unmap of a different range.
 */
struct nss_skb_cb {
	dma_addr_t dma;
	bool tx;	/* a frame handed down, not a buffer handed over */
	bool bidir;	/* mapped both ways: the firmware may hand it back filled */
	u64 cookie;	/* host slot and generation, never an skb address */
};

#define NSS_SKB_CB(skb) ((struct nss_skb_cb *)(skb)->cb)

static_assert(sizeof(struct nss_skb_cb) <= sizeof_field(struct sk_buff, cb));

struct nss_lent_buffer {
	struct sk_buff *skb;
	u64 cookie;
};

/* A host-to-firmware ring and the buffers in flight on it. */
struct nss_h2n_ring {
	struct h2n_descriptor *desc;
	spinlock_t lock;
	u32 hlos_index;
};

struct nss_n2h_ring {
	struct n2h_descriptor *desc;
	u32 hlos_index;
};

/* One message in flight at a time, in a block the host owns outright.
 *
 * The firmware writes its answer into the same buffer the request arrived
 * in and hands it back, so the buffer is read and written by both sides and
 * belongs in coherent memory rather than in a streaming mapping whose
 * direction would have to be wrong in one of the two phases. It is also what
 * identifies the reply: the descriptor comes back carrying the address the
 * host handed out, so nothing has to trust a pointer the firmware echoed.
 */
struct nss_msg {
	void *buf;
	dma_addr_t dma;
	struct mutex lock;
	struct completion done;
	/* A timed-out request still owns the buffer. */
	bool lost;
};

struct nss_core;
void nss_pool_notify(struct nss_core *core, const struct nss_cmn_msg *cm, u32 len);

struct nss_irq_ctx {
	struct nss_core *core;
	struct napi_struct napi;
	enum nss_cause cause;
	int irq;
	bool napi_added;
};

/* One coherent block handed to the firmware, kept so it can be released. */
struct nss_alloc {
	struct list_head node;
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

struct nss_clk_cfg {
	const char *name;
	unsigned long rate;
};

/* One unsolicited message type: how many arrived, and what the first one
 * said. The body is kept, not just the header, because what a message is
 * worth reading for is the numbers in it - a request the host must answer
 * carries them there, and so does a statistics push. The first word is the
 * interface the message came from.
 */
#define NSS_MSG_SEEN_MAX	64

/* A data buffer the firmware handed up that is not an ordinary frame. What
 * it carries is a metadata area whose shape is not derivable from the header
 * the host has, so the first few are kept whole and read off the device.
 */
struct nss_ext_seen {
	u32 iface;
	u32 type;
	u32 len;
	u32 offs;
	u32 word[16];
};

struct nss_msg_seen {
	u64 count;
	u32 words;
	u32 lastwords;
	u32 word[64];
	u32 last[256];
};

struct nss_soc;

/* Where a firmware line's messages differ from the layouts in nss_fw.h. */
struct nss_fw_line {
	const char *name;
	u32 peer_msg_len;
	u32 soc_stats_skip;	/* bytes missing before the receive counters */
	u32 vdev_stats_len;
	u32 vdev_stats_hole;	/* first word the line lacks inside the block */
	bool mesh;
};

struct nss_core {
	struct device *dev;
	const struct nss_soc *soc;
	const struct nss_fw_line *fw;
	void __iomem *nphys;
	void __iomem *qgic;
	void __iomem *imem;
	void __iomem *misc_reset;
	resource_size_t imem_size;
	u32 imem_base;
	u32 imem_used;
	phys_addr_t load_addr;
	resource_size_t region_base;
	resource_size_t region_size;
	unsigned long core_rate;
	u32 id;
	bool loaded;
	/* Only the first SOS from a released image can finish its boot. */
	spinlock_t state_lock;
	bool booting;
	bool running;
	bool wifili_probed;
	bool wifili_started;
	bool mesh_capable;
	bool clocks_on;
	/* Asked to run: set and cleared through debugfs, cleared by faults. */
	bool up;
	/* Wi-Fi was started on this core, so a restart starts it again. */
	bool wifi_on;
	bool faulted;
	u32 faults;
	unsigned long fault_window;
	struct work_struct restart;
	struct clk **aux_clks;
	int num_aux_clks;
	unsigned long phys_armed;
	struct work_struct arm;
	/* Buffers the host lends this core, and the firmware's high-water
	 * mark: the two are kept equal because the firmware hands back
	 * whatever it holds above its mark.
	 */
	u32 pool_size;
	u32 pool_floor;
	u32 pool_used;
	u32 pool_failed;
	u32 pool_failed_seen;
	u32 pool_grows;
	u32 pool_shrinks;
	u32 pool_errors;
	unsigned long pool_sample_at;
	unsigned long pool_idle_at;
	bool pool_adaptive;
	bool pool_disabled;
	struct delayed_work pool_work;
	void **pbuf_page;
	dma_addr_t *pbuf_dma;
	u32 pbuf_pages;
	struct mutex lock;
	struct dentry *debugfs;

	struct list_head allocs;
	struct nss_if_mem_map *if_map;
	struct nss_log_descriptor *log;
	u32 log_entries;
	struct nss_log_entry *shadow;
	u32 shadow_seen;
	u32 shadow_held;
	struct hrtimer shadow_timer;
	spinlock_t shadow_lock;
	struct nss_h2n_ring h2n[NSS_H2N_RINGS];
	struct nss_n2h_ring n2h[NSS_N2H_RINGS];
	struct nss_irq_ctx irq[NSS_CAUSE_MAX];
	struct net_device *ndev;
	struct net_device *conduit;
	/* A core has run on the EDMA block since it was last reset. */
	bool edma_stale;
	struct work_struct edma_reset;

	struct nss_msg msg;
	struct net_device __rcu *iface[NSS_INTERFACE_MAX];
	u64 rx_iface[NSS_INTERFACE_MAX];
	struct nss_msg_seen seen[NSS_MSG_SEEN_MAX];
	struct nss_ext_seen ext[4];
	u8 ext_seen;
	u64 rx_type[8];
	u64 notify;
	u64 tx_posted;
	/* Several receive polls at once. */
	atomic64_t tx_done;
	atomic64_t rx_desync;
	atomic64_t link_desc_seen;
	u64 link_desc_returned;
	atomic_t buffers_queued;
	u64 tx_dup;		/* transmitted buffers the firmware returned again */

	/* Every buffer lent to the firmware, so that halting the core can
	 * take them back. The descriptor that lent one cannot be used for
	 * that: the firmware holds far more buffers than a ring has entries,
	 * so by the time it holds a poolful the descriptors that handed the
	 * first ones over have been written many times since.
	 */
	struct nss_lent_buffer *lent;
	u32 *lent_free;
	u32 lent_free_n;
	u32 lent_size;
	u32 lent_shift;
	spinlock_t lent_lock;
	struct completion booted;

	struct clk_bulk_data clks[];
};

int nss_meminfo_init(struct nss_core *core);
void *nss_mem_alloc(struct nss_core *core, size_t size, u32 align,
		    u32 *addr_out);
void nss_mem_free(struct nss_core *core, void *cpu);
void nss_mem_free_all(struct nss_core *core);
int nss_wifi_pool_set(struct nss_core *core, u32 payloads);
void nss_pbuf_pool_free(struct nss_core *core);
void nss_core_stop(struct nss_core *core);
void nss_core_block(struct nss_core *core);
void nss_core_fault(struct nss_core *core);
void nss_core_resume(struct nss_core *core);
int nss_rings_start(struct nss_core *core);
void nss_iface_bind(struct nss_core *core);
void nss_iface_unbind(struct nss_core *core);
void nss_rings_stop(struct nss_core *core);
void nss_rings_quiesce(struct nss_core *core);
int nss_data_send(struct nss_core *core, struct sk_buff *skb, u32 if_num,
		  u16 flags);
void nss_doorbell(struct nss_core *core, u32 intr);
int nss_msg_init(struct nss_core *core);
int nss_msg_send(struct nss_core *core, void *msg, size_t len);
int nss_msg_send_quiet(struct nss_core *core, void *msg, size_t len);
int nss_msg_transact(struct nss_core *core, void *msg, size_t len, size_t room);
bool nss_msg_complete(struct nss_core *core, const struct n2h_descriptor *desc);
int nss_msg_probe(struct nss_core *core, struct seq_file *s);
void nss_msg_seen(struct nss_core *core, const struct nss_cmn_msg *ncm,
		  u32 len);
int nss_wifili_probe(struct nss_core *core, struct seq_file *s);
int nss_wifili_start(struct seq_file *s);
ssize_t nss_wifili_state(struct nss_core *core, char *buf);
ssize_t nss_flow_summary(char *buf);
void nss_wifili_resume(struct nss_core *core);
void nss_wifili_halted(struct nss_core *core);
bool nss_wifili_reset(struct nss_core *core, bool started);
int nss_wifili_tx(struct seq_file *s);
void nss_msg_census(const struct nss_cmn_msg *ncm);
void nss_msg_census_print(struct seq_file *s);
void nss_wifili_stats_print(struct seq_file *s);
void nss_wifili_vdev_stats_print(struct seq_file *s);
void nss_wifili_vdev_notify(struct nss_core *core, const struct nss_cmn_msg *ncm, u32 len);
void nss_wifili_notify(struct nss_core *core, const struct nss_cmn_msg *ncm,
		       u32 len);
void nss_wifili_bind(struct nss_core *core);
bool nss_wifili_owns(const struct net_device *dev);
bool nss_wifili_is_soc(u32 if_num);
void nss_wifili_me_snoop(u32 if_num, struct sk_buff *skb);
bool nss_wifili_special_recv(struct nss_core *core, struct napi_struct *napi,
			     const struct n2h_descriptor *desc, struct sk_buff *skb);
bool nss_wifili_rx_80211(u32 if_num, struct net_device *dev,
			 struct sk_buff *skb, struct napi_struct *napi);
void nss_wifili_me_print(struct seq_file *s);
void nss_wifili_me_exit(void);
int nss_wifili_vdev_msg(struct nss_core *core, int if_num, u32 type,
			const void *body, size_t len);
int nss_wifili_vdev_cmd(struct nss_core *core, int if_num, u32 cmd, u32 value);

struct nss_wifi_mesh_ops;

bool nss_mesh_probe(struct nss_core *core);
int nss_mesh_add(struct nss_core *core, struct net_device *dev, int link,
		 const u8 *mac, const struct nss_wifi_mesh_ops *ops, void *priv);
int nss_mesh_attach(struct nss_core *core, int link);
void nss_mesh_stop(struct nss_core *core, int link);
void nss_mesh_del(struct nss_core *core, int link);
void nss_mesh_reset(void);
int nss_mesh_ifnum(int ifindex);
bool nss_mesh_notify(struct nss_core *core, const struct nss_cmn_msg *ncm,
		     u32 len);
bool nss_mesh_recv(struct nss_core *core, struct napi_struct *napi,
		   const struct n2h_descriptor *desc, struct sk_buff *skb);
void nss_mesh_print(struct seq_file *s);
void nss_mesh_exit(void);
int nss_phys_arm(struct nss_core *core, unsigned long mask);
int nss_phys_msg(struct nss_core *core, int if_num, u32 type,
		 const void *body, size_t len);
void nss_phys_arm_ports(struct nss_core *core);
void nss_phys_event(struct nss_core *core, unsigned long event,
		    struct net_device *dev);
void nss_flow_bind(struct nss_core *core);
bool nss_flow_is_ours(int ifindex);
int nss_flow_ifnum(struct nss_core *core, int ifindex, u32 *mtu);
int nss_flow_pppoe_hold(struct nss_core *core, u16 sid, const u8 *server,
			const u8 *local, int port, u32 mtu, u32 tag);
void nss_flow_pppoe_drop(struct nss_core *core, int if_num);

struct nf_conn;

/* One direction of a bridged connection, indexed by conntrack direction:
 * the port its frames arrive on and the address they are sent to.
 */
struct nss_flow_br_dir {
	int ifnum;
	u32 mtu;
	u8 dmac[ETH_ALEN];
	u16 vid;		/* tag of the port this direction arrives on */
	u16 vtpid;		/* 0: untagged */
};

int nss_flow_br_add(struct nf_conn *ct, const struct nss_flow_br_dir *dir,
		    u32 priority);
void nss_flow_br_del(const struct nf_conn *ct);
unsigned long nss_flow_br_used(const struct nf_conn *ct);
void nss_bridge_start(struct nss_core *core);
void nss_bridge_stop(void);
void nss_bridge_print(struct seq_file *s);
void nss_flow_flush(void);
int nss_flow_configure(struct nss_core *core);
void nss_flow_withdraw_dev(const struct net_device *dev);
int nss_flow_dyn_alloc(struct nss_core *core, u32 type);
void nss_flow_dyn_free(struct nss_core *core, u32 type, int if_num);
void nss_flow_notify(const struct nss_cmn_msg *ncm, u32 len);
void nss_flow_print(struct seq_file *s);
int nss_flow_init(void);
void nss_flow_exit(void);
bool nss_redir_is_usb(const struct net_device *dev);
bool nss_redir_bound(int ifindex);
bool nss_redir_recv(struct nss_core *core, const struct n2h_descriptor *desc,
		    struct sk_buff *skb);
void nss_redir_bind(struct nss_core *core);
void nss_redir_arm(struct nss_core *core);
void nss_redir_halt(struct nss_core *core);
void nss_redir_print(struct seq_file *s);
int nss_redir_init(void);
void nss_redir_exit(void);
void nss_mc_bind(struct nss_core *core);
void nss_mc_flush(void);
void nss_mc_print(struct seq_file *s);
int nss_mc_init(void);
void nss_mc_exit(void);
int nss_log_show_ring(struct nss_core *core, struct seq_file *s);
void nss_log_dump(struct nss_core *core, const char *why);
int nss_log_shadow_init(struct nss_core *core);
void nss_log_shadow_start(struct nss_core *core);
void nss_log_shadow_stop(struct nss_core *core);

#endif /* __NSS_DRV_H */
