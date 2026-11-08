// SPDX-License-Identifier: GPL-2.0
/*
 * DPMAIF: the MD's packet data path (BSP hif/ccci_hif_dpmaif.c and
 * dpmaif_drv.c, MT6297 paths), one WWAN network link per MD network
 * interface (the BSP's ccmniN). A link starts on the interface of its link
 * id; the MD picks a new interface for each activation ("+EIF: <tx><if>,
 * ifup" on the AT port), so the bearer moves the link there through
 * /sys/class/net/<link>/md_interface.
 *
 * DL: the AP lends the HW receive buffers through the BAT ring; per packet
 * the HW writes a message PIT (interface, checksum result) and a payload PIT
 * (BAT index, length) into the PIT ring. Buffers come back out of order: a
 * BAT slot is lent again once every slot before it is back. The MD also
 * places "ignore" PITs in the ring (its reorder workaround), which the AP
 * hands back in batches when one of them asks for it.
 * UL: per packet a message DRB (length, interface) and a payload DRB in the
 * DRB ring of queue 0; the HW's read index frees them.
 */
#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/if_arp.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/wwan.h>

#include "ccci.h"

#define DPMAIF_AO_PHYS		0x10014000
#define DPMAIF_PD_PHYS		0x1022d000
#define DPMAIF_MD_MISC_PHYS	0x1022c000
#define INFRA_AO_MEM_PHYS	0x10002000
#define DPMAIF_SPI		209

/* AO block: UL at +0, DL at +0x400, MD DL config at +0x800 */
#define AO_UL_CH_STA(q)		(0x000 + 4 * (q))	/* [31:16] read index (words) */
#define AO_UL_CHNL_ARB0		0x01c	/* [3:0] queue ready, [11:8] arbitration */
#define AO_UL_CHNL_CON0(q)	(0x020 + 0x10 * (q))	/* DRB base [31:0] */
#define AO_UL_CHNL_CON1(q)	(0x024 + 0x10 * (q))	/* [31:24] base [39:32], [15:0] words */
#define AO_UL_AP_L2TIMR0	0x080	/* UL interrupt mask */
#define AO_UL_AP_L2TIMCR0	0x084	/* ... clear bits */
#define AO_UL_AP_L2TIMSR0	0x088	/* ... set bits */
#define AO_UL_AP_L1TIMR0	0x08c
#define AO_UL_APDL_L2TIMR0	0x090	/* DL interrupt mask */
#define AO_UL_APDL_L2TIMCR0	0x094
#define AO_UL_APDL_L2TIMSR0	0x098
#define AO_UL_IP_BUSY_MASK	0x09c
#define AO_DL_PKTINFO_CON0	0x400	/* [31:16] BATs per packet, [15:8] remain min / 16, [7:0] RB PIT check */
#define AO_DL_PKTINFO_CON1	0x404	/* MTU */
#define AO_DL_PKTINFO_CON2	0x408	/* [31:24] PIT check, [16:8] buffer / 128, [7:0] reserved */
#define AO_DL_RDY_CHK_THRES	0x40c	/* [31] csum, [23:22] align, [21:16] BAT check, [13] perf, [6:0] irq mask */
#define AO_DL_PIT_STA0		0x420
#define AO_DL_PIT_STA1		0x424	/* [15:0] size */
#define AO_DL_PIT_STA2		0x428	/* [31:16] read, [15:0] write index */
#define AO_DL_PIT_STA3		0x42c	/* [31:16] MD (dummy) index, [0] busy */
#define AO_MISC_CFG1		0x804	/* [0] MD address remap on */
#define AO_REMAP_DOMAIN		0x820
#define AO_REMAP_CACHE		0x824
#define AO_REMAP_BANK(i)	(0x830 + 4 * (i))	/* banks 0 and 1, maps A-D */

/* PD block: UL at +0, DL at +0x100, WDMA at +0x300, AP misc at +0x400 */
#define PD_UL_RESERVE_RW	0x0a8	/* dummy PIT count for the MD */
#define PD_UL_RESERVE_AO_RW	0x0ac
#define PD_UL_ADD_DESC_CH(q)	(0x0b0 + 4 * (q))
#define PD_DL_BAT_INIT		0x100
#define PD_DL_BAT_ADD		0x104
#define PD_DL_BAT_INIT_CON0	0x108	/* base [31:0] */
#define PD_DL_BAT_INIT_CON1	0x10c	/* [31:24] base [39:32], [22] perf, [16] on, [15:0] size */
#define PD_DL_PIT_INIT		0x120
#define PD_DL_PIT_ADD		0x124
#define PD_DL_PIT_INIT_CON0	0x128
#define PD_DL_PIT_INIT_CON1	0x12c
#define PD_DL_PIT_INIT_CON2	0x130
#define PD_DL_PIT_INIT_CON3	0x134	/* [31:16] MD (dummy) index, [0] on */
#define PD_DL_RESERVE_RW	0x1a8
#define PD_DL_DBG_STA1		0x1b4	/* [25] idle */
#define PD_DL_DBG_STA7		0x1cc	/* FIFO push [25:20] / pop [10:5] index, idle [16] [0] */
#define PD_DL_FIFO_STA		0x358	/* [0] idle */
#define PD_WDMA_WR_CMD_CON0	0x300
#define PD_WDMA_WR_CHNL_CON3	0x314
#define PD_AP_UL_L2TISAR0	0x400	/* UL interrupt status, write 1 to clear */
#define PD_AP_DL_L2TISAR0	0x450	/* DL interrupt status */
#define PD_AP_IP_BUSY		0x460
#define PD_AP_CG_EN		0x468

#define MD_L1TIMSR0		0x01c	/* MD misc */
#define MD_DL_RB_PIT_INIT	0x100

#define INFRA_RST0_AO		0x140
#define INFRA_RST1_AO		0x144
#define INFRA_RST0_PD		0x150
#define INFRA_RST1_PD		0x154
#define INFRA_DPMAIF_CTRL	0xc00
#define INFRA_PROT_DPMAIF	BIT(10)		/* infracfg_ao_mem + 0 */

#define HW_BUSY			BIT(31)		/* *_ADD, *_INIT: write pending */
#define INIT_ALLSET		BIT(0)		/* *_INIT: all settings, else enable bit only */

#define UL_INT_DONE(q)		BIT(q)
#define UL_INT_EN		(GENMASK(4, 0) | GENMASK(24, 10))	/* done, MD / power not ready, length */
#define UL_INT_LEN_ERR		GENMASK(24, 20)
#define DL_INT_DONE		BIT(0)
#define DL_INT_EN		(BIT(0) | BIT(1) | BIT(6))	/* done, buffer length, MTU */

#define BAT_ENTRIES		8192
#define PIT_ENTRIES		(2 * BAT_ENTRIES)
#define DRB_ENTRIES		2048
#define RX_BUFS			1024	/* buffers lent to the HW at a time */
#define RX_BUF_SIZE		3584
#define DL_MTU			(3 * 1024 + 8)
#define LINK_MTU_MAX		3000
#define PIT_CHK_NUM		124
#define BAT_CHK_NUM		62
#define RB_PIT_CHK_NUM		64
#define DUMMY_PIT_AIDX		1024
#define DUMMY_PIT_MAX		0x3fffff
#define MD_DUMMYPIT_EN		BIT(31)
#define NUM_LINKS		22	/* ccmni0..21 */
#define HW_TIMEOUT_US		20000

/* PIT: 4 words; w0 bit 0 tells message from payload, w3 is common */
struct dpmaif_pit {
	__le32 w[4];
};
#define PIT_MSG			BIT(0)
#define PIT_CONT		BIT(1)		/* more payload PITs follow */
#define PIT_FRAG		BIT(2)		/* fragment BAT (not used) */
#define PIT_BID			GENMASK(15, 3)
#define PIT_LEN			GENMASK(31, 16)
#define PIT_MSG_CSUM		GENMASK(3, 2)	/* 0: checksums good */
#define PIT_MSG_CH		GENMASK(23, 16)
#define PIT_MSG_DROP		BIT(31)
#define PIT_IGNORE		BIT(16)		/* w3: MD dummy entry */
#define PIT_RELOAD		BIT(23)		/* w3: hand the dummies back now */

struct dpmaif_bat {
	__le32 addr, addr_ext;
};

struct dpmaif_drb {
	__le32 w0, w1;
};
#define DRB_MSG			1		/* w0 type, else payload */
#define DRB_CONT		BIT(2)
#define DRB_ADDR_EXT		GENMASK(15, 8)
#define DRB_LEN			GENMASK(31, 16)
#define DRB_MSG_CH		GENMASK(23, 16)	/* message w1 */

struct ccci_dpmaif {
	struct ccci_md *md;
	struct device *dev;
	void __iomem *ao, *pd, *md_misc, *infra_mem;
	struct clk_bulk_data clks[2];
	int irq;
	bool running;
	struct net_device *napi_dev;
	struct napi_struct napi;

	/* DL */
	struct dpmaif_pit *pit;
	dma_addr_t pit_dma;
	u32 pit_rd;
	struct dpmaif_bat *bat;
	dma_addr_t bat_dma;
	u32 bat_wr, bat_rel;		/* lent: [bat_rel, bat_wr) */
	struct sk_buff **bat_skb;	/* NULL once the HW gave the slot back */
	dma_addr_t *bat_skb_dma;
	u8 rx_ch, rx_csum;		/* from the last message PIT */
	bool rx_drop;
	u32 dummies;
	bool reload;

	/* UL, queue 0 */
	struct dpmaif_drb *drb;
	dma_addr_t drb_dma;
	u32 drb_wr, drb_rel;
	struct sk_buff **drb_skb;	/* on payload DRBs */
	dma_addr_t *drb_skb_dma;
	spinlock_t tx_lock;
	bool tx_stopped;

	struct net_device __rcu *links[NUM_LINKS];	/* by MD interface */
};

/* netdev private data of a link */
struct dpmaif_link {
	struct ccci_dpmaif *dp;
	u32 ch;			/* MD interface */
};

/* DPMAIF_MAIN, CLDMA_BCLK (the BSP's infra-dpmaif-clk / -blk-clk) */
static const u32 dpmaif_clk_ids[] = { 105, 56 };

static u32 ring_next(u32 i, u32 size)
{
	return i + 1 == size ? 0 : i + 1;
}

static u32 ring_used(u32 rd, u32 wr, u32 size)
{
	return wr >= rd ? wr - rd : size - rd + wr;
}

static void rmw(void __iomem *reg, u32 clr, u32 set)
{
	writel((readl(reg) & ~clr) | set, reg);
}

/* *_ADD / *_INIT handshake: wait for the HW to take the last write */
static int hw_write(void __iomem *reg, u32 val, bool wait_done)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout_atomic(reg, v, !(v & HW_BUSY), 1, HW_TIMEOUT_US);
	if (ret)
		return ret;
	writel(val | HW_BUSY, reg);
	if (!wait_done)
		return 0;
	return readl_poll_timeout_atomic(reg, v, !(v & HW_BUSY), 1, HW_TIMEOUT_US);
}

static void clear_ip_busy(struct ccci_dpmaif *dp)
{
	u32 v = readl(dp->pd + PD_AP_IP_BUSY);

	if (v)
		writel(v, dp->pd + PD_AP_IP_BUSY);
}

static void dl_irq_mask(struct ccci_dpmaif *dp, bool mask)
{
	writel(DL_INT_DONE, dp->ao + (mask ? AO_UL_APDL_L2TIMSR0 : AO_UL_APDL_L2TIMCR0));
	rmw(dp->ao + AO_DL_RDY_CHK_THRES, mask ? 0 : DL_INT_DONE, mask ? DL_INT_DONE : 0);
}

static void ul_irq_mask(struct ccci_dpmaif *dp, bool mask)
{
	writel(UL_INT_DONE(0), dp->ao + (mask ? AO_UL_AP_L2TIMSR0 : AO_UL_AP_L2TIMCR0));
}

/* ---- DL ---- */

static int rx_buf_lend(struct ccci_dpmaif *dp, gfp_t gfp)
{
	u32 i = dp->bat_wr;
	struct sk_buff *skb;
	dma_addr_t addr;

	skb = __dev_alloc_skb(RX_BUF_SIZE, gfp);
	if (!skb)
		return -ENOMEM;
	addr = dma_map_single(dp->dev, skb->data, RX_BUF_SIZE, DMA_FROM_DEVICE);
	if (dma_mapping_error(dp->dev, addr)) {
		dev_kfree_skb_any(skb);
		return -ENOMEM;
	}
	dp->bat[i].addr = cpu_to_le32(lower_32_bits(addr));
	dp->bat[i].addr_ext = cpu_to_le32(upper_32_bits(addr) & 0xff);
	dp->bat_skb[i] = skb;
	dp->bat_skb_dma[i] = addr;
	dp->bat_wr = ring_next(i, BAT_ENTRIES);
	return 0;
}

/* lend a new buffer for each slot back in order; returns how many */
static u32 rx_refill(struct ccci_dpmaif *dp)
{
	u32 n = 0;

	while (dp->bat_rel != dp->bat_wr && !dp->bat_skb[dp->bat_rel]) {
		if (rx_buf_lend(dp, GFP_ATOMIC))
			break;
		dp->bat_rel = ring_next(dp->bat_rel, BAT_ENTRIES);
		n++;
	}
	return n;
}

static void rx_free_bufs(struct ccci_dpmaif *dp)
{
	u32 i;

	for (i = 0; i < BAT_ENTRIES; i++) {
		if (!dp->bat_skb[i])
			continue;
		dma_unmap_single(dp->dev, dp->bat_skb_dma[i], RX_BUF_SIZE, DMA_FROM_DEVICE);
		dev_kfree_skb_any(dp->bat_skb[i]);
		dp->bat_skb[i] = NULL;
	}
}

/* stop PIT writes and wait until the DL path has drained */
static int dl_idle(struct ccci_dpmaif *dp)
{
	void __iomem *pd = dp->pd;
	u32 v;

	rmw(pd + PD_DL_PIT_INIT_CON3, BIT(0), 0);
	if (hw_write(pd + PD_DL_PIT_INIT, 0, true) ||
	    readl_poll_timeout_atomic(dp->ao + AO_DL_PIT_STA3, v, !(v & BIT(0)), 1, HW_TIMEOUT_US) ||
	    readl_poll_timeout_atomic(pd + PD_DL_DBG_STA1, v, v & BIT(25), 1, HW_TIMEOUT_US) ||
	    readl_poll_timeout_atomic(pd + PD_DL_DBG_STA7, v,
				      ((v >> 20) & 0x3f) == ((v >> 5) & 0x3f) &&
				      (v & (BIT(16) | BIT(0))) == (BIT(16) | BIT(0)),
				      1, HW_TIMEOUT_US) ||
	    readl_poll_timeout_atomic(pd + PD_DL_FIFO_STA, v, v & BIT(0), 1, HW_TIMEOUT_US))
		return -ETIMEDOUT;
	return 0;
}

/* hand the dummy PITs seen so far back to the MD (BSP add_apit_num) */
static void rx_return_dummies(struct ccci_dpmaif *dp)
{
	void __iomem *pd = dp->pd;
	u32 sta2, rd, wr, aidx, size, room, v;

	writel(0, dp->md_misc + MD_DL_RB_PIT_INIT);
	if (dl_idle(dp)) {
		dev_err_ratelimited(dp->dev, "DL does not go idle\n");
		goto pit_on;
	}
	sta2 = readl(dp->ao + AO_DL_PIT_STA2);
	rd = sta2 >> 16;
	wr = sta2 & 0xffff;
	aidx = readl(dp->ao + AO_DL_PIT_STA3) >> 16;
	size = readl(dp->ao + AO_DL_PIT_STA1) & 0xffff;
	room = rd <= aidx ? size - aidx + rd : rd - aidx;
	if (!size || dp->dummies + PIT_CHK_NUM >= room)
		goto pit_on;	/* later, with more room */
	aidx = (aidx + dp->dummies) % size;
	writel(readl(dp->ao + AO_DL_PIT_STA0), pd + PD_DL_PIT_INIT_CON0);
	writel(readl(dp->ao + AO_DL_PIT_STA1), pd + PD_DL_PIT_INIT_CON1);
	writel(sta2, pd + PD_DL_PIT_INIT_CON2);
	writel(aidx << 16 | BIT(0), pd + PD_DL_PIT_INIT_CON3);
	if (hw_write(pd + PD_DL_PIT_INIT, INIT_ALLSET | wr << 4, true))
		dev_err_ratelimited(dp->dev, "PIT re-init timeout\n");
	v = (readl(pd + PD_UL_RESERVE_RW) & ~MD_DUMMYPIT_EN) + dp->dummies;
	if (v >= DUMMY_PIT_MAX)
		v -= DUMMY_PIT_MAX;
	writel(v | MD_DUMMYPIT_EN, pd + PD_UL_RESERVE_RW);
	writel(0xff, pd + PD_UL_RESERVE_AO_RW);
	writel(BIT(0), dp->md_misc + MD_L1TIMSR0);
	dp->dummies = 0;
	dp->reload = false;
	goto out;
pit_on:
	rmw(pd + PD_DL_PIT_INIT_CON3, 0, BIT(0));
	hw_write(pd + PD_DL_PIT_INIT, 0, true);
out:
	writel(BIT(7), dp->md_misc + MD_DL_RB_PIT_INIT);
}

/* return consumed PITs and refilled BAT slots to the HW */
static void rx_notify(struct ccci_dpmaif *dp, u32 pits)
{
	u32 bats = rx_refill(dp);

	if (bats && hw_write(dp->pd + PD_DL_BAT_ADD, bats, false))
		dev_err_ratelimited(dp->dev, "BAT add timeout\n");
	if (pits && hw_write(dp->pd + PD_DL_PIT_ADD, pits, true))
		dev_err_ratelimited(dp->dev, "PIT add timeout\n");
	if (dp->reload)
		rx_return_dummies(dp);
}

static void rx_deliver(struct ccci_dpmaif *dp, struct sk_buff *skb)
{
	struct net_device *ndev = NULL;

	rcu_read_lock();
	if (dp->rx_ch < NUM_LINKS)
		ndev = rcu_dereference(dp->links[dp->rx_ch]);
	if (!ndev) {
		rcu_read_unlock();
		dev_info_ratelimited(dp->dev, "rx for interface %u, no link\n", dp->rx_ch);
		dev_kfree_skb_any(skb);
		return;
	}
	switch (skb->data[0] & 0xf0) {
	case 0x40:
		skb->protocol = htons(ETH_P_IP);
		break;
	case 0x60:
		skb->protocol = htons(ETH_P_IPV6);
		break;
	default:
		DEV_STATS_INC(ndev, rx_errors);
		rcu_read_unlock();
		dev_kfree_skb_any(skb);
		return;
	}
	skb->dev = ndev;
	skb_reset_network_header(skb);
	if (!dp->rx_csum && (ndev->features & NETIF_F_RXCSUM))
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	DEV_STATS_INC(ndev, rx_packets);
	DEV_STATS_ADD(ndev, rx_bytes, skb->len);
	napi_gro_receive(&dp->napi, skb);
	rcu_read_unlock();
}

static void rx_payload(struct ccci_dpmaif *dp, const struct dpmaif_pit *pit)
{
	u32 w0 = le32_to_cpu(pit->w[0]), bid = FIELD_GET(PIT_BID, w0);
	u32 len = FIELD_GET(PIT_LEN, w0);
	u64 addr = (u64)le32_to_cpu(pit->w[2]) << 32 | le32_to_cpu(pit->w[1]);
	struct sk_buff *skb;
	u64 off;

	if (bid >= BAT_ENTRIES || !dp->bat_skb[bid]) {
		dev_err_ratelimited(dp->dev, "PIT %u: bad buffer %u\n", dp->pit_rd, bid);
		return;
	}
	skb = dp->bat_skb[bid];
	dp->bat_skb[bid] = NULL;
	dma_unmap_single(dp->dev, dp->bat_skb_dma[bid], RX_BUF_SIZE, DMA_FROM_DEVICE);
	off = addr - dp->bat_skb_dma[bid];
	if ((w0 & (PIT_CONT | PIT_FRAG)) || off + len > RX_BUF_SIZE || dp->rx_drop) {
		if (!dp->rx_drop)
			dev_err_ratelimited(dp->dev, "PIT %u: %#x len %u off %llu dropped\n",
					    dp->pit_rd, w0, len, off);
		dev_kfree_skb_any(skb);
		return;
	}
	skb_reserve(skb, off);
	skb_put(skb, len);
	rx_deliver(dp, skb);
}

static int rx_poll(struct ccci_dpmaif *dp, int budget)
{
	u32 wr = readl(dp->ao + AO_DL_PIT_STA2) & 0xffff, pits = 0;
	int done = 0;

	if (wr >= PIT_ENTRIES)
		return 0;
	while (dp->pit_rd != wr && done < budget) {
		const struct dpmaif_pit *pit = &dp->pit[dp->pit_rd];
		u32 w0 = le32_to_cpu(READ_ONCE(pit->w[0]));
		u32 w3 = le32_to_cpu(READ_ONCE(pit->w[3]));

		if (w3 & PIT_IGNORE) {
			dp->reload = w3 & PIT_RELOAD;
			dp->dummies++;
		} else if (w0 & PIT_MSG) {
			dp->rx_ch = FIELD_GET(PIT_MSG_CH, w0);
			dp->rx_csum = FIELD_GET(PIT_MSG_CSUM, w0);
			dp->rx_drop = w0 & PIT_MSG_DROP;
		} else {
			rx_payload(dp, pit);
			done++;
		}
		dp->pit_rd = ring_next(dp->pit_rd, PIT_ENTRIES);
		if (++pits == 128) {
			rx_notify(dp, pits);
			pits = 0;
		}
	}
	rx_notify(dp, pits);
	return done;
}

/* ---- UL ---- */

static void tx_release(struct ccci_dpmaif *dp)
{
	u32 rd = (readl(dp->ao + AO_UL_CH_STA(0)) >> 16) / 2;

	if (rd >= DRB_ENTRIES)
		return;
	spin_lock(&dp->tx_lock);
	while (dp->drb_rel != rd) {
		u32 i = dp->drb_rel;
		struct sk_buff *skb = dp->drb_skb[i];

		if (skb) {
			dma_unmap_single(dp->dev, dp->drb_skb_dma[i], skb->len, DMA_TO_DEVICE);
			dev_consume_skb_any(skb);
			dp->drb_skb[i] = NULL;
		}
		dp->drb_rel = ring_next(i, DRB_ENTRIES);
	}
	if (dp->tx_stopped) {
		int i;

		dp->tx_stopped = false;
		rcu_read_lock();
		for (i = 0; i < NUM_LINKS; i++) {
			struct net_device *ndev = rcu_dereference(dp->links[i]);

			if (ndev)
				netif_wake_queue(ndev);
		}
		rcu_read_unlock();
	}
	spin_unlock(&dp->tx_lock);
}

static void tx_free_all(struct ccci_dpmaif *dp)
{
	u32 i;

	for (i = 0; i < DRB_ENTRIES; i++) {
		if (!dp->drb_skb[i])
			continue;
		dma_unmap_single(dp->dev, dp->drb_skb_dma[i], dp->drb_skb[i]->len,
				 DMA_TO_DEVICE);
		dev_kfree_skb_any(dp->drb_skb[i]);
		dp->drb_skb[i] = NULL;
	}
}

static netdev_tx_t link_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct dpmaif_link *l = wwan_netdev_drvpriv(ndev);
	struct ccci_dpmaif *dp = l->dp;
	u32 i, j, len;
	dma_addr_t addr;

	if (!READ_ONCE(dp->running) || skb_linearize(skb) || skb->len > LINK_MTU_MAX)
		goto drop;
	len = skb->len;
	addr = dma_map_single(dp->dev, skb->data, len, DMA_TO_DEVICE);
	if (dma_mapping_error(dp->dev, addr))
		goto drop;

	spin_lock_bh(&dp->tx_lock);
	if (DRB_ENTRIES - 1 - ring_used(dp->drb_rel, dp->drb_wr, DRB_ENTRIES) < 2) {
		dp->tx_stopped = true;
		netif_stop_queue(ndev);
		spin_unlock_bh(&dp->tx_lock);
		dma_unmap_single(dp->dev, addr, len, DMA_TO_DEVICE);
		return NETDEV_TX_BUSY;
	}
	i = dp->drb_wr;
	j = ring_next(i, DRB_ENTRIES);
	dp->drb[i].w0 = cpu_to_le32(DRB_MSG | DRB_CONT | FIELD_PREP(DRB_LEN, len));
	dp->drb[i].w1 = cpu_to_le32(FIELD_PREP(DRB_MSG_CH, READ_ONCE(l->ch)));
	dp->drb[j].w0 = cpu_to_le32(FIELD_PREP(DRB_ADDR_EXT, upper_32_bits(addr) & 0xff) |
				    FIELD_PREP(DRB_LEN, len));
	dp->drb[j].w1 = cpu_to_le32(lower_32_bits(addr));
	dp->drb_skb[j] = skb;
	dp->drb_skb_dma[j] = addr;
	dp->drb_wr = ring_next(j, DRB_ENTRIES);
	/* two DRBs, in words */
	if (hw_write(dp->pd + PD_UL_ADD_DESC_CH(0), 4, true))
		dev_err_ratelimited(dp->dev, "DRB add timeout\n");
	spin_unlock_bh(&dp->tx_lock);
	DEV_STATS_INC(ndev, tx_packets);
	DEV_STATS_ADD(ndev, tx_bytes, len);
	return NETDEV_TX_OK;

drop:
	DEV_STATS_INC(ndev, tx_dropped);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

/* ---- interrupt, NAPI ---- */

static irqreturn_t dpmaif_isr(int irq, void *data)
{
	struct ccci_dpmaif *dp = data;
	u32 ul = readl(dp->pd + PD_AP_UL_L2TISAR0) & ~readl(dp->ao + AO_UL_AP_L2TIMR0);
	u32 dl = readl(dp->pd + PD_AP_DL_L2TISAR0) & ~readl(dp->ao + AO_UL_APDL_L2TIMR0);
	bool poll = false;

	clear_ip_busy(dp);
	if (ul) {
		writel(ul, dp->pd + PD_AP_UL_L2TISAR0);
		/* MD / power not ready: its data path was asleep, the HW retries */
		if (ul & UL_INT_LEN_ERR)
			dev_warn_ratelimited(dp->dev, "UL length error %#x\n", ul);
		if (ul & UL_INT_DONE(0)) {
			ul_irq_mask(dp, true);
			poll = true;
		}
	}
	if (dl) {
		writel(dl, dp->pd + PD_AP_DL_L2TISAR0);
		if (dl & ~DL_INT_DONE)
			dev_warn_ratelimited(dp->dev, "DL interrupt %#x\n", dl);
		if (dl & DL_INT_DONE) {
			dl_irq_mask(dp, true);
			poll = true;
		}
	}
	if (poll)
		napi_schedule(&dp->napi);
	return ul || dl ? IRQ_HANDLED : IRQ_NONE;
}

static int dpmaif_poll(struct napi_struct *napi, int budget)
{
	struct ccci_dpmaif *dp = container_of(napi, struct ccci_dpmaif, napi);
	int done;

	tx_release(dp);
	done = rx_poll(dp, budget);
	if (done < budget && napi_complete_done(napi, done)) {
		clear_ip_busy(dp);
		ul_irq_mask(dp, false);
		dl_irq_mask(dp, false);
	}
	return done;
}

/* ---- WWAN links ---- */

static const struct net_device_ops link_ops = {
	.ndo_start_xmit = link_xmit,
};

static void link_setup(struct net_device *ndev)
{
	ndev->netdev_ops = &link_ops;
	ndev->type = ARPHRD_NONE;
	ndev->flags = IFF_POINTOPOINT | IFF_NOARP;
	ndev->mtu = ETH_DATA_LEN;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = LINK_MTU_MAX;
	ndev->tx_queue_len = 1000;
	ndev->features = NETIF_F_RXCSUM | NETIF_F_GRO;
	ndev->hw_features = NETIF_F_RXCSUM;
	ndev->needs_free_netdev = true;
}

static ssize_t md_interface_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	struct dpmaif_link *l = wwan_netdev_drvpriv(to_net_dev(dev));

	return sysfs_emit(buf, "%u\n", READ_ONCE(l->ch));
}

static ssize_t md_interface_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct net_device *ndev = to_net_dev(dev);
	struct dpmaif_link *l = wwan_netdev_drvpriv(ndev);
	struct ccci_dpmaif *dp = l->dp;
	struct net_device *other;
	u32 ch;
	int ret = kstrtou32(buf, 0, &ch);

	if (ret)
		return ret;
	if (ch >= NUM_LINKS)
		return -EINVAL;
	rtnl_lock();
	other = rtnl_dereference(dp->links[ch]);
	if (other && other != ndev) {
		ret = -EBUSY;
	} else if (!other) {
		RCU_INIT_POINTER(dp->links[l->ch], NULL);
		WRITE_ONCE(l->ch, ch);
		rcu_assign_pointer(dp->links[ch], ndev);
	}
	rtnl_unlock();
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(md_interface);

static struct attribute *link_attrs[] = {
	&dev_attr_md_interface.attr, NULL,
};

static const struct attribute_group link_attr_group = {
	.attrs = link_attrs,
};

static int link_new(void *ctxt, struct net_device *ndev, u32 if_id,
		    struct netlink_ext_ack *extack)
{
	struct ccci_dpmaif *dp = ctxt;
	struct dpmaif_link *l = wwan_netdev_drvpriv(ndev);
	int ret;

	if (if_id >= NUM_LINKS)
		return -EINVAL;
	if (rtnl_dereference(dp->links[if_id]))
		return -EBUSY;
	l->dp = dp;
	l->ch = if_id;
	ndev->sysfs_groups[0] = &link_attr_group;
	ret = register_netdevice(ndev);
	if (!ret)
		rcu_assign_pointer(dp->links[if_id], ndev);
	return ret;
}

static void link_del(void *ctxt, struct net_device *ndev, struct list_head *head)
{
	struct ccci_dpmaif *dp = ctxt;
	struct dpmaif_link *l = wwan_netdev_drvpriv(ndev);

	RCU_INIT_POINTER(dp->links[l->ch], NULL);
	unregister_netdevice_queue(ndev, head);
}

static const struct wwan_ops dpmaif_wwan_ops = {
	.priv_size = sizeof(struct dpmaif_link),
	.setup = link_setup,
	.newlink = link_new,
	.dellink = link_del,
};

/* ---- start / stop ---- */

static void hw_reset(struct ccci_dpmaif *dp)
{
	void __iomem *infra = dp->md->infra;

	rmw(dp->infra_mem, INFRA_PROT_DPMAIF, 0);
	writel(BIT(6), infra + INFRA_RST0_AO);
	writel(BIT(6), infra + INFRA_RST1_AO);
	writel(BIT(2), infra + INFRA_RST0_PD);
	writel(BIT(2), infra + INFRA_RST1_PD);
}

/* MD view of its memory for the DL path: 8 x 64 MiB from md_mem_base */
static void hw_md_remap(struct ccci_dpmaif *dp)
{
	u32 base = dp->md->md_mem_base;
	int i;

	writel(1 << 8 | 1, dp->ao + AO_REMAP_DOMAIN);
	writel(7 << 8 | 6, dp->ao + AO_REMAP_CACHE);
	for (i = 0; i < 8; i++, base += SZ_64M)
		writel((base >> 24 & 0xff) | ((base + SZ_32M) >> 8 & 0xff0000) |
		       BIT(16) | BIT(0), dp->ao + AO_REMAP_BANK(i));
	rmw(dp->ao + AO_MISC_CFG1, 0, BIT(0));
}

static int hw_irq_init(struct ccci_dpmaif *dp)
{
	u32 v;

	writel(~0, dp->pd + PD_AP_UL_L2TISAR0);
	writel(UL_INT_EN, dp->ao + AO_UL_AP_L2TIMCR0);
	writel((u32)~UL_INT_EN, dp->ao + AO_UL_AP_L2TIMSR0);
	if (readl_poll_timeout(dp->ao + AO_UL_AP_L2TIMR0, v, (v & UL_INT_EN) != UL_INT_EN,
			       10, HW_TIMEOUT_US))
		return -ETIMEDOUT;
	writel(~0, dp->pd + PD_AP_DL_L2TISAR0);
	writel(DL_INT_EN, dp->ao + AO_UL_APDL_L2TIMCR0);
	writel((u32)~DL_INT_EN, dp->ao + AO_UL_APDL_L2TIMSR0);
	rmw(dp->ao + AO_DL_RDY_CHK_THRES, 0x7f, (u32)~DL_INT_EN & 0x7f);
	if (readl_poll_timeout(dp->ao + AO_UL_APDL_L2TIMR0, v, (v & DL_INT_EN) != DL_INT_EN,
			       10, HW_TIMEOUT_US))
		return -ETIMEDOUT;
	writel(~0, dp->pd + PD_AP_IP_BUSY);
	writel(0, dp->ao + AO_UL_IP_BUSY_MASK);
	return 0;
}

static int hw_rx_init(struct ccci_dpmaif *dp)
{
	void __iomem *ao = dp->ao, *pd = dp->pd;
	int i, ret;

	rmw(ao + AO_DL_PKTINFO_CON0, GENMASK(15, 8), (64 / 16) << 8);
	rmw(ao + AO_DL_PKTINFO_CON2, GENMASK(16, 8) | GENMASK(7, 0), (RX_BUF_SIZE / 128) << 8);
	rmw(ao + AO_DL_PKTINFO_CON0, GENMASK(31, 16), 1 << 16);
	rmw(ao + AO_DL_RDY_CHK_THRES, GENMASK(23, 22), BIT(23));	/* 64 byte aligned */
	writel(DL_MTU, ao + AO_DL_PKTINFO_CON1);
	rmw(ao + AO_DL_PKTINFO_CON2, GENMASK(31, 24), PIT_CHK_NUM << 24);
	rmw(ao + AO_DL_RDY_CHK_THRES, GENMASK(21, 16), BAT_CHK_NUM << 16);
	rmw(ao + AO_DL_PKTINFO_CON0, GENMASK(7, 0), RB_PIT_CHK_NUM);
	rmw(pd + PD_DL_BAT_INIT_CON1, 0, BIT(22));
	rmw(ao + AO_DL_RDY_CHK_THRES, 0, BIT(13));
	/* MD dummy PITs (reorder workaround) */
	rmw(pd + PD_DL_PIT_INIT_CON3, GENMASK(31, 16), DUMMY_PIT_AIDX << 16);
	writel(DUMMY_PIT_AIDX | MD_DUMMYPIT_EN, pd + PD_UL_RESERVE_RW);

	writel(lower_32_bits(dp->bat_dma), pd + PD_DL_BAT_INIT_CON0);
	rmw(pd + PD_DL_BAT_INIT_CON1, GENMASK(31, 24) | GENMASK(16, 0),
	    (upper_32_bits(dp->bat_dma) & 0xff) << 24 | BAT_ENTRIES);
	writel(lower_32_bits(dp->pit_dma), pd + PD_DL_PIT_INIT_CON0);
	rmw(pd + PD_DL_PIT_INIT_CON1, GENMASK(31, 24) | GENMASK(15, 0),
	    (upper_32_bits(dp->pit_dma) & 0xff) << 24 | PIT_ENTRIES);
	rmw(pd + PD_DL_PIT_INIT_CON3, 0, BIT(0));
	ret = hw_write(pd + PD_DL_BAT_INIT, INIT_ALLSET, true);
	if (!ret)
		ret = hw_write(pd + PD_DL_PIT_INIT, INIT_ALLSET, true);
	if (ret)
		return ret;
	rmw(ao + AO_DL_RDY_CHK_THRES, 0, BIT(31));	/* checksum check */

	for (i = 0; i < RX_BUFS; i++) {
		ret = rx_buf_lend(dp, GFP_KERNEL);
		if (ret)
			return ret;
	}
	ret = hw_write(pd + PD_DL_BAT_ADD, RX_BUFS, false);
	if (ret)
		return ret;
	rmw(pd + PD_DL_BAT_INIT_CON1, 0, BIT(16));
	return hw_write(pd + PD_DL_BAT_INIT, 0, true);
}

static void hw_tx_init(struct ccci_dpmaif *dp)
{
	writel(lower_32_bits(dp->drb_dma), dp->ao + AO_UL_CHNL_CON0(0));
	rmw(dp->ao + AO_UL_CHNL_CON1(0), GENMASK(31, 24) | GENMASK(15, 0),
	    (upper_32_bits(dp->drb_dma) & 0xff) << 24 | DRB_ENTRIES * 2);
	rmw(dp->ao + AO_UL_CHNL_ARB0, 0, BIT(0) | BIT(8));
}

/* reset the IP and set up the rings, interrupts and the RX side */
static int hw_init(struct ccci_dpmaif *dp)
{
	int ret;

	hw_reset(dp);
	rx_free_bufs(dp);
	tx_free_all(dp);
	memset(dp->pit, 0, PIT_ENTRIES * sizeof(*dp->pit));
	memset(dp->bat, 0, BAT_ENTRIES * sizeof(*dp->bat));
	memset(dp->drb, 0, DRB_ENTRIES * sizeof(*dp->drb));
	dp->pit_rd = dp->bat_wr = dp->bat_rel = dp->drb_wr = dp->drb_rel = 0;
	dp->dummies = 0;
	dp->reload = dp->rx_drop = dp->tx_stopped = false;

	rmw(dp->infra_mem, 0, INFRA_PROT_DPMAIF);
	writel(0x7f, dp->pd + PD_AP_CG_EN);
	rmw(dp->pd + PD_WDMA_WR_CHNL_CON3, GENMASK(31, 28), 0xe << 28);
	rmw(dp->pd + PD_WDMA_WR_CMD_CON0, GENMASK(15, 0), 0);
	hw_md_remap(dp);
	writel(BIT(9) | BIT(10) | BIT(15) | BIT(16), dp->ao + AO_UL_AP_L1TIMR0);
	writel(0xff, dp->pd + PD_DL_RESERVE_RW);
	ret = hw_irq_init(dp);
	if (!ret)
		ret = hw_rx_init(dp);
	return ret;
}

int ccci_dpmaif_start(struct ccci_md *md)
{
	struct ccci_dpmaif *dp = md->dp;
	int ret, i;

	if (dp->running)
		return 0;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(dp->clks), dp->clks);
	if (ret)
		return ret;
	/* IP busy reporting on, no MD-to-AP wakeup event */
	rmw(md->infra + INFRA_DPMAIF_CTRL, BIT(13), BIT(12));
	/*
	 * About one boot in five the first DL BAT init write stays busy
	 * (PD_DL_BAT_INIT 0x80000001); without a data path the MD never sends
	 * HS2. Waiting in the poll does not help, a reset and a new init some
	 * time later does (right away it can fail again).
	 */
	for (i = 0; i < 10; i++) {
		ret = hw_init(dp);
		if (ret != -ETIMEDOUT)
			break;
		if (!i)
			dev_warn(dp->dev, "DPMAIF init timed out (BAT init %#x), retrying\n",
				 readl(dp->pd + PD_DL_BAT_INIT));
		msleep(200);
	}
	if (i && !ret)
		dev_info(dp->dev, "DPMAIF init done at attempt %d\n", i + 1);
	if (ret) {
		dev_err(dp->dev, "DPMAIF init: %d\n", ret);
		hw_reset(dp);
		rx_free_bufs(dp);
		clk_bulk_disable_unprepare(ARRAY_SIZE(dp->clks), dp->clks);
		return ret;
	}
	hw_tx_init(dp);
	napi_enable(&dp->napi);
	WRITE_ONCE(dp->running, true);
	enable_irq(dp->irq);
	return 0;
}

void ccci_dpmaif_stop(struct ccci_md *md)
{
	struct ccci_dpmaif *dp = md->dp;

	if (!dp->running)
		return;
	WRITE_ONCE(dp->running, false);
	disable_irq(dp->irq);
	napi_disable(&dp->napi);
	spin_lock_bh(&dp->tx_lock);	/* no xmit in flight */
	spin_unlock_bh(&dp->tx_lock);
	hw_reset(dp);
	rx_free_bufs(dp);
	tx_free_all(dp);
	clk_bulk_disable_unprepare(ARRAY_SIZE(dp->clks), dp->clks);
}

int ccci_dpmaif_init(struct ccci_md *md)
{
	struct device *dev = md->dev;
	struct ccci_dpmaif *dp;
	int ret;

	dp = devm_kzalloc(dev, sizeof(*dp), GFP_KERNEL);
	if (!dp)
		return -ENOMEM;
	dp->md = md;
	dp->dev = dev;
	spin_lock_init(&dp->tx_lock);
	dp->ao = devm_ioremap(dev, DPMAIF_AO_PHYS, 0x1000);
	dp->pd = devm_ioremap(dev, DPMAIF_PD_PHYS, 0x1000);
	dp->md_misc = devm_ioremap(dev, DPMAIF_MD_MISC_PHYS, 0x1000);
	dp->infra_mem = devm_ioremap(dev, INFRA_AO_MEM_PHYS, 4);
	if (!dp->ao || !dp->pd || !dp->md_misc || !dp->infra_mem)
		return -ENOMEM;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40));
	if (ret)
		return ret;
	dp->pit = dmam_alloc_coherent(dev, PIT_ENTRIES * sizeof(*dp->pit), &dp->pit_dma, GFP_KERNEL);
	dp->bat = dmam_alloc_coherent(dev, BAT_ENTRIES * sizeof(*dp->bat), &dp->bat_dma, GFP_KERNEL);
	dp->drb = dmam_alloc_coherent(dev, DRB_ENTRIES * sizeof(*dp->drb), &dp->drb_dma, GFP_KERNEL);
	dp->bat_skb = devm_kcalloc(dev, BAT_ENTRIES, sizeof(*dp->bat_skb), GFP_KERNEL);
	dp->bat_skb_dma = devm_kcalloc(dev, BAT_ENTRIES, sizeof(*dp->bat_skb_dma), GFP_KERNEL);
	dp->drb_skb = devm_kcalloc(dev, DRB_ENTRIES, sizeof(*dp->drb_skb), GFP_KERNEL);
	dp->drb_skb_dma = devm_kcalloc(dev, DRB_ENTRIES, sizeof(*dp->drb_skb_dma), GFP_KERNEL);
	if (!dp->pit || !dp->bat || !dp->drb || !dp->bat_skb || !dp->bat_skb_dma ||
	    !dp->drb_skb || !dp->drb_skb_dma)
		return -ENOMEM;

	ret = ccci_infra_clks(md, dpmaif_clk_ids, ARRAY_SIZE(dp->clks), dp->clks);
	if (ret)
		return dev_err_probe(dev, ret, "DPMAIF clocks\n");
	dp->irq = ccci_gic_spi(DPMAIF_SPI, IRQ_TYPE_LEVEL_HIGH);
	if (dp->irq < 0) {
		ret = dp->irq;
		goto err_clk;
	}
	ret = devm_request_irq(dev, dp->irq, dpmaif_isr, IRQF_NO_AUTOEN, "dpmaif", dp);
	if (ret)
		goto err_clk;

	dp->napi_dev = alloc_netdev_dummy(0);
	if (!dp->napi_dev) {
		ret = -ENOMEM;
		goto err_clk;
	}
	netif_napi_add(dp->napi_dev, &dp->napi, dpmaif_poll);
	md->dp = dp;
	/* link 0 (ccmni0) exists from the start; others: ip link add type wwan */
	ret = wwan_register_ops(dev, &dpmaif_wwan_ops, dp, 0);
	if (ret)
		goto err_napi;
	return 0;

err_napi:
	md->dp = NULL;
	netif_napi_del(&dp->napi);
	free_netdev(dp->napi_dev);
err_clk:
	clk_bulk_put(ARRAY_SIZE(dp->clks), dp->clks);
	return ret;
}

void ccci_dpmaif_exit(struct ccci_md *md)
{
	struct ccci_dpmaif *dp = md->dp;

	wwan_unregister_ops(md->dev);
	netif_napi_del(&dp->napi);
	free_netdev(dp->napi_dev);
	rx_free_bufs(dp);
	tx_free_all(dp);
	clk_bulk_put(ARRAY_SIZE(dp->clks), dp->clks);
}
