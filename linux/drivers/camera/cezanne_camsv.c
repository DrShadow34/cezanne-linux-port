// SPDX-License-Identifier: GPL-2.0
/*
 * Redmi K30 Ultra camera front end: seninf (CSI-2 RX + mux) feeding CAMSV raw
 * DMA engines, one V4L2 capture device per sensor ("channel"):
 *
 *   port 0  front S5K3T2:  CSI0 D-PHY 4 lanes -> SENINF1 -> seninf mux 1 ->
 *           cam mux 3 -> CAMSV0, IMGO through SMI larb14 port 3
 *   port 1  main IMX682:   CSI3 C-PHY 3 trios -> SENINF7 -> seninf mux 2 ->
 *           cam mux 3 -> CAMSV0 (shared: one channel streams at a time)
 *   port 2  ultrawide HI1337: CSI2 D-PHY 4 lanes -> SENINF5 -> seninf mux 3
 *   port 3  macro S5K5E9:  CSI1 2 lanes -> SENINF4 -> seninf mux 4
 *   port 4  mono OV02B1B:  CSI1 1 lane -> SENINF3 -> seninf mux 5 (shares
 *           the CSI1 lane mapping with the macro: not both at once)
 *
 * (Engines as without the IOMMU; with it every channel defaults to CAMSV2.)
 *
 * CAMSV1-6 sit behind SMI larb13, whose traffic always goes through the MDP
 * IOMMU (0x1f027000): with the DT's "iommus" (larb13 CAMSV ports 3-8) the
 * channels use CAMSV2 by default and buffers are IOVAs below 4 GiB;
 * without it only CAMSV0 (larb14, MMU bypass, CMA below 4 GiB) works.
 * The "engines" parameter picks the engine per channel (cam mux 3 + n).
 * CAMSV1 ignores RCNT_INC (drops every frame): not used.
 *
 * seninf and MIPI RX are programmed from register snapshots of MIUI's
 * previews (replay_*.h, mainline/camera/dumps); CAMSV from the vendor HAL
 * (libimageio_plat_drv.so CAMSV_TOP_CTRL / DMAO_CAMSV_B / CAMSV_BUF_CTRL).
 * Seninf muxes run at 1 pixel/clock (CAMSV gives IMGO errors at 2), CAMSV
 * writes RAW10 as 16 bits per pixel (FMT_SEL 1, PAK 13; PAK 9 would pack it
 * MediaTek style, 4 pixels in 5 bytes as one little-endian bit stream,
 * which nothing reads) or RAW8 (FMT_SEL 0, PAK 8: the top 8 of the 10
 * bits) as the second format, and the larb ports
 * have their MMU bypassed (no IOMMU driver yet), so buffers come from CMA
 * below 4 GiB.
 *
 * Buffers: FBC hands the DMA one buffer per RCNT_INC; the IMGO address is
 * double-buffered and latches at SOF (0x800 bit 0).  A new address is only
 * programmed once the previous one has latched (SOF interrupt), and buffers
 * complete in programming order as the FBC write count advances.  The count
 * also advances for frames that never reach the buffer (a few a second,
 * more right after stream-on): markers in each buffer tell (cam_mark_buf()).
 *
 * The media device is named "mtk-seninf" so that libcamera's simple pipeline
 * handler picks it up (with software_isp enabled in its configuration file).
 */
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mc.h>
#include <media/videobuf2-dma-contig.h>

struct cam_reg { u32 ofs, val; };
#include "replay_front.h"
#include "replay_main.h"
#include "replay_uw.h"
#include "replay_macro.h"
#include "replay_mono.h"

/* seninf (0x1a004000): per-receiver blocks every 0x1000, cam mux block */
#define SENINF_MUX_SRC(n)	(0x0010 + 4 * ((n) / 4))	/* byte per mux */
#define SENINF_MUX_CTRL_0(n)	(0x1000 * (n) + 0x0d00)
#define SENINF_MUX_CTRL_1(n)	(0x1000 * (n) + 0x0d04)
#define CAM_MUX_CTRL(n)		(0x0400 + 4 * ((n) / 4))	/* byte per cam mux */
#define CAM_MUX_EN		0x0410
#define CAM_MUX_OPT(n)		(0x0420 + 4 * (n))
#define CAM_MUX_CHK_CTL_0(n)	(0x0500 + 0x10 * (n))	/* pixel mode */
#define CAM_MUX_CHK_CTL_1(n)	(0x0504 + 0x10 * (n))	/* expected size */

/* CAMSV n (0x1a090000 + 0x1000 n) */
#define CAMSV_MODULE_EN		0x0010
#define   MODULE_TG_EN		BIT(0)
#define   MODULE_PAK_EN		BIT(2)
#define   MODULE_IMGO_EN	BIT(4)
#define   MODULE_DEFAULT	0x50000000
#define CAMSV_FMT_SEL		0x0014	/* 0 RAW8, 1 RAW10, 2 RAW12 */
#define CAMSV_INT_EN		0x0018
#define CAMSV_INT_STATUS	0x001c	/* read clear */
#define   INT_VS1		BIT(0)
#define   INT_TG_ERR		BIT(4)
#define   INT_TG_GBERR		BIT(5)
#define   INT_SOF		BIT(6)
#define   INT_PASS1_DONE	(BIT(10) | BIT(11) | BIT(12))
#define   INT_IMGO_ERR		BIT(16)
#define   INT_IMGO_OVERRUN	BIT(17)
#define   INT_ERRORS		(INT_TG_ERR | INT_TG_GBERR | INT_IMGO_ERR | \
				 INT_IMGO_OVERRUN)
#define CAMSV_SW_CTL		0x0020
#define CAMSV_IMGO_FBC		0x002c	/* bit 0: RCNT_INC */
#define CAMSV_CLK_EN		0x0030
#define CAMSV_PAK		0x004c	/* [4:0] mode; [6:5] px mode */
#define   PAK_RAW8		8	/* 1 byte per pixel (FMT_SEL 0: the top 8 bits) */
#define   PAK_RAW10_UNPACKED	13	/* 16 bits per pixel, LSB aligned (FMT_SEL 1) */
#define CAMSV_TG_SEN_MODE	0x0130
#define   SEN_MODE_CMOS_EN	BIT(0)
#define   SEN_MODE_TIME_STP_EN	BIT(16)
#define CAMSV_TG_VF_CON		0x0134
#define CAMSV_TG_GRAB_PXL	0x0138
#define CAMSV_TG_GRAB_LIN	0x013c
#define CAMSV_TG_PATH_CFG	0x0140
#define CAMSV_IMGO_BASE		0x0420
#define CAMSV_IMGO_BASE_MSB	0x0424
#define CAMSV_IMGO_OFST		0x0428
#define CAMSV_IMGO_XSIZE	0x0430
#define CAMSV_IMGO_YSIZE	0x0434
#define CAMSV_IMGO_STRIDE	0x0438
/*
 * The CAMSV writes something to the address in 0x804 once per frame (frame
 * header / timestamp?), on IMGO's larb port: at 0 that is an IOMMU fault per
 * frame (or a write to physical 0 in MMU bypass). It gets a scratch buffer.
 * Found by bisecting which register silenced the faults.
 */
#define CAMSV_FRAME_WRITE_BASE	0x0804
#define CAM_SCRATCH_SIZE	SZ_64K
#define CAMSV_IMGO_ERR_STAT	0x0760
#define CAMSV_DMA_DB_EN		0x0800	/* bit 0: per-frame address latch */
#define CAMSV_FBC_IMGO_CTL1	0x0a00
#define   FBC_MODE		BIT(11)
#define   FBC_EN		BIT(15)
#define CAMSV_FBC_IMGO_CTL2	0x0a04	/* [15:8] write count */

/* SMI larbs: per-port MMU enable */
#define LARB_NONSEC_CON(port)	(0x380 + 4 * (port))

/* 64: the GPU (Panfrost v7+) imports linear dma-bufs only with 64-byte rows */
#define CAM_STRIDE(w)		ALIGN(w, 64)

#define CAM_NUM_CHANNELS	5
#define CAM_NUM_CAMSV		4

/* CAMSV engine per channel (0 = CAMSV0 on larb14, n = CAMSVn on larb13) */
static unsigned int engines[CAM_NUM_CHANNELS];
static int num_engines;
module_param_array(engines, uint, &num_engines, 0444);
MODULE_PARM_DESC(engines, "CAMSV engine per channel (front,main,uw,macro,mono); default 2 with the IOMMU (mono 3), else 0");

static bool raw10 = true;
module_param(raw10, bool, 0444);
MODULE_PARM_DESC(raw10, "Offer 10-bit raw (16 bits per pixel) before 8-bit (default: yes)");

#define ENGINE_CAM_MUX(n)	(3 + (n))
#define LARB14_CAMSV0_PORT	3

struct cam_chan_cfg {
	const char *name;
	const struct cam_reg *mipi_rx, *seninf;
	unsigned int n_mipi_rx, n_seninf;
	u8 receiver;		/* SENINFn - 1 */
	u8 seninf_mux;		/* seninf mux n - 1 */
	u32 width, height, code;
	u32 pixfmt, pixfmt8;	/* 10 bits in 16, 8 bits */
	bool own_engine;	/* streams next to another channel: CAMSV3 */
};

static const struct cam_chan_cfg cam_chan_cfgs[CAM_NUM_CHANNELS] = {
	{
		.name = "front",
		.mipi_rx = replay_front_mipi_rx,
		.n_mipi_rx = ARRAY_SIZE(replay_front_mipi_rx),
		.seninf = replay_front_seninf,
		.n_seninf = ARRAY_SIZE(replay_front_seninf),
		.receiver = 0, .seninf_mux = 0,
		.width = 2592, .height = 1940,
		.code = MEDIA_BUS_FMT_SGRBG10_1X10, .pixfmt = V4L2_PIX_FMT_SGRBG10, .pixfmt8 = V4L2_PIX_FMT_SGRBG8,
	}, {
		.name = "main",
		.mipi_rx = replay_main_mipi_rx,
		.n_mipi_rx = ARRAY_SIZE(replay_main_mipi_rx),
		.seninf = replay_main_seninf,
		.n_seninf = ARRAY_SIZE(replay_main_seninf),
		.receiver = 6, .seninf_mux = 1,
		.width = 4624, .height = 3472,
		.code = MEDIA_BUS_FMT_SRGGB10_1X10, .pixfmt = V4L2_PIX_FMT_SRGGB10, .pixfmt8 = V4L2_PIX_FMT_SRGGB8,
	}, {
		.name = "uw",
		.mipi_rx = replay_uw_mipi_rx,
		.n_mipi_rx = ARRAY_SIZE(replay_uw_mipi_rx),
		.seninf = replay_uw_seninf,
		.n_seninf = ARRAY_SIZE(replay_uw_seninf),
		.receiver = 4, .seninf_mux = 2,
		.width = 4208, .height = 3120,
		.code = MEDIA_BUS_FMT_SGRBG10_1X10, .pixfmt = V4L2_PIX_FMT_SGRBG10, .pixfmt8 = V4L2_PIX_FMT_SGRBG8,
	}, {
		.name = "macro",
		.mipi_rx = replay_macro_mipi_rx,
		.n_mipi_rx = ARRAY_SIZE(replay_macro_mipi_rx),
		.seninf = replay_macro_seninf,
		.n_seninf = ARRAY_SIZE(replay_macro_seninf),
		.receiver = 3, .seninf_mux = 3,
		.width = 2592, .height = 1940,
		.code = MEDIA_BUS_FMT_SGRBG10_1X10, .pixfmt = V4L2_PIX_FMT_SGRBG10, .pixfmt8 = V4L2_PIX_FMT_SGRBG8,
	}, {
		.name = "mono",
		.mipi_rx = replay_mono_mipi_rx,
		.n_mipi_rx = ARRAY_SIZE(replay_mono_mipi_rx),
		.seninf = replay_mono_seninf,
		.n_seninf = ARRAY_SIZE(replay_mono_seninf),
		.receiver = 2, .seninf_mux = 4,
		.width = 1600, .height = 1200,
		.code = MEDIA_BUS_FMT_Y10_1X10, .pixfmt = V4L2_PIX_FMT_Y10, .pixfmt8 = V4L2_PIX_FMT_GREY,
		.own_engine = true,
	},
};

struct cam_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
	u8 *va;			/* kernel mapping for the frame markers */
};

struct cezanne_cam;

struct cam_chan {
	struct cezanne_cam *cam;
	const struct cam_chan_cfg *cfg;
	unsigned int sv;		/* CAMSV engine */
	void __iomem *camsv;
	int irq;
	bool powered;
	bool raw8;		/* the 8-bit format is selected */

	struct video_device vdev;
	struct media_pad vpad;
	struct media_pipeline pipe;
	struct vb2_queue queue;
	struct mutex lock;
	struct v4l2_subdev *sensor;

	spinlock_t irqlock;		/* everything below */
	struct list_head queued;	/* waiting for the DMA */
	struct list_head inflight;	/* programmed, in order */
	bool latch_pending;		/* last address not latched yet */
	bool streaming;
	u8 skip;		/* completions to drop after stream-on */
	u8 wcnt;
	u32 sequence;
	u32 errors;
	u32 unwritten;		/* completions dropped: markers still there */
};

struct cam_async {
	struct v4l2_async_connection asc;
	struct cam_chan *ch;
};

struct cezanne_cam {
	struct device *dev;
	void __iomem *seninf;
	void __iomem *mipi_rx;
	void __iomem *camsv;
	void __iomem *larb14;
	bool iommu;			/* larb13 engines usable */
	void *scratch;			/* for CAMSV_FRAME_WRITE_BASE */
	dma_addr_t scratch_dma;
	struct resource *res[3];
	struct clk_bulk_data *clks;
	int num_clks;
	struct mutex power_lock;	/* power_count, engine_owner, seninf */
	int power_count;
	struct cam_chan *engine_owner[CAM_NUM_CAMSV];
	struct dentry *debugfs;

	struct media_device mdev;
	struct v4l2_device v4l2;
	struct v4l2_async_notifier notifier;
	struct cam_chan ch[CAM_NUM_CHANNELS];
};

static inline struct cam_buf *to_cam_buf(struct vb2_v4l2_buffer *vb)
{
	return container_of(vb, struct cam_buf, vb);
}

/* the selected format: 10 bits in 16, or the top 8 */
static unsigned int cam_line_bytes(const struct cam_chan *ch)
{
	return ch->cfg->width * (ch->raw8 ? 1 : 2);
}

static unsigned int cam_sizeimage(const struct cam_chan *ch)
{
	return CAM_STRIDE(cam_line_bytes(ch)) * ch->cfg->height;
}

/*
 * Frame markers: a pattern no RAW10 sample can have (every word above
 * 0x3ff; as RAW8 bytes merely improbable) at the start of the first line
 * and the end of the last one, put there before the DMA gets the buffer.
 * A completion whose buffer still carries either was not (fully) written.
 * Such completions came a few a second (all with the first line untouched,
 * 2026-11-08: the write count advanced without the frame): delivered, they
 * repeated an older frame, and right after stream-on, in buffers that are
 * not zeroed (IOMMU allocations), handed leftover memory to the software
 * ISP, whose statistics aborted on a sample beyond 10 bits (Snapshot died
 * on the 2nd-3rd picture, each one restarts the stream twice). Such a
 * buffer goes back to the DMA instead of to the application; debugfs
 * cezanne-camsv/unwritten-<channel> counts them.
 */
#define CAM_MARK_BYTES	32

static const u16 cam_mark[CAM_MARK_BYTES / 2] = {
	0xffff, 0xdead, 0xbeef, 0xfeed, 0xcafe, 0xf00d, 0xd00d, 0xface,
	0xc0de, 0xb0b0, 0xe1e1, 0xd1ce, 0xbabe, 0xfade, 0xc0c0, 0xdeaf,
};

/* offset of the marker at the end of the last line */
static unsigned int cam_mark_last(const struct cam_chan *ch)
{
	return CAM_STRIDE(cam_line_bytes(ch)) * (ch->cfg->height - 1) +
	       cam_line_bytes(ch) - CAM_MARK_BYTES;
}

/* writes both markers and cleans them to memory; any context */
static void cam_mark_buf(struct cam_chan *ch, struct cam_buf *buf)
{
	struct vb2_buffer *vb = &buf->vb.vb2_buf;
	dma_addr_t dma = vb2_dma_contig_plane_dma_addr(vb, 0);
	unsigned int last = cam_mark_last(ch);

	if (!buf->va)
		return;
	memcpy(buf->va, cam_mark, CAM_MARK_BYTES);
	memcpy(buf->va + last, cam_mark, CAM_MARK_BYTES);
	dma_sync_single_range_for_device(vb->vb2_queue->dev, dma, 0,
					 CAM_MARK_BYTES, DMA_FROM_DEVICE);
	dma_sync_single_range_for_device(vb->vb2_queue->dev, dma, last,
					 CAM_MARK_BYTES, DMA_FROM_DEVICE);
}

/* false: a marker survived, the DMA did not write the whole frame */
static bool cam_buf_written(struct cam_chan *ch, struct cam_buf *buf)
{
	struct vb2_buffer *vb = &buf->vb.vb2_buf;
	dma_addr_t dma = vb2_dma_contig_plane_dma_addr(vb, 0);
	unsigned int last = cam_mark_last(ch);

	if (!buf->va)
		return true;
	dma_sync_single_range_for_cpu(vb->vb2_queue->dev, dma, 0,
				      CAM_MARK_BYTES, DMA_FROM_DEVICE);
	dma_sync_single_range_for_cpu(vb->vb2_queue->dev, dma, last,
				      CAM_MARK_BYTES, DMA_FROM_DEVICE);
	return memcmp(buf->va, cam_mark, CAM_MARK_BYTES) &&
	       memcmp(buf->va + last, cam_mark, CAM_MARK_BYTES);
}

static int cam_power(struct cam_chan *ch, bool on)
{
	struct cezanne_cam *c = ch->cam;
	int ret = 0;

	if (on == ch->powered)
		return 0;
	mutex_lock(&c->power_lock);
	if (on) {
		if (!c->power_count) {
			ret = pm_runtime_resume_and_get(c->dev);
			if (ret)
				goto out;
			ret = clk_bulk_prepare_enable(c->num_clks, c->clks);
			if (ret) {
				pm_runtime_put(c->dev);
				goto out;
			}
		}
		c->power_count++;
		ch->powered = true;
	} else {
		ch->powered = false;
		if (!--c->power_count) {
			clk_bulk_disable_unprepare(c->num_clks, c->clks);
			pm_runtime_put(c->dev);
		}
	}
out:
	mutex_unlock(&c->power_lock);
	return ret;
}

/* -------------------------------------------------------------------------
 * Hardware
 */

/* Gives the DMA the next queued buffer; irqlock held. */
static void cam_program_next(struct cam_chan *ch)
{
	struct cam_buf *buf;
	dma_addr_t addr;

	if (!ch->streaming || ch->latch_pending || list_empty(&ch->queued))
		return;
	buf = list_first_entry(&ch->queued, struct cam_buf, list);
	list_move_tail(&buf->list, &ch->inflight);
	addr = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
	writel(lower_32_bits(addr), ch->camsv + CAMSV_IMGO_BASE);
	writel(upper_32_bits(addr), ch->camsv + CAMSV_IMGO_BASE_MSB);
	writel(readl(ch->camsv + CAMSV_IMGO_FBC) | BIT(0),
	       ch->camsv + CAMSV_IMGO_FBC);
	ch->latch_pending = true;
}

static irqreturn_t camsv_irq(int irq, void *data)
{
	struct cam_chan *ch = data;
	struct cam_buf *buf;
	u32 st;
	u8 wcnt, n;

	spin_lock(&ch->irqlock);
	if (!ch->powered || !ch->streaming) {
		spin_unlock(&ch->irqlock);
		return IRQ_NONE;
	}
	st = readl(ch->camsv + CAMSV_INT_STATUS);
	if (!st) {
		spin_unlock(&ch->irqlock);
		return IRQ_NONE;
	}
	if (st & INT_ERRORS) {
		/* level-type: report once, then mask (else an IRQ storm) */
		if (!(ch->errors & st))
			dev_warn_ratelimited(ch->cam->dev,
					     "%s: CAMSV error status %#x, IMGO %#x\n",
					     ch->cfg->name, st,
					     readl(ch->camsv + CAMSV_IMGO_ERR_STAT));
		ch->errors |= st & INT_ERRORS;
		writel(readl(ch->camsv + CAMSV_INT_EN) & ~(st & INT_ERRORS),
		       ch->camsv + CAMSV_INT_EN);
	}
	if (st & INT_SOF) {
		ch->sequence++;
		ch->latch_pending = false;
	}
	/* completed frames, in programming order */
	wcnt = readl(ch->camsv + CAMSV_FBC_IMGO_CTL2) >> 8;
	n = wcnt - ch->wcnt;
	ch->wcnt = wcnt;
	while (n-- && !list_empty(&ch->inflight)) {
		buf = list_first_entry(&ch->inflight, struct cam_buf, list);
		/*
		 * The first completion after stream-on carries no image (the
		 * buffer is untouched: stale memory with IOMMU buffers, which
		 * flashed garbage on camera switches and captures): reuse it.
		 * Later ones like it: the markers.
		 */
		if (ch->skip) {
			ch->skip--;
			list_move_tail(&buf->list, &ch->queued);
			continue;
		}
		if (!cam_buf_written(ch, buf)) {
			ch->unwritten++;
			cam_mark_buf(ch, buf);
			list_move_tail(&buf->list, &ch->queued);
			continue;
		}
		list_del(&buf->list);
		buf->vb.vb2_buf.timestamp = ktime_get_ns();
		buf->vb.sequence = ch->sequence;
		buf->vb.field = V4L2_FIELD_NONE;
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}
	cam_program_next(ch);
	spin_unlock(&ch->irqlock);
	return IRQ_HANDLED;
}

static void cam_rmw(void __iomem *reg, u32 mask, u32 val)
{
	writel((readl(reg) & ~mask) | val, reg);
}

static void cam_hw_setup(struct cam_chan *ch)
{
	const struct cam_chan_cfg *cfg = ch->cfg;
	struct cezanne_cam *c = ch->cam;
	void __iomem *sv = ch->camsv;
	unsigned int m = cfg->seninf_mux, cm = ENGINE_CAM_MUX(ch->sv);
	int i;

	/* CAMSV0: physical DMA addresses, no IOMMU page table for this port */
	if (!ch->sv)
		writel(0, c->larb14 + LARB_NONSEC_CON(LARB14_CAMSV0_PORT));

	mutex_lock(&c->power_lock);	/* shared seninf top / cam mux words */
	/* MIPI RX analog first, then seninf in address order */
	for (i = 0; i < cfg->n_mipi_rx; i++)
		writel(cfg->mipi_rx[i].val, c->mipi_rx + cfg->mipi_rx[i].ofs);
	for (i = 0; i < cfg->n_seninf; i++)
		writel(cfg->seninf[i].val, c->seninf + cfg->seninf[i].ofs);

	/* seninf mux <- receiver, 1 pixel/clock, reset */
	cam_rmw(c->seninf + SENINF_MUX_SRC(m), 0xff << (8 * (m % 4)),
		cfg->receiver << (8 * (m % 4)));
	writel(0x001f0008, c->seninf + SENINF_MUX_CTRL_1(m));
	writel(0x7, c->seninf + SENINF_MUX_CTRL_0(m));
	writel(0x1, c->seninf + SENINF_MUX_CTRL_0(m));

	/* cam mux (-> CAMSV) <- seninf mux */
	cam_rmw(c->seninf + CAM_MUX_CTRL(cm), 0xf << (8 * (cm % 4)),
		m << (8 * (cm % 4)));
	writel(0, c->seninf + CAM_MUX_OPT(cm));
	writel(0, c->seninf + CAM_MUX_CHK_CTL_0(cm));
	writel(cfg->height << 16 | cfg->width, c->seninf + CAM_MUX_CHK_CTL_1(cm));
	cam_rmw(c->seninf + CAM_MUX_EN, 0, BIT(cm));
	mutex_unlock(&c->power_lock);

	/* CAMSV: reset, TG window, RAW8 pack, IMGO, FBC */
	writel(0x4, sv + CAMSV_SW_CTL);
	writel(0x0, sv + CAMSV_SW_CTL);
	writel(0x1, sv + CAMSV_SW_CTL);
	writel(0x0, sv + CAMSV_SW_CTL);
	writel(SEN_MODE_TIME_STP_EN, sv + CAMSV_TG_SEN_MODE);
	writel(cfg->width << 16, sv + CAMSV_TG_GRAB_PXL);
	writel(cfg->height << 16, sv + CAMSV_TG_GRAB_LIN);
	writel(0, sv + CAMSV_TG_PATH_CFG);
	readl(sv + CAMSV_INT_STATUS);
	writel(INT_VS1 | INT_SOF | INT_PASS1_DONE | INT_ERRORS,
	       sv + CAMSV_INT_EN);
	writel(ch->raw8 ? 0 : 1, sv + CAMSV_FMT_SEL);
	writel(0x8005, sv + CAMSV_CLK_EN);	/* TG, PAK, DMA */
	writel(ch->raw8 ? PAK_RAW8 : PAK_RAW10_UNPACKED, sv + CAMSV_PAK);
	writel(0x000e0010, sv + 0x02d0);	/* vendor [20:16] = 0xe */
	writel(0, sv + 0x0058);			/* vendor clears bit 7 */
	writel(0xffffffff, sv + 0x07c4);
	writel(1, sv + CAMSV_DMA_DB_EN);
	writel(0, sv + CAMSV_IMGO_OFST);
	writel(cam_line_bytes(ch) - 1, sv + CAMSV_IMGO_XSIZE);
	writel(cfg->height - 1, sv + CAMSV_IMGO_YSIZE);
	writel(lower_32_bits(c->scratch_dma), sv + CAMSV_FRAME_WRITE_BASE);
	writel(CAM_STRIDE(cam_line_bytes(ch)), sv + CAMSV_IMGO_STRIDE);
	writel(MODULE_DEFAULT | MODULE_TG_EN | MODULE_PAK_EN, sv + CAMSV_MODULE_EN);
	writel(SEN_MODE_TIME_STP_EN | SEN_MODE_CMOS_EN, sv + CAMSV_TG_SEN_MODE);
	writel(MODULE_DEFAULT | MODULE_TG_EN | MODULE_PAK_EN | MODULE_IMGO_EN,
	       sv + CAMSV_MODULE_EN);
	writel(FBC_MODE, sv + CAMSV_FBC_IMGO_CTL1);
	writel(FBC_EN | BIT(10) | FBC_MODE, sv + CAMSV_FBC_IMGO_CTL1);
}

static void cam_hw_stop(struct cam_chan *ch)
{
	void __iomem *sv = ch->camsv;

	writel(0, sv + CAMSV_TG_VF_CON);
	writel(0, sv + CAMSV_TG_SEN_MODE);
	writel(0, sv + CAMSV_INT_EN);
	writel(MODULE_DEFAULT, sv + CAMSV_MODULE_EN);
	writel(0, sv + CAMSV_FBC_IMGO_CTL1);
	readl(sv + CAMSV_INT_STATUS);
}

/* -------------------------------------------------------------------------
 * vb2
 */

static int cam_queue_setup(struct vb2_queue *q, unsigned int *nbuf,
			   unsigned int *nplanes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	struct cam_chan *ch = vb2_get_drv_priv(q);
	unsigned int size = cam_sizeimage(ch);

	/*
	 * Cached buffers: the software ISP reads every frame on the CPU for
	 * its statistics, and reads from the default uncached mapping are
	 * several times slower.  vb2 syncs the caches when a buffer is
	 * dequeued (and for dma-buf CPU access); set here because REQBUFS
	 * resets it from the user flags right before calling queue_setup.
	 */
	if (!vb2_get_num_buffers(q))
		q->non_coherent_mem = true;

	if (*nplanes)
		return (*nplanes != 1 || sizes[0] < size) ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

static int cam_buf_prepare(struct vb2_buffer *vb)
{
	struct cam_chan *ch = vb2_get_drv_priv(vb->vb2_queue);
	struct cam_buf *buf = to_cam_buf(to_vb2_v4l2_buffer(vb));
	unsigned int size = cam_sizeimage(ch);

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, size);
	/* mapped here, where it may sleep; the markers are checked in the IRQ */
	buf->va = vb2_plane_vaddr(vb, 0);
	cam_mark_buf(ch, buf);
	return 0;
}

static void cam_buf_queue(struct vb2_buffer *vb)
{
	struct cam_chan *ch = vb2_get_drv_priv(vb->vb2_queue);
	struct cam_buf *buf = to_cam_buf(to_vb2_v4l2_buffer(vb));
	unsigned long flags;

	spin_lock_irqsave(&ch->irqlock, flags);
	list_add_tail(&buf->list, &ch->queued);
	cam_program_next(ch);
	spin_unlock_irqrestore(&ch->irqlock, flags);
}

static void cam_return_buffers(struct cam_chan *ch,
			       enum vb2_buffer_state state)
{
	struct cam_buf *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&ch->irqlock, flags);
	list_splice_tail_init(&ch->inflight, &ch->queued);
	list_for_each_entry_safe(buf, tmp, &ch->queued, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&ch->irqlock, flags);
}

/* the channels share CAMSV engines: one stream per engine */
static int cam_claim_engine(struct cam_chan *ch)
{
	struct cezanne_cam *c = ch->cam;
	int ret = 0;

	mutex_lock(&c->power_lock);
	if (c->engine_owner[ch->sv])
		ret = -EBUSY;
	else
		c->engine_owner[ch->sv] = ch;
	mutex_unlock(&c->power_lock);
	if (ret)
		dev_dbg(c->dev, "%s: CAMSV%u busy\n", ch->cfg->name, ch->sv);
	return ret;
}

static void cam_release_engine(struct cam_chan *ch)
{
	struct cezanne_cam *c = ch->cam;

	mutex_lock(&c->power_lock);
	if (c->engine_owner[ch->sv] == ch)
		c->engine_owner[ch->sv] = NULL;
	mutex_unlock(&c->power_lock);
}

static void cam_notify_streaming(struct cam_chan *ch)
{
	sysfs_notify(&ch->cam->dev->kobj, NULL,
		     ch == &ch->cam->ch[0] ? "streaming" : "streaming_main");
}

static void cam_set_streaming(struct cam_chan *ch, bool on)
{
	unsigned long flags;

	spin_lock_irqsave(&ch->irqlock, flags);
	ch->streaming = on;
	spin_unlock_irqrestore(&ch->irqlock, flags);
	cam_notify_streaming(ch);
}

static int cam_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct cam_chan *ch = vb2_get_drv_priv(q);
	unsigned long flags;
	int ret;

	if (!ch->sensor) {
		ret = -ENODEV;
		goto err_return;
	}
	ret = cam_claim_engine(ch);
	if (ret)
		goto err_return;
	ret = video_device_pipeline_start(&ch->vdev, &ch->pipe);
	if (ret)
		goto err_return;
	ret = cam_power(ch, true);
	if (ret)
		goto err_pipeline;

	cam_hw_setup(ch);
	spin_lock_irqsave(&ch->irqlock, flags);
	ch->wcnt = readl(ch->camsv + CAMSV_FBC_IMGO_CTL2) >> 8;
	ch->sequence = 0;
	ch->errors = 0;
	ch->latch_pending = false;
	ch->skip = 1;
	ch->streaming = true;
	cam_program_next(ch);		/* first buffer before the TG starts */
	spin_unlock_irqrestore(&ch->irqlock, flags);
	cam_notify_streaming(ch);
	writel(1, ch->camsv + CAMSV_TG_VF_CON);

	ret = v4l2_subdev_call(ch->sensor, video, s_stream, 1);
	if (ret) {
		dev_err(ch->cam->dev, "%s: sensor stream on: %d\n",
			ch->cfg->name, ret);
		goto err_hw;
	}
	return 0;

err_hw:
	cam_set_streaming(ch, false);
	cam_hw_stop(ch);
	cam_power(ch, false);
err_pipeline:
	video_device_pipeline_stop(&ch->vdev);
	cam_release_engine(ch);
err_return:
	cam_return_buffers(ch, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void cam_stop_streaming(struct vb2_queue *q)
{
	struct cam_chan *ch = vb2_get_drv_priv(q);

	v4l2_subdev_call(ch->sensor, video, s_stream, 0);
	cam_set_streaming(ch, false);
	cam_hw_stop(ch);
	synchronize_irq(ch->irq);
	cam_power(ch, false);
	video_device_pipeline_stop(&ch->vdev);
	cam_release_engine(ch);
	cam_return_buffers(ch, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops cam_vb2_ops = {
	.queue_setup = cam_queue_setup,
	.buf_prepare = cam_buf_prepare,
	.buf_queue = cam_buf_queue,
	.start_streaming = cam_start_streaming,
	.stop_streaming = cam_stop_streaming,
};

/* -------------------------------------------------------------------------
 * V4L2 video node (MC-centric: the format is fixed by the sensor mode)
 */

static void cam_fill_pix(struct cam_chan *ch, struct v4l2_pix_format *pix,
			 bool raw8)
{
	const struct cam_chan_cfg *cfg = ch->cfg;
	unsigned int line = cfg->width * (raw8 ? 1 : 2);

	pix->width = cfg->width;
	pix->height = cfg->height;
	pix->pixelformat = raw8 ? cfg->pixfmt8 : cfg->pixfmt;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = CAM_STRIDE(line);
	pix->sizeimage = CAM_STRIDE(line) * cfg->height;
	pix->colorspace = V4L2_COLORSPACE_RAW;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int cam_querycap(struct file *file, void *fh,
			struct v4l2_capability *cap)
{
	strscpy(cap->driver, "mtk-seninf", sizeof(cap->driver));
	strscpy(cap->card, "Redmi K30 Ultra CAMSV", sizeof(cap->card));
	return 0;
}

/* 10 bits (in 16) first, 8 bits (the top ones) second */
static int cam_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	struct cam_chan *ch = video_drvdata(file);

	if (f->index > (raw10 ? 1 : 0) ||
	    (f->mbus_code && f->mbus_code != ch->cfg->code))
		return -EINVAL;
	f->pixelformat = f->index || !raw10 ? ch->cfg->pixfmt8 : ch->cfg->pixfmt;
	return 0;
}

static int cam_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct cam_chan *ch = video_drvdata(file);

	cam_fill_pix(ch, &f->fmt.pix, ch->raw8);
	return 0;
}

static int cam_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct cam_chan *ch = video_drvdata(file);

	cam_fill_pix(ch, &f->fmt.pix,
		     !raw10 || f->fmt.pix.pixelformat == ch->cfg->pixfmt8);
	return 0;
}

static int cam_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct cam_chan *ch = video_drvdata(file);

	if (vb2_is_busy(&ch->queue))
		return -EBUSY;
	ch->raw8 = !raw10 || f->fmt.pix.pixelformat == ch->cfg->pixfmt8;
	cam_fill_pix(ch, &f->fmt.pix, ch->raw8);
	return 0;
}

static int cam_enum_framesizes(struct file *file, void *fh,
			       struct v4l2_frmsizeenum *fsize)
{
	struct cam_chan *ch = video_drvdata(file);

	if (fsize->index || (fsize->pixel_format != ch->cfg->pixfmt &&
			     fsize->pixel_format != ch->cfg->pixfmt8))
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = ch->cfg->width;
	fsize->discrete.height = ch->cfg->height;
	return 0;
}

static const struct v4l2_ioctl_ops cam_ioctl_ops = {
	.vidioc_querycap = cam_querycap,
	.vidioc_enum_fmt_vid_cap = cam_enum_fmt,
	.vidioc_g_fmt_vid_cap = cam_g_fmt,
	.vidioc_try_fmt_vid_cap = cam_try_fmt,
	.vidioc_s_fmt_vid_cap = cam_s_fmt,
	.vidioc_enum_framesizes = cam_enum_framesizes,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations cam_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
	.poll = vb2_fop_poll,
};

static int cam_link_validate(struct media_link *link)
{
	struct cam_chan *ch = container_of(link->sink->entity, struct cam_chan,
					   vdev.entity);
	const struct cam_chan_cfg *cfg = ch->cfg;
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = link->source->index,
	};
	int ret;

	ret = v4l2_subdev_call_state_active(ch->sensor, pad, get_fmt, &fmt);
	if (ret)
		return ret;
	if (fmt.format.code != cfg->code || fmt.format.width != cfg->width ||
	    fmt.format.height != cfg->height)
		return -EPIPE;
	return 0;
}

static const struct media_entity_operations cam_entity_ops = {
	.link_validate = cam_link_validate,
};

/* -------------------------------------------------------------------------
 * Sensor binding
 */

static int cam_notify_bound(struct v4l2_async_notifier *nf,
			    struct v4l2_subdev *sd,
			    struct v4l2_async_connection *asc)
{
	struct cam_chan *ch = container_of(asc, struct cam_async, asc)->ch;
	int pad, ret;

	pad = media_entity_get_fwnode_pad(&sd->entity, sd->fwnode,
					  MEDIA_PAD_FL_SOURCE);
	if (pad < 0)
		pad = 0;
	ret = media_create_pad_link(&sd->entity, pad, &ch->vdev.entity, 0,
				    MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		return ret;
	ch->sensor = sd;
	dev_info(ch->cam->dev, "%s: bound %s\n", ch->cfg->name, sd->name);
	return 0;
}

static void cam_notify_unbind(struct v4l2_async_notifier *nf,
			      struct v4l2_subdev *sd,
			      struct v4l2_async_connection *asc)
{
	container_of(asc, struct cam_async, asc)->ch->sensor = NULL;
}

static int cam_notify_complete(struct v4l2_async_notifier *nf)
{
	struct cezanne_cam *c = container_of(nf, struct cezanne_cam, notifier);
	int ret;

	ret = v4l2_device_register_subdev_nodes(&c->v4l2);
	if (ret)
		return ret;
	return media_device_register(&c->mdev);
}

static const struct v4l2_async_notifier_operations cam_notify_ops = {
	.bound = cam_notify_bound,
	.unbind = cam_notify_unbind,
	.complete = cam_notify_complete,
};

/* one sensor per port: port n -> channel n */
static int cam_parse_endpoints(struct cezanne_cam *c)
{
	struct fwnode_handle *ep;
	int ret, n = 0;

	v4l2_async_nf_init(&c->notifier, &c->v4l2);
	fwnode_graph_for_each_endpoint(dev_fwnode(c->dev), ep) {
		struct fwnode_endpoint fep;
		struct cam_async *ca;

		ret = fwnode_graph_parse_endpoint(ep, &fep);
		if (ret || fep.port >= CAM_NUM_CHANNELS)
			continue;
		ca = v4l2_async_nf_add_fwnode_remote(&c->notifier, ep,
						     struct cam_async);
		if (IS_ERR(ca)) {
			fwnode_handle_put(ep);
			v4l2_async_nf_cleanup(&c->notifier);
			return PTR_ERR(ca);
		}
		ca->ch = &c->ch[fep.port];
		n++;
	}
	if (!n)
		return dev_err_probe(c->dev, -ENODEV, "no sensor endpoint\n");
	c->notifier.ops = &cam_notify_ops;
	ret = v4l2_async_nf_register(&c->notifier);
	if (ret)
		v4l2_async_nf_cleanup(&c->notifier);
	return ret;
}

/* -------------------------------------------------------------------------
 * sysfs / debugfs
 */

/* "1" while frames are captured: cezanne-popupd raises the camera for it */
static ssize_t streaming_show(struct device *dev, struct device_attribute *a,
			      char *buf)
{
	struct cezanne_cam *c = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(c->ch[0].streaming));
}
static DEVICE_ATTR_RO(streaming);

static ssize_t streaming_main_show(struct device *dev,
				   struct device_attribute *a, char *buf)
{
	struct cezanne_cam *c = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(c->ch[1].streaming));
}
static DEVICE_ATTR_RO(streaming_main);

static struct attribute *cam_attrs[] = {
	&dev_attr_streaming.attr,
	&dev_attr_streaming_main.attr,
	NULL
};
ATTRIBUTE_GROUPS(cam);

/*
 * debugfs "regs": seninf, MIPI RX and the CAMSV engines in tools/camdump.py's
 * format (16-byte name, u32 phys, u32 len, words), only while powered.
 */
static ssize_t regs_read(struct file *f, char __user *ubuf, size_t len,
			 loff_t *ppos)
{
	struct cezanne_cam *c = f->private_data;
	void __iomem *bases[3] = { c->seninf, c->mipi_rx, c->camsv };
	size_t total = 0, off = 0;
	ssize_t ret;
	char *buf;
	int b, i;

	mutex_lock(&c->power_lock);
	if (!c->power_count) {
		mutex_unlock(&c->power_lock);
		return -EIO;
	}
	for (b = 0; b < 3; b++)
		total += resource_size(c->res[b]);
	total += 24 * (2 + resource_size(c->res[2]) / 0x1000);
	buf = kvzalloc(total, GFP_KERNEL);
	if (!buf) {
		mutex_unlock(&c->power_lock);
		return -ENOMEM;
	}
	for (b = 0; b < 3; b++) {
		u32 size = resource_size(c->res[b]);
		u32 phys = c->res[b]->start;
		u32 step = b == 2 ? 0x1000 : size;
		u32 o;

		for (o = 0; o < size; o += step) {
			if (b == 2)
				snprintf(buf + off, 16, "camsv%u", o / 0x1000);
			else
				strscpy(buf + off, b ? "mipi_rx" : "seninf", 16);
			*(u32 *)(buf + off + 16) = phys + o;
			*(u32 *)(buf + off + 20) = step;
			off += 24;
			for (i = 0; i < step; i += 4, off += 4)
				*(u32 *)(buf + off) = readl(bases[b] + o + i);
		}
	}
	mutex_unlock(&c->power_lock);
	ret = simple_read_from_buffer(ubuf, len, ppos, buf, off);
	kvfree(buf);
	return ret;
}

static const struct file_operations regs_fops = {
	.open = simple_open,
	.read = regs_read,
	.llseek = default_llseek,
};

/* -------------------------------------------------------------------------
 * Probe
 */

static int cam_register_video(struct cam_chan *ch)
{
	struct video_device *vdev = &ch->vdev;
	struct vb2_queue *q = &ch->queue;
	int ret;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = ch;
	q->buf_struct_size = sizeof(struct cam_buf);
	q->ops = &cam_vb2_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 1;
	q->lock = &ch->lock;
	q->dev = ch->cam->dev;
	ret = vb2_queue_init(q);
	if (ret)
		return ret;

	snprintf(vdev->name, sizeof(vdev->name), "cezanne-%s", ch->cfg->name);
	vdev->fops = &cam_fops;
	vdev->ioctl_ops = &cam_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->v4l2_dev = &ch->cam->v4l2;
	vdev->queue = q;
	vdev->lock = &ch->lock;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			    V4L2_CAP_IO_MC;
	vdev->entity.ops = &cam_entity_ops;
	ch->vpad.flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	ret = media_entity_pads_init(&vdev->entity, 1, &ch->vpad);
	if (ret)
		return ret;
	video_set_drvdata(vdev, ch);
	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		media_entity_cleanup(&vdev->entity);
	return ret;
}

static void cam_unregister_videos(struct cezanne_cam *c)
{
	int i;

	for (i = 0; i < CAM_NUM_CHANNELS; i++) {
		if (!video_is_registered(&c->ch[i].vdev))
			continue;
		video_unregister_device(&c->ch[i].vdev);
		media_entity_cleanup(&c->ch[i].vdev.entity);
	}
}

static int cam_init_chan(struct platform_device *pdev, struct cezanne_cam *c,
			 int n)
{
	struct cam_chan *ch = &c->ch[n];
	int ret;

	ch->cam = c;
	ch->cfg = &cam_chan_cfgs[n];
	ch->raw8 = !raw10;
	ch->sv = n < num_engines ? engines[n] :
		 !c->iommu ? 0 : ch->cfg->own_engine ? 3 : 2;
	if (ch->sv >= CAM_NUM_CAMSV || !ch->sv != !c->iommu)
		return dev_err_probe(c->dev, -EINVAL, "%s: no CAMSV%u\n",
				     ch->cfg->name, ch->sv);
	ch->camsv = c->camsv + 0x1000 * ch->sv;
	mutex_init(&ch->lock);
	spin_lock_init(&ch->irqlock);
	INIT_LIST_HEAD(&ch->queued);
	INIT_LIST_HEAD(&ch->inflight);
	ch->irq = platform_get_irq(pdev, ch->sv);
	if (ch->irq < 0)
		return ch->irq;
	ret = devm_request_irq(c->dev, ch->irq, camsv_irq, IRQF_SHARED,
			       dev_name(c->dev), ch);
	if (ret)
		return dev_err_probe(c->dev, ret, "irq %d\n", n);
	return 0;
}

static int cezanne_cam_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cezanne_cam *c;
	int i, ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = dev;
	mutex_init(&c->power_lock);

	c->res[0] = platform_get_resource_byname(pdev, IORESOURCE_MEM, "seninf");
	c->res[1] = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mipi_rx");
	c->res[2] = platform_get_resource_byname(pdev, IORESOURCE_MEM, "camsv");
	c->seninf = devm_ioremap_resource(dev, c->res[0]);
	c->mipi_rx = devm_ioremap_resource(dev, c->res[1]);
	c->camsv = devm_ioremap_resource(dev, c->res[2]);
	/* the larbs belong to the SMI driver once there is one: plain ioremap */
	c->larb14 = devm_platform_ioremap_resource_byname(pdev, "larb");
	if (IS_ERR(c->seninf) || IS_ERR(c->mipi_rx) || IS_ERR(c->camsv) ||
	    IS_ERR(c->larb14))
		return -ENOMEM;
	c->iommu = device_iommu_mapped(dev);
	if (resource_size(c->res[2]) < 0x1000 * (c->iommu ? CAM_NUM_CAMSV : 1))
		return dev_err_probe(dev, -EINVAL, "camsv region too small\n");
	c->num_clks = devm_clk_bulk_get_all(dev, &c->clks);
	if (c->num_clks < 0)
		return dev_err_probe(dev, c->num_clks, "clocks\n");
	platform_set_drvdata(pdev, c);
	/* larb13: IOVAs (region 0, patch 0043); CAMSV0: CMA below 4 GiB */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "dma mask\n");
	c->scratch = dmam_alloc_coherent(dev, CAM_SCRATCH_SIZE, &c->scratch_dma,
					 GFP_KERNEL);
	if (!c->scratch)
		return -ENOMEM;
	for (i = 0; i < CAM_NUM_CHANNELS; i++) {
		ret = cam_init_chan(pdev, c, i);
		if (ret)
			return ret;
	}
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	c->mdev.dev = dev;
	strscpy(c->mdev.model, "Redmi K30 Ultra camera", sizeof(c->mdev.model));
	/* libcamera's simple pipeline handler matches on this */
	strscpy(c->mdev.driver_name, "mtk-seninf", sizeof(c->mdev.driver_name));
	media_device_init(&c->mdev);
	c->v4l2.mdev = &c->mdev;
	ret = v4l2_device_register(dev, &c->v4l2);
	if (ret)
		goto err_mdev;
	for (i = 0; i < CAM_NUM_CHANNELS; i++) {
		ret = cam_register_video(&c->ch[i]);
		if (ret)
			goto err_video;
	}
	ret = cam_parse_endpoints(c);
	if (ret)
		goto err_video;

	c->debugfs = debugfs_create_dir("cezanne-camsv", NULL);
	debugfs_create_file("regs", 0400, c->debugfs, c, &regs_fops);
	for (i = 0; i < CAM_NUM_CHANNELS; i++) {
		char name[32];

		snprintf(name, sizeof(name), "unwritten-%s", c->ch[i].cfg->name);
		debugfs_create_u32(name, 0444, c->debugfs, &c->ch[i].unwritten);
	}
	dev_info(dev, "camera front end, %d clocks\n", c->num_clks);
	return 0;

err_video:
	cam_unregister_videos(c);
	v4l2_device_unregister(&c->v4l2);
err_mdev:
	media_device_cleanup(&c->mdev);
	return ret;
}

static void cezanne_cam_remove(struct platform_device *pdev)
{
	struct cezanne_cam *c = platform_get_drvdata(pdev);
	int i;

	debugfs_remove_recursive(c->debugfs);
	media_device_unregister(&c->mdev);
	v4l2_async_nf_unregister(&c->notifier);
	v4l2_async_nf_cleanup(&c->notifier);
	cam_unregister_videos(c);
	v4l2_device_unregister(&c->v4l2);
	media_device_cleanup(&c->mdev);
	for (i = 0; i < CAM_NUM_CHANNELS; i++)
		cam_power(&c->ch[i], false);
}

static const struct of_device_id cezanne_cam_of_match[] = {
	{ .compatible = "xiaomi,cezanne-camsv" },
	{ }
};
MODULE_DEVICE_TABLE(of, cezanne_cam_of_match);

static struct platform_driver cezanne_cam_driver = {
	.probe = cezanne_cam_probe,
	.remove = cezanne_cam_remove,
	.driver = {
		.name = "cezanne-camsv",
		.of_match_table = cezanne_cam_of_match,
		.dev_groups = cam_groups,
	},
};
module_platform_driver(cezanne_cam_driver);

MODULE_DESCRIPTION("Redmi K30 Ultra camera front end (seninf + CAMSV)");
MODULE_LICENSE("GPL");
