/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cezanne-venc: V4L2 stateful H.264 encoder for the MT6885 VENC (both
 * cores, the NBM engines in the MM SRAM).
 * Register model: NOTES "Video codec: H.264 encoder bring-up" and
 * mainline/vcodec/proto/venc_h264.py, venc_dual.py.
 */
#ifndef CEZANNE_VENC_H
#define CEZANNE_VENC_H

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#define VENC_CORES		2
#define VENC_REG_SIZE		0x2000		/* per core */
#define VENC_TIMEOUT_MS		1000

#define VENC_MIN_W		64
#define VENC_MIN_H		64
#define VENC_MAX_W		4096
#define VENC_MAX_H		4096	/* either side, e.g. 1080x2400 portrait */
#define VENC_MAX_MBS		(4096 / 16 * 2304 / 16)	/* the area of 4096x2304 */

/* registers (offsets per core) */
#define VENC_MODE		0x000	/* 0x0200bf60: rate control unit off */
#define VENC_GOP_QPI		0x004	/* I QP [31:26], intra period [27:14], IDR period [13:0] */
#define VENC_QPP		0x008	/* P QP [15:10] + flags */
#define VENC_SEQ_FLAGS		0x00c
#define VENC_FRAME_TYPE		0x010
#define VENC_FRAME_CNT		0x014
#define VENC_POC_CNT		0x018
#define VENC_IDR_CNT		0x01c	/* the counter at the last IDR */
#define VENC_MB_SIZE		0x024	/* mb_h << 16 | mb_w */
#define VENC_PITCH_FMT		0x028
#define VENC_MB_COUNT		0x02c
#define VENC_PROFILE		0x030
#define VENC_RC_EN		0x048
#define VENC_RC_FPS		0x04c
#define VENC_RC_QP_RANGE	0x050
#define VENC_START		0x058	/* 4: encode a frame */
#define VENC_IRQ_STATUS		0x05c
#define VENC_IRQ_ACK		0x060
#define VENC_BS_ADDR		0x064	/* all addresses >> 4 */
#define VENC_BS_SIZE		0x068	/* bytes >> 7 */
#define VENC_BS_ALIGN		128	/* the output start's granularity */
#define VENC_SRC_Y		0x06c
#define VENC_SRC_C		0x070
#define VENC_REC		0x074	/* compressed reconstruction, luma + chroma */
#define VENC_REC_C		0x078	/* unused by the hardware, written anyway */
#define VENC_REF		0x07c
#define VENC_REF_C		0x080
#define VENC_MV_OUT		0x084
#define VENC_MV_IN		0x088
#define VENC_RC_CODE		0x08c
#define VENC_RC_INFO		0x090
#define VENC_SRC_CR		0x094	/* three-plane input */
#define VENC_BS_BYTES		0x098	/* bytes written, start code included */
#define VENC_IRQ_EN		0x0a4
#define VENC_SW_HRST_N		0x0a8
#define VENC_SW_PAUSE		0x0ac
#define VENC_CLK_EN		0x0ec
#define VENC_CORE_CNT		0x13c	/* (cores - 1) << 16 */
#define VENC_CORE_DIST		0x1168	/* MB rows a frame trails the one before << 4 | 10 */
#define VENC_CORE_SYNC		0x1170	/* buffer both cores share */
#define VENC_CORE_ID		0x11c0	/* 0 (BSP: the encoder instance) */
#define VENC_CORE_FRAME		0x11c4	/* -frame */
/*
 * RGB input, converted by the hardware (BSP H264_SetArgb; libvcodecdrv.so
 * tables by VENC_YUV_FORMAT 11..23): one plane, +028 layout 0
 */
#define VENC_RGB_CSC		0x144	/* conversion: */
#define VENC_CSC_709		BIT(3)	/* BT.709, else BT.601 */
#define VENC_CSC_LIMITED	BIT(4)	/* limited range out, else full */
#define VENC_RGB_FMT		0x1ec	/* 0: YUV input, else: */
#define VENC_RGB_EN		BIT(31)
#define VENC_RGB_4BYTES		BIT(8)	/* 32 bits a pixel, else 24 */
#define VENC_RGB_ALPHA_FIRST	BIT(4)	/* A / X byte first in memory */
#define VENC_RGB_BGR		BIT(0)	/* B before R in memory */
#define VENC_RGB_PITCH		0x1f0	/* bytes */

#define VENC_IRQ_FRAME		BIT(2)
#define VENC_IRQ_DRAM_FULL	BIT(3)
#define VENC_IRQ_PAUSE		BIT(4)

/* +028 [31:29] input layout (vendor H.264 path accepts these three) */
#define VENC_FMT_NV12		0
#define VENC_FMT_3PLANE		3
#define VENC_FMT_NV21		4

struct venc_fmt {
	u32 fourcc;
	u8 planes;		/* memory planes */
	u8 comps;		/* colour planes: 2 semi-planar, 3 planar, 1 RGB */
	u8 hw;			/* +028 layout */
	bool swap_uv;		/* YVU: Cr plane first */
	u32 rgb;		/* +1ec for RGB input, else 0 */
	u8 bpp;			/* RGB: bytes a pixel */
};

struct venc_buf {
	void *va;
	dma_addr_t dma;
	size_t size;
};

struct venc_ctx;

/* a frame on a core; done jobs complete in their context's order */
struct venc_job {
	struct venc_ctx *ctx;
	struct vb2_v4l2_buffer *src, *dst;
	unsigned int core;
	bool idr, done;
	unsigned int hdr_off;		/* bytes before the hardware's slice */
	int qp;
	u32 bytes;
	enum vb2_buffer_state state;
};

struct venc_core {
	struct venc_dev *d;
	unsigned int id;
	void __iomem *regs;
	int irq;
	struct venc_job *job;		/* running, under hw_lock */
	struct delayed_work watchdog;
	u32 irq_status;
	ktime_t start, t_irq;
};

struct venc_sram;

struct venc_dev {
	struct device *dev;
	struct device *pd[VENC_CORES];
	struct device *larb[VENC_CORES];
	void __iomem *larb_regs[VENC_CORES];
	struct venc_sram *sram;
	struct venc_core core[VENC_CORES];
	unsigned int ncores;
	spinlock_t hw_lock;
	struct mutex done_lock;		/* completing jobs */
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
	struct mutex vdev_lock;
	/* usage for monitors (vcodec_* attributes) */
	spinlock_t stats_lock;
	u64 busy_ns, jobs;
	atomic64_t mem_used;
	struct mutex clients_lock;
	struct list_head clients;
};

/* simple frame-level rate control: QP from the size of earlier frames */
struct venc_rc {
	int qp_q8;		/* P QP, 8 fractional bits */
	s64 fullness;		/* virtual buffer, bits over budget */
	u32 frames;
};

struct venc_ctx {
	struct v4l2_fh fh;
	struct venc_dev *dev;
	struct v4l2_ctrl_handler ctrl_hdl;
	struct list_head client;
	pid_t tgid;
	char comm[16];
	u64 busy_ns, jobs;

	const struct venc_fmt *src_fmt;
	struct v4l2_pix_format_mplane src, dst;
	struct v4l2_rect crop;		/* visible size, at 0,0 */
	struct v4l2_fract tpf;		/* time per frame */
	enum v4l2_colorspace colorspace;
	enum v4l2_xfer_func xfer_func;
	enum v4l2_ycbcr_encoding ycbcr_enc;
	enum v4l2_quantization quant;

	/* controls */
	struct v4l2_ctrl *c_bitrate, *c_rc_mode, *c_rc_en, *c_gop, *c_profile, *c_level,
			 *c_qpi, *c_qpp, *c_qpmin, *c_qpmax, *c_header, *c_repeat, *c_entropy,
			 *c_quality;
	bool force_key;

	/* stream state (start_streaming) */
	struct venc_buf rec[2], mv[2], rc_code, rc_info[VENC_CORES], sync;
	u32 frame, last_idr, idr_count, since_idr;
	bool header_sent;		/* separate header mode: header buffer done */
	struct venc_rc rc;
	u32 sequence_out, sequence_cap;
	/* the parameter sets of this stream */
	u8 hdr[128];
	unsigned int hdr_len;
	/*
	 * Frames on the cores, oldest first. Their buffers stay at the head
	 * of the m2m queues until they complete, so the drain helpers see the
	 * last source buffer as usual.
	 */
	struct venc_job frames[VENC_CORES];
	unsigned int job_head, job_count;	/* under hw_lock */
	wait_queue_head_t idle_wq;
};

static inline struct venc_ctx *file_to_venc_ctx(struct file *f)
{
	return container_of(file_to_v4l2_fh(f), struct venc_ctx, fh);
}

static inline u32 venc_r(struct venc_core *c, u32 reg)
{
	return readl(c->regs + reg);
}

static inline void venc_w(struct venc_core *c, u32 reg, u32 val)
{
	writel(val, c->regs + reg);
}

/* the core a context's next frame takes: frames alternate, as the BSP's */
static inline unsigned int venc_next_core(struct venc_ctx *ctx)
{
	return ctx->frame % ctx->dev->ncores;
}

/* venc_h264.c */
int venc_h264_ctrls_init(struct venc_ctx *ctx);
int venc_h264_start(struct venc_ctx *ctx);
void venc_h264_stop(struct venc_ctx *ctx);
unsigned int venc_h264_headers(struct venc_ctx *ctx, u8 *buf, unsigned int size);
void venc_h264_run(struct venc_ctx *ctx, struct venc_job *job);
void venc_h264_done(struct venc_ctx *ctx, struct venc_job *job);
size_t venc_h264_rec_size(u32 mb_w, u32 mb_h);
void venc_hw_reset(struct venc_core *c);

/* venc_sram.c */
extern struct platform_driver venc_sram_driver;
struct venc_sram *venc_sram_find(struct device *dev);
int venc_sram_on(struct venc_sram *s);
void venc_sram_off(struct venc_sram *s);

#endif
