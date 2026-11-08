// SPDX-License-Identifier: GPL-2.0
/*
 * MT6885 MM SRAM (0x160000 bytes at 0x1e000000), where the encoder's NBM
 * engines keep their data. As the BSP's slbc_power_on -> mmsram_power_on:
 * the MDP domain and the SRAM clocks, then the controller (its registers
 * are secure except between two SMCs) and the SRAM's own SMI common, which
 * the BSP programs in smi_bus_prepare_enable for larbs 7 and 8. Routing the
 * NBM larb ports to that common is the encoder's part (venc_drv.c).
 */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "venc.h"

#define MTK_SIP_MMSRAM_CONTROL	0xc200051d	/* SMC64 SiP (BSP mtk_secure_api.h) */
#define MMSRAM_ENABLE_SECURE	0

/* controller (BSP mmsram.c) */
#define SRAM_INTEN0		0x000
#define SRAM_INTEN1		0x004
#define SRAM_INSTA0		0x010
#define SRAM_INSTA1		0x014
#define SRAM_SEC_ADDR0		0x040	/* BIT(24) | size: all of it non-secure */
#define SRAM_APC0_LATCH_EN	0x090	/* first violation latches */
#define SRAM_APC0_VIO_ADDR	0x0a4
#define SRAM_APC0_VIO_ID	0x0a8
#define SRAM_APC1_LATCH_EN	0x0c0
#define SRAM_MPU0_LATCH_EN	0x110
#define SRAM_MPU0_VIO_ADDR	0x124
#define SRAM_MPU0_VIO_ID	0x128
#define SRAM_MPU1_LATCH_EN	0x140

/* the SRAM's SMI common (BSP smi_sram_comm_conf_pair) */
#define SMI_L1LEN		0x100
#define SMI_BUS_SEL		0x220
#define SMI_DCM			0x300
#define SMI_DUMMY		0x444

struct venc_sram {
	struct device *dev;
	void __iomem *ctrl, *smi;
	u32 size;
	int irq;
	struct clk_bulk_data clks[3];
	struct mutex lock;
	unsigned int users;
};

/* the controller's registers are reachable between secure(false) and (true) */
static int venc_sram_secure(bool on)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_MMSRAM_CONTROL, MMSRAM_ENABLE_SECURE, on, 0, 0, 0, 0, 0, &res);
	return res.a0 ? -EIO : 0;
}

/* BSP init_mmsram_reg: the whole SRAM non-secure, violations latched and raised */
static int venc_sram_init(struct venc_sram *s, bool on)
{
	int ret = venc_sram_secure(false);

	if (ret)
		return ret;
	if (on) {
		writel(BIT(24) | s->size, s->ctrl + SRAM_SEC_ADDR0);
		writel(0, s->ctrl + SRAM_INSTA0);
		writel(0, s->ctrl + SRAM_INSTA1);
		writel(0x20, s->ctrl + SRAM_APC0_LATCH_EN);
		writel(0x20, s->ctrl + SRAM_APC1_LATCH_EN);
		writel(0xff0, s->ctrl + SRAM_MPU0_LATCH_EN);
		writel(0xff0, s->ctrl + SRAM_MPU1_LATCH_EN);
		writel(0x6ff0e0e0, s->ctrl + SRAM_INTEN0);
		writel(0x000037f8, s->ctrl + SRAM_INTEN1);
	} else {
		writel(0, s->ctrl + SRAM_INTEN0);
		writel(0, s->ctrl + SRAM_INTEN1);
		writel(0, s->ctrl + SRAM_INSTA0);
		writel(0, s->ctrl + SRAM_INSTA1);
	}
	return venc_sram_secure(true);
}

int venc_sram_on(struct venc_sram *s)
{
	int ret = 0;

	mutex_lock(&s->lock);
	if (s->users++)
		goto out;
	ret = pm_runtime_resume_and_get(s->dev);
	if (ret)
		goto err;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(s->clks), s->clks);
	if (ret)
		goto err_pm;
	ret = venc_sram_init(s, true);
	if (ret) {
		dev_err(s->dev, "MMSRAM_CONTROL SMC failed: SRAM not set up\n");
		goto err_clk;
	}
	writel(0x2, s->smi + SMI_L1LEN);
	writel(0x1111, s->smi + SMI_BUS_SEL);
	writel(0x4f1, s->smi + SMI_DCM);
	writel(0x1, s->smi + SMI_DUMMY);
	enable_irq(s->irq);
	goto out;

err_clk:
	clk_bulk_disable_unprepare(ARRAY_SIZE(s->clks), s->clks);
err_pm:
	pm_runtime_put(s->dev);
err:
	s->users--;
out:
	mutex_unlock(&s->lock);
	return ret;
}

void venc_sram_off(struct venc_sram *s)
{
	mutex_lock(&s->lock);
	if (!WARN_ON(!s->users) && !--s->users) {
		disable_irq(s->irq);
		venc_sram_init(s, false);
		clk_bulk_disable_unprepare(ARRAY_SIZE(s->clks), s->clks);
		pm_runtime_put(s->dev);
	}
	mutex_unlock(&s->lock);
}

static irqreturn_t venc_sram_irq(int irq, void *data)
{
	struct venc_sram *s = data;

	mutex_lock(&s->lock);
	if (s->users && !venc_sram_secure(false)) {
		dev_err_ratelimited(s->dev,
				    "access violation: status %08x %08x, apc0 %08x id %05x, mpu0 %06x id %05x\n",
				    readl(s->ctrl + SRAM_INSTA0), readl(s->ctrl + SRAM_INSTA1),
				    readl(s->ctrl + SRAM_APC0_VIO_ADDR),
				    readl(s->ctrl + SRAM_APC0_VIO_ID) & 0xfffff,
				    readl(s->ctrl + SRAM_MPU0_VIO_ADDR) & 0x1fffff,
				    readl(s->ctrl + SRAM_MPU0_VIO_ID) & 0xfffff);
		writel(0, s->ctrl + SRAM_INSTA0);
		writel(0, s->ctrl + SRAM_INSTA1);
		venc_sram_secure(true);
	}
	mutex_unlock(&s->lock);
	return IRQ_HANDLED;
}

/* the encoder's SRAM (DT cezanne,mmsram); -EPROBE_DEFER until it is bound */
struct venc_sram *venc_sram_find(struct device *dev)
{
	struct device_node *np = of_parse_phandle(dev->of_node, "cezanne,mmsram", 0);
	struct platform_device *pdev;
	struct venc_sram *s;

	if (!np)
		return ERR_PTR(-ENODEV);
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);
	s = platform_get_drvdata(pdev);
	if (!s || !device_link_add(dev, &pdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		s = ERR_PTR(-EPROBE_DEFER);
	put_device(&pdev->dev);
	return s;
}

static int venc_sram_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct venc_sram *s;
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = dev;
	mutex_init(&s->lock);
	s->ctrl = devm_platform_ioremap_resource_byname(pdev, "ctrl");
	if (IS_ERR(s->ctrl))
		return PTR_ERR(s->ctrl);
	s->smi = devm_platform_ioremap_resource_byname(pdev, "smi");
	if (IS_ERR(s->smi))
		return PTR_ERR(s->smi);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram");
	if (!res)
		return -EINVAL;
	s->size = resource_size(res);
	s->clks[0].id = "smi";
	s->clks[1].id = "gals";
	s->clks[2].id = "sram";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(s->clks), s->clks);
	if (ret)
		return ret;
	s->irq = platform_get_irq(pdev, 0);
	if (s->irq < 0)
		return s->irq;
	ret = devm_request_threaded_irq(dev, s->irq, NULL, venc_sram_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN, "mmsram", s);
	if (ret)
		return ret;
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, s);
	dev_info(dev, "%u KiB at %pa\n", s->size / SZ_1K, &res->start);
	return 0;
}

static const struct of_device_id venc_sram_of_match[] = {
	{ .compatible = "cezanne,mt6885-mmsram" },
	{ }
};
MODULE_DEVICE_TABLE(of, venc_sram_of_match);

struct platform_driver venc_sram_driver = {
	.probe = venc_sram_probe,
	.driver = {
		.name = "cezanne-mmsram",
		.of_match_table = venc_sram_of_match,
		.suppress_bind_attrs = true,
	},
};
