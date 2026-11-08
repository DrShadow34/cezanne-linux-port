// SPDX-License-Identifier: GPL-2.0
/*
 * MT6885 SCP (tinysys, RISC-V) bring-up for mainline on the Redmi K30 Ultra.
 *
 * lk only stages the SCP firmware in normal boot mode ("DISABLE SCP" in
 * recovery mode, which is how Arch is booted), so this driver does lk's part
 * as well: it copies the scp1 partition image into SCP SRAM and its DRAM
 * section into the same carve-out lk uses, fills the region info, and then
 * does what the BSP scp/v02 driver does: register the tinysys mailbox IPI and
 * release the core from reset.  The firmware reports SCP_READY, after which
 * the ULPOSC calibration and a fixed OPP are sent the way scp_dvfs.c does.
 *
 * Exported API (scp.h) is the subset of the BSP one that mtk_nanohub uses.
 */
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/arm-smccc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "scp.h"
#include "scp_ipi_table.h"

#define SCP_FW_NAME		"mediatek/mt6885/scp.img"

/* scp.img: MediaTek partition image records, 0x200 header, 16-byte aligned */
#define IMG_MAGIC		0x58881688
#define IMG_HDR_SIZE		0x200
#define IMG_NAME_SCP		"tinysys-scp-RV33_A"
#define IMG_NAME_SCP_DRAM	"tinysys-scp-RV33_A_dram"

/* carve-out layout lk uses in normal boot (expdb: load_scp) */
#define RSV_LOADER_OFS		0x0
#define RSV_LOADER_SIZE		0x2000
#define RSV_FW_OFS		0x2000
#define RSV_DRAM_OFS		0x200000

#define SCP_REGION_INFO_OFFSET	0x4

/* scp_reg.h, mt6885 */
#define SCP_SCP2SPM_VOL_LV	0x0020		/* cfg */
#define SCP_TO_SPM_REG		0x0094		/* cfg */
#define R_CORE_SW_RSTN_CLR	0x0000		/* cfg_core0/1 */
#define R_CORE_SW_RSTN_SET	0x0004
#define R_CORE_WDT_IRQ		0x0030
#define  B_WDT_IRQ		BIT(0)
#define R_CORE_STATUS		0x0070
#define R_CORE_MON_PC		0x0080
#define R_CORE_MON_LR		0x0084
#define R_CORE_MON_SP		0x0088
#define DRAM_RESV_ADDR_REG	0x0044		/* cfg_core0, GENERAL_REG1 */
#define DRAM_RESV_SIZE_REG	0x0048		/* GENERAL_REG2 */
#define EXPECTED_FREQ_REG	0x004c		/* GENERAL_REG3 */
#define CURRENT_FREQ_REG	0x0050		/* GENERAL_REG4 */
#define CLK_SW_SEL		0x0000		/* clkctrl */
#define SCP_CLK_CTRL_L1_SRAM_PD	0x002c
#define SCP_CPU0_SRAM_PD	0x0080
#define SCP_CPU1_SRAM_PD	0x0084
#define SCP_CLK_CTRL_TCM_TAIL_SRAM_PD 0x0094
#define SCP_L2TCM_SRAM_PD_0	0x00c0		/* as MT8192, lk powers these */
#define SCP_L2TCM_SRAM_PD_1	0x00c4
#define SCP_L2TCM_SRAM_PD_2	0x00c8
#define INFRA_IRQ_SET		0x0b14		/* scpsys (infracfg_ao) */
#define INFRA_IRQ_CLEAR		0x0b18
#define  AP_AWAKE_LOCK		0
#define  AP_AWAKE_UNLOCK	1
#define SCP_AWAKE_TIMEOUT	100000

/* scp_dvfs.h */
#define CLK_OPP0		196
#define SLP_DBG_CMD_ULPOSC_CALI_VAL 7
#define MTK_SIP_KERNEL_SCP_DVFS_CTRL 0xc2000232
#define SCP_REQ_26M		BIT(0)
#define SCP_REQ_IFR		BIT(1)
#define SCP_REQ_SYSPLL1		BIT(2)

/* logger, scp/v03/scp_logger.c (LOGGER_CTRL protocol of this firmware) */
#define SCP_LOGGER_IPI_INIT	0x4C4F4701
#define SCP_LOGGER_IPI_ENABLE	0x4C4F4702
#define SCP_LOGGER_IPI_WAKEUP	0x4C4F4703
#define PLT_LOG_ENABLE		0x504C5402
#define SCP_LOGGER_ON		(BIT(8) | BIT(0))
#define DRAM_BUF_LEN		(1 * 1024 * 1024)

struct scp_region_info_st {
	u32 ap_loader_start;
	u32 ap_loader_size;
	u32 ap_firmware_start;
	u32 ap_firmware_size;
	u32 ap_dram_start;
	u32 ap_dram_size;
	u32 ap_dram_backup_start;
	u32 struct_size;
	u32 scp_log_thru_ap_uart;
	u32 TaskContext_ptr;
	u32 scpctl;
	u32 regdump_start;
	u32 regdump_size;
};

struct log_ctrl_s {
	u32 base;
	u32 size;
	u32 enable;
	u32 info_ofs;
	u32 buff_ofs;
	u32 buff_size;
};

struct buffer_info_s {
	u32 r_pos;
	u32 w_pos;
};

struct scp_log_info {
	u32 scp_log_dram_addr;
	u32 scp_log_buf_addr;
	u32 scp_log_start_addr;
	u32 scp_log_end_addr;
	u32 scp_log_buf_maxlen;
};

struct scp_logger_ctrl_msg {
	u32 cmd;
	union {
		struct {
			u32 addr;
			u32 size;
		} init;
		struct {
			u32 enable;
		} flag;
		struct scp_log_info info;
	} u;
};

/* ULPOSC2 calibration this unit's BSP kernel measured (MIUI dmesg) */
static const struct { u16 freq, cali; } ulposc_cali[] = {
	{ 196, 0x5d }, { 280, 0x5e }, { 360, 0x5d },
};

/* BSP scp_reservedmem_define.h sizes, packed in this order */
static const u32 scp_mblock_size[NUMS_MEM_ID] = {
	[SENS_MEM_ID] = 0x100000,
	[SCP_A_LOGGER_MEM_ID] = 0x180000,
};

static struct {
	struct device *dev;
	void __iomem *sram;
	void __iomem *cfg;
	void __iomem *clkctrl;
	void __iomem *cfg_core0;
	void __iomem *cfg_core1;
	void __iomem *scpsys;
	u32 sram_size;
	u32 tcm_size;

	phys_addr_t rsv_phys;		/* lk "SCP-reserved" equivalent */
	size_t rsv_size;
	void *rsv_virt;
	phys_addr_t shm_phys;		/* reserve-memory-scp_share */
	size_t shm_size;
	void *shm_virt;
	struct { phys_addr_t phys; void *virt; size_t size; } mblock[NUMS_MEM_ID];

	struct clk *clk_mux;
	struct clk *clk_26m;
	struct clk *clk_opp0;
	bool clk_on;

	struct scp_region_info_st region;
	struct scp_log_info log_info;
	struct log_ctrl_s *log_ctl;
	bool log_inited;

	struct work_struct ready_work;
	struct dentry *debugfs;
} scp;

static unsigned int scp_ready;
static int awake_count;
static DEFINE_SPINLOCK(awake_lock);
static BLOCKING_NOTIFIER_HEAD(scp_A_notifier_list);
static DEFINE_MUTEX(scp_A_notify_mutex);

static unsigned int msg_scp_ready0, msg_scp_ready1;
static unsigned int msg_scp_err_info0[PIN_IN_SIZE_SCP_ERROR_INFO_0];
static unsigned int msg_scp_err_info1[PIN_IN_SIZE_SCP_ERROR_INFO_1];
static unsigned int slp_ipi_ackdata0;
static struct scp_logger_ctrl_msg msg_logger_ctrl;

char *core_ids[SCP_CORE_TOTAL] = {"SCP A"};

/* -------------------------------------------------------------------------
 * awake lock (scp_awake.c): wake the SCP before touching its mailbox SRAM
 */
int scp_awake_lock(void *_scp_id)
{
	unsigned long flags;
	int count = 0, ret = -1;
	u32 tmp;

	if (!scp_ready)
		return -1;

	spin_lock_irqsave(&awake_lock, flags);
	if (awake_count > 0) {
		awake_count++;
		spin_unlock_irqrestore(&awake_lock, flags);
		return 0;
	}

	writel(0xA0 | (1 << AP_AWAKE_LOCK), scp.scpsys + INFRA_IRQ_SET);
	while (++count != SCP_AWAKE_TIMEOUT) {
		tmp = readl(scp.scpsys + INFRA_IRQ_SET);
		if ((tmp & 0xf0) != 0xA0) {
			dev_notice(scp.dev, "awake: INFRA_IRQ_SET %x\n", tmp);
			break;
		}
		if (!((tmp & 0x0f) & (1 << AP_AWAKE_LOCK))) {
			ret = 0;
			break;
		}
		udelay(10);
	}
	writel(0xA0 | (1 << AP_AWAKE_LOCK), scp.scpsys + INFRA_IRQ_CLEAR);

	if (ret == 0)
		awake_count++;
	else
		dev_notice_ratelimited(scp.dev, "awake lock timeout\n");
	spin_unlock_irqrestore(&awake_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(scp_awake_lock);

int scp_awake_unlock(void *_scp_id)
{
	unsigned long flags;
	int count = 0, ret = -1;
	u32 tmp;

	if (!scp_ready)
		return -1;

	spin_lock_irqsave(&awake_lock, flags);
	if (awake_count > 1) {
		awake_count--;
		spin_unlock_irqrestore(&awake_lock, flags);
		return 0;
	}

	writel(0xA0 | (1 << AP_AWAKE_UNLOCK), scp.scpsys + INFRA_IRQ_SET);
	while (++count != SCP_AWAKE_TIMEOUT) {
		tmp = readl(scp.scpsys + INFRA_IRQ_SET);
		if ((tmp & 0xf0) != 0xA0)
			break;
		if (!((tmp & 0x0f) & (1 << AP_AWAKE_UNLOCK))) {
			ret = 0;
			break;
		}
		udelay(10);
	}
	writel(0xA0 | (1 << AP_AWAKE_UNLOCK), scp.scpsys + INFRA_IRQ_CLEAR);

	if (awake_count > 0)
		awake_count--;
	spin_unlock_irqrestore(&awake_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(scp_awake_unlock);

int scp_clr_spm_reg(void *unused)
{
	writel(0x1, scp.cfg + SCP_TO_SPM_REG);
	return 0;
}

/* -------------------------------------------------------------------------
 * legacy IPI wrapper (scp_wrapper_ipi.c): IPI_CHRE/CHREX/SENSOR multiplexed
 * over the MPOOL pins with an {id, len} header
 */
#define IPI_NO_USE 0xFF

static char msg_legacy_ipi_mpool_0[PIN_IN_SIZE_SCP_MPOOL * MBOX_SLOT_SIZE];
static char msg_legacy_ipi_mpool_1[PIN_IN_SIZE_SCP_MPOOL * MBOX_SLOT_SIZE];

static struct scp_ipi_desc mpool[SCP_NR_IPI - IPI_MPOOL - 1];
static struct scp_ipi_wrapper scp_ipi_legacy_id[] = {
	{
		.out_id_0 = IPI_OUT_SCP_MPOOL_0,
		.out_id_1 = IPI_OUT_SCP_MPOOL_1,
		.in_id_0 = IPI_IN_SCP_MPOOL_0,
		.in_id_1 = IPI_IN_SCP_MPOOL_1,
		.out_size = PIN_OUT_SIZE_SCP_MPOOL,
		.in_size = PIN_IN_SIZE_SCP_MPOOL,
		.msg_0 = msg_legacy_ipi_mpool_0,
		.msg_1 = msg_legacy_ipi_mpool_1,
	},
};

/*
 * The firmware announces itself once at boot (SCP_INIT_DONE on IPI_SENSOR,
 * CHRE hello on IPI_CHRE), usually before mtk_nanohub is loaded.  Keep
 * messages for ids nobody has registered yet and replay them on
 * registration.
 */
#define PENDING_MAX	16
struct pending_msg {
	u32 id;
	u32 len;
	u8 data[(PIN_IN_SIZE_SCP_MPOOL - 2) * MBOX_SLOT_SIZE];
};
static struct pending_msg pending[PENDING_MAX];
static unsigned int pending_cnt;
static DEFINE_SPINLOCK(pending_lock);

static int scp_legacy_handler(unsigned int id, void *prdata, void *data,
			      unsigned int len)
{
	void (*handler)(int id, void *data, unsigned int len) = NULL;
	u32 pkt_id = *(u32 *)data;
	u32 pkt_len = *(u32 *)(data + 4);
	unsigned long flags;

	if (pkt_len > sizeof(pending[0].data))
		pkt_len = sizeof(pending[0].data);
	dev_dbg_ratelimited(scp.dev, "mpool rx: ipi %u id %u len %u\n", id,
			    pkt_id, pkt_len);
	if (pkt_id > IPI_MPOOL && pkt_id < SCP_NR_IPI)
		handler = mpool[pkt_id - IPI_MPOOL - 1].handler;
	if (handler) {
		handler(pkt_id, data + 8, pkt_len);
		return 0;
	}

	spin_lock_irqsave(&pending_lock, flags);
	if (pending_cnt < PENDING_MAX) {
		pending[pending_cnt].id = pkt_id;
		pending[pending_cnt].len = pkt_len;
		memcpy(pending[pending_cnt].data, data + 8, pkt_len);
		pending_cnt++;
	}
	spin_unlock_irqrestore(&pending_lock, flags);
	return 0;
}

static void scp_replay_pending(enum ipi_id id,
	void (*handler)(int id, void *data, unsigned int len))
{
	struct pending_msg msg;
	unsigned long flags;
	unsigned int i;

	for (;;) {
		spin_lock_irqsave(&pending_lock, flags);
		for (i = 0; i < pending_cnt && pending[i].id != id; i++)
			;
		if (i == pending_cnt) {
			spin_unlock_irqrestore(&pending_lock, flags);
			return;
		}
		msg = pending[i];
		memmove(&pending[i], &pending[i + 1],
			(pending_cnt - i - 1) * sizeof(pending[0]));
		pending_cnt--;
		spin_unlock_irqrestore(&pending_lock, flags);

		dev_info(scp.dev, "replaying early ipi %u message (%u bytes)\n",
			 id, msg.len);
		handler(id, msg.data, msg.len);
	}
}

enum scp_ipi_status scp_ipi_registration(enum ipi_id id,
	void (*ipi_handler)(int id, void *data, unsigned int len),
	const char *name)
{
	if (id >= SCP_NR_IPI || id == IPI_MPOOL || !ipi_handler)
		return SCP_IPI_ERROR;

	mpool[id - IPI_MPOOL - 1].handler = ipi_handler;
	scp_replay_pending(id, ipi_handler);
	return SCP_IPI_DONE;
}
EXPORT_SYMBOL_GPL(scp_ipi_registration);

enum scp_ipi_status scp_ipi_unregistration(enum ipi_id id)
{
	if (id >= SCP_NR_IPI || id == IPI_MPOOL)
		return SCP_IPI_ERROR;

	mpool[id - IPI_MPOOL - 1].handler = NULL;
	return SCP_IPI_DONE;
}
EXPORT_SYMBOL_GPL(scp_ipi_unregistration);

enum scp_ipi_status scp_ipi_send(enum ipi_id id, void *buf,
	unsigned int len, unsigned int wait, enum scp_core_id scp_id)
{
	/* a full mbox pin worth of data */
	u32 pkt[PIN_OUT_SIZE_SCP_MPOOL];
	struct scp_ipi_wrapper *w = &scp_ipi_legacy_id[0];
	int ret;

	if (id >= SCP_NR_IPI || id == IPI_MPOOL)
		return SCP_IPI_ERROR;
	if (!is_scp_ready(scp_id))
		return SCP_IPI_NOT_READY;
	if (len > (w->out_size - 2) * MBOX_SLOT_SIZE)
		return SCP_IPI_ERROR;

	pkt[0] = id;
	pkt[1] = len;
	memcpy(&pkt[2], buf, len);

	ret = mtk_ipi_send(&scp_ipidev,
			   scp_id == SCP_CORE0_ID ? w->out_id_0 : w->out_id_1,
			   0, pkt, w->out_size, wait * SCP_IPI_LEGACY_WAIT);
	if (ret == IPI_ACTION_DONE)
		return SCP_IPI_DONE;
	if (ret == IPI_PIN_BUSY)
		return SCP_IPI_BUSY;
	return SCP_IPI_ERROR;
}
EXPORT_SYMBOL_GPL(scp_ipi_send);

/* mbox_setup_pin_table() from scp_wrapper_ipi.c */
static void mbox_setup_pin_table(unsigned int mbox)
{
	int i, last_ofs = 0, last_idx = 0, last_slot = 0, last_sz = 0;

	for (i = 0; i < SCP_TOTAL_SEND_PIN; i++) {
		if (mbox == scp_mbox_pin_send[i].mbox) {
			scp_mbox_pin_send[i].offset = last_ofs + last_slot;
			scp_mbox_pin_send[i].pin_index = last_idx + last_sz;
			last_idx = scp_mbox_pin_send[i].pin_index;
			if (scp_mbox_info[mbox].is64d == 1) {
				last_sz = DIV_ROUND_UP(
					   scp_mbox_pin_send[i].msg_size, 2);
				last_ofs = last_sz * 2;
				last_slot = last_idx * 2;
			} else {
				last_sz = scp_mbox_pin_send[i].msg_size;
				last_ofs = last_sz;
				last_slot = last_idx;
			}
		} else if (mbox < scp_mbox_pin_send[i].mbox)
			break;
	}

	for (i = 0; i < SCP_TOTAL_RECV_PIN; i++) {
		if (mbox == scp_mbox_pin_recv[i].mbox) {
			scp_mbox_pin_recv[i].offset = last_ofs + last_slot;
			scp_mbox_pin_recv[i].pin_index = last_idx + last_sz;
			last_idx = scp_mbox_pin_recv[i].pin_index;
			if (scp_mbox_info[mbox].is64d == 1) {
				last_sz = DIV_ROUND_UP(
					   scp_mbox_pin_recv[i].msg_size, 2);
				last_ofs = last_sz * 2;
				last_slot = last_idx * 2;
			} else {
				last_sz = scp_mbox_pin_recv[i].msg_size;
				last_ofs = last_sz;
				last_slot = last_idx;
			}
		} else if (mbox < scp_mbox_pin_recv[i].mbox)
			break;
	}

	if (last_idx > 32 ||
	   (last_ofs + last_slot) > (scp_mbox_info[mbox].is64d + 1) * 32)
		dev_err(scp.dev, "mbox%d ofs(%d)/slot(%d) exceed the maximum\n",
			mbox, last_idx, last_ofs + last_slot);
}

/* -------------------------------------------------------------------------
 * misc API
 */
unsigned int is_scp_ready(enum scp_core_id scp_id)
{
	return scp_id == SCP_A_ID && scp_ready;
}
EXPORT_SYMBOL_GPL(is_scp_ready);

void scp_A_register_notify(struct notifier_block *nb)
{
	mutex_lock(&scp_A_notify_mutex);
	blocking_notifier_chain_register(&scp_A_notifier_list, nb);
	if (scp_ready)
		nb->notifier_call(nb, SCP_EVENT_READY, NULL);
	mutex_unlock(&scp_A_notify_mutex);
}
EXPORT_SYMBOL_GPL(scp_A_register_notify);

void scp_A_unregister_notify(struct notifier_block *nb)
{
	mutex_lock(&scp_A_notify_mutex);
	blocking_notifier_chain_unregister(&scp_A_notifier_list, nb);
	mutex_unlock(&scp_A_notify_mutex);
}
EXPORT_SYMBOL_GPL(scp_A_unregister_notify);

phys_addr_t scp_get_reserve_mem_phys(enum scp_reserve_mem_id_t id)
{
	return id < NUMS_MEM_ID ? scp.mblock[id].phys : 0;
}
EXPORT_SYMBOL_GPL(scp_get_reserve_mem_phys);

phys_addr_t scp_get_reserve_mem_virt(enum scp_reserve_mem_id_t id)
{
	return id < NUMS_MEM_ID ? (phys_addr_t)(uintptr_t)scp.mblock[id].virt : 0;
}
EXPORT_SYMBOL_GPL(scp_get_reserve_mem_virt);

phys_addr_t scp_get_reserve_mem_size(enum scp_reserve_mem_id_t id)
{
	return id < NUMS_MEM_ID ? scp.mblock[id].size : 0;
}
EXPORT_SYMBOL_GPL(scp_get_reserve_mem_size);

/*
 * The SCP runs at a fixed OPP0 (196 MHz on ULPOSC2): sensors need ~30 MHz,
 * so the BSP feature/frequency bookkeeping collapses to "on".
 */
void scp_register_feature(enum feature_id id)
{
}
EXPORT_SYMBOL_GPL(scp_register_feature);

void scp_deregister_feature(enum feature_id id)
{
}
EXPORT_SYMBOL_GPL(scp_deregister_feature);

void scp_register_sensor(enum feature_id id, enum scp_sensor_id sensor_id)
{
}
EXPORT_SYMBOL_GPL(scp_register_sensor);

void scp_deregister_sensor(enum feature_id id, enum scp_sensor_id sensor_id)
{
}
EXPORT_SYMBOL_GPL(scp_deregister_sensor);

void scp_wdt_reset(int cpu_id)
{
	dev_warn(scp.dev, "wdt reset requested for core %d (ignored)\n",
		 cpu_id);
}
EXPORT_SYMBOL_GPL(scp_wdt_reset);

/* -------------------------------------------------------------------------
 * DVFS-lite (scp_dvfs.c)
 */
static unsigned long scp_resource_req(unsigned int req)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_KERNEL_SCP_DVFS_CTRL, req, 0, 0, 0, 0, 0, 0,
		      &res);
	return res.a0;
}

static void scp_sync_ulposc_cali(void)
{
	u32 ipi_data[2];
	u16 *p = (u16 *)&ipi_data[1];
	int i, ret;

	ret = mtk_ipi_register(&scp_ipidev, IPI_OUT_C_SLEEP_0, NULL, NULL,
			       &slp_ipi_ackdata0);
	if (ret && ret != IPI_DUPLEX)
		dev_err(scp.dev, "sleep ipi register fail %d\n", ret);

	ipi_data[0] = SLP_DBG_CMD_ULPOSC_CALI_VAL;
	for (i = 0; i < ARRAY_SIZE(ulposc_cali); i++) {
		p[0] = ulposc_cali[i].freq;
		p[1] = ulposc_cali[i].cali;
		ret = mtk_ipi_send_compl(&scp_ipidev, IPI_OUT_C_SLEEP_0,
					 IPI_SEND_WAIT, ipi_data,
					 PIN_OUT_C_SIZE_SLEEP_0, 500);
		if (ret != IPI_ACTION_DONE)
			dev_err(scp.dev, "ulposc cali %d send fail %d\n",
				ulposc_cali[i].freq, ret);
	}

	dev_info(scp.dev, "ulposc calibration sent, CLK_SW_SEL=%#x\n",
		 readl(scp.clkctrl + CLK_SW_SEL));
}

static void scp_request_opp0(void)
{
	u32 value = CLK_OPP0, cur;
	int timeout = 50, ret;

	writel(CLK_OPP0, scp.cfg_core0 + EXPECTED_FREQ_REG);
	cur = readl(scp.cfg_core0 + CURRENT_FREQ_REG);
	if (cur == CLK_OPP0)
		return;

	/* scp_vcore_request(CLK_OPP0): 0.575 V is enough, VCORE is higher */
	writel(0x0008, scp.cfg + SCP_SCP2SPM_VOL_LV);
	scp_resource_req(SCP_REQ_26M | SCP_REQ_IFR | SCP_REQ_SYSPLL1);
	if (scp.clk_opp0)
		clk_set_parent(scp.clk_mux, scp.clk_opp0);

	scp_awake_lock(NULL);
	do {
		ret = mtk_ipi_send(&scp_ipidev, IPI_OUT_DVFS_SET_FREQ_0,
				   IPI_SEND_WAIT, &value,
				   PIN_OUT_SIZE_DVFS_SET_FREQ_0, 0);
		mdelay(2);
		cur = readl(scp.cfg_core0 + CURRENT_FREQ_REG);
	} while (cur != CLK_OPP0 && --timeout > 0);
	scp_awake_unlock(NULL);

	scp_resource_req(0);
	dev_info(scp.dev, "freq request %d MHz: current %u (ipi %d)\n",
		 CLK_OPP0, cur, ret);
}

/* -------------------------------------------------------------------------
 * logger (scp_logger.c)
 */
static int scp_logger_ctrl_handler(unsigned int id, void *prdata, void *data,
				   unsigned int len)
{
	struct scp_logger_ctrl_msg *msg = data;
	struct scp_log_info *info = &msg->u.info;

	if (msg->cmd != SCP_LOGGER_IPI_INIT)
		return 0;	/* WAKEUP: buffer filling up, nothing to do */

	scp.log_info = *info;
	if (info->scp_log_dram_addr + 4 <= scp.sram_size)
		writel(scp.mblock[SCP_A_LOGGER_MEM_ID].phys,
		       scp.sram + info->scp_log_dram_addr);
	scp.log_inited = true;
	return 0;
}

static void scp_logger_dram_init(void)
{
	struct log_ctrl_s *ctl = scp.mblock[SCP_A_LOGGER_MEM_ID].virt;
	struct buffer_info_s *bi;

	ctl->base = PLT_LOG_ENABLE;
	ctl->enable = 0;
	ctl->size = sizeof(*ctl);
	ctl->info_ofs = ctl->size;
	ctl->buff_ofs = ROUNDUP(ctl->info_ofs + sizeof(*bi), 4);
	ctl->buff_size = DRAM_BUF_LEN;
	bi = (void *)ctl + ctl->info_ofs;
	bi->r_pos = 0;
	bi->w_pos = 0;
	scp.log_ctl = ctl;
}

static void scp_logger_start(void)
{
	struct scp_logger_ctrl_msg msg = {
		.cmd = SCP_LOGGER_IPI_INIT,
		.u.init.addr = scp.mblock[SCP_A_LOGGER_MEM_ID].phys,
		.u.init.size = scp.mblock[SCP_A_LOGGER_MEM_ID].size,
	};
	int ret, retry = 50;

	do {
		ret = mtk_ipi_send(&scp_ipidev, IPI_OUT_LOGGER_CTRL, 0, &msg,
				   PIN_OUT_SIZE_LOGGER_CTRL, 0);
		if (ret == IPI_ACTION_DONE)
			break;
		msleep(2);
	} while (--retry);
	scp.log_ctl->enable = 1;

	msg.cmd = SCP_LOGGER_IPI_ENABLE;
	msg.u.flag.enable = SCP_LOGGER_ON;
	ret = mtk_ipi_send(&scp_ipidev, IPI_OUT_LOGGER_CTRL, 0, &msg,
			   PIN_OUT_SIZE_LOGGER_CTRL, 0);
	if (ret != IPI_ACTION_DONE)
		dev_warn(scp.dev, "logger enable ipi fail %d\n", ret);
}

/* debugfs "log": drain the DRAM log ring (scp_A_log_read) */
static ssize_t scp_log_read(struct file *f, char __user *ubuf, size_t len,
			    loff_t *ppos)
{
	struct log_ctrl_s *ctl = scp.log_ctl;
	struct buffer_info_s *bi;
	char *buf;
	u32 r, w, n;
	ssize_t ret;

	if (!ctl || !ctl->enable)
		return 0;
	bi = (void *)ctl + ctl->info_ofs;
	buf = (char *)ctl + ctl->buff_ofs;
	r = READ_ONCE(bi->r_pos);
	w = READ_ONCE(bi->w_pos);
	if (r >= ctl->buff_size || w >= ctl->buff_size || r == w)
		return 0;
	n = w > r ? w - r : ctl->buff_size - r;
	n = min_t(u32, n, len);
	if (copy_to_user(ubuf, buf + r, n))
		return -EFAULT;
	r += n;
	if (r >= ctl->buff_size)
		r = 0;
	WRITE_ONCE(bi->r_pos, r);
	ret = n;
	return ret;
}

static const struct file_operations scp_log_fops = {
	.read = scp_log_read,
	.llseek = noop_llseek,
};

/* debugfs "last_log": the SRAM ring (scp_A_get_last_log) */
static int scp_last_log_show(struct seq_file *m, void *v)
{
	struct scp_log_info *li = &scp.log_info;
	u32 end, i, max;

	if (!scp.log_inited || li->scp_log_end_addr + 4 > scp.sram_size ||
	    li->scp_log_buf_addr + li->scp_log_buf_maxlen > scp.sram_size)
		return 0;
	max = li->scp_log_buf_maxlen;
	end = readl(scp.sram + li->scp_log_end_addr);
	if (end >= max)
		end = 0;
	for (i = (end + 1) % max; i != end; i = (i + 1) % max) {
		char c = readb(scp.sram + li->scp_log_buf_addr + i);

		if (c)
			seq_putc(m, c);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(scp_last_log);

static int scp_status_show(struct seq_file *m, void *v)
{
	seq_printf(m, "ready: %u\n", scp_ready);
	seq_printf(m, "core0 status %#x pc %#x lr %#x sp %#x\n",
		   readl(scp.cfg_core0 + R_CORE_STATUS),
		   readl(scp.cfg_core0 + R_CORE_MON_PC),
		   readl(scp.cfg_core0 + R_CORE_MON_LR),
		   readl(scp.cfg_core0 + R_CORE_MON_SP));
	seq_printf(m, "core1 status %#x pc %#x lr %#x sp %#x\n",
		   readl(scp.cfg_core1 + R_CORE_STATUS),
		   readl(scp.cfg_core1 + R_CORE_MON_PC),
		   readl(scp.cfg_core1 + R_CORE_MON_LR),
		   readl(scp.cfg_core1 + R_CORE_MON_SP));
	seq_printf(m, "freq expected %u current %u, CLK_SW_SEL %#x\n",
		   readl(scp.cfg_core0 + EXPECTED_FREQ_REG),
		   readl(scp.cfg_core0 + CURRENT_FREQ_REG),
		   readl(scp.clkctrl + CLK_SW_SEL));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(scp_status);

/* -------------------------------------------------------------------------
 * ready / error / watchdog
 */
static void scp_ready_ws(struct work_struct *ws)
{
	/* patch from the BSP: clear the SPM interrupt */
	writel(0xff, scp.cfg + SCP_TO_SPM_REG);

	/*
	 * The BSP drops its scp_sel reference here; other users keep that
	 * clock up there.  Here it is the only one, and gating it makes the
	 * next mailbox access an SError.  Keep it on.
	 */
	dev_info(scp.dev, "ready: ulposc calibration\n");
	scp_sync_ulposc_cali();
	dev_info(scp.dev, "ready: frequency\n");
	scp_request_opp0();
	dev_info(scp.dev, "ready: logger\n");
	scp_logger_start();

	dev_info(scp.dev, "ready: notifying clients\n");
	mutex_lock(&scp_A_notify_mutex);
	blocking_notifier_call_chain(&scp_A_notifier_list, SCP_EVENT_READY,
				     NULL);
	mutex_unlock(&scp_A_notify_mutex);
}

static int scp_A_ready_ipi_handler(unsigned int id, void *prdata, void *data,
				   unsigned int len)
{
	u32 image_size = *(u32 *)data;

	if (scp_ready)
		return 0;
	scp_ready = 1;
	if (image_size != scp.tcm_size)
		dev_err(scp.dev, "image size mismatch AP=%#x SCP=%#x\n",
			scp.tcm_size, image_size);
	dev_info(scp.dev, "SCP ready\n");
	schedule_work(&scp.ready_work);
	return 0;
}

static int scp_err_info_handler(unsigned int id, void *prdata, void *data,
				unsigned int len)
{
	dev_err(scp.dev, "SCP error info (ipi %u): %*phN\n", id,
		min_t(unsigned int, len, 40), data);
	return 0;
}

static irqreturn_t scp_A_irq_handler(int irq, void *dev_id)
{
	u32 reg0 = readl(scp.cfg_core0 + R_CORE_WDT_IRQ);
	u32 reg1 = readl(scp.cfg_core1 + R_CORE_WDT_IRQ);

	if (reg0 | reg1) {
		dev_err(scp.dev, "SCP watchdog: core0 pc %#x lr %#x, core1 pc %#x lr %#x\n",
			readl(scp.cfg_core0 + R_CORE_MON_PC),
			readl(scp.cfg_core0 + R_CORE_MON_LR),
			readl(scp.cfg_core1 + R_CORE_MON_PC),
			readl(scp.cfg_core1 + R_CORE_MON_LR));
		if (reg0)
			writel(B_WDT_IRQ, scp.cfg_core0 + R_CORE_WDT_IRQ);
		if (reg1)
			writel(B_WDT_IRQ, scp.cfg_core1 + R_CORE_WDT_IRQ);
	}
	return IRQ_HANDLED;
}

/* -------------------------------------------------------------------------
 * firmware staging (what lk's load_scp() does in normal boot)
 */
static const void *img_find(const struct firmware *fw, const char *name,
			    u32 *size)
{
	size_t off = 0;

	while (off + IMG_HDR_SIZE <= fw->size) {
		const u32 *hdr = (const u32 *)(fw->data + off);
		u32 sz = hdr[1];

		if (hdr[0] != IMG_MAGIC || off + IMG_HDR_SIZE + sz > fw->size)
			break;
		if (!strncmp((const char *)&hdr[2], name, 32)) {
			*size = sz;
			return fw->data + off + IMG_HDR_SIZE;
		}
		off = ALIGN(off + IMG_HDR_SIZE + sz, 16);
	}
	return NULL;
}

/* mtk_scp.c scp_sram_power_on(): one block at a time */
static void scp_sram_power_on(void __iomem *addr)
{
	int i;

	for (i = 31; i >= 0; i--)
		writel(GENMASK(i, 0), addr);
	writel(0, addr);
}

static void scp_enable_sram(void)
{
	u32 reg;

	/* the TCM itself: powered down in recovery boots, lk skips the SCP */
	scp_sram_power_on(scp.clkctrl + SCP_L2TCM_SRAM_PD_0);
	scp_sram_power_on(scp.clkctrl + SCP_L2TCM_SRAM_PD_1);
	scp_sram_power_on(scp.clkctrl + SCP_L2TCM_SRAM_PD_2);

	/* enable one block at a time */
	for (reg = 0xffffffff; reg != 0;) {
		reg >>= 1;
		writel(reg, scp.clkctrl + SCP_CPU0_SRAM_PD);
		writel(reg, scp.clkctrl + SCP_CPU1_SRAM_PD);
	}
	writel(0, scp.clkctrl + SCP_CLK_CTRL_L1_SRAM_PD);
	writel(0, scp.clkctrl + SCP_CLK_CTRL_TCM_TAIL_SRAM_PD);
}

static int scp_stage_firmware(void)
{
	struct scp_region_info_st *ri = &scp.region;
	const struct firmware *fw;
	const void *tcm, *dram;
	u32 tcm_sz, dram_sz, dram_round;
	int ret;

	ret = request_firmware(&fw, SCP_FW_NAME, scp.dev);
	if (ret) {
		dev_err(scp.dev, "%s: %d\n", SCP_FW_NAME, ret);
		return ret;
	}

	tcm = img_find(fw, IMG_NAME_SCP, &tcm_sz);
	dram = img_find(fw, IMG_NAME_SCP_DRAM, &dram_sz);
	dram_round = ROUNDUP(dram_sz, 1024);
	if (!tcm || !dram || tcm_sz > scp.tcm_size ||
	    tcm_sz <= RSV_LOADER_SIZE || RSV_FW_OFS + tcm_sz > RSV_DRAM_OFS ||
	    RSV_DRAM_OFS + dram_round * 4 > scp.rsv_size) {
		dev_err(scp.dev, "bad image (tcm %p/%#x dram %p/%#x)\n",
			tcm, tcm_sz, dram, dram_sz);
		ret = -EINVAL;
		goto out;
	}

	/*
	 * DRAM copies lk keeps.  The image's first 8 KiB is a loader that, on
	 * every core reset, copies ap_firmware_size bytes from
	 * ap_firmware_start to TCM + ap_loader_size: the firmware copy is the
	 * image from offset 0x2000 (ap_firmware_size stays the full image size,
	 * as lk sets it; the tail reads zeros).
	 */
	memset(scp.rsv_virt, 0, scp.rsv_size);
	memcpy(scp.rsv_virt + RSV_LOADER_OFS, tcm, RSV_LOADER_SIZE);
	memcpy(scp.rsv_virt + RSV_FW_OFS, tcm + RSV_LOADER_SIZE,
	       tcm_sz - RSV_LOADER_SIZE);
	memcpy(scp.rsv_virt + RSV_DRAM_OFS, dram, dram_sz);
	memcpy(scp.rsv_virt + RSV_DRAM_OFS + dram_round * 2, dram, dram_sz);

	/* SRAM: the image runs in place, region info at offset 4 */
	memset_io(scp.sram, 0, scp.sram_size);
	memcpy_toio(scp.sram, tcm, tcm_sz);

	memcpy_fromio(ri, scp.sram + SCP_REGION_INFO_OFFSET, sizeof(*ri));
	ri->ap_loader_start = scp.rsv_phys + RSV_LOADER_OFS;
	ri->ap_loader_size = RSV_LOADER_SIZE;
	ri->ap_firmware_start = scp.rsv_phys + RSV_FW_OFS;
	ri->ap_firmware_size = tcm_sz;
	ri->ap_dram_start = scp.rsv_phys + RSV_DRAM_OFS;
	ri->ap_dram_size = dram_sz;
	ri->ap_dram_backup_start = ri->ap_dram_start + dram_round * 2;
	ri->scp_log_thru_ap_uart = 0;
	ri->scpctl = 0;
	memcpy_toio(scp.sram + SCP_REGION_INFO_OFFSET, ri,
		    offsetof(struct scp_region_info_st, TaskContext_ptr));
	writel(ri->scp_log_thru_ap_uart, scp.sram + SCP_REGION_INFO_OFFSET +
	       offsetof(struct scp_region_info_st, scp_log_thru_ap_uart));
	writel(ri->scpctl, scp.sram + SCP_REGION_INFO_OFFSET +
	       offsetof(struct scp_region_info_st, scpctl));
	wmb();

	dev_info(scp.dev, "staged %#x + %#x bytes, task ctx %#x, struct %#x\n",
		 tcm_sz, dram_sz, ri->TaskContext_ptr, ri->struct_size);
out:
	release_firmware(fw);
	return ret;
}

/* -------------------------------------------------------------------------
 * probe
 */
static int scp_map_rsv(struct device_node *np, int idx, phys_addr_t *phys,
		       size_t *size, void **virt)
{
	struct device_node *rn = of_parse_phandle(np, "memory-region", idx);
	struct reserved_mem *rmem;

	if (!rn)
		return -ENODEV;
	rmem = of_reserved_mem_lookup(rn);
	of_node_put(rn);
	if (!rmem)
		return -ENODEV;
	*phys = rmem->base;
	*size = rmem->size;
	*virt = devm_memremap(scp.dev, rmem->base, rmem->size, MEMREMAP_WC);
	return IS_ERR(*virt) ? PTR_ERR(*virt) : 0;
}

static int cezanne_scp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource *res;
	phys_addr_t ofs;
	int i, ret, irq;

	scp.dev = dev;
	INIT_WORK(&scp.ready_work, scp_ready_ws);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "scp_sram_base");
	scp.sram = devm_ioremap_resource(dev, res);
	if (IS_ERR(scp.sram))
		return PTR_ERR(scp.sram);
	scp.sram_size = resource_size(res);
	scp.cfg = devm_platform_ioremap_resource_byname(pdev, "scp_cfgreg");
	scp.clkctrl = devm_platform_ioremap_resource_byname(pdev, "scp_clkreg");
	scp.cfg_core0 = devm_platform_ioremap_resource_byname(pdev, "scp_cfgreg_core0");
	scp.cfg_core1 = devm_platform_ioremap_resource_byname(pdev, "scp_cfgreg_core1");
	if (IS_ERR(scp.cfg) || IS_ERR(scp.clkctrl) ||
	    IS_ERR(scp.cfg_core0) || IS_ERR(scp.cfg_core1))
		return -ENOMEM;
	/* infracfg_ao, owned by the clock driver too: map without claiming */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "scpsys");
	scp.scpsys = res ? devm_ioremap(dev, res->start, resource_size(res)) : NULL;
	if (!scp.scpsys)
		return -ENOMEM;

	if (of_property_read_u32(np, "scp_sramSize", &scp.tcm_size))
		return -EINVAL;

	ret = scp_map_rsv(np, 0, &scp.rsv_phys, &scp.rsv_size, &scp.rsv_virt);
	if (ret)
		return dev_err_probe(dev, ret, "image carve-out\n");
	ret = scp_map_rsv(np, 1, &scp.shm_phys, &scp.shm_size, &scp.shm_virt);
	if (ret)
		return dev_err_probe(dev, ret, "shared memory\n");
	for (i = 0, ofs = 0; i < NUMS_MEM_ID; i++) {
		scp.mblock[i].phys = scp.shm_phys + ofs;
		scp.mblock[i].virt = scp.shm_virt + ofs;
		scp.mblock[i].size = scp_mblock_size[i];
		ofs += scp_mblock_size[i];
	}
	if (ofs > scp.shm_size)
		return dev_err_probe(dev, -EINVAL, "shared memory too small\n");
	memset(scp.shm_virt, 0, scp.shm_size);

	scp.clk_mux = devm_clk_get(dev, "main");
	if (IS_ERR(scp.clk_mux))
		return dev_err_probe(dev, PTR_ERR(scp.clk_mux), "main clock\n");
	scp.clk_26m = devm_clk_get_optional(dev, "clk26m");
	scp.clk_opp0 = devm_clk_get_optional(dev, "opp0");

	/* scp_init(): park the SCP on 26 MHz and keep 26M/infra up meanwhile */
	ret = clk_prepare_enable(scp.clk_mux);
	if (ret)
		return ret;
	scp.clk_on = true;
	if (scp.clk_26m)
		clk_set_parent(scp.clk_mux, scp.clk_26m);
	scp_resource_req(SCP_REQ_26M);

	/* hold both cores in reset while the SRAM is (re)loaded */
	writel(1, scp.cfg_core0 + R_CORE_SW_RSTN_SET);
	writel(1, scp.cfg_core1 + R_CORE_SW_RSTN_SET);
	udelay(10);
	scp_enable_sram();

	ret = scp_stage_firmware();
	if (ret)
		goto err_clk;

	irq = platform_get_irq_byname(pdev, "ipc0");
	if (irq > 0 && devm_request_irq(dev, irq, scp_A_irq_handler, 0,
					"scp-ipc0", NULL))
		dev_warn(dev, "ipc0 irq unavailable\n");
	irq = platform_get_irq_byname(pdev, "ipc1");
	if (irq > 0 && devm_request_irq(dev, irq, scp_A_irq_handler, 0,
					"scp-ipc1", NULL))
		dev_warn(dev, "ipc1 irq unavailable\n");

	for (i = 0; i < SCP_MBOX_TOTAL; i++) {
		scp_mbox_info[i].mbdev = &scp_mboxdev;
		if (mtk_mbox_probe(pdev, &scp_mboxdev, i) != MBOX_DONE) {
			ret = -ENODEV;
			goto err_clk;
		}
		mbox_setup_pin_table(i);
	}
	ret = mtk_ipi_device_register(&scp_ipidev, pdev, &scp_mboxdev,
				      SCP_IPI_COUNT);
	if (ret)
		goto err_clk;

	mtk_ipi_register(&scp_ipidev, IPI_IN_SCP_MPOOL_0, scp_legacy_handler,
			 NULL, msg_legacy_ipi_mpool_0);
	mtk_ipi_register(&scp_ipidev, IPI_IN_SCP_MPOOL_1, scp_legacy_handler,
			 NULL, msg_legacy_ipi_mpool_1);
	mtk_ipi_register(&scp_ipidev, IPI_IN_SCP_READY_0,
			 scp_A_ready_ipi_handler, NULL, &msg_scp_ready0);
	mtk_ipi_register(&scp_ipidev, IPI_IN_SCP_READY_1,
			 scp_A_ready_ipi_handler, NULL, &msg_scp_ready1);
	mtk_ipi_register(&scp_ipidev, IPI_IN_SCP_ERROR_INFO_0,
			 scp_err_info_handler, NULL, msg_scp_err_info0);
	mtk_ipi_register(&scp_ipidev, IPI_IN_SCP_ERROR_INFO_1,
			 scp_err_info_handler, NULL, msg_scp_err_info1);
	mtk_ipi_register(&scp_ipidev, IPI_IN_LOGGER_CTRL,
			 scp_logger_ctrl_handler, NULL, &msg_logger_ctrl);
	scp_logger_dram_init();

	scp.debugfs = debugfs_create_dir("scp", NULL);
	debugfs_create_file("status", 0444, scp.debugfs, NULL, &scp_status_fops);
	debugfs_create_file("last_log", 0444, scp.debugfs, NULL, &scp_last_log_fops);
	debugfs_create_file("log", 0444, scp.debugfs, NULL, &scp_log_fops);

	/* reset_scp(SCP_ALL_ENABLE) */
	writel((u32)scp.shm_phys, scp.cfg_core0 + DRAM_RESV_ADDR_REG);
	writel((u32)scp.shm_size, scp.cfg_core0 + DRAM_RESV_SIZE_REG);
	writel(1, scp.cfg_core0 + R_CORE_SW_RSTN_CLR);
	dsb(sy);
	dev_info(dev, "SCP released from reset (shm %pa, image %pa)\n",
		 &scp.shm_phys, &scp.rsv_phys);
	return 0;

err_clk:
	clk_disable_unprepare(scp.clk_mux);
	scp.clk_on = false;
	return ret;
}

static void cezanne_scp_remove(struct platform_device *pdev)
{
	int i;

	writel(1, scp.cfg_core0 + R_CORE_SW_RSTN_SET);
	writel(1, scp.cfg_core1 + R_CORE_SW_RSTN_SET);
	scp_ready = 0;
	cancel_work_sync(&scp.ready_work);
	for (i = 0; i < SCP_MBOX_TOTAL; i++)
		if (scp_mbox_info[i].irq_num > 0)
			free_irq(scp_mbox_info[i].irq_num, &scp_mbox_info[i]);
	debugfs_remove_recursive(scp.debugfs);
	if (scp.clk_on)
		clk_disable_unprepare(scp.clk_mux);
	scp.clk_on = false;
}

static const struct of_device_id cezanne_scp_of_match[] = {
	{ .compatible = "mediatek,mt6885-scp-tinysys" },
	{ }
};
MODULE_DEVICE_TABLE(of, cezanne_scp_of_match);

static struct platform_driver cezanne_scp_driver = {
	.probe = cezanne_scp_probe,
	.remove = cezanne_scp_remove,
	.driver = {
		.name = "cezanne-scp",
		.of_match_table = cezanne_scp_of_match,
		/* the firmware and IPI state cannot be torn down and rebuilt */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(cezanne_scp_driver);

MODULE_FIRMWARE(SCP_FW_NAME);
MODULE_DESCRIPTION("MT6885 SCP tinysys loader and IPI");
MODULE_LICENSE("GPL");
