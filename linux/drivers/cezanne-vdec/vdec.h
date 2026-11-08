/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cezanne-vdec: native V4L2 stateless decoder for the MT6885 (Redmi K30
 * Ultra) video decoder. Two engines per picture: LAT (bitstream parsing,
 * 0x16010000) and core (reconstruction, 0x16020000); they run together
 * ("racing"), so a job starts both and completes when both have raised
 * their decode-done IRQ. No IOMMU user yet: larb4 / larb5 ports stay in
 * physical mode, buffers come from a 32-bit DMA pool.
 *
 * See NOTES.md "Video codec" for what is known about the registers.
 */
#ifndef CEZANNE_VDEC_H
#define CEZANNE_VDEC_H

#include <linux/path.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-h264.h>
#include <media/v4l2-vp9.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#define VDEC_REG_SIZE		0x40000

/* blocks, offsets from 0x16000000 */
#define SOC_MCORE_TOP		0x00000		/* SPM status of the codec cores */

/* infracfg_ao AXI firewall for the VDEC2 (core) subsystem */
#define INFRACFG_AO_BASE	0x10001000
#define INFRACFG_PROT_MM_2_CLR	0xdd0
#define INFRACFG_PROT_VDEC2_BITS	(BIT(6) | BIT(7))
#define SOC_UFO_ENC		0x00800
#define SOC_2000		0x02000
#define SOC_RACING		0x04000		/* LAT<->core "inner racing" pointers */
#define SOC_GCON		0x0f000		/* vdecsys_soc gate control */
#define LAT_LARB		0x0d000		/* larb5 sleep-protect window */
#define CORE_LARB		0x2e000		/* larb4 sleep-protect window */
#define CORE_GCON		0x2f000		/* vdecsys gate control */
#define LAT_MISC		0x10000
#define LAT_VLD			0x11000
#define LAT_VLD_TOP		0x11800
#define LAT_AVC_VLD		0x12000
#define LAT_AVC_MV		0x13000
#define LAT_BS2			0x14800
#define LAT_VP9			0x15400
#define LAT_PP			0x16000
#define CORE_VLD		0x20000
#define CORE_VLD_TOP		0x20800
#define CORE_MC			0x21000
#define CORE_AVC_VLD		0x22000
#define CORE_AVC_MV		0x23000
#define CORE_PP			0x24000
#define CORE_MISC		0x25000
#define CORE_BS2		0x26800
#define CORE_VP9		0x28400
#define CORE_VP8		0x27800		/* VP8 VLD (bool decoder, barrel shifters) */

#define MISC_IRQ_CFG		0xa4
#define MISC_IRQ_DONE		BIT(16)

#define VDEC_MIN_W		64
#define VDEC_MIN_H		64
#define VDEC_MAX_W		4096
#define VDEC_VP8_MAX		2048
#define VDEC_MAX_H		2304

enum { VDEC_CORE, VDEC_LAT, VDEC_NUM_HW };

/*
 * Private ioctl on the video node: allocate a media request for the
 * caller (argument: int __user * receiving the fd), as MEDIA_IOC_REQUEST_ALLOC
 * on the media device does. For clients that may open the video node but
 * not the media node (Firefox's sandboxed decoder process).
 */
#define CEZANNE_VDEC_IOC_REQUEST_ALLOC	_IO('V', BASE_VIDIOC_PRIVATE + 0)
/* argument: the request fd (by value); MEDIA_REQUEST_IOC_QUEUE / _REINIT */
#define CEZANNE_VDEC_IOC_REQUEST_QUEUE	_IO('V', BASE_VIDIOC_PRIVATE + 1)
#define CEZANNE_VDEC_IOC_REQUEST_REINIT	_IO('V', BASE_VIDIOC_PRIVATE + 2)

struct vdec_dev {
	struct device *dev;
	struct path media_path;		/* our media node, for the private ioctl */
	const struct cred *media_cred;
	struct device *pd[VDEC_NUM_HW];
	struct device *larb[VDEC_NUM_HW];
	/* MDP-domain SMI common: the power path for the core's bus */
	struct device *smi_core;
	/* infracfg_ao: VDEC2 AXI firewall (PROT_EN_MM_2) */
	void __iomem *infracfg;
	void __iomem *regs;
	int irq[VDEC_NUM_HW];

	struct v4l2_device v4l2_dev;
	struct media_device mdev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
	struct mutex vdev_lock;

	spinlock_t irq_lock;
	u32 pending;			/* engines still running: BIT(VDEC_*) */
	u32 irq_status[VDEC_NUM_HW];
	struct delayed_work watchdog;
	struct vdec_ctx *cur;		/* context of the running job */

	/*
	 * Usage for monitors (sysfs vcodec_*, read by cezanne-top): time from
	 * a job's start to its completion, per device and per open file.
	 */
	spinlock_t stats_lock;
	ktime_t job_start;
	u64 busy_ns, jobs;
	atomic64_t mem_used;		/* bytes in our buffers, all contexts */
	struct mutex clients_lock;
	struct list_head clients;	/* vdec_ctx.client */
};

/* per capture buffer: its motion-vector buffer (needed while referenced) */
struct vdec_dst_buf {
	struct v4l2_m2m_buffer base;
	void *mv_va;
	dma_addr_t mv_dma;
	size_t mv_size;
};

/* the 3-slot core working buffers MIUI rotates (vld_top+23c / +240) */
#define VDEC_WORK_SLOTS		3

struct vdec_work_buf {
	void *va;
	dma_addr_t dma;
	size_t size;
};

/* a modified reference list may repeat pictures: up to 32 entries */
#define VDEC_H264_LIST_LEN	V4L2_H264_REF_LIST_LEN
#define VDEC_H264_NO_REF	0xff	/* "no reference picture" list entry */

/*
 * Slice header of the first slice NAL of the access unit, parsed from the
 * bitstream (vdec_h264.c). Frame-based clients do not send SLICE_PARAMS,
 * and the header words / reference lists need these fields.
 */
struct vdec_h264_shdr {
	bool parsed;
	u32 type;			/* 0 P, 1 B, 2 I, 3 SP, 4 SI */
	bool idr;
	u8 nal_ref_idc;
	u32 frame_num;
	u32 l0, l1;			/* num_ref_idx_lX_active_minus1 in effect */
	bool direct_spatial;
	s32 qp_delta;
	bool mmco5;
	/* ref_pic_list_modification(): commands per list, idc 3 excluded */
	bool mod_flag[2];
	u8 nmod[2];
	struct {
		u8 idc;
		u32 val;
	} mod[2][VDEC_H264_LIST_LEN];
};

struct vdec_h264_run {
	const struct v4l2_ctrl_h264_sps *sps;
	const struct v4l2_ctrl_h264_pps *pps;
	const struct v4l2_ctrl_h264_decode_params *dec;
	/* NULL unless the PPS flags a scaling matrix */
	const struct v4l2_ctrl_h264_scaling_matrix *sm;
	struct vdec_h264_shdr sh;

	/* avc_vld+088 / +090 / +098 / +09c, computed in vdec_h264.c */
	struct {
		u32 sps_word, pps_word, ref_word, slice_word, type_word;
	} words;

	struct vb2_v4l2_buffer *src, *dst;
	dma_addr_t bs_dma;		/* start of the Annex-B data */
	u32 bs_len;			/* bytes */
	u32 bs_size;			/* bytes in the source plane */
	dma_addr_t y_dma, c_dma, mv_dma;
	s32 poc_top, poc_bottom;	/* of this picture (DECODE_PARAMS) */

	/* DPB entry i -> buffer addresses (0 if the slot is unused or the
	 * reference buffer is gone), FrameNumWrap / LongTermFrameIdx */
	dma_addr_t ref_y[V4L2_H264_NUM_DPB_ENTRIES];
	dma_addr_t ref_c[V4L2_H264_NUM_DPB_ENTRIES];
	dma_addr_t ref_mv[V4L2_H264_NUM_DPB_ENTRIES];
	int ref_fnw[V4L2_H264_NUM_DPB_ENTRIES];
	bool ref_lt[V4L2_H264_NUM_DPB_ENTRIES];
	/* capture buffer index: the picture's id in the core tables */
	u8 ref_buf[V4L2_H264_NUM_DPB_ENTRIES];
	/* reference DPB entries in table order (PicNum descending) */
	u8 dpb_order[V4L2_H264_NUM_DPB_ENTRIES];
	u8 n_dpb;
	/* the two list tables: DPB indices or VDEC_H264_NO_REF */
	u8 l0[VDEC_H264_LIST_LEN], l1[VDEC_H264_LIST_LEN];
	u8 n_l0, n_l1;

	struct vdec_work_buf *ube, *err;
};

enum vdec_codec { VDEC_H264, VDEC_VP9, VDEC_VP8, VDEC_AV1, VDEC_HEVC };

struct vdec_hevc_run;

struct vdec_av1_state;
struct vdec_av1_run;

/* VP9 per-stream state: the four saved probability contexts, the
 * motion-vector buffers of the two engines (one each, as MIUI) and the
 * buffers the LAT dumps its context / counts into after each picture */
struct vdec_vp9_state {
	struct v4l2_vp9_frame_context fctx[4];
	struct vdec_work_buf lat_mv, core_mv, save_a, save_b, lat_seg, core_seg;
	u32 last_w, last_h;
	bool last_show, last_intra, have_last, last_key;
	/* backward adaptation of the running picture (frame_parallel = 0):
	 * done after the LAT dumped its symbol counts */
	bool adapt, counts_buf, refresh;
	u8 hw_tx_mode, hw_refmode;	/* read by the LAT from the compressed header */
	u8 comp_fixed;			/* persists like libvpx's comp_fixed_ref */
	u32 comp_word;			/* vp9+0b0 of the running picture */
	u8 adapt_idx, adapt_tx_mode, adapt_interp, adapt_refmode;
	bool adapt_intra, adapt_use_128;
	u32 adapt_flags;
	struct v4l2_vp9_frame_context pre, post;
};

/*
 * VP8 per-stream state: the segment-ID SRAM of the core (saved after and
 * restored before each picture, as MIUI and mainline's MT8173 driver do),
 * two working buffers (mc+a20 / +a24) and what the last keyframe header
 * said about pixel clamping.
 */
#define VDEC_VP8_SEG_WORDS	1088
/* per-picture area after Y and C that mc+850 points at (MIUI: 8 KiB at
 * 320x240, 64 KiB at 1280x720) */
#define VDEC_VP8_AUX(w, h)	ALIGN((w) / 16 * ((h) / 16) * 32, SZ_4K)
struct vdec_vp8_state {
	u32 seg_map[VDEC_VP8_SEG_WORDS];
	struct vdec_work_buf wb[2];
	bool clamping_off;
};

struct vdec_vp8_run {
	const struct v4l2_ctrl_vp8_frame *frame;
	struct vb2_v4l2_buffer *src, *dst;
	dma_addr_t bs_dma;
	u32 bs_len, bs_size;
	dma_addr_t y_dma, c_dma, aux_dma;
	dma_addr_t ref_y[3];		/* last, golden, alt */
	u32 w, h;			/* frame size */
	u32 c_off;			/* chroma after luma in every frame buffer */
	bool key;
	u32 part_end;			/* first partition end, from the frame start */
	unsigned int nparts;
	u32 part_off[8], part_size[8];
	u32 dq[4][3];			/* per segment Y2, Y1, UV: dc << 16 | ac */
	u8 lf_level[4];			/* per segment */
};

struct vdec_vp9_run {
	const struct v4l2_ctrl_vp9_frame *frame;
	const struct v4l2_ctrl_vp9_compressed_hdr *hdr;
	struct v4l2_vp9_frame_context probs;	/* written to the LAT (pre-delta) */
	struct vb2_v4l2_buffer *src, *dst;
	dma_addr_t bs_dma;
	u32 bs_len, bs_size;
	u32 w, h;				/* frame size */
	dma_addr_t y_dma, c_dma;
	dma_addr_t ref_y[3], ref_c[3];		/* LAST, GOLDEN, ALTREF */
	bool inter, use_prev_mvs;
	u32 groups;				/* compressed-header probability groups */
	u32 quant[16];				/* ac << 16 | dc, 8 segments x (Y, UV) */
	struct vdec_work_buf *ube, *err;
};

struct vdec_ctx {
	struct v4l2_fh fh;
	struct vdec_dev *dev;
	struct v4l2_ctrl_handler ctrl_hdl;
	struct v4l2_format src_fmt, dst_fmt;
	enum vdec_codec codec;
	struct vdec_vp9_state *vp9;
	struct vdec_vp8_state *vp8;
	struct vdec_av1_state *av1;
	u32 mb_w, mb_h;
	/* visible picture size (SPS cropping); the capture format and buffers
	 * are the 16x32-aligned coded size, the display rect comes back from
	 * VIDIOC_G_SELECTION so a client crops the tiled output correctly */
	u32 disp_w, disp_h;
	/* capture in V4L2_PIX_FMT_MT2110T (10 bit) instead of MM21 */
	bool cap_10bit;
	struct vdec_work_buf ube[VDEC_WORK_SLOTS], err[VDEC_WORK_SLOTS];
	unsigned int slot;
	/* usage accounting: the opening process and its decode time */
	struct list_head client;
	pid_t tgid;
	char comm[TASK_COMM_LEN];
	u64 busy_ns, jobs;
};

static inline struct vdec_ctx *file_to_vdec_ctx(struct file *f)
{
	return container_of(file_to_v4l2_fh(f), struct vdec_ctx, fh);
}

extern int vdec_trace;	/* bring-up: log every register access (one job) */

/* the VP9 probability / count table ports: thousands of accesses a picture */
static inline bool vdec_trace_port(u32 off)
{
	return (off >= 0x15400 + 0x198 && off <= 0x15400 + 0x1cc) ||
	       (off >= 0x28400 + 0x198 && off <= 0x28400 + 0x1cc) ||
	       /* VP8: coefficient table, segment-ID SRAM */
	       off == 0x27800 + 0x100 || off == 0x27800 + 0x104 ||
	       off == 0x21000 + 0x93c || off == 0x21000 + 0x940;
}

static inline void vdec_w(struct vdec_dev *d, u32 off, u32 val)
{
	if (vdec_trace > 0 && !vdec_trace_port(off))
		pr_info("vdec W %05x %08x\n", off, val);
	writel(val, d->regs + off);
}

static inline u32 vdec_r(struct vdec_dev *d, u32 off)
{
	u32 v = readl(d->regs + off);

	if (vdec_trace > 0 && !vdec_trace_port(off))
		pr_info("vdec R %05x %08x\n", off, v);
	return v;
}

/* vdec_h264.c */
int vdec_h264_ctrls_init(struct v4l2_ctrl_handler *hdl);
int vdec_h264_start(struct vdec_ctx *ctx);
void vdec_h264_stop(struct vdec_ctx *ctx);
int vdec_h264_run(struct vdec_ctx *ctx);
size_t vdec_h264_mv_size(u32 mb_w, u32 mb_h);

/* vdec_vp9.c */
int vdec_vp9_ctrls_init(struct v4l2_ctrl_handler *hdl);
int vdec_vp9_start(struct vdec_ctx *ctx);
void vdec_vp9_stop(struct vdec_ctx *ctx);
int vdec_vp9_run(struct vdec_ctx *ctx);
void vdec_vp9_done(struct vdec_ctx *ctx, bool ok);

/* vdec_vp8.c */
int vdec_vp8_ctrls_init(struct v4l2_ctrl_handler *hdl);
int vdec_vp8_start(struct vdec_ctx *ctx);
void vdec_vp8_stop(struct vdec_ctx *ctx);
int vdec_vp8_run(struct vdec_ctx *ctx);
void vdec_vp8_done(struct vdec_ctx *ctx, bool ok);

/* vdec_av1.c */
int vdec_av1_ctrls_init(struct v4l2_ctrl_handler *hdl);
int vdec_av1_start(struct vdec_ctx *ctx);
void vdec_av1_stop(struct vdec_ctx *ctx);
int vdec_av1_run(struct vdec_ctx *ctx);
void vdec_av1_done(struct vdec_ctx *ctx, bool ok);

/* vdec_hevc.c, vdec_hevc_hw.c */
int vdec_hevc_ctrls_init(struct v4l2_ctrl_handler *hdl);
int vdec_hevc_run(struct vdec_ctx *ctx);
void vdec_hw_hevc_lat(struct vdec_ctx *ctx, const struct vdec_hevc_run *run);
void vdec_hw_hevc_core(struct vdec_ctx *ctx, const struct vdec_hevc_run *run);
void vdec_hw_hevc_start(struct vdec_dev *d);

/* vdec_av1_hw.c */
void vdec_hw_av1_lat(struct vdec_ctx *ctx, const struct vdec_av1_run *run);
void vdec_hw_av1_core(struct vdec_ctx *ctx, const struct vdec_av1_run *run);
void vdec_hw_av1_start(struct vdec_dev *d);

/* vdec_vp8_hw.c */
int vdec_hw_vp8(struct vdec_ctx *ctx, const struct vdec_vp8_run *run);
void vdec_hw_vp8_start(struct vdec_dev *d);
void vdec_hw_vp8_save_seg(struct vdec_dev *d, u32 *map);

/* vdec_vp9_hw.c */
void vdec_hw_vp9_lat(struct vdec_ctx *ctx, const struct vdec_vp9_run *run);
void vdec_hw_vp9_core(struct vdec_ctx *ctx, const struct vdec_vp9_run *run);
void vdec_hw_vp9_start(struct vdec_dev *d);
int vdec_hw_vp9_save(struct vdec_dev *d, dma_addr_t a, dma_addr_t b);
void vdec_hw_vp9_read_counts(struct vdec_dev *d, u32 *buf);
void vdec_hw_vp9_read_probs(struct vdec_dev *d, struct v4l2_vp9_frame_context *c);

/* vdec_h264_hw.c */
void vdec_hw_init(struct vdec_dev *d);
void vdec_hw_lat_prologue(struct vdec_dev *d, u32 c8, u32 cc, u32 r178);
void vdec_hw_racing_cfg(struct vdec_dev *d);
void vdec_hw_lat_racing_tail(struct vdec_dev *d);
void vdec_hw_ring(struct vdec_dev *d, u32 top, u32 mb_w, u32 mb_h);
size_t vdec_hw_ube_size(u32 mb_w, u32 mb_h);
size_t vdec_hw_err_size(u32 mb_w, u32 mb_h);
void vdec_hw_dump(struct vdec_dev *d, const char *where);
void vdec_hw_h264_lat(struct vdec_ctx *ctx, const struct vdec_h264_run *run);
void vdec_hw_h264_core(struct vdec_ctx *ctx, const struct vdec_h264_run *run);
void vdec_hw_start(struct vdec_dev *d);
/* vdec_drv.c */
void vdec_job_done(struct vdec_dev *d, enum vb2_buffer_state state);

#endif
