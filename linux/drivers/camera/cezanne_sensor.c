// SPDX-License-Identifier: GPL-2.0
/*
 * The Redmi K30 Ultra's smaller rear sensors, one table-driven driver:
 *
 *   Hynix HI1337   13 MP ultrawide   4208x3120 RAW10 GRBG, 4 lanes, 30 fps
 *   Samsung S5K5E9  5 MP macro       2592x1940 RAW10 GRBG, 2 lanes, 30 fps
 *   OmniVision OV02B1B 2 MP mono     1600x1200 RAW10 mono, 1 lane, 30 fps
 *                                    (the "depth" camera: no colour filter)
 *
 * Each runs the vendor imgsensor driver's init + preview tables
 * (cezanne_sensor_regs.h, tools/extract_aux_sensors.py) as its only mode.
 * Power-up order (vendor imgsensor_cfg_table.c): reset asserted, supplies in
 * the listed order, the optional "pwdn" GPIO (OV02B1B: "enable depth vio")
 * released, MCLK, reset released.
 *
 * Register access differs per chip: HI1337 16-bit address / 16-bit value,
 * S5K5E9 16-bit address / 8-bit value (SMIA), OV02B1B 8-bit address / 8-bit
 * value in pages selected through 0xfd.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

struct sensor_reg { u16 addr; u16 val; };
#include "cezanne_sensor_regs.h"

#define SENSOR_MCLK		24000000
#define SENSOR_MAX_SUPPLIES	3

enum sensor_regmap { A16_V16, A16_V8, A8_V8_PAGED };

struct sensor;

struct sensor_desc {
	const char *name;
	const char *model;		/* subdev name: libcamera's sensor model */
	enum sensor_regmap regmap;
	u16 id_reg, id;			/* OV02B1B: page 0 */
	const struct sensor_reg *init, *mode;
	unsigned int n_init, n_mode;
	const char * const *supplies;	/* in power-up order */
	unsigned int n_supplies;
	unsigned int reset_delay_us;	/* MCLK on -> reset released */
	unsigned int pwdn_after;	/* "pwdn" released after this many supplies */
	bool ctrls_after_stream;	/* values written before stream-on are lost */

	u32 width, height, code;
	u64 pixel_rate, link_freq;
	u32 line_len;			/* in pixel_rate units */
	u32 frame_len, frame_len_max;
	u32 exp_min, exp_margin;
	u32 gain_min, gain_max, gain_default;	/* register codes */

	int (*set_exposure)(struct sensor *s, u32 lines);
	int (*set_gain)(struct sensor *s, u32 code);
	int (*set_frame_len)(struct sensor *s, u32 lines);
	int (*set_stream)(struct sensor *s, bool on);
	int (*set_test_pattern)(struct sensor *s, u32 pattern);
};

struct sensor {
	const struct sensor_desc *desc;
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct clk *mclk;
	struct gpio_desc *reset, *pwdn;
	struct regulator_bulk_data supplies[SENSOR_MAX_SUPPLIES];
	u8 page;			/* OV02B1B: current page, 0xff unknown */
	bool streaming;
};

static inline struct sensor *to_sensor(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sensor, sd);
}

/* -------------------------------------------------------------------------
 * Register access
 */

static int sensor_write(struct sensor *s, u16 reg, u16 val)
{
	u8 buf[4];
	int len, ret;

	switch (s->desc->regmap) {
	case A16_V16:
		buf[0] = reg >> 8; buf[1] = reg; buf[2] = val >> 8; buf[3] = val;
		len = 4;
		break;
	case A16_V8:
		buf[0] = reg >> 8; buf[1] = reg; buf[2] = val;
		len = 3;
		break;
	default:
		buf[0] = reg; buf[1] = val;
		len = 2;
		break;
	}
	ret = i2c_master_send(s->client, buf, len);
	return ret == len ? 0 : (ret < 0 ? ret : -EIO);
}

static int sensor_read8(struct sensor *s, u16 reg, u8 *val)
{
	u8 addr[2] = { reg >> 8, reg & 0xff };
	bool a8 = s->desc->regmap == A8_V8_PAGED;
	struct i2c_msg msgs[2] = {
		{ .addr = s->client->addr, .len = a8 ? 1 : 2, .buf = a8 ? addr + 1 : addr },
		{ .addr = s->client->addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	int ret = i2c_transfer(s->client->adapter, msgs, 2);

	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

/* OV02B1B: register in a page (0xfd selects it) */
static int ov_write(struct sensor *s, u8 page, u8 reg, u8 val)
{
	int ret;

	if (s->page != page) {
		ret = sensor_write(s, 0xfd, page);
		if (ret)
			return ret;
		s->page = page;
	}
	return sensor_write(s, reg, val);
}

/* each write on its own: HI1337's init loads firmware through a data port */
static int sensor_write_table(struct sensor *s, const struct sensor_reg *t,
			      unsigned int n)
{
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = sensor_write(s, t[i].addr, t[i].val);
		if (ret)
			return ret;
		if (s->desc->regmap == A8_V8_PAGED && t[i].addr == 0xfd)
			s->page = t[i].val;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * Per-chip controls
 */

/* HI1337 8-bit register (the 16-bit map has a few byte-wide ones) */
static int hi1337_write8(struct sensor *s, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	int ret = i2c_master_send(s->client, buf, 3);

	return ret == 3 ? 0 : (ret < 0 ? ret : -EIO);
}

/* HI1337: 24-bit exposure 0x020d:0x020a, frame length 0x0211:0x020e */
static int hi1337_set_exposure(struct sensor *s, u32 lines)
{
	int ret = sensor_write(s, 0x020a, lines & 0xffff);

	return ret ?: hi1337_write8(s, 0x020d, (lines >> 16) & 0xff);
}

/* 8-bit at 0x0213: 16 x gain - 16 (1x..16x) */
static int hi1337_set_gain(struct sensor *s, u32 code)
{
	return hi1337_write8(s, 0x0213, code);
}

static int hi1337_set_frame_len(struct sensor *s, u32 lines)
{
	int ret = sensor_write(s, 0x020e, lines & 0xffff);

	return ret ?: hi1337_write8(s, 0x0211, (lines >> 16) & 0xff);
}

static int hi1337_set_stream(struct sensor *s, bool on)
{
	return sensor_write(s, 0x0b00, on ? 0x0100 : 0x0000);
}

/* SMIA (S5K5E9): exposure 0x0202, gain 0x0204 = 32 x gain, frame 0x0340 */
static int smia_write16(struct sensor *s, u16 reg, u16 val)
{
	int ret = sensor_write(s, reg, val >> 8);

	return ret ?: sensor_write(s, reg + 1, val & 0xff);
}

static int s5k5e9_set_exposure(struct sensor *s, u32 lines)
{
	return smia_write16(s, 0x0202, lines);
}

static int s5k5e9_set_gain(struct sensor *s, u32 code)
{
	return smia_write16(s, 0x0204, code);
}

static int s5k5e9_set_frame_len(struct sensor *s, u32 lines)
{
	return smia_write16(s, 0x0340, lines);
}

static int s5k5e9_set_stream(struct sensor *s, bool on)
{
	return sensor_write(s, 0x0100, on);
}

static int s5k5e9_set_test_pattern(struct sensor *s, u32 pattern)
{
	return sensor_write(s, 0x0601, pattern ? 0x02 : 0x00);
}

/*
 * OV02B1B, page 1: exposure 0x0e:0x0f, gain 0x22 (16 = 1x), frame length
 * 0x27:0x28 plus the vblank 0x14:0x15 (= frame length - 1220); 0xfe = 2
 * applies the page-1 writes at the next frame.
 */
#define OV02B1B_BASE_FRAME_LEN	1220

static int ov02b1b_apply(struct sensor *s)
{
	return ov_write(s, 0x01, 0xfe, 0x02);
}

static int ov02b1b_set_exposure(struct sensor *s, u32 lines)
{
	int ret = ov_write(s, 0x01, 0x0e, lines >> 8);

	ret = ret ?: ov_write(s, 0x01, 0x0f, lines & 0xff);
	return ret ?: ov02b1b_apply(s);
}

static int ov02b1b_set_gain(struct sensor *s, u32 code)
{
	int ret = ov_write(s, 0x01, 0x22, code);

	return ret ?: ov02b1b_apply(s);
}

static int ov02b1b_set_frame_len(struct sensor *s, u32 lines)
{
	u32 vb = lines - OV02B1B_BASE_FRAME_LEN;
	int ret = ov_write(s, 0x01, 0x27, lines >> 8);

	ret = ret ?: ov_write(s, 0x01, 0x28, lines & 0xff);
	ret = ret ?: ov_write(s, 0x01, 0x14, vb >> 8);
	ret = ret ?: ov_write(s, 0x01, 0x15, vb & 0xff);
	return ret ?: ov02b1b_apply(s);
}

static int ov02b1b_set_stream(struct sensor *s, bool on)
{
	return ov_write(s, 0x03, 0xc2, on);
}

/* -------------------------------------------------------------------------
 * Sensor descriptions
 */

static const char * const hi1337_supplies[] = { "avdd", "dvdd", "dovdd" };
static const char * const s5k5e9_supplies[] = { "avdd", "dvdd", "dovdd" };
static const char * const ov02b1b_supplies[] = { "dovdd", "avdd" };

static const struct sensor_desc hi1337_desc = {
	.name = "HI1337", .model = "hi1337",
	.regmap = A16_V16,
	.id_reg = 0x0716, .id = 0x1337,
	.init = hi1337_init, .n_init = ARRAY_SIZE(hi1337_init),
	.mode = hi1337_mode, .n_mode = ARRAY_SIZE(hi1337_mode),
	.supplies = hi1337_supplies, .n_supplies = ARRAY_SIZE(hi1337_supplies),
	.reset_delay_us = 3000,
	.width = 4208, .height = 3120, .code = MEDIA_BUS_FMT_SGRBG10_1X10,
	/* vendor .pre: pclk 576 MHz, 1440 Mbps x 4 lanes */
	.pixel_rate = 576000000, .link_freq = 720000000,
	.line_len = 5760, .frame_len = 3333, .frame_len_max = 0xffffff,
	.exp_min = 4, .exp_margin = 4,
	.gain_min = 0, .gain_max = 240, .gain_default = 0,
	.set_exposure = hi1337_set_exposure,
	.set_gain = hi1337_set_gain,
	.set_frame_len = hi1337_set_frame_len,
	.set_stream = hi1337_set_stream,
};

static const struct sensor_desc s5k5e9_desc = {
	.name = "S5K5E9", .model = "s5k5e9",
	.regmap = A16_V8,
	.id_reg = 0x0000, .id = 0x559b,
	.init = s5k5e9_init, .n_init = ARRAY_SIZE(s5k5e9_init),
	.mode = s5k5e9_mode, .n_mode = ARRAY_SIZE(s5k5e9_mode),
	.supplies = s5k5e9_supplies, .n_supplies = ARRAY_SIZE(s5k5e9_supplies),
	.reset_delay_us = 2000,
	.width = 2592, .height = 1940, .code = MEDIA_BUS_FMT_SGRBG10_1X10,
	/* vendor .pre: pclk 190 MHz, MIPI 175.2 Mpx/s = 876 Mbps x 2 lanes */
	.pixel_rate = 190000000, .link_freq = 438000000,
	.line_len = 3112, .frame_len = 2030, .frame_len_max = 0xffff,
	.exp_min = 2, .exp_margin = 10,
	.gain_min = 32, .gain_max = 512, .gain_default = 32,
	.set_exposure = s5k5e9_set_exposure,
	.set_gain = s5k5e9_set_gain,
	.set_frame_len = s5k5e9_set_frame_len,
	.set_stream = s5k5e9_set_stream,
	.set_test_pattern = s5k5e9_set_test_pattern,
};

static const struct sensor_desc ov02b1b_desc = {
	.name = "OV02B1B", .model = "ov02b1b",
	.regmap = A8_V8_PAGED,
	.id_reg = 0x02, .id = 0x002b,
	.init = ov02b1b_init, .n_init = ARRAY_SIZE(ov02b1b_init),
	.supplies = ov02b1b_supplies, .n_supplies = ARRAY_SIZE(ov02b1b_supplies),
	.pwdn_after = 1,		/* DOVDD, pwdn, AVDD */
	/* streaming kept the init table's gain 0xff, not the control's */
	.ctrls_after_stream = true,
	.reset_delay_us = 1000,
	.width = 1600, .height = 1200, .code = MEDIA_BUS_FMT_Y10_1X10,
	/*
	 * vendor .pre: pclk 16.5 MHz, line length 448, frame length 1221 (30 fps);
	 * 4 x that in pixels so that a line is wider than the image.
	 * MIPI: 1600 x 1221 x 30 x 10 bit on one lane, ~600 Mbps.
	 */
	.pixel_rate = 66000000, .link_freq = 300000000,
	.line_len = 1792, .frame_len = 1221, .frame_len_max = 0x84c3,
	.exp_min = 4, .exp_margin = 7,
	.gain_min = 0x10, .gain_max = 0xf8, .gain_default = 0x10,
	.set_exposure = ov02b1b_set_exposure,
	.set_gain = ov02b1b_set_gain,
	.set_frame_len = ov02b1b_set_frame_len,
	.set_stream = ov02b1b_set_stream,
};

/* -------------------------------------------------------------------------
 * Power, streaming
 */

static int sensor_power_on(struct sensor *s)
{
	const struct sensor_desc *d = s->desc;
	unsigned int i;
	int ret;

	gpiod_set_value_cansleep(s->reset, 1);
	usleep_range(1000, 1500);
	for (i = 0; i < d->n_supplies; i++) {
		if (i == d->pwdn_after)
			gpiod_set_value_cansleep(s->pwdn, 0);
		ret = regulator_enable(s->supplies[i].consumer);
		if (ret)
			goto off;
		usleep_range(1000, 1500);
	}
	gpiod_set_value_cansleep(s->pwdn, 0);
	ret = clk_prepare_enable(s->mclk);
	if (ret)
		goto off;
	usleep_range(d->reset_delay_us, d->reset_delay_us + 500);
	gpiod_set_value_cansleep(s->reset, 0);
	usleep_range(10000, 11000);
	s->page = 0xff;
	return 0;

off:
	gpiod_set_value_cansleep(s->pwdn, 1);
	while (i--)
		regulator_disable(s->supplies[i].consumer);
	return ret;
}

static void sensor_power_off(struct sensor *s)
{
	unsigned int i = s->desc->n_supplies;

	gpiod_set_value_cansleep(s->reset, 1);
	clk_disable_unprepare(s->mclk);
	gpiod_set_value_cansleep(s->pwdn, 1);
	while (i--)
		regulator_disable(s->supplies[i].consumer);
}

static int sensor_read_id(struct sensor *s, u16 *id)
{
	u8 hi, lo;
	int ret;

	if (s->desc->regmap == A8_V8_PAGED) {
		ret = sensor_write(s, 0xfd, 0x00);
		if (ret)
			return ret;
		s->page = 0;
	}
	ret = sensor_read8(s, s->desc->id_reg, &hi);
	ret = ret ?: sensor_read8(s, s->desc->id_reg + 1, &lo);
	*id = hi << 8 | lo;
	return ret;
}

static int sensor_start(struct sensor *s)
{
	const struct sensor_desc *d = s->desc;
	int ret;

	ret = sensor_write_table(s, d->init, d->n_init);
	if (!ret && d->mode)
		ret = sensor_write_table(s, d->mode, d->n_mode);
	if (ret)
		return ret;
	s->streaming = true;		/* let the control writes through */
	if (d->ctrls_after_stream) {
		ret = d->set_stream(s, true);
		return ret ?: __v4l2_ctrl_handler_setup(&s->ctrls);
	}
	ret = __v4l2_ctrl_handler_setup(&s->ctrls);
	if (ret)
		return ret;
	return d->set_stream(s, true);
}

static int sensor_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sensor *s = container_of(ctrl->handler, struct sensor, ctrls);
	const struct sensor_desc *d = s->desc;

	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 max = d->height + ctrl->val - d->exp_margin;

		__v4l2_ctrl_modify_range(s->exposure, d->exp_min, max, 1,
					 min_t(s64, s->exposure->default_value, max));
	}
	if (!s->streaming)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		return d->set_exposure(s, ctrl->val);
	case V4L2_CID_ANALOGUE_GAIN:
		return d->set_gain(s, ctrl->val);
	case V4L2_CID_VBLANK:
		return d->set_frame_len(s, d->height + ctrl->val);
	case V4L2_CID_TEST_PATTERN:
		return d->set_test_pattern ? d->set_test_pattern(s, ctrl->val) : 0;
	default:
		return 0;
	}
}

static const struct v4l2_ctrl_ops sensor_ctrl_ops = {
	.s_ctrl = sensor_set_ctrl,
};

static int sensor_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct sensor *s = to_sensor(sd);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	if (enable && !s->streaming) {
		ret = sensor_power_on(s);
		if (!ret) {
			ret = sensor_start(s);
			if (ret) {
				dev_err(&s->client->dev, "start: %d\n", ret);
				s->streaming = false;
				sensor_power_off(s);
			}
		}
	} else if (!enable && s->streaming) {
		s->desc->set_stream(s, false);
		usleep_range(5000, 5500);
		sensor_power_off(s);
		s->streaming = false;
	}
	v4l2_subdev_unlock_state(state);
	return ret;
}

/* -------------------------------------------------------------------------
 * Pad ops: one mode
 */

static void sensor_fill_fmt(struct sensor *s, struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = s->desc->width;
	fmt->height = s->desc->height;
	fmt->code = s->desc->code;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int sensor_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct sensor *s = to_sensor(sd);

	sensor_fill_fmt(s, v4l2_subdev_state_get_format(state, 0));
	*v4l2_subdev_state_get_crop(state, 0) =
		(struct v4l2_rect){ 0, 0, s->desc->width, s->desc->height };
	return 0;
}

static int sensor_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = to_sensor(sd)->desc->code;
	return 0;
}

static int sensor_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	const struct sensor_desc *d = to_sensor(sd)->desc;

	if (fse->index || fse->code != d->code)
		return -EINVAL;
	fse->min_width = fse->max_width = d->width;
	fse->min_height = fse->max_height = d->height;
	return 0;
}

static int sensor_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	sensor_fill_fmt(to_sensor(sd), &fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	return 0;
}

static int sensor_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	const struct sensor_desc *d = to_sensor(sd)->desc;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r = (struct v4l2_rect){ 0, 0, d->width, d->height };
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_subdev_video_ops sensor_video_ops = {
	.s_stream = sensor_s_stream,
};

static const struct v4l2_subdev_pad_ops sensor_pad_ops = {
	.enum_mbus_code = sensor_enum_mbus_code,
	.enum_frame_size = sensor_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = sensor_set_fmt,
	.get_selection = sensor_get_selection,
};

static const struct v4l2_subdev_ops sensor_ops = {
	.video = &sensor_video_ops,
	.pad = &sensor_pad_ops,
};

static const struct v4l2_subdev_internal_ops sensor_internal_ops = {
	.init_state = sensor_init_state,
};

static const char * const sensor_test_patterns[] = {
	"Disabled",
	"Colour Bars",
};

static int sensor_init_controls(struct sensor *s)
{
	const struct sensor_desc *d = s->desc;
	struct v4l2_ctrl_handler *h = &s->ctrls;
	const struct v4l2_ctrl_ops *ops = &sensor_ctrl_ops;
	struct v4l2_fwnode_device_properties props;
	u32 hblank = d->line_len - d->width;
	u32 vblank = d->frame_len - d->height;
	struct v4l2_ctrl *ctrl;
	int ret;

	v4l2_ctrl_handler_init(h, 10);
	v4l2_ctrl_new_std(h, ops, V4L2_CID_PIXEL_RATE, d->pixel_rate,
			  d->pixel_rate, 1, d->pixel_rate);
	ctrl = v4l2_ctrl_new_int_menu(h, ops, V4L2_CID_LINK_FREQ, 0, 0,
				      (const s64 *)&d->link_freq);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	ctrl = v4l2_ctrl_new_std(h, ops, V4L2_CID_HBLANK, hblank, hblank, 1,
				 hblank);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(h, ops, V4L2_CID_VBLANK, vblank,
			  d->frame_len_max - d->height, 1, vblank);
	s->exposure = v4l2_ctrl_new_std(h, ops, V4L2_CID_EXPOSURE, d->exp_min,
					d->frame_len - d->exp_margin, 1,
					(d->frame_len - d->exp_margin) / 2);
	v4l2_ctrl_new_std(h, ops, V4L2_CID_ANALOGUE_GAIN, d->gain_min,
			  d->gain_max, 1, d->gain_default);
	if (d->set_test_pattern)
		v4l2_ctrl_new_std_menu_items(h, ops, V4L2_CID_TEST_PATTERN,
					     ARRAY_SIZE(sensor_test_patterns) - 1,
					     0, 0, sensor_test_patterns);

	ret = v4l2_fwnode_device_parse(&s->client->dev, &props);
	if (!ret)
		ret = v4l2_ctrl_new_fwnode_properties(h, ops, &props);
	if (!ret)
		ret = h->error;
	if (ret) {
		v4l2_ctrl_handler_free(h);
		return ret;
	}
	s->sd.ctrl_handler = h;
	return 0;
}

static int sensor_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	const struct sensor_desc *d = of_device_get_match_data(dev);
	struct sensor *s;
	unsigned int i;
	u16 id;
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->client = client;
	s->desc = d;

	s->mclk = devm_clk_get(dev, NULL);
	if (IS_ERR(s->mclk))
		return dev_err_probe(dev, PTR_ERR(s->mclk), "mclk\n");
	ret = clk_set_rate(s->mclk, SENSOR_MCLK);
	if (ret)
		dev_warn(dev, "mclk rate: %d\n", ret);
	s->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(s->reset))
		return dev_err_probe(dev, PTR_ERR(s->reset), "reset gpio\n");
	s->pwdn = devm_gpiod_get_optional(dev, "pwdn", GPIOD_OUT_HIGH);
	if (IS_ERR(s->pwdn))
		return dev_err_probe(dev, PTR_ERR(s->pwdn), "pwdn gpio\n");
	for (i = 0; i < d->n_supplies; i++)
		s->supplies[i].supply = d->supplies[i];
	ret = devm_regulator_bulk_get(dev, d->n_supplies, s->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "supplies\n");

	ret = sensor_power_on(s);
	if (ret)
		return dev_err_probe(dev, ret, "power on\n");
	ret = sensor_read_id(s, &id);
	sensor_power_off(s);
	if (ret)
		return dev_err_probe(dev, ret, "%s: chip id read\n", d->name);
	if (id != d->id)
		return dev_err_probe(dev, -ENODEV, "%s: chip id 0x%04x\n",
				     d->name, id);

	v4l2_i2c_subdev_init(&s->sd, client, &sensor_ops);
	v4l2_i2c_subdev_set_name(&s->sd, client, d->model, NULL);
	s->sd.internal_ops = &sensor_internal_ops;
	s->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = sensor_init_controls(s);
	if (ret)
		return dev_err_probe(dev, ret, "controls\n");
	s->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&s->sd.entity, 1, &s->pad);
	if (ret)
		goto free_ctrls;
	s->sd.state_lock = s->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&s->sd);
	if (ret)
		goto cleanup_entity;
	ret = v4l2_async_register_subdev_sensor(&s->sd);
	if (ret)
		goto cleanup_subdev;

	dev_info(dev, "%s, chip id 0x%04x (mclk %lu Hz)\n", d->name, id,
		 clk_get_rate(s->mclk));
	return 0;

cleanup_subdev:
	v4l2_subdev_cleanup(&s->sd);
cleanup_entity:
	media_entity_cleanup(&s->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(&s->ctrls);
	return ret;
}

static void sensor_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sensor *s = to_sensor(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s->ctrls);
	if (s->streaming)
		sensor_power_off(s);
}

static const struct of_device_id sensor_of_match[] = {
	{ .compatible = "hynix,hi1337", .data = &hi1337_desc },
	{ .compatible = "samsung,s5k5e9", .data = &s5k5e9_desc },
	{ .compatible = "ovti,ov02b1b", .data = &ov02b1b_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, sensor_of_match);

static struct i2c_driver sensor_driver = {
	.driver = {
		.name = "cezanne-sensor",
		.of_match_table = sensor_of_match,
	},
	.probe = sensor_probe,
	.remove = sensor_remove,
};
module_i2c_driver(sensor_driver);

MODULE_DESCRIPTION("Redmi K30 Ultra ultrawide, macro and mono sensors");
MODULE_LICENSE("GPL");
