// SPDX-License-Identifier: GPL-2.0
/*
 * apuprobe: first power-up of the MT6885 APU (AI accelerator) and its MDLA0
 * core, the way the BSP's apusys power driver does it
 * (apusys/power/2.0/mt6885/hal_config_power.c, apu_power_clock.c,
 * clk-mt6885-pg.c spm_mtcmos_vpu). Bring-up test: powers the APU conn and
 * MDLA0 at load, prints the MDLA's id registers, stays loaded with a
 * debugfs peek / poke file, powers everything down again at unload.
 *
 * The rails (MT6359 VPROC1 = vvpu, VPROC2 = vmdla, VSRAM_MD = vsram_apu)
 * are left as lk set them (0.75 / 0.75 / 0.85 V, above the BSP's 0.575 /
 * 0.575 / 0.75 V defaults); only a handle is held so they stay on.
 *
 *   echo 0x19036500 > /sys/kernel/debug/apuprobe/peek; cat .../peek
 *   echo "0x19036500 0x1" > .../peek      (write)
 *
 * Commands run from a DMA buffer (mem_mb, CMA, below 4 GiB, physical
 * addresses: the APU IOMMU is left alone): "mem" reads / writes it at the
 * file offset, "mem_addr" is its bus address, "run" takes "<command offset
 * (hex)> <count> [core]" or "<offset0> <count0> <offset1> <count1>" (both
 * cores at once), issues them and polls until the last one is done; reading
 * "run" gives the result. Both MDLA cores (cores=2) run from APUPLL at
 * mdla_mhz (BSP OPP: 700 MHz at the 0.75 V lk leaves on VPROC2).
 */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>

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

/* address bits 33:32 the reviser adds to MDLA / VPU / EDMA accesses: 0
 * for physical addresses below 4 GiB (MIUI: 3, its IOMMU's 34-bit IOVAs) */
static int boundary;
module_param(boundary, int, 0444);

/* MDLA_AXI_CTRL bits 1 + 10, which the BSP sets for its ION (IOMMU) buffers */
static int axi_ion;
module_param(axi_ion, int, 0444);

/* command / data buffer (CMA, below 4 GiB) in MiB: networks need more than
 * the test patterns (x4plus int16 weights alone are 33 MB) */
static uint mem_mb = 8;
module_param(mem_mb, uint, 0444);
#define MEM_SIZE		((size_t)mem_mb << 20)

/* MDLA cores to power (1 or 2) */
static uint cores = 2;
module_param(cores, uint, 0444);

/* MDLA clock (dsp6_sel, both cores) from APUPLL in MHz, 0: leave the mux
 * as it is (univpll_d6_d2, 208 MHz) */
static uint mdla_mhz = 700;
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
static struct dentry *dir;
static bool conn_on, mdla_on[2];
static u32 peek_addr, peek_val;
static struct platform_device *pdev;
static void *mem;
static dma_addr_t mem_dma;
static char run_result[320];

static struct clk *top_clk(struct device_node *np, int id)
{
	struct of_phandle_args args = { .np = np, .args_count = 1, .args = { id } };

	return of_clk_get_from_provider(&args);
}

static struct regulator *rail(const char *name)
{
	struct regulator *r = regulator_get_optional(NULL, name);

	if (IS_ERR(r)) {
		pr_warn("apuprobe: no regulator %s (%ld), left as is\n", name, PTR_ERR(r));
		return NULL;
	}
	if (regulator_enable(r)) {
		regulator_put(r);
		return NULL;
	}
	pr_info("apuprobe: %s %d uV\n", name, regulator_get_voltage(r));
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
		pr_err("apuprobe: domain %d stays %s (INTF_PWR_RDY %#x)\n",
		       dom, on ? "off" : "on", v);
	return ret;
}

static void rpc_fifo_wait(void)
{
	u32 v;

	if (readl_poll_timeout_atomic(rpc + RPC_TOP_CON, v, !(v & BIT(31)), 10, 5000))
		pr_err("apuprobe: RPC fifo busy (TOP_CON %#x)\n", v);
}

static void dump(const char *when)
{
	pr_info("apuprobe: %s: SPM OTHER_PWR_STATUS %#x CROSS_WAKE %#x EXT_BUCK_ISO %#x; RPC TOP_CON %#x TOP_SEL %#x INTF_PWR_RDY %#x\n",
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
	pr_info("apuprobe: SMC infra2apu sram %#lx, boundary %d: mdla %#lx vpu %#lx edma %#lx\n",
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
		pr_err("apuprobe: SPM did not power the APU (OTHER_PWR_STATUS %#x)\n", v);
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
	return 0;
}

/* mdla_multi_core_sw_rst(): what the BSP does to a core that timed out */
static void mdla_soft_reset(int i)
{
	writel(0xff, mcfg[i] + MDLA_SW_RST);
	udelay(10);
	writel(0, mcfg[i] + MDLA_SW_RST);
	mdla_init(i);
}

/* mdla_reset(): engines' DCM / CG defaults, the done IRQ unmasked, AXI
 * ctrl as the BSP sets it for ION buffers only with axi_ion */
static void mdla_init(int i)
{
	writel(0xffffffff, mcfg[i] + CG_CLR);
	writel(MDLA_IRQ_MASK & ~MDLA_IRQ_SWCMD_DONE, mcmd[i] + MREG_TOP_G_INTP2);
	writel(0, mcmd[i] + MREG_TOP_ENG0);
	writel(0, mcmd[i] + MREG_TOP_ENG1);
	writel(0, mcmd[i] + MREG_TOP_ENG2);
	writel(0x600, mcmd[i] + MREG_TOP_ENG11);
	pr_info("apuprobe: MDLA%d AXI_CTRL %#x AXI1_CTRL %#x at reset\n", i,
		readl(mcfg[i] + MDLA_AXI_CTRL), readl(mcfg[i] + MDLA_AXI1_CTRL));
	if (axi_ion) {
		writel(readl(mcfg[i] + MDLA_AXI_CTRL) | MDLA_AXI_CTRL_MASK, mcfg[i] + MDLA_AXI_CTRL);
		writel(readl(mcfg[i] + MDLA_AXI1_CTRL) | MDLA_AXI_CTRL_MASK, mcfg[i] + MDLA_AXI1_CTRL);
	}
}

static void mdla_power_off(int i)
{
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
	int i, ret;

	for (i = 0; fast_bus && i < ARRAY_SIZE(bus); i++) {
		bus_parent[i] = clk_get_parent(bus[i]);
		ret = clk_set_parent(bus[i], clk_univpll_d5);
		pr_info("apuprobe: %s %lu Hz (%d)\n", __clk_get_name(bus[i]),
			clk_get_rate(bus[i]), ret);
		if (ret)
			return ret;
	}
	if (!mdla_mhz)
		return 0;
	dsp6_parent = clk_get_parent(clk_dsp6);
	apupll_rate0 = clk_get_rate(clk_apupll_pll);
	ret = clk_set_rate(clk_apupll_pll, 2 * mdla_mhz * 1000000UL);
	if (!ret)
		ret = clk_set_parent(clk_dsp6, clk_apupll);
	pr_info("apuprobe: MDLA clock: apupll %lu Hz, dsp6 %lu Hz (%d)\n",
		clk_get_rate(clk_apupll_pll), clk_get_rate(clk_dsp6), ret);
	return ret;
}

/* debugfs peek / poke inside the powered APU (0x19000000-0x190fffff) and
 * its VLM SRAM (0x1d000000-0x1d0fffff) */
static bool addr_ok(u32 a)
{
	return !(a & 3) && ((a >= 0x19000000 && a < 0x19100000) ||
			    (a >= 0x1d000000 && a < 0x1d100000));
}

static ssize_t peek_write(struct file *f, const char __user *ubuf, size_t len, loff_t *off)
{
	char buf[48];
	void __iomem *p;
	u32 a, v;
	int n;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	n = sscanf(buf, "%x %x", &a, &v);
	if (n < 1 || !addr_ok(a) || !conn_on)
		return -EINVAL;
	p = ioremap(a & PAGE_MASK, PAGE_SIZE);
	if (!p)
		return -ENOMEM;
	if (n == 2)
		writel(v, p + (a & ~PAGE_MASK));
	peek_addr = a;
	peek_val = readl(p + (a & ~PAGE_MASK));
	iounmap(p);
	return len;
}

static ssize_t peek_read(struct file *f, char __user *ubuf, size_t len, loff_t *off)
{
	char buf[32];
	int n = scnprintf(buf, sizeof(buf), "%08x %08x\n", peek_addr, peek_val);

	return simple_read_from_buffer(ubuf, len, off, buf, n);
}

static const struct file_operations peek_fops = {
	.owner = THIS_MODULE,
	.write = peek_write,
	.read = peek_read,
};

static ssize_t mem_read(struct file *f, char __user *ubuf, size_t len, loff_t *off)
{
	return simple_read_from_buffer(ubuf, len, off, mem, MEM_SIZE);
}

static ssize_t mem_write(struct file *f, const char __user *ubuf, size_t len, loff_t *off)
{
	return simple_write_to_buffer(mem, MEM_SIZE, off, ubuf, len);
}

static const struct file_operations mem_fops = {
	.owner = THIS_MODULE,
	.read = mem_read,
	.write = mem_write,
	.llseek = default_llseek,
};

static ssize_t mem_addr_read(struct file *f, char __user *ubuf, size_t len, loff_t *off)
{
	char buf[24];
	int n = scnprintf(buf, sizeof(buf), "%llx\n", (u64)mem_dma);

	return simple_read_from_buffer(ubuf, len, off, buf, n);
}

static const struct file_operations mem_addr_fops = {
	.owner = THIS_MODULE,
	.read = mem_addr_read,
};

/* mdla_issue_ce() on core i (polled instead of the IRQ) */
static int mdla_issue(int i, u32 off, u32 count)
{
	u32 intp;

	if (i >= cores || !mdla_on[i] || !count || off & 0x3f || off + count * 0x1c0 > MEM_SIZE)
		return -EINVAL;
	intp = readl(mcmd[i] + MREG_TOP_G_INTP0);
	if (!(intp & MDLA_IRQ_CDMA_FIFO_EMPTY)) {
		scnprintf(run_result, sizeof(run_result), "busy: MDLA%d INTP0 %#x\n", i, intp);
		return -EBUSY;
	}
	writel(intp, mcmd[i] + MREG_TOP_G_INTP0);	/* clear stale status */
	wmb();
	writel(mem_dma + off, mcmd[i] + MREG_TOP_G_CDMA1);
	writel(count, mcmd[i] + MREG_TOP_G_CDMA2);
	writel(1, mcmd[i] + MREG_TOP_G_CDMA3);
	return 0;
}

/* the last command carries SWCMD_FINISH_INT_EN; FIN3 is the finished
 * command's id (patterns reuse ids, so it is no completion test) */
static int mdla_wait(int i, u32 count, ktime_t t0, int *len)
{
	u32 intp, fin;
	int ret;

	ret = readl_poll_timeout(mcmd[i] + MREG_TOP_G_INTP0, intp,
				 intp & MDLA_IRQ_SWCMD_DONE, 20, 2000000);
	fin = readl(mcmd[i] + MREG_TOP_G_FIN3);
	*len += scnprintf(run_result + *len, sizeof(run_result) - *len,
			  "%s%s: fin %u/%u in %lld us, INTP0 %#x CDMA4 %#x IDLE %#x FIN0 %#x",
			  *len ? "; MDLA1 " : "", ret ? "timeout" : "done", fin, count,
			  ktime_us_delta(ktime_get(), t0), intp,
			  readl(mcmd[i] + MREG_TOP_G_CDMA4), readl(mcmd[i] + MREG_TOP_G_IDLE),
			  readl(mcmd[i] + MREG_TOP_G_FIN0));
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
static int mdla_run(const u32 *off, const u32 *n)
{
	ktime_t t0 = ktime_get();
	int i, ret = 0, len = 0;

	for (i = 0; i < 2; i++)
		if (n[i]) {
			ret = mdla_issue(i, off[i], n[i]);
			if (ret)
				return ret;
		}
	for (i = 0; i < 2; i++)
		if (n[i])
			ret = mdla_wait(i, n[i], t0, &len) ?: ret;
	if (len < sizeof(run_result) - 1)
		strcpy(run_result + len, "\n");
	return ret;
}

static ssize_t run_write(struct file *f, const char __user *ubuf, size_t len, loff_t *off)
{
	char buf[48];
	u32 a[4], at[2] = { 0 }, n[2] = { 0 };
	int k, ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	k = sscanf(buf, "%x %u %x %u", &a[0], &a[1], &a[2], &a[3]);
	if (k == 2 || (k == 3 && a[2] < 2)) {	/* "off n [core]" */
		at[k == 3 ? a[2] : 0] = a[0];
		n[k == 3 ? a[2] : 0] = a[1];
	} else if (k == 4) {			/* both cores */
		at[0] = a[0]; n[0] = a[1];
		at[1] = a[2]; n[1] = a[3];
	} else {
		return -EINVAL;
	}
	ret = mdla_run(at, n);
	return ret == -EINVAL ? ret : len;
}

static ssize_t run_read(struct file *f, char __user *ubuf, size_t len, loff_t *off)
{
	return simple_read_from_buffer(ubuf, len, off, run_result, strlen(run_result));
}

static const struct file_operations run_fops = {
	.owner = THIS_MODULE,
	.write = run_write,
	.read = run_read,
};

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
		writel((u32)((mem_dma + MEM_SIZE - PAGE_SIZE) >> 1), p + REG_MMU_IVRP_PADDR);
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

static void put_all(void)
{
	struct clk *c[] = { clk_ipu_if, clk_dsp, clk_dsp7, clk_dsp6 };
	struct regulator *r[] = { reg_vsram, reg_vmdla, reg_vvpu };
	int i;

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
	if (!IS_ERR_OR_NULL(clk_apupll))
		clk_put(clk_apupll);
	if (!IS_ERR_OR_NULL(clk_apupll_pll))
		clk_put(clk_apupll_pll);
	if (!IS_ERR_OR_NULL(clk_univpll_d5))
		clk_put(clk_univpll_d5);
	for (i = 0; i < ARRAY_SIZE(c); i++)
		if (!IS_ERR_OR_NULL(c[i]))
			clk_put(c[i]);
	for (i = 0; i < ARRAY_SIZE(r); i++)
		if (r[i]) {
			regulator_disable(r[i]);
			regulator_put(r[i]);
		}
}

static void free_mem(void)
{
	if (mem)
		dma_free_coherent(&pdev->dev, MEM_SIZE, mem, mem_dma);
	mem = NULL;
	if (pdev)
		platform_device_unregister(pdev);
	pdev = NULL;
}

static int __init apuprobe_init(void)
{
	struct device_node *top;
	int i, ret;

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

	top = of_find_compatible_node(NULL, NULL, "mediatek,mt6885-topckgen");
	if (!top) {
		unmap_all();
		return -ENODEV;
	}
	clk_dsp = top_clk(top, TOP_MUX_DSP);
	clk_dsp6 = top_clk(top, TOP_MUX_DSP6);
	clk_dsp7 = top_clk(top, TOP_MUX_DSP7);
	clk_ipu_if = top_clk(top, TOP_MUX_IPU_IF);
	clk_apupll = top_clk(top, TOP_APUPLL_CK);
	clk_univpll_d5 = top_clk(top, TOP_UNIVPLL_D5);
	of_node_put(top);
	top = of_find_compatible_node(NULL, NULL, "mediatek,mt6885-apmixedsys");
	clk_apupll_pll = top ? top_clk(top, APMIXED_APUPLL) : ERR_PTR(-ENODEV);
	of_node_put(top);
	if (IS_ERR(clk_dsp) || IS_ERR(clk_dsp6) || IS_ERR(clk_dsp7) || IS_ERR(clk_ipu_if) ||
	    IS_ERR(clk_apupll) || IS_ERR(clk_apupll_pll) || IS_ERR(clk_univpll_d5)) {
		pr_err("apuprobe: topckgen clocks missing\n");
		ret = -ENODEV;
		goto fail_noclk;
	}

	reg_vvpu = rail("VPROC1");
	reg_vmdla = rail("VPROC2");
	reg_vsram = rail("VSRAM_MD");

	pr_info("apuprobe: SPM before clocks: OTHER_PWR_STATUS %#x CROSS_WAKE %#x EXT_BUCK_ISO %#x\n",
		readl(spm + OTHER_PWR_STATUS), readl(spm + SPM_CROSS_WAKE_M01_REQ),
		readl(spm + EXT_BUCK_ISO));

	/* vcore + conn clock sources (enable_apu_vcore/conn_clksrc) */
	for (; clks_on < 3; clks_on++) {
		ret = clk_prepare_enable((struct clk *[]){ clk_ipu_if, clk_dsp, clk_dsp7 }[clks_on]);
		if (ret)
			goto fail;
	}
	pr_info("apuprobe: dsp %lu dsp6 %lu dsp7 %lu ipu_if %lu Hz\n",
		clk_get_rate(clk_dsp), clk_get_rate(clk_dsp6),
		clk_get_rate(clk_dsp7), clk_get_rate(clk_ipu_if));
	dump("clocks on");
	ret = mdla_clock();
	if (ret)
		goto fail;

	if (readl(rpc + RPC_INTF_PWR_RDY) & BIT(DOM_CONN)) {
		/* a hung MDLA kept the APU up through the last unload: take the
		 * conn over, reset and power-cycle MDLA0 */
		pr_warn("apuprobe: APU still powered, adopting it\n");
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
		for (i = 0, ret = 0; i < cores && !ret; i++)
			ret = mdla_power_on(i);
		dump("adopted");
		if (ret)
			goto fail;
		goto powered;
	}

	ret = rpc_init();
	dump("rpc init");
	if (ret)
		goto fail;

	ret = conn_power_on();
	dump("conn on");
	if (ret)
		goto fail;
	pr_info("apuprobe: CG vcore %#x conn %#x\n", readl(vcore + CG_CON), readl(conn + CG_CON));

	ret = clk_prepare_enable(clk_dsp6);
	if (ret)
		goto fail;
	clks_on++;
	for (i = 0, ret = 0; i < cores && !ret; i++)
		ret = mdla_power_on(i);
	dump("mdla on");
	if (ret)
		goto fail;
powered:

	for (i = 0; i < cores; i++)
		pr_info("apuprobe: MDLA%d CG_CON %#x G_REV %#x COREINFO %#x IDLE %#x INTP0 %#x INTP2 %#x CDMA0 %#x\n",
			i, readl(mcfg[i] + CG_CON), readl(mcmd[i] + MREG_TOP_G_REV),
			readl(mcmd[i] + MREG_TOP_G_COREINFO), readl(mcmd[i] + MREG_TOP_G_IDLE),
			readl(mcmd[i] + MREG_TOP_G_INTP0), readl(mcmd[i] + MREG_TOP_G_INTP2),
			readl(mcmd[i] + MREG_TOP_G_CDMA0));

	pdev = platform_device_register_simple("apuprobe", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		pdev = NULL;
		goto fail;
	}
	ret = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto fail;
	mem = dma_alloc_coherent(&pdev->dev, MEM_SIZE, &mem_dma, GFP_KERNEL);
	if (!mem) {
		ret = -ENOMEM;
		goto fail;
	}
	iommu_ivrp();
	pr_info("apuprobe: %zu KiB command / data buffer at %pad\n", MEM_SIZE / 1024, &mem_dma);

	dir = debugfs_create_dir("apuprobe", NULL);
	debugfs_create_file("peek", 0600, dir, NULL, &peek_fops);
	debugfs_create_file_size("mem", 0600, dir, NULL, &mem_fops, MEM_SIZE);
	debugfs_create_file("mem_addr", 0400, dir, NULL, &mem_addr_fops);
	debugfs_create_file("run", 0600, dir, NULL, &run_fops);
	return 0;

fail:
	free_mem();
	mdla_all_off();
	if (conn_on)
		conn_power_off();
	dump("after failure");
fail_noclk:
	put_all();
	unmap_all();
	return ret;
}

static void __exit apuprobe_exit(void)
{
	debugfs_remove_recursive(dir);
	free_mem();
	mdla_all_off();
	if (conn_on)
		conn_power_off();
	dump("off");
	put_all();
	unmap_all();
}

module_init(apuprobe_init);
module_exit(apuprobe_exit);
MODULE_LICENSE("GPL");
