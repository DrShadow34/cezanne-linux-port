/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT6885 (Redmi K30 Ultra) modem interface: CCCI over CCIF.
 *
 * Native driver, written from the MediaTek 4.14 BSP (drivers/misc/mediatek/
 * eccci, GPL) as a register / protocol reference and checked against what
 * MIUI 14's kernel does (/proc/ccci_dump, strace of its modem userspace).
 * lk loads and verifies the modem image and sets up the memory layout (boot
 * slot only, see NOTES.md "Boot chooser"); this driver restarts the modem,
 * answers its handshakes and passes its channels to userspace
 * (cezanne-modemd: NVRAM file service, RPC; AT commands).
 */
#ifndef CEZANNE_CCCI_H
#define CEZANNE_CCCI_H

#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

struct wwan_port;
struct ccci_dpmaif;

/* CCIF registers (AP side) */
#define APCCIF_CON		0x00
#define APCCIF_BUSY		0x04
#define APCCIF_START		0x08
#define APCCIF_TCHNUM		0x0c
#define APCCIF_RCHNUM		0x10
#define APCCIF_ACK		0x14
#define APCCIF_CHDATA		0x100
#define CCIF_SRAM_SIZE		512

/* CCIF channels (MD_GENERATION 6297): ring queue n = channel n */
#define CCIF_QUEUES		16
#define CCIF_CH_SRAM		15	/* runtime data exchange through SRAM */
#define CCIF_CH_CCB_WAKEUP	7
#define CCIF_CH_EXP_BASE	15	/* exception channels 16.. */

/* CCCI logical channels used here (BSP enum CCCI_CH) */
#define CCCI_CONTROL_RX		0
#define CCCI_CONTROL_TX		1
#define CCCI_SYSTEM_RX		2
#define CCCI_SYSTEM_TX		3
#define CCCI_PCM_RX		4	/* speech messages */
#define CCCI_PCM_TX		5
#define CCCI_UART1_RX		6
#define CCCI_UART1_TX		8
#define CCCI_UART2_RX		10	/* AT, ttyC0 */
#define CCCI_UART2_TX		12
#define CCCI_FS_RX		14
#define CCCI_FS_TX		15
#define CCCI_RPC_RX		32
#define CCCI_RPC_TX		33
#define CCCI_IPC_RX		34
#define CCCI_IPC_TX		36
#define CCCI_IPC_UART_RX	38	/* ttyC2 (AGPS) */
#define CCCI_IPC_UART_TX	40
#define CCCI_MD_LOG_RX		42	/* ttyC1 */
#define CCCI_MD_LOG_TX		43
#define CCCI_STATUS_RX		67
#define CCCI_STATUS_TX		68
#define CCCI_MAX_CH		256	/* for sequence numbers */

/* control messages (ccci_header.data[1]) */
#define MD_INIT_START_BOOT	0x0	/* also MD_NORMAL_BOOT (HS2) */
#define MD_EX			0x4
#define MD_EX_REC_OK		0x6
#define MD_EX_PASS		0x8
#define MD_INIT_CHK_ID		0x5555ffff

struct ccci_header {
	u32 data[2];
	u16 channel;
	u16 seq_num:15;
	u16 assert_bit:1;
	u32 reserved;
} __packed;

/* shared-memory ring buffer (BSP ccci_ringbuf.c) */
#define RBF_HEADER		0xee0000ee
#define RBF_FOOTER		0xff0000ff
#define PKG_HEADER		0xaabbaabb
#define PKG_FOOTER		0xccddeeff

struct ccci_ringbuf_ctl {
	u32 read;
	u32 write;
	u32 length;
};

struct ccci_ringbuf {
	struct ccci_ringbuf_ctl rx, tx;
	u8 buffer[];
};

#define RINGBUF_CTL_LEN		(8 + sizeof(struct ccci_ringbuf) + 8)

/* runtime data (handshake 1) */
#define FEATURE_COUNT		64
#define MD_FEATURE_QUERY_PATTERN 0x49434343
#define AP_FEATURE_QUERY_PATTERN 0x43434349

struct md_query_ap_feature {
	u32 head_pattern;
	u8 feature_set[FEATURE_COUNT];	/* support_mask:4, version:4 */
	u32 tail_pattern;
	u8 reserved[152];
} __packed;

struct ap_query_md_feature_v2_1 {
	u32 head_pattern;
	u8 feature_set[FEATURE_COUNT];
	u32 share_memory_support;
	u32 ap_runtime_data_addr;
	u32 ap_runtime_data_size;
	u32 md_runtime_data_addr;
	u32 md_runtime_data_size;
	u32 noncached_mpu_start_addr;
	u32 noncached_mpu_total_size;
	u32 cached_mpu_start_addr;
	u32 cached_mpu_total_size;
	u32 reserve_addr[12];
	u32 tail_pattern;
} __packed;

/* CCIF SRAM: MD's feature query in, our answer out */
#define SRAM_DL_HEADER		0
#define SRAM_MD_RT_DATA		16
#define SRAM_UP_HEADER		(16 + sizeof(struct md_query_ap_feature))
#define SRAM_AP_RT_DATA		(SRAM_UP_HEADER + 16)

/*
 * Shared memory: the AP <-> MD1 non-cacheable bank (ap_md_nc_smem,
 * 0x8e000000, MD view 0x40000000) and the cacheable one (ap_md_c_smem,
 * 0x88000000, MD view 0x48000000). Offsets as MIUI 14's kernel lays them
 * out (BSP md1_6297_noncacheable_fat / md1_6297_cacheable, same as lk's
 * md_mpu tags).
 */
#define NC_PHYS			0x8e000000
#define C_PHYS			0x88000000
#define NC_MD_VIEW		0x40000000
#define C_MD_VIEW		0x48000000
#define NC_MDCCCI_DBG		0x00000	/* exception share memory, 64 KiB */
#define NC_RUNTIME_DATA		0x0e800	/* AP 2 KiB + MD 2 KiB */
#define NC_CCISM_SCP		0x10000
#define NC_CCB_CTRL		0x18000
#define NC_NETD			0x19000
#define NC_USB			0x1b000
#define NC_AUDIO		0x1c000	/* RAW_AUDIO: speech messages / parameters */
#define NC_AUDIO_SIZE		0xd000
#define NC_CCISM_MCU		0x29000
#define NC_CCISM_MCU_EXP	0xdd400
#define NC_CCISM_MCU_EXP_SIZE	0x1e400
#define NC_MDSS_DBG		0x00800
#define NC_MDSS_DBG_SIZE	0x3800
#define NC_WIFI_PROXY		0x100000
#define NC_DFD			0x1000000
#define NC_DFD_SIZE		0x800000
#define NC_SIZE			0x1800000
#define C_MD_CONSYS		0x000000
#define C_NVRAM_CACHE		0x180000
#define C_CCB_DHL		0x900000
#define C_RAW_DHL		0xb00000
#define C_USIP			0x4900000
#define C_SIZE			0x4960000
#define RUNTIME_AP_SIZE		0x800
#define RUNTIME_MD_SIZE		0x800

struct ccci_queue {
	struct ccci_ringbuf *rb;	/* normal ring (exception ring unused) */
	spinlock_t lock;
	unsigned int ch;		/* CCIF channel */
};

/* a channel pair passed to userspace as /dev/<name> */
struct ccci_port {
	const char *name;
	u16 rx_ch, tx_ch;
	u8 txq;
	bool user_header;		/* reads / writes carry the ccci_header */
	struct ccci_md *md;
	struct cdev cdev;
	struct device *dev;
	struct wwan_port *wwan;		/* instead of the char device */
	struct sk_buff_head rxq;
	wait_queue_head_t wq;
	atomic_t users;
	unsigned long rx_drops;
	u32 smem_off, smem_size;	/* nc smem the port can mmap */
};

enum { MD_STATE_OFF, MD_STATE_BOOTING, MD_STATE_HS1, MD_STATE_READY,
	MD_STATE_EXCEPTION };

struct ccci_md {
	struct device *dev;
	void __iomem *ccif_ap;
	void __iomem *ccif_md;
	void __iomem *infra;		/* infracfg_ao */
	void __iomem *spm;
	void __iomem *pericfg;
	void __iomem *ccif4, *ccif5;	/* MD side of CCIF4 / CCIF5 */
	int irq_ccif, irq_ccif_exp, irq_wdt;
	struct clk_bulk_data *clks;
	int num_clks;

	void *nc;			/* AP view of the non-cacheable bank */
	void *c;			/* ... and of the cacheable one */

	struct ccci_queue txq[CCIF_QUEUES], rxq[CCIF_QUEUES];
	u32 rings_size;
	struct work_struct rx_work;
	unsigned long pending;		/* CCIF channels to service */
	u16 seq[CCCI_MAX_CH];

	/* boot */
	u32 md_img_size;		/* from lk's "md1img" tag */
	u32 md_mem_base;		/* MD memory, AP view ("hdr_tbl_inf") */
	struct work_struct boot_work;
	struct mutex lock;
	int state;
	bool first_boot;
	struct completion hs1, hs2;
	unsigned long last_fs;		/* jiffies of the last FS request */
	unsigned long ex_channels;	/* exception CCIF channels seen */

	struct ccci_dpmaif *dp;		/* packet data path */

	struct ccci_port *ports;
	int num_ports;
	struct mutex ports_lock;	/* WWAN ports appear / go */
	dev_t devt;
};

/* ccif.c */
int ccci_ccif_init(struct ccci_md *md);
void ccci_ccif_reset(struct ccci_md *md);
irqreturn_t ccci_ccif_isr(int irq, void *data);
void ccci_ccif_send_ch(struct ccci_md *md, unsigned int ch);
void ccci_sram_write(struct ccci_md *md, u32 off, const void *src, size_t len);
void ccci_sram_read(struct ccci_md *md, u32 off, void *dst, size_t len);
int ccci_send(struct ccci_md *md, int txq, u16 channel, u32 data0, u32 data1,
	      u32 reserved, const void *payload, size_t len);
int ccci_send_raw(struct ccci_md *md, int txq, void *pkt, size_t len);
void ccci_rx_work(struct work_struct *work);

/* rtdata.c */
void ccci_send_runtime_data(struct ccci_md *md, const void *query);

/* port.c */
int ccci_ports_init(struct ccci_md *md);
void ccci_ports_exit(struct ccci_md *md);
void ccci_ports_online(struct ccci_md *md, bool on);
bool ccci_port_recv(struct ccci_md *md, const struct ccci_header *h,
		    const void *pkt, size_t len);

/* dpmaif.c */
int ccci_dpmaif_init(struct ccci_md *md);
void ccci_dpmaif_exit(struct ccci_md *md);
int ccci_dpmaif_start(struct ccci_md *md);
void ccci_dpmaif_stop(struct ccci_md *md);

/* core.c */
int ccci_gic_spi(u32 spi, u32 type);
int ccci_infra_clks(struct ccci_md *md, const u32 *ids, int n,
		    struct clk_bulk_data *clks);
void ccci_recv_control(struct ccci_md *md, const struct ccci_header *h,
		       const void *pkt, size_t len);

#endif
