// SPDX-License-Identifier: GPL-2.0
/*
 * Modem power, boot and handshakes (BSP modem_sys1.c md_cd_start,
 * mt6885/md_sys1_platform.c, clk-mt6885-pg.c spm_mtcmos_ctrl_md1,
 * fsm/ccci_fsm.c fsm_routine_start).
 *
 * Boot: lk has started the MD boot ROM; once it reports PASS the MD is
 * powered off, the share memory and CCIF are reset, the MD is powered on
 * and released through ATF. It then asks for the runtime data (HS1),
 * reads its NVRAM through the file service and reports MD_NORMAL_BOOT
 * (HS2). Userspace starts it by writing 1 to the "boot" attribute once its
 * file service (/dev/ccci_fs) and RPC server (/dev/ccci_rpc) are open.
 */
#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ccci.h"

#define SIP_CCCI_CONTROL	0xc2000505
#define MD_POWER_CONFIG		6
#define MD_KERNEL_BOOT_UP	0
#define MD_CHECK_FLAG		2
#define MD_CHECK_DONE		3
#define MD_BOOT_STATUS		4

#define CCIF_AP_PHYS		0x10209000
#define CCIF_MD_PHYS		0x1020a000
#define INFRA_PHYS		0x10001000
#define SPM_PHYS		0x10006000
#define PERICFG_PHYS		0x10003000
#define CCIF4_MD_PHYS		0x1024d000
#define CCIF5_MD_PHYS		0x1025d000
#define PERI_PCCIF5_STATE	0x30c	/* MD_PCCIF5_SW_READY / PWR_ON */
#define LK_TAGS_PHYS		0xbff70000	/* ccci-tag-mem, lk's boot info */
#define LK_TAGS_SIZE		0x10000

/* infracfg_ao: MD source clock request and MD1 bus protection */
#define INFRA_MD_SRCCLKENA	0xf0c
#define PROT_SET		0x2a0
#define PROT_CLR		0x2a4
#define PROT_STA1		0x228
#define VDNR_SET		0xb84
#define VDNR_CLR		0xb88
#define VDNR_STA1		0xb90
#define SUB_VDNR_SET		0xbb8
#define SUB_VDNR_CLR		0xbbc
#define SUB_VDNR_STA1		0xbc4
#define MD1_PROT1		BIT(7)
#define MD1_PROT2		(BIT(10) | BIT(21) | BIT(29))
#define MD1_PROT2_SUB		(BIT(6) | BIT(7))

/* SPM: MD1 power domain, srclken_o1 */
#define POWERON_CONFIG_EN	0x000
#define SPM_CLK_CON		0x008
#define PWR_STATUS		0x16c
#define MD1_PWR_CON		0x300
#define MD_EXT_BUCK_ISO_CON	0x398
#define PWR_RST_B		BIT(0)
#define PWR_ON			BIT(2)
#define MD1_PWR_STA		BIT(0)
#define SPM_PROJECT_KEY		(0xb16 << 16)

#define GIC_SPI			0
#define CCIF_SPI		194	/* data channels */
#define CCIF_EXP_SPI		195	/* exception channels */
#define MD_WDT_SPI		78

#define HS_TIMEOUT		(30 * HZ)

/* infracfg_ao gates of the CCIF blocks the MD uses (mt6885-clk.h) */
static const u32 ccif_clk_ids[] = { 44, 47, 39, 40, 95, 96, 107, 94 };

static const char *const state_names[] = {
	[MD_STATE_OFF] = "off", [MD_STATE_BOOTING] = "booting",
	[MD_STATE_HS1] = "handshake", [MD_STATE_READY] = "ready",
	[MD_STATE_EXCEPTION] = "exception",
};

static struct arm_smccc_res md_smc(unsigned long sub)
{
	struct arm_smccc_res r;

	arm_smccc_smc(SIP_CCCI_CONTROL, MD_POWER_CONFIG, sub, 0, 0, 0, 0, 0, &r);
	return r;
}

static void set_state(struct ccci_md *md, int state)
{
	if (md->state == state)
		return;
	if (md->state == MD_STATE_READY)
		ccci_ports_online(md, false);
	md->state = state;
	if (state == MD_STATE_READY)
		ccci_ports_online(md, true);
	dev_info(md->dev, "state %s\n", state_names[state]);
	sysfs_notify(&md->dev->kobj, NULL, "state");
}

static int prot_set(struct ccci_md *md, u32 set, u32 sta, u32 mask)
{
	u32 v;

	writel(mask, md->infra + set);
	return readl_poll_timeout(md->infra + sta, v, (v & mask) == mask, 10, 10000);
}

static void md_power_off(struct ccci_md *md)
{
	u32 v;

	writel(SPM_PROJECT_KEY | 1, md->spm + POWERON_CONFIG_EN);
	if (prot_set(md, PROT_SET, PROT_STA1, MD1_PROT1) ||
	    prot_set(md, VDNR_SET, VDNR_STA1, MD1_PROT2) ||
	    prot_set(md, SUB_VDNR_SET, SUB_VDNR_STA1, MD1_PROT2_SUB))
		dev_warn(md->dev, "MD1 bus protection not acked\n");
	writel(readl(md->spm + MD1_PWR_CON) & ~PWR_ON, md->spm + MD1_PWR_CON);
	if (readl_poll_timeout(md->spm + PWR_STATUS, v, !(v & MD1_PWR_STA), 10, 10000))
		dev_warn(md->dev, "MD1 power status still on\n");
	writel(readl(md->spm + MD_EXT_BUCK_ISO_CON) | BIT(0), md->spm + MD_EXT_BUCK_ISO_CON);
	writel(readl(md->spm + MD_EXT_BUCK_ISO_CON) | BIT(1), md->spm + MD_EXT_BUCK_ISO_CON);
	writel(readl(md->spm + MD1_PWR_CON) & ~PWR_RST_B, md->spm + MD1_PWR_CON);

	v = readl(md->infra + INFRA_MD_SRCCLKENA) & ~0xff;
	writel(v, md->infra + INFRA_MD_SRCCLKENA);
}

static int md_power_on(struct ccci_md *md)
{
	u32 v;

	/* VMODEM / VSRAM_MD keep lk's settings: they are never turned off */
	v = (readl(md->infra + INFRA_MD_SRCCLKENA) & ~0xff) | 0x21;
	writel(v, md->infra + INFRA_MD_SRCCLKENA);
	writel(SPM_PROJECT_KEY | 1, md->spm + POWERON_CONFIG_EN);
	writel(readl(md->spm + SPM_CLK_CON) | BIT(21), md->spm + SPM_CLK_CON);

	writel(readl(md->spm + MD_EXT_BUCK_ISO_CON) & ~BIT(0), md->spm + MD_EXT_BUCK_ISO_CON);
	writel(readl(md->spm + MD_EXT_BUCK_ISO_CON) & ~BIT(1), md->spm + MD_EXT_BUCK_ISO_CON);
	writel(readl(md->spm + MD1_PWR_CON) | PWR_RST_B, md->spm + MD1_PWR_CON);
	writel(readl(md->spm + MD1_PWR_CON) | PWR_ON, md->spm + MD1_PWR_CON);
	if (readl_poll_timeout(md->spm + PWR_STATUS, v, v & MD1_PWR_STA, 10, 10000)) {
		dev_err(md->dev, "MD1 power on timeout\n");
		return -ETIMEDOUT;
	}
	writel(MD1_PROT2, md->infra + VDNR_CLR);
	writel(MD1_PROT2_SUB, md->infra + SUB_VDNR_CLR);
	writel(MD1_PROT1, md->infra + PROT_CLR);
	return 0;
}

/*
 * BSP clear_smem_region() for a first boot: everything but the regions the
 * MD keeps (CCB control, audio, MD-CONSYS, USIP). Done on every boot: a
 * restart with only the vendor's restart set cleared times out in HS1. The
 * CCB control headers are written afresh by ccb_ctrl_init().
 */
static void clear_smem(struct ccci_md *md)
{
	static const struct { u32 off, size; } nc[] = {
		{ 0, NC_CCB_CTRL },			/* debug .. CCISM_SCP */
		{ NC_NETD, NC_AUDIO - NC_NETD },	/* NETD, USB */
		{ NC_CCISM_MCU, NC_CCISM_MCU_EXP + NC_CCISM_MCU_EXP_SIZE - NC_CCISM_MCU },
		{ NC_WIFI_PROXY, 0x8000 },
		{ NC_DFD, NC_DFD_SIZE },
	}, c[] = {
		{ C_NVRAM_CACHE, 0x700000 },
		{ C_CCB_DHL, C_RAW_DHL - C_CCB_DHL },
		{ C_RAW_DHL, C_USIP - C_RAW_DHL },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(nc); i++)
		memset(md->nc + nc[i].off, 0, nc[i].size);
	for (i = 0; i < ARRAY_SIZE(c); i++)
		memset(md->c + c[i].off, 0, c[i].size);
	wmb();
}

/*
 * CCB (DHL logging) buffers, one 64-byte control header each: BSP
 * ccb_configs for MT6885 (ccb_gear_id 12, CCB_DATA_SIZE in rtdata.c). DHL
 * takes slots 0-17 (control, exception, PS, HW loggers, dummies, padding),
 * then MD monitor and META.
 */
static const struct { u32 dl_page, ul_page, dl_size, ul_size; } ccb_slots[] = {
	[0] = { 1024, 1024, 1024 * 32, 1024 * 32 },		/* DHL control */
	[1] = { 20 * 1024, 128, 20 * 1024 * 6, 128 },		/* MD exception */
	[2] = { 512, 1024, 512 * 72, 1024 * 32 },		/* PS */
	[3] = { 512, 128, 512 * 64, 128 },			/* HW logger 1 */
	[4] = { 512, 128, 512 * 64, 128 },			/* HW logger 2 */
	[5] = { 512, 128, 512 * 256, 128 },			/* HW logger 3 */
	[6 ... 16] = { 128, 128, 128, 128 },			/* dummies */
	[17] = { 128, 128, 128 * 5, 128 },			/* padding */
	[18] = { 512, 1024, 512 * 32, 1024 * 16 },		/* MD monitor */
	[19] = { 65 * 1024, 63 * 1024, 65 * 1024 * 8, 63 * 1024 * 8 },	/* META */
};

#define CCB_GUARD_END	0xeeff0011

/*
 * What MIUI's ccci_mdinit writes before every MD boot (libccci_util
 * ccci_ccb_init_users): per header DL then UL half, indices zero, page size,
 * buffer size, end guard. Without them the MD asserts in
 * ccci_shm_bm_chk_ccb_status (ccci_shm_bm.c 602-611: page size zero or not
 * 32-byte aligned, buffer size zero or not a page multiple). Until
 * 2026-10-30 the MD lived on the headers MIUI had left in RAM.
 */
static void ccb_ctrl_init(struct ccci_md *md)
{
	u32 *h = md->nc + NC_CCB_CTRL;
	int i;

	memset(h, 0, NC_NETD - NC_CCB_CTRL);
	for (i = 0; i < ARRAY_SIZE(ccb_slots); i++, h += 16) {
		h[5] = ccb_slots[i].dl_page;
		h[6] = ccb_slots[i].dl_size;
		h[7] = CCB_GUARD_END;
		h[13] = ccb_slots[i].ul_page;
		h[14] = ccb_slots[i].ul_size;
		h[15] = CCB_GUARD_END;
	}
	wmb();
}

/*
 * What the vendor's clock-off step resets after an MD has run (BSP
 * ccci_set_clk_cg): the MD's PCCIF5 ready / power flags and CCIF4 / CCIF5
 * channel acks. Left set, a restarted MD never reaches HS1. CCIF clocks on.
 */
static void md_clear_peer(struct ccci_md *md)
{
	writel(0, md->pericfg + PERI_PCCIF5_STATE);
	udelay(1000);
	writel(0xff, md->ccif4 + APCCIF_ACK);
	writel(0xff, md->ccif5 + APCCIF_ACK);
}

static void md_stop(struct ccci_md *md)
{
	if (md->state == MD_STATE_OFF)
		return;
	disable_irq(md->irq_ccif);
	disable_irq(md->irq_ccif_exp);
	disable_irq(md->irq_wdt);
	cancel_work_sync(&md->rx_work);
	md_power_off(md);
	ccci_dpmaif_stop(md);
	clk_bulk_disable_unprepare(md->num_clks, md->clks);
	set_state(md, MD_STATE_OFF);
}

static int md_start(struct ccci_md *md)
{
	struct arm_smccc_res r;
	unsigned long deadline;
	int i, ret;

	if (md->first_boot) {
		/* lk started the boot ROM, which checks the MD image */
		for (i = 0; i < 100; i++) {
			r = md_smc(MD_CHECK_DONE);
			if (!r.a0)
				break;
			msleep(20);
		}
		if (r.a0)
			dev_err(md->dev, "MD boot ROM did not pass\n");
		r = md_smc(MD_BOOT_STATUS);
		dev_info(md->dev, "boot ROM %s, status %#lx %#lx\n",
			 i < 100 ? "pass" : "fail", r.a1, r.a2);
		md_power_off(md);
	} else {
		md_stop(md);
	}
	set_state(md, MD_STATE_BOOTING);
	reinit_completion(&md->hs1);
	reinit_completion(&md->hs2);
	md->pending = 0;
	md->ex_channels = 0;

	clear_smem(md);
	ccb_ctrl_init(md);
	ccci_ccif_init(md);
	ret = clk_bulk_prepare_enable(md->num_clks, md->clks);
	if (ret)
		goto fail;
	md_clear_peer(md);
	ccci_ccif_reset(md);
	/* the BSP starts the data path before the MD (BOOT_WAITING_FOR_HS1) */
	if (ccci_dpmaif_start(md))
		dev_warn(md->dev, "no packet data path\n");
	ret = md_power_on(md);
	if (ret)
		goto fail_clk;
	r = md_smc(MD_KERNEL_BOOT_UP);
	dev_info(md->dev, "MD released: %#lx %#lx %#lx\n", r.a0, r.a1, r.a2);
	md->first_boot = false;
	enable_irq(md->irq_ccif);
	enable_irq(md->irq_ccif_exp);
	enable_irq(md->irq_wdt);

	if (!wait_for_completion_timeout(&md->hs1, HS_TIMEOUT)) {
		r = md_smc(MD_BOOT_STATUS);
		dev_err(md->dev, "HS1 timeout, boot status %#lx %#lx\n", r.a1, r.a2);
		set_state(md, MD_STATE_EXCEPTION);
		return -ETIMEDOUT;
	}
	/* HS2 comes after the MD has read its NVRAM: wait while it does */
	deadline = jiffies + HS_TIMEOUT;
	while (!wait_for_completion_timeout(&md->hs2, HZ)) {
		if (md->state == MD_STATE_EXCEPTION)
			return -EIO;
		if (time_before(deadline, READ_ONCE(md->last_fs) + 10 * HZ))
			deadline = READ_ONCE(md->last_fs) + 10 * HZ;
		if (time_after(jiffies, deadline)) {
			dev_err(md->dev, "HS2 timeout\n");
			set_state(md, MD_STATE_EXCEPTION);
			return -ETIMEDOUT;
		}
	}
	return 0;

fail_clk:
	ccci_dpmaif_stop(md);
	clk_bulk_disable_unprepare(md->num_clks, md->clks);
fail:
	set_state(md, MD_STATE_OFF);
	return ret;
}

static void boot_work(struct work_struct *work)
{
	struct ccci_md *md = container_of(work, struct ccci_md, boot_work);

	mutex_lock(&md->lock);
	md_start(md);
	mutex_unlock(&md->lock);
}

void ccci_recv_control(struct ccci_md *md, const struct ccci_header *h,
		       const void *pkt, size_t len)
{
	switch (h->data[1]) {
	case MD_INIT_START_BOOT:
		if (h->reserved == MD_INIT_CHK_ID) {
			if (len < sizeof(*h) + sizeof(struct md_query_ap_feature)) {
				dev_err(md->dev, "HS1: short query (%zu)\n", len);
				break;
			}
			set_state(md, MD_STATE_HS1);
			ccci_send_runtime_data(md, pkt + sizeof(*h));
			complete(&md->hs1);
		} else {
			set_state(md, MD_STATE_READY);
			complete(&md->hs2);
		}
		break;
	case MD_EX:
	case MD_EX_REC_OK:
	case MD_EX_PASS:
		dev_err(md->dev, "MD exception message %#x %#x\n",
			h->data[1], h->reserved);
		set_state(md, MD_STATE_EXCEPTION);
		break;
	default:
		dev_info(md->dev, "control message %#x %#x\n",
			 h->data[1], h->reserved);
	}
}

static irqreturn_t wdt_isr(int irq, void *data)
{
	struct ccci_md *md = data;

	dev_err(md->dev, "MD watchdog\n");
	set_state(md, MD_STATE_EXCEPTION);
	return IRQ_HANDLED;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct ccci_md *md = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", state_names[md->state]);
}
static DEVICE_ATTR_RO(state);

static ssize_t boot_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct ccci_md *md = dev_get_drvdata(dev);
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	if (on) {
		queue_work(system_long_wq, &md->boot_work);
	} else {
		flush_work(&md->boot_work);
		mutex_lock(&md->lock);
		md_stop(md);
		mutex_unlock(&md->lock);
	}
	return count;
}
static DEVICE_ATTR_WO(boot);

static struct attribute *ccci_attrs[] = {
	&dev_attr_state.attr, &dev_attr_boot.attr, NULL,
};
ATTRIBUTE_GROUPS(ccci);

int ccci_gic_spi(u32 spi, u32 type)
{
	struct irq_fwspec fw = {
		.param_count = 3,
		.param = { GIC_SPI, spi, type },
	};
	struct device_node *gic = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
	int irq;

	if (!gic)
		return -ENODEV;
	fw.fwnode = of_fwnode_handle(gic);
	irq = irq_create_fwspec_mapping(&fw);
	of_node_put(gic);
	return irq ? irq : -EINVAL;
}

/* infracfg_ao gates by mt6885-clk.h id; release with clk_bulk_put */
int ccci_infra_clks(struct ccci_md *md, const u32 *ids, int n,
		    struct clk_bulk_data *clks)
{
	struct of_phandle_args args = { .args_count = 1 };
	int i;

	args.np = of_find_compatible_node(NULL, NULL, "mediatek,mt6885-infracfg_ao");
	if (!args.np)
		return -ENODEV;
	for (i = 0; i < n; i++) {
		args.args[0] = ids[i];
		clks[i].clk = of_clk_get_from_provider(&args);
		if (IS_ERR(clks[i].clk)) {
			int ret = PTR_ERR(clks[i].clk);

			while (--i >= 0)
				clk_put(clks[i].clk);
			of_node_put(args.np);
			return ret;
		}
	}
	of_node_put(args.np);
	return 0;
}

static int get_clocks(struct ccci_md *md)
{
	md->num_clks = ARRAY_SIZE(ccif_clk_ids);
	md->clks = devm_kcalloc(md->dev, md->num_clks, sizeof(*md->clks), GFP_KERNEL);
	if (!md->clks)
		return -ENOMEM;
	return ccci_infra_clks(md, ccif_clk_ids, md->num_clks, md->clks);
}

/* lk's tag list: { char name[64]; u32 data_offset, data_size, next } */
static int lk_tag_u32(struct ccci_md *md, const char *name, u32 *val)
{
	void *base = memremap(LK_TAGS_PHYS, LK_TAGS_SIZE, MEMREMAP_WC);
	u32 off = 0, t[3];
	int i, ret = -ENOENT;

	if (!base)
		return -ENOMEM;
	for (i = 0; i < 64 && off + 76 <= LK_TAGS_SIZE; i++) {
		char tag[64];

		memcpy(tag, base + off, sizeof(tag));
		memcpy(t, base + off + 64, sizeof(t));
		if (!tag[0] || t[0] + 4 > LK_TAGS_SIZE)
			break;
		if (!strncmp(tag, name, sizeof(tag))) {
			memcpy(val, base + t[0], 4);
			ret = 0;
			break;
		}
		off = t[2];
	}
	memunmap(base);
	return ret;
}

static int ccci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ccci_md *md;
	int ret;

	md = devm_kzalloc(dev, sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;
	md->dev = dev;
	md->first_boot = true;
	mutex_init(&md->lock);
	init_completion(&md->hs1);
	init_completion(&md->hs2);
	INIT_WORK(&md->rx_work, ccci_rx_work);
	INIT_WORK(&md->boot_work, boot_work);
	platform_set_drvdata(pdev, md);

	md->ccif_ap = devm_ioremap(dev, CCIF_AP_PHYS, 0x1000);
	md->ccif_md = devm_ioremap(dev, CCIF_MD_PHYS, 0x1000);
	md->infra = devm_ioremap(dev, INFRA_PHYS, 0x1000);
	md->spm = devm_ioremap(dev, SPM_PHYS, 0x1000);
	md->pericfg = devm_ioremap(dev, PERICFG_PHYS, 0x1000);
	md->ccif4 = devm_ioremap(dev, CCIF4_MD_PHYS, 0x1000);
	md->ccif5 = devm_ioremap(dev, CCIF5_MD_PHYS, 0x1000);
	md->nc = devm_memremap(dev, NC_PHYS, NC_SIZE, MEMREMAP_WC);
	md->c = devm_memremap(dev, C_PHYS, C_SIZE, MEMREMAP_WC);
	if (!md->ccif_ap || !md->ccif_md || !md->infra || !md->spm ||
	    !md->pericfg || !md->ccif4 || !md->ccif5 ||
	    IS_ERR(md->nc) || IS_ERR(md->c))
		return -ENOMEM;

	ret = lk_tag_u32(md, "md1img", &md->md_img_size);
	if (ret)
		return dev_err_probe(dev, ret, "no md1img tag from lk (recovery boot?)\n");
	ret = lk_tag_u32(md, "hdr_tbl_inf", &md->md_mem_base);	/* u64 base, size */
	if (ret)
		return dev_err_probe(dev, ret, "no hdr_tbl_inf tag from lk\n");

	ret = get_clocks(md);
	if (ret)
		return dev_err_probe(dev, ret, "CCIF clocks\n");

	md->irq_ccif = ccci_gic_spi(CCIF_SPI, IRQ_TYPE_LEVEL_HIGH);
	md->irq_ccif_exp = ccci_gic_spi(CCIF_EXP_SPI, IRQ_TYPE_LEVEL_HIGH);
	md->irq_wdt = ccci_gic_spi(MD_WDT_SPI, IRQ_TYPE_EDGE_RISING);
	if (md->irq_ccif < 0 || md->irq_ccif_exp < 0 || md->irq_wdt < 0) {
		ret = -EINVAL;
		goto err_clk;
	}
	ret = devm_request_irq(dev, md->irq_ccif, ccci_ccif_isr, IRQF_NO_AUTOEN,
			       "ccif", md);
	if (!ret)
		ret = devm_request_irq(dev, md->irq_ccif_exp, ccci_ccif_isr,
				       IRQF_NO_AUTOEN, "ccif-exp", md);
	if (!ret)
		ret = devm_request_threaded_irq(dev, md->irq_wdt, NULL, wdt_isr,
						IRQF_ONESHOT | IRQF_NO_AUTOEN,
						"md-wdt", md);
	if (ret)
		goto err_clk;

	ret = ccci_dpmaif_init(md);
	if (ret)
		goto err_clk;
	ret = ccci_ports_init(md);
	if (ret)
		goto err_dpmaif;
	dev_info(dev, "MD image %#x bytes, ready to boot\n", md->md_img_size);
	return 0;

err_dpmaif:
	ccci_dpmaif_exit(md);
err_clk:
	clk_bulk_put(md->num_clks, md->clks);
	return ret;
}

static void ccci_remove(struct platform_device *pdev)
{
	struct ccci_md *md = platform_get_drvdata(pdev);

	cancel_work_sync(&md->boot_work);
	mutex_lock(&md->lock);
	md_stop(md);
	mutex_unlock(&md->lock);
	ccci_ports_exit(md);
	ccci_dpmaif_exit(md);
	clk_bulk_put(md->num_clks, md->clks);
}

static struct platform_driver ccci_driver = {
	.probe = ccci_probe,
	.remove = ccci_remove,
	.driver = {
		.name = "cezanne-ccci",
		.dev_groups = ccci_groups,
	},
};

static struct platform_device *ccci_pdev;

static int __init ccci_init(void)
{
	int ret = platform_driver_register(&ccci_driver);

	if (ret)
		return ret;
	ccci_pdev = platform_device_register_simple("cezanne-ccci", -1, NULL, 0);
	if (IS_ERR(ccci_pdev)) {
		platform_driver_unregister(&ccci_driver);
		return PTR_ERR(ccci_pdev);
	}
	return 0;
}
module_init(ccci_init);

static void __exit ccci_exit(void)
{
	platform_device_unregister(ccci_pdev);
	platform_driver_unregister(&ccci_driver);
}
module_exit(ccci_exit);

MODULE_DESCRIPTION("MT6885 modem (CCCI over CCIF)");
MODULE_LICENSE("GPL");
