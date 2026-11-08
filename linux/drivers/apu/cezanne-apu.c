// SPDX-License-Identifier: GPL-2.0
/*
 * cezanne-apu: the MT6885 APU's two MDLA cores (MediaTek Deep Learning
 * Accelerator) for userspace: /dev/cezanne-apu (root only).
 *
 * open powers the APU up (conn, both MDLA cores at mdla_mhz from APUPLL,
 * the APU bus muxes on univpll_d5), release powers it down; one opener.
 * Above 700 MHz VPROC2 (the MDLA rail) goes from lk's 0.75 V to 0.8 V while
 * the APU is up (BSP OPP: 800 MHz at 0.8 V; needs the DT's voltage range,
 * else the clock stays at 700).
 * CZ_APU_IOC_ALLOC gives the opener one DMA buffer that mmap maps: from the
 * DT's APU pool (/cezanne-apu memory-region: CMA in one 4 GiB window above
 * 4 GiB, whose bits 33:32 the reviser adds to the MDLA's 32-bit addresses)
 * or else the default CMA area below 4 GiB; CZ_APU_IOC_RUN issues command
 * batches from it on one or both cores and waits until they are done (the
 * cores' done interrupts, /cezanne-apu interrupts in the DT; polled without
 * them or with use_irq=0).
 *
 * The MDLA reads and writes physical addresses (the APU IOMMU is not set
 * up), so whoever writes commands reaches all of memory: the device stays
 * root only, the users are root services that run fixed networks
 * (cezanne-upscaled).  Power-up sequence, command format and the traps:
 * mainline/apu/README.md (the RE tool apuprobe.c has the same power-up
 * with debugfs instead of this device; never load both).
 */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>

struct cz_apu_alloc {
	__u64 size;		/* in */
	__u64 bus;		/* out: the buffer's address for the MDLA (32 bits) */
};

struct cz_apu_run {
	__u32 off[2];		/* command offsets in the buffer, per core */
	__u32 n[2];		/* command counts (0: core not used) */
	__u32 timeout_ms;	/* per run, 0: 2 s */
	__u32 us;		/* out: time taken */
};

#define CZ_APU_IOC_ALLOC	_IOWR('A', 0, struct cz_apu_alloc)
#define CZ_APU_IOC_RUN		_IOWR('A', 1, struct cz_apu_run)
#define CZ_APU_MAX_BUF		SZ_2G
#define CZ_APU_ALLOC_TRIES	8

#define SPM_BASE		0x10006000
#define RPC_BASE		0x190f0000
#define APU_CONN_BASE		0x19020000
#define APU_VCORE_BASE		0x19029000
#define MDLA0_CFG_BASE		0x19034000
#define MDLA0_BIU_BASE		0x19035000
#define MDLA0_CMD_BASE		0x19036000
#define MDLA1_CFG_BASE		0x19038000	/* MDLA1: MDLA0 + 0x4000 */
#define MDLA1_CMD_BASE		0x1903a000
#define APU_IOMMU0_BASE		0x19010000
#define APU_IOMMU1_BASE		0x19015000
#define REG_MMU_IVRP_PADDR	0x114

/* SPM */
#define POWERON_CONFIG_EN	0x000
#define SPM_PROJECT_CODE	0xb16
#define OTHER_PWR_STATUS	0x178	/* bit 5: APU (VPU) */
#define EXT_BUCK_ISO		0x39c	/* bits 0, 5: vproc1 / vproc2 isolation */
#define SPM_CROSS_WAKE_M01_REQ	0x670	/* bit 0: APMCU_WAKEUP_APU */

/* APU RPC (remote power controller) */
#define RPC_TOP_CON		0x000	/* bit 0 sleep request, bit 31 fifo busy */
#define RPC_TOP_SEL		0x004
#define RPC_SW_FIFO_WE		0x008	/* domain | BIT(4) = on */
#define RPC_INTF_PWR_RDY	0x044	/* bit n: domain n powered */
#define RPC_PWR_CON(n)		(0x100 + 4 * (n))
#define RPC_SW_TYPE(n)		(0x200 + 0x10 * (n))

#define DOM_CONN		0
#define DOM_MDLA(i)		(6 + (i))

/* subsystem clock gates: CON / SET / CLR */
#define CG_CON			0x0
#define CG_CLR			0x8
#define APU_VCORE_CGS		0xf
#define APU_CONN_CGS		0x3fffe
#define MDLA_CGS		0x7fff

/* MDLA config block */
#define MDLA_SW_RST		0x00c
#define MDLA_AXI_CTRL		0x120
#define MDLA_AXI1_CTRL		0x124
#define MDLA_AXI_CTRL_MASK	(BIT(1) | BIT(10))

/* MDLA command block (mdla/mt6885/mdla_hw_reg.h) */
#define MREG_TOP_G_REV		0x500
#define MREG_TOP_G_INTP0	0x504
#define MREG_TOP_G_INTP2	0x50c
#define MREG_TOP_G_CDMA0	0x510
#define MREG_TOP_G_CDMA1	0x514	/* command address */
#define MREG_TOP_G_CDMA2	0x518	/* command count */
#define MREG_TOP_G_CDMA3	0x51c	/* batch + 1: go */
#define MREG_TOP_G_CDMA4	0x520
#define MREG_TOP_G_FIN0		0x534
#define MREG_TOP_G_FIN3		0x584	/* last finished command id */
#define MREG_TOP_G_IDLE		0x544
#define MREG_TOP_ENG0		0x550
#define MREG_TOP_ENG1		0x554
#define MREG_TOP_ENG2		0x558
#define MREG_TOP_ENG11		0x57c
#define MREG_TOP_G_COREINFO	0x588
#define MDLA_IRQ_SWCMD_DONE	BIT(2)
#define MDLA_IRQ_CDMA_FIFO_EMPTY BIT(5)
#define MDLA_IRQ_MASK		0x1fffff


/* ATF services the BSP uses on every APU power-up (mtk_secure_api.h,
 * mnoc/mt6885/mnoc_hw.c, reviser/1.0/reviser_hw.c): the reviser's
 * registers are secure */
#define MTK_SIP_APUSYS_MNOC_CONTROL	0xc2000519
#define MNOC_INFRA2APU_SRAM_EN		0
#define MNOC_INFRA2APU_SRAM_DIS		1
#define MTK_SIP_APUSYS_CONTROL		0xc200051e
#define OP_REVISER_SET_BOUNDARY		0
#define BOUNDARY_ALL_NO_CHANGE		0xffffffff

/* address bits 33:32 the reviser adds to MDLA / VPU / EDMA accesses: -1:
 * those of the buffer's pool (0: the default CMA area below 4 GiB; MIUI: 3,
 * its IOMMU's 34-bit IOVAs) */
static int boundary = -1;
module_param(boundary, int, 0444);
static bool own_pool;		/* the DT's APU pool is the device's CMA area */



/* MDLA cores to power (1 or 2) */
static uint cores = 2;
module_param(cores, uint, 0444);

/* wait for the done interrupts (GIC SPI 413 / 414, level) instead of
 * polling INTP0; without them in the DT the driver polls */
static bool use_irq = true;
module_param(use_irq, bool, 0444);

/* with the interrupts: look for the end of a run this long before sleeping
 * on them. A core in cpu-off / cluster-off (150 / 350 us) wakes too late
 * for short runs: depth's (~0.3 ms each) took 50 % longer on the IRQ
 * alone, U^2-Net's 11 % (2026-11-08) */
static uint spin_us = 1000;
module_param(spin_us, uint, 0644);

/* MDLA clock (dsp6_sel, both cores) from APUPLL in MHz, 0: leave the mux
 * as it is (univpll_d6_d2, 208 MHz) */
static uint mdla_mhz = 800;
module_param(mdla_mhz, uint, 0444);

/* APU conn (dsp_sel), IOMMU (dsp7_sel) and vcore (ipu_if_sel) muxes onto
 * univpll_d5 (499.2 MHz) instead of univpll_d6_d2 (208): BSP OPP at the
 * 0.75 V on VPROC1 allows conn 550 / IOMMU 499, ipu_if 550 at the 0.725 V
 * Vcore. Only ever reparent these muxes: clk_set_rate() on them propagates
 * (CLK_SET_RATE_PARENT) and re-tuned UNIVPLL and TVDPLL (2026-11-04) */
static bool fast_bus = true;
module_param(fast_bus, bool, 0444);

/* topckgen mux ids (dt-bindings/clock/mt6885-clk.h) */
#define TOP_MUX_DSP		13
#define TOP_MUX_DSP6		19
#define TOP_MUX_DSP7		20
#define TOP_MUX_IPU_IF		21
#define TOP_APUPLL_CK		125	/* apupll / 2 */
#define TOP_UNIVPLL_D5		101
#define APMIXED_APUPLL		16	/* only the APU muxes use it */

static void __iomem *spm, *rpc, *conn, *vcore, *mcfg[2], *mcmd[2];
static struct clk *clk_dsp, *clk_dsp6, *clk_dsp7, *clk_ipu_if, *clk_apupll, *clk_apupll_pll;
static struct clk *clk_univpll_d5;
static struct clk *dsp6_parent;	/* to restore at unload */
static unsigned long apupll_rate0;	/* lk's APUPLL rate, restored at unload */
static struct clk *bus_parent[3];	/* dsp_sel, dsp7_sel, ipu_if_sel */
static int clks_on;	/* how many of clk_ipu_if, clk_dsp, clk_dsp7, clk_dsp6 */
static struct regulator *reg_vvpu, *reg_vmdla, *reg_vsram;
static bool rails_on[3];		/* vvpu, vmdla, vsram enabled by us */
static bool vmdla_raised;		/* VPROC2 at 0.8 V for > 700 MHz */
#define VMDLA_UV		750000	/* lk's, the BSP's up to 700 MHz */
#define VMDLA_FAST_UV		800000	/* BSP OPP 0: 800 MHz */
static bool conn_on, mdla_on[2];
static int mdla_irq[2];		/* 0: polled */
static bool irq_on[2];		/* enabled while the core is powered */
static struct completion mdla_done[2];
static struct platform_device *pdev;
static void *mem;		/* the opener's buffer */
static dma_addr_t mem_dma;
static size_t mem_size;
static DEFINE_MUTEX(lock);	/* one opener; ioctls serialised */
static bool in_use;

static struct clk *top_clk(struct device_node *np, int id)
{
	struct of_phandle_args args = { .np = np, .args_count = 1, .args = { id } };

	return of_clk_get_from_provider(&args);
}

/* the rails as lk left them (no DT constraints: enable / disable only
 * count users, the voltages stay); held while the APU is up */
static struct regulator *rail(const char *name)
{
	struct regulator *r = regulator_get_optional(NULL, name);

	if (IS_ERR(r)) {
		pr_warn("cezanne-apu: no regulator %s (%ld), left as is\n", name, PTR_ERR(r));
		return NULL;
	}
	return r;
}

static void spm_unlock(void)
{
	writel(SPM_PROJECT_CODE << 16 | BIT(0), spm + POWERON_CONFIG_EN);
}

static int rpc_wait(int dom, bool on)
{
	u32 v;
	int ret = readl_poll_timeout_atomic(rpc + RPC_INTF_PWR_RDY, v,
					    !!(v & BIT(dom)) == on, 10, 5000);
	if (ret)
		pr_err("cezanne-apu: domain %d stays %s (INTF_PWR_RDY %#x)\n",
		       dom, on ? "off" : "on", v);
	return ret;
}

static void rpc_fifo_wait(void)
{
	u32 v;

	if (readl_poll_timeout_atomic(rpc + RPC_TOP_CON, v, !(v & BIT(31)), 10, 5000))
		pr_err("cezanne-apu: RPC fifo busy (TOP_CON %#x)\n", v);
}

static void dump(const char *when)
{
	pr_debug("cezanne-apu: %s: SPM OTHER_PWR_STATUS %#x CROSS_WAKE %#x EXT_BUCK_ISO %#x; RPC TOP_CON %#x TOP_SEL %#x INTF_PWR_RDY %#x\n",
		when, readl(spm + OTHER_PWR_STATUS), readl(spm + SPM_CROSS_WAKE_M01_REQ),
		readl(spm + EXT_BUCK_ISO), readl(rpc + RPC_TOP_CON),
		readl(rpc + RPC_TOP_SEL), readl(rpc + RPC_INTF_PWR_RDY));
}

/* hw_init_setting(): memory groups to PD mode, sub-domain switches forced
 * on (the RPC sequences them), RPC IRQ masked / WFI bypassed, then the
 * boot-time sleep request that leaves the conn off */
static int rpc_init(void)
{
	static const u8 types[] = { [0] = 0xff, [2] = 0x7, [3] = 0x7, [4] = 0x7,
				    [6] = 0x3, [7] = 0x3 };
	int i;

	for (i = 0; i < ARRAY_SIZE(types); i++)
		if (types[i])
			writel(types[i], rpc + RPC_SW_TYPE(i));
	for (i = 2; i <= 7; i++)
		if (i != 5)
			writel(readl(rpc + RPC_PWR_CON(i)) | BIT(1), rpc + RPC_PWR_CON(i));
	writel(readl(rpc + RPC_TOP_SEL) | 0x9e | BIT(10), rpc + RPC_TOP_SEL);
	udelay(100);
	if (!(readl(rpc + RPC_INTF_PWR_RDY) & BIT(DOM_CONN)))
		return 0;	/* already asleep: never request sleep twice */
	writel(readl(rpc + RPC_TOP_CON) | BIT(0), rpc + RPC_TOP_CON);
	return rpc_wait(DOM_CONN, false);
}

static unsigned long sip(u32 fn, u32 a1, u32 a2, u32 a3, u32 a4)
{
	struct arm_smccc_res res;

	arm_smccc_smc(fn, a1, a2, a3, a4, 0, 0, 0, &res);
	return res.a0;
}

/* mnoc_apusys_top_after_pwr_on() + reviser_power_on_cb() */
static void conn_secure_on(void)
{
	/* two MDLA, three VPU, two EDMA contexts, 4 bits each */
	u32 mdla = ~0xffu | boundary << 4 | boundary;
	u32 vpu = ~0xfffu | boundary << 8 | boundary << 4 | boundary;
	unsigned long r1, r2, r3, r4;

	r1 = sip(MTK_SIP_APUSYS_MNOC_CONTROL, MNOC_INFRA2APU_SRAM_EN, 0, 0, 0);
	r2 = sip(MTK_SIP_APUSYS_CONTROL, OP_REVISER_SET_BOUNDARY, mdla,
		 BOUNDARY_ALL_NO_CHANGE, BOUNDARY_ALL_NO_CHANGE);
	r3 = sip(MTK_SIP_APUSYS_CONTROL, OP_REVISER_SET_BOUNDARY,
		 BOUNDARY_ALL_NO_CHANGE, vpu, BOUNDARY_ALL_NO_CHANGE);
	r4 = sip(MTK_SIP_APUSYS_CONTROL, OP_REVISER_SET_BOUNDARY,
		 BOUNDARY_ALL_NO_CHANGE, BOUNDARY_ALL_NO_CHANGE, mdla);
	pr_debug("cezanne-apu: SMC infra2apu sram %#lx, boundary %d: mdla %#lx vpu %#lx edma %#lx\n",
		r1, boundary, r2, r3, r4);
}

static int conn_power_on(void)
{
	u32 v;
	int ret;

	spm_unlock();
	writel(readl(spm + EXT_BUCK_ISO) & ~0x21, spm + EXT_BUCK_ISO);
	writel(readl(spm + SPM_CROSS_WAKE_M01_REQ) | BIT(0), spm + SPM_CROSS_WAKE_M01_REQ);
	ret = readl_poll_timeout_atomic(spm + OTHER_PWR_STATUS, v, v & BIT(5), 10, 100000);
	if (ret) {
		pr_err("cezanne-apu: SPM did not power the APU (OTHER_PWR_STATUS %#x)\n", v);
		return ret;
	}
	ret = rpc_wait(DOM_CONN, true);
	if (ret)
		return ret;
	conn_on = true;
	writel(APU_VCORE_CGS, vcore + CG_CLR);
	writel(APU_CONN_CGS, conn + CG_CLR);
	conn_secure_on();
	return 0;
}

static void conn_power_off(void)
{
	sip(MTK_SIP_APUSYS_MNOC_CONTROL, MNOC_INFRA2APU_SRAM_DIS, 0, 0, 0);
	spm_unlock();
	writel(readl(spm + SPM_CROSS_WAKE_M01_REQ) & ~BIT(0), spm + SPM_CROSS_WAKE_M01_REQ);
	udelay(10);
	writel(readl(rpc + RPC_TOP_SEL) | 0x9e, rpc + RPC_TOP_SEL);
	writel(readl(rpc + RPC_TOP_CON) | BIT(0), rpc + RPC_TOP_CON);
	rpc_wait(DOM_CONN, false);
	conn_on = false;
}

static void mdla_init(int i);

static int mdla_power_on(int i)
{
	int ret, retry;

	for (retry = 0; retry < 3; retry++) {
		rpc_fifo_wait();
		writel(DOM_MDLA(i) | BIT(4), rpc + RPC_SW_FIFO_WE);
		ret = rpc_wait(DOM_MDLA(i), true);
		if (!ret)
			break;
	}
	if (ret)
		return ret;
	mdla_on[i] = true;
	writel(MDLA_CGS, mcfg[i] + CG_CLR);
	mdla_init(i);
	if (mdla_irq[i]) {
		enable_irq(mdla_irq[i]);
		irq_on[i] = true;
	}
	return 0;
}

/* mdla_scheduler_2_1(): SWCMD_DONE acked in INTP0 (write 1 to clear); the
 * line stays enabled only while the core is powered (no register access
 * to a domain that is off) */
static irqreturn_t mdla_isr(int irq, void *data)
{
	int i = (struct completion *)data - mdla_done;

	if (!(readl(mcmd[i] + MREG_TOP_G_INTP0) & MDLA_IRQ_SWCMD_DONE))
		return IRQ_NONE;
	writel(MDLA_IRQ_SWCMD_DONE, mcmd[i] + MREG_TOP_G_INTP0);
	complete(&mdla_done[i]);
	return IRQ_HANDLED;
}

/* mdla_multi_core_sw_rst(): what the BSP does to a core that timed out */
static void mdla_soft_reset(int i)
{
	writel(0xff, mcfg[i] + MDLA_SW_RST);
	udelay(10);
	writel(0, mcfg[i] + MDLA_SW_RST);
	mdla_init(i);
}

/* mdla_reset(): engines' DCM / CG defaults, the done IRQ unmasked; the
 * BSP's AXI "ION" bits stay clear (they route the MDLA through the APU
 * IOMMU, which has no page table here: the first command hangs) */
static void mdla_init(int i)
{
	writel(0xffffffff, mcfg[i] + CG_CLR);
	writel(MDLA_IRQ_MASK & ~MDLA_IRQ_SWCMD_DONE, mcmd[i] + MREG_TOP_G_INTP2);
	writel(0, mcmd[i] + MREG_TOP_ENG0);
	writel(0, mcmd[i] + MREG_TOP_ENG1);
	writel(0, mcmd[i] + MREG_TOP_ENG2);
	writel(0x600, mcmd[i] + MREG_TOP_ENG11);
	if (readl(mcfg[i] + MDLA_AXI_CTRL) & MDLA_AXI_CTRL_MASK)
		pr_warn("cezanne-apu: MDLA%d AXI_CTRL %#x: IOMMU path set\n", i,
			readl(mcfg[i] + MDLA_AXI_CTRL));
}

static void mdla_power_off(int i)
{
	if (irq_on[i]) {
		disable_irq(mdla_irq[i]);
		irq_on[i] = false;
	}
	mdla_soft_reset(i);
	rpc_fifo_wait();
	writel(DOM_MDLA(i), rpc + RPC_SW_FIFO_WE);
	rpc_wait(DOM_MDLA(i), false);
	mdla_on[i] = false;
}

static void mdla_all_off(void)
{
	int i;

	for (i = 1; i >= 0; i--)
		if (mdla_on[i])
			mdla_power_off(i);
}

/* config_apupll(): the APUPLL itself to twice the rate (apupll_ck =
 * apupll / 2), then dsp6_sel onto apupll_ck; the bus muxes onto
 * univpll_d5. Only reparenting, done before dsp6_sel is first enabled */
static int mdla_clock(void)
{
	struct clk *bus[] = { clk_dsp, clk_dsp7, clk_ipu_if };
	uint mhz;
	int i, ret;

	for (i = 0; fast_bus && i < ARRAY_SIZE(bus); i++) {
		bus_parent[i] = clk_get_parent(bus[i]);
		ret = clk_set_parent(bus[i], clk_univpll_d5);
		pr_debug("cezanne-apu: %s %lu Hz (%d)\n", __clk_get_name(bus[i]),
			clk_get_rate(bus[i]), ret);
		if (ret)
			return ret;
	}
	if (!mdla_mhz)
		return 0;
	mhz = mdla_mhz;
	if (mhz > 700) {
		/* the voltage first, then the clock */
		ret = reg_vmdla ? regulator_set_voltage(reg_vmdla, VMDLA_FAST_UV, VMDLA_FAST_UV) : -ENODEV;
		if (ret) {
			pr_warn_once("cezanne-apu: VPROC2 stays at %d uV (%d): MDLA at 700 MHz\n",
				     reg_vmdla ? regulator_get_voltage(reg_vmdla) : 0, ret);
			mhz = 700;
		} else {
			vmdla_raised = true;
		}
	}
	dsp6_parent = clk_get_parent(clk_dsp6);
	apupll_rate0 = clk_get_rate(clk_apupll_pll);
	ret = clk_set_rate(clk_apupll_pll, 2 * mhz * 1000000UL);
	if (!ret)
		ret = clk_set_parent(clk_dsp6, clk_apupll);
	pr_debug("cezanne-apu: MDLA clock: apupll %lu Hz, dsp6 %lu Hz, VPROC2 %d uV (%d)\n",
		clk_get_rate(clk_apupll_pll), clk_get_rate(clk_dsp6),
		reg_vmdla ? regulator_get_voltage(reg_vmdla) : 0, ret);
	return ret;
}

/* mdla_issue_ce() on core i */
static int mdla_issue(int i, u32 off, u32 count)
{
	u32 intp;

	if (i >= cores || !mdla_on[i] || !count || off & 0x3f ||
	    (u64)off + count * 0x1c0 > mem_size)
		return -EINVAL;
	intp = readl(mcmd[i] + MREG_TOP_G_INTP0);
	if (!(intp & MDLA_IRQ_CDMA_FIFO_EMPTY))
		return -EBUSY;
	writel(intp, mcmd[i] + MREG_TOP_G_INTP0);	/* clear stale status */
	if (irq_on[i]) {
		/* a handler that saw the stale status is done before the
		 * completion is reset */
		synchronize_irq(mdla_irq[i]);
		reinit_completion(&mdla_done[i]);
	}
	wmb();
	writel(lower_32_bits(mem_dma + off), mcmd[i] + MREG_TOP_G_CDMA1);
	writel(count, mcmd[i] + MREG_TOP_G_CDMA2);
	writel(1, mcmd[i] + MREG_TOP_G_CDMA3);
	return 0;
}

/* done: the handler has run, or SWCMD_DONE is up before it (it then finds
 * nothing to ack; the next issue resets the completion) */
static bool mdla_finished(int i)
{
	return completion_done(&mdla_done[i]) ||
	       readl(mcmd[i] + MREG_TOP_G_INTP0) & MDLA_IRQ_SWCMD_DONE;
}

/* the last command carries SWCMD_FINISH_INT_EN; FIN3 is the finished
 * command's id (patterns reuse ids, so it is no completion test) */
static int mdla_wait(int i, u32 timeout_us)
{
	u32 spin = min(READ_ONCE(spin_us), timeout_us);
	bool fin;
	u32 intp;
	int ret;

	if (irq_on[i]) {
		/* (a 0 timeout would poll for ever) */
		ret = spin ? read_poll_timeout(mdla_finished, fin, fin, 20, spin, false, i) : -ETIMEDOUT;
		if (ret)
			ret = wait_for_completion_timeout(&mdla_done[i],
							  usecs_to_jiffies(timeout_us - spin)) ? 0 : -ETIMEDOUT;
		intp = readl(mcmd[i] + MREG_TOP_G_INTP0);
		if (ret && intp & MDLA_IRQ_SWCMD_DONE) {
			pr_warn_once("cezanne-apu: MDLA%d done without its interrupt\n", i);
			ret = 0;
		}
	} else {
		ret = readl_poll_timeout(mcmd[i] + MREG_TOP_G_INTP0, intp,
					 intp & MDLA_IRQ_SWCMD_DONE, 20, timeout_us);
	}
	if (ret)
		pr_err("cezanne-apu: MDLA%d timeout: FIN3 %u INTP0 %#x IDLE %#x\n", i,
		       readl(mcmd[i] + MREG_TOP_G_FIN3), intp, readl(mcmd[i] + MREG_TOP_G_IDLE));
	writel(intp, mcmd[i] + MREG_TOP_G_INTP0);
	writel(1, mcmd[i] + MREG_TOP_G_CDMA4);
	if (ret) {
		/* mdla_timeout_handle(): power-cycle the core's domain */
		mdla_power_off(i);
		ret = mdla_power_on(i) ?: -ETIMEDOUT;
	}
	return ret;
}

/* n[i]: commands for core i at off[i] (0: core idle) */
static int mdla_run(const u32 *off, const u32 *n, u32 timeout_us)
{
	int i, ret = 0;

	for (i = 0; i < 2; i++)
		if (n[i]) {
			ret = mdla_issue(i, off[i], n[i]);
			if (ret)
				return ret;
		}
	for (i = 0; i < 2; i++)
		if (n[i])
			ret = mdla_wait(i, timeout_us) ?: ret;
	return ret;
}

/* a faulting APU IOMMU access lands in the buffer's last page, not at 0 */
static void iommu_ivrp(void)
{
	static const u32 bases[] = { APU_IOMMU0_BASE, APU_IOMMU1_BASE };
	void __iomem *p;
	int i;

	for (i = 0; i < ARRAY_SIZE(bases); i++) {
		p = ioremap(bases[i], 0x1000);
		if (!p)
			continue;
		writel((u32)((mem_dma + mem_size - PAGE_SIZE) >> 1), p + REG_MMU_IVRP_PADDR);
		iounmap(p);
	}
}

static void unmap_all(void)
{
	void __iomem **m[] = { &spm, &rpc, &conn, &vcore, &mcfg[0], &mcmd[0], &mcfg[1], &mcmd[1] };
	int i;

	for (i = 0; i < ARRAY_SIZE(m); i++)
		if (*m[i]) {
			iounmap(*m[i]);
			*m[i] = NULL;
		}
}


static void unmap_all(void);

static void free_mem(void)
{
	if (mem)
		dma_free_coherent(&pdev->dev, mem_size, mem, mem_dma);
	mem = NULL;
	mem_size = 0;
}

static void apu_power_down(void)
{
	struct clk *c[] = { clk_ipu_if, clk_dsp, clk_dsp7, clk_dsp6 };
	struct regulator *r[] = { reg_vsram, reg_vmdla, reg_vvpu };
	int i;

	mdla_all_off();
	if (conn_on)
		conn_power_off();
	dump("off");
	while (clks_on)
		clk_disable_unprepare(c[--clks_on]);
	if (dsp6_parent)
		clk_set_parent(clk_dsp6, dsp6_parent);
	dsp6_parent = NULL;
	if (apupll_rate0)
		clk_set_rate(clk_apupll_pll, apupll_rate0);
	apupll_rate0 = 0;
	for (i = 0; i < ARRAY_SIZE(bus_parent); i++)
		if (bus_parent[i])
			clk_set_parent((struct clk *[]){ clk_dsp, clk_dsp7, clk_ipu_if }[i], bus_parent[i]);
	memset(bus_parent, 0, sizeof(bus_parent));
	if (vmdla_raised)	/* the clock is off and back on its parent: now the voltage */
		regulator_set_voltage(reg_vmdla, VMDLA_UV, VMDLA_FAST_UV);
	vmdla_raised = false;
	for (i = 0; i < ARRAY_SIZE(r); i++)
		if (rails_on[2 - i])
			regulator_disable(r[i]);
	memset(rails_on, 0, sizeof(rails_on));
}

static int apu_power_up(void)
{
	struct regulator *r[] = { reg_vvpu, reg_vmdla, reg_vsram };
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(r); i++)
		rails_on[i] = r[i] && !regulator_enable(r[i]);

	/* vcore + conn clock sources (enable_apu_vcore/conn_clksrc) */
	for (; clks_on < 3; clks_on++) {
		ret = clk_prepare_enable((struct clk *[]){ clk_ipu_if, clk_dsp, clk_dsp7 }[clks_on]);
		if (ret)
			goto fail;
	}
	ret = mdla_clock();
	if (ret)
		goto fail;

	if (readl(rpc + RPC_INTF_PWR_RDY) & BIT(DOM_CONN)) {
		/* a hung MDLA kept the APU up (it does not sleep then): take the
		 * conn over, reset and power-cycle the cores */
		pr_warn("cezanne-apu: APU still powered, adopting it\n");
		ret = clk_prepare_enable(clk_dsp6);
		if (ret)
			goto fail;
		clks_on++;
		spm_unlock();
		writel(readl(spm + EXT_BUCK_ISO) & ~0x21, spm + EXT_BUCK_ISO);
		writel(readl(spm + SPM_CROSS_WAKE_M01_REQ) | BIT(0), spm + SPM_CROSS_WAKE_M01_REQ);
		conn_on = true;
		writel(APU_VCORE_CGS, vcore + CG_CLR);
		writel(APU_CONN_CGS, conn + CG_CLR);
		conn_secure_on();
		for (i = 0; i < 2; i++)
			if (readl(rpc + RPC_INTF_PWR_RDY) & BIT(DOM_MDLA(i))) {
				mdla_on[i] = true;
				writel(MDLA_CGS, mcfg[i] + CG_CLR);
				mdla_power_off(i);
			}
	} else {
		ret = rpc_init();
		if (!ret)
			ret = conn_power_on();
		if (ret)
			goto fail;
		ret = clk_prepare_enable(clk_dsp6);
		if (ret)
			goto fail;
		clks_on++;
	}
	for (i = 0, ret = 0; i < cores && !ret; i++)
		ret = mdla_power_on(i);
	dump("up");
	if (ret)
		goto fail;
	return 0;
fail:
	pr_err("cezanne-apu: power-up failed (%d)\n", ret);
	apu_power_down();
	return ret;
}

static int cz_open(struct inode *ino, struct file *f)
{
	int ret = 0;

	mutex_lock(&lock);
	if (in_use)
		ret = -EBUSY;
	else
		ret = apu_power_up();
	if (!ret)
		in_use = true;
	mutex_unlock(&lock);
	return ret;
}

static int cz_release(struct inode *ino, struct file *f)
{
	mutex_lock(&lock);
	free_mem();
	apu_power_down();
	in_use = false;
	mutex_unlock(&lock);
	return 0;
}

static long cz_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	void __user *up = (void __user *)arg;
	struct cz_apu_alloc a;
	struct cz_apu_run r;
	ktime_t t0;
	long ret;
	int tries;

	mutex_lock(&lock);
	switch (cmd) {
	case CZ_APU_IOC_ALLOC:
		ret = -EFAULT;
		if (copy_from_user(&a, up, sizeof(a)))
			break;
		ret = -EINVAL;
		if (mem || !a.size || a.size > CZ_APU_MAX_BUF)
			break;
		mem_size = PAGE_ALIGN(a.size);
		/* CMA: a range is busy while pages in it fail to migrate (5
		 * passes); a failed try still moves most of them out, so try
		 * again (quietly: each failure dumps the CMA state and warns) */
		for (tries = 0; !mem && tries < CZ_APU_ALLOC_TRIES; tries++)
			mem = dma_alloc_coherent(&pdev->dev, mem_size, &mem_dma,
						 GFP_KERNEL | __GFP_NOWARN);
		ret = -ENOMEM;
		if (!mem) {
			pr_warn("cezanne-apu: no %zu MiB DMA buffer (%d tries)\n",
				mem_size >> 20, tries);
			mem_size = 0;
			break;
		}
		if (tries > 1)
			pr_info("cezanne-apu: %zu MiB DMA buffer on try %d\n", mem_size >> 20, tries);
		/* the MDLA reaches one 4 GiB window, the boundary's */
		if (upper_32_bits(mem_dma) != boundary ||
		    upper_32_bits(mem_dma + mem_size - 1) != boundary) {
			pr_err("cezanne-apu: buffer at %pad not in the boundary's window (%d)\n",
			       &mem_dma, boundary);
			free_mem();
			break;
		}
		iommu_ivrp();
		a.bus = lower_32_bits(mem_dma);
		ret = copy_to_user(up, &a, sizeof(a)) ? -EFAULT : 0;
		break;
	case CZ_APU_IOC_RUN:
		ret = -EFAULT;
		if (copy_from_user(&r, up, sizeof(r)))
			break;
		ret = -EINVAL;
		if (!mem)
			break;
		t0 = ktime_get();
		ret = mdla_run(r.off, r.n, (r.timeout_ms ?: 2000) * 1000);
		r.us = ktime_us_delta(ktime_get(), t0);
		if (copy_to_user(up, &r, sizeof(r)))
			ret = -EFAULT;
		break;
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&lock);
	return ret;
}

static int cz_mmap(struct file *f, struct vm_area_struct *vma)
{
	int ret = -EINVAL;

	mutex_lock(&lock);
	if (mem && !vma->vm_pgoff && vma->vm_end - vma->vm_start <= mem_size)
		ret = dma_mmap_coherent(&pdev->dev, vma, mem, mem_dma,
					vma->vm_end - vma->vm_start);
	mutex_unlock(&lock);
	return ret;
}

static const struct file_operations cz_fops = {
	.owner = THIS_MODULE,
	.open = cz_open,
	.release = cz_release,
	.unlocked_ioctl = cz_ioctl,
	.mmap = cz_mmap,
};

static struct miscdevice cz_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "cezanne-apu",
	.fops = &cz_fops,
	.mode = 0600,
};

static void put_clocks(void)
{
	struct clk *c[] = { clk_ipu_if, clk_dsp, clk_dsp7, clk_dsp6, clk_apupll,
			    clk_apupll_pll, clk_univpll_d5 };
	struct regulator *r[] = { reg_vsram, reg_vmdla, reg_vvpu };
	int i;

	for (i = 0; i < ARRAY_SIZE(c); i++)
		if (!IS_ERR_OR_NULL(c[i]))
			clk_put(c[i]);
	for (i = 0; i < ARRAY_SIZE(r); i++)
		if (r[i])
			regulator_put(r[i]);
}

/* the DMA buffer's pool: the DT's (/cezanne-apu memory-region, a reusable
 * shared-dma-pool inside one 4 GiB window, which sets the boundary) or the
 * default CMA area below 4 GiB */
static int apu_pool(void)
{
	struct device_node *np = of_find_node_by_path("/cezanne-apu");
	u64 mask = DMA_BIT_MASK(32);
	int window = 0;
	struct resource r;

	if (np && !of_reserved_mem_region_to_resource(np, 0, &r)) {
		if (upper_32_bits(r.start) != upper_32_bits(r.end))
			pr_warn("cezanne-apu: pool %pR crosses a 4 GiB window\n", &r);
		else if (!of_reserved_mem_device_init_by_idx(&pdev->dev, np, 0)) {
			own_pool = true;
			window = upper_32_bits(r.start);
			mask = DMA_BIT_MASK(34);
			pr_info("cezanne-apu: DMA buffers from %pR\n", &r);
		}
	}
	of_node_put(np);
	if (boundary < 0)
		boundary = window;
	return dma_coerce_mask_and_coherent(&pdev->dev, mask);
}

/* the cores' done interrupts (/cezanne-apu interrupts, MIUI's mtk_mdla
 * node: one per core), off until the core is powered; without them the
 * driver polls */
static void mdla_irqs(void)
{
	static const char *const names[] = { "cezanne-mdla0", "cezanne-mdla1" };
	struct device_node *np = use_irq ? of_find_node_by_path("/cezanne-apu") : NULL;
	int i, irq;

	for (i = 0; i < 2; i++) {
		init_completion(&mdla_done[i]);
		irq = np ? of_irq_get(np, i) : 0;
		if (irq > 0 && !request_irq(irq, mdla_isr, IRQF_NO_AUTOEN, names[i], &mdla_done[i]))
			mdla_irq[i] = irq;
	}
	of_node_put(np);
	if (!mdla_irq[0] || !mdla_irq[1])
		pr_info("cezanne-apu: MDLA%s polled\n", !mdla_irq[0] && !mdla_irq[1] ? "s" :
			!mdla_irq[0] ? "0" : "1");
}

static void mdla_free_irqs(void)
{
	int i;

	for (i = 0; i < 2; i++)
		if (mdla_irq[i]) {
			free_irq(mdla_irq[i], &mdla_done[i]);
			mdla_irq[i] = 0;
		}
}

static int __init cz_init(void)
{
	struct device_node *np;
	int ret;

	spm = ioremap(SPM_BASE, 0x1000);
	rpc = ioremap(RPC_BASE, 0x1000);
	conn = ioremap(APU_CONN_BASE, 0x1000);
	vcore = ioremap(APU_VCORE_BASE, 0x1000);
	mcfg[0] = ioremap(MDLA0_CFG_BASE, 0x1000);
	mcmd[0] = ioremap(MDLA0_CMD_BASE, 0x1000);
	mcfg[1] = ioremap(MDLA1_CFG_BASE, 0x1000);
	mcmd[1] = ioremap(MDLA1_CMD_BASE, 0x1000);
	cores = clamp(cores, 1u, 2u);
	if (!spm || !rpc || !conn || !vcore || !mcfg[0] || !mcmd[0] || !mcfg[1] || !mcmd[1]) {
		unmap_all();
		return -ENOMEM;
	}

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6885-topckgen");
	if (!np) {
		unmap_all();
		return -ENODEV;
	}
	clk_dsp = top_clk(np, TOP_MUX_DSP);
	clk_dsp6 = top_clk(np, TOP_MUX_DSP6);
	clk_dsp7 = top_clk(np, TOP_MUX_DSP7);
	clk_ipu_if = top_clk(np, TOP_MUX_IPU_IF);
	clk_apupll = top_clk(np, TOP_APUPLL_CK);
	clk_univpll_d5 = top_clk(np, TOP_UNIVPLL_D5);
	of_node_put(np);
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6885-apmixedsys");
	clk_apupll_pll = np ? top_clk(np, APMIXED_APUPLL) : ERR_PTR(-ENODEV);
	of_node_put(np);
	if (IS_ERR(clk_dsp) || IS_ERR(clk_dsp6) || IS_ERR(clk_dsp7) || IS_ERR(clk_ipu_if) ||
	    IS_ERR(clk_apupll) || IS_ERR(clk_apupll_pll) || IS_ERR(clk_univpll_d5)) {
		pr_err("cezanne-apu: topckgen / apmixedsys clocks missing\n");
		ret = -ENODEV;
		goto fail;
	}
	reg_vvpu = rail("VPROC1");
	reg_vmdla = rail("VPROC2");
	reg_vsram = rail("VSRAM_MD");

	pdev = platform_device_register_simple("cezanne-apu", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		pdev = NULL;
		goto fail;
	}
	ret = apu_pool();
	if (ret)
		goto fail;
	mdla_irqs();
	cz_misc.parent = &pdev->dev;
	ret = misc_register(&cz_misc);
	if (ret)
		goto fail;
	return 0;

fail:
	mdla_free_irqs();
	if (own_pool)
		of_reserved_mem_device_release(&pdev->dev);
	if (pdev)
		platform_device_unregister(pdev);
	put_clocks();
	unmap_all();
	return ret;
}

static void __exit cz_exit(void)
{
	misc_deregister(&cz_misc);
	mdla_free_irqs();
	if (own_pool)
		of_reserved_mem_device_release(&pdev->dev);
	platform_device_unregister(pdev);
	put_clocks();
	unmap_all();
}

module_init(cz_init);
module_exit(cz_exit);
MODULE_DESCRIPTION("MT6885 APU MDLA cores for userspace (cezanne)");
MODULE_LICENSE("GPL");
