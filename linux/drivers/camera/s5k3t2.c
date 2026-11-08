// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S5K3T2 20 MP image sensor (Redmi K30 Ultra pop-up front camera).
 *
 * V4L2 subdev with the one mode we have tables for: the vendor preview
 * mode, 2592x1940 RAW10 (2x2 binned), 4 lanes at 534 Mbps, 30 fps.
 * Power-up order from the vendor imgsensor_cfg_table.c (S5K3T2SUNNY):
 * RST low, DVDD 1.1 V, AVDD 2.8 V, DOVDD 1.8 V (+1 ms), RST high (+3 ms),
 * MCLK 24 MHz (+5 ms).  16-bit register addresses, chip ID at 0x0000.
 *
 * Timing (vendor .pre): pixel clock 678.26 MHz, line length 10880, frame
 * length 2078 lines.  Exposure 0x0202 in lines (frame length - 8 at most),
 * analogue gain 0x0204 = 32 x gain (1x..16x), frame length 0x0340.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "s5k3t2_regs.h"

#define S5K3T2_REG_CHIP_ID	0x0000
#define S5K3T2_CHIP_ID		0x3142
#define S5K3T2_MCLK		24000000

#define S5K3T2_REG_FRAME_CNT	0x0005
#define S5K3T2_REG_STREAM	0x0100
#define S5K3T2_REG_EXPOSURE	0x0202
#define S5K3T2_REG_AGAIN	0x0204
#define S5K3T2_REG_FRAME_LEN	0x0340
#define S5K3T2_REG_TEST_PATTERN	0x0600

#define S5K3T2_WIDTH		2592
#define S5K3T2_HEIGHT		1940
#define S5K3T2_PIXEL_RATE	678260000
#define S5K3T2_LINK_FREQ	267000000
#define S5K3T2_LINE_LEN		10880
#define S5K3T2_FRAME_LEN	2078
#define S5K3T2_FRAME_LEN_MAX	0xfffa
#define S5K3T2_EXP_MARGIN	8
#define S5K3T2_EXP_MIN		4
#define S5K3T2_AGAIN_MIN	0x20
#define S5K3T2_AGAIN_MAX	0x200

static const s64 s5k3t2_link_freqs[] = { S5K3T2_LINK_FREQ };

static const char * const s5k3t2_test_patterns[] = {
	"Disabled",
	"Solid Colour",
	"Colour Bars",
};

struct s5k3t2 {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct clk *mclk;
	struct gpio_desc *reset;
	struct regulator *dvdd, *avdd, *dovdd;
	u16 chip_id;
	bool streaming;
};

static inline struct s5k3t2 *to_s5k3t2(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5k3t2, sd);
}

static int s5k3t2_read16(struct s5k3t2 *s, u16 reg, u16 *val)
{
	u8 addr[2] = { reg >> 8, reg & 0xff };
	u8 buf[2];
	struct i2c_msg msgs[2] = {
		{ .addr = s->client->addr, .len = 2, .buf = addr },
		{ .addr = s->client->addr, .flags = I2C_M_RD, .len = 2, .buf = buf },
	};
	int ret = i2c_transfer(s->client->adapter, msgs, 2);

	if (ret != 2)
		return ret < 0 ? ret : -EIO;
	*val = (buf[0] << 8) | buf[1];
	return 0;
}

static int s5k3t2_write(struct s5k3t2 *s, u16 reg, u16 val, int len)
{
	u8 buf[4] = { reg >> 8, reg & 0xff, val >> 8, val & 0xff };
	int ret;

	if (len == 1)
		buf[2] = val & 0xff;
	ret = i2c_master_send(s->client, buf, 2 + len);
	return ret == 2 + len ? 0 : (ret < 0 ? ret : -EIO);
}

static int s5k3t2_read8(struct s5k3t2 *s, u16 reg, u8 *val)
{
	u8 addr[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{ .addr = s->client->addr, .len = 2, .buf = addr },
		{ .addr = s->client->addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	int ret = i2c_transfer(s->client->adapter, msgs, 2);

	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static int s5k3t2_write_table(struct s5k3t2 *s, const struct s5k3t2_reg *t,
			      int n)
{
	int i, ret;

	for (i = 0; i < n; i++) {
		ret = s5k3t2_write(s, t[i].addr, t[i].val, 2);
		if (ret)
			return ret;
	}
	return 0;
}

static int s5k3t2_power_on(struct s5k3t2 *s)
{
	int ret;

	gpiod_set_value_cansleep(s->reset, 1);		/* in reset */
	ret = regulator_enable(s->dvdd);
	if (ret)
		return ret;
	ret = regulator_enable(s->avdd);
	if (ret)
		goto off_dvdd;
	ret = regulator_enable(s->dovdd);
	if (ret)
		goto off_avdd;
	usleep_range(1000, 1500);
	gpiod_set_value_cansleep(s->reset, 0);
	usleep_range(3000, 3500);
	ret = clk_prepare_enable(s->mclk);
	if (ret)
		goto off_dovdd;
	usleep_range(5000, 5500);
	return 0;

off_dovdd:
	regulator_disable(s->dovdd);
off_avdd:
	regulator_disable(s->avdd);
off_dvdd:
	regulator_disable(s->dvdd);
	return ret;
}

static void s5k3t2_power_off(struct s5k3t2 *s)
{
	clk_disable_unprepare(s->mclk);
	gpiod_set_value_cansleep(s->reset, 1);
	regulator_disable(s->dovdd);
	regulator_disable(s->avdd);
	regulator_disable(s->dvdd);
}

/* vendor sensor_init() + preview_setting(), then the current controls */
static int s5k3t2_start(struct s5k3t2 *s)
{
	static const struct s5k3t2_reg unlock[] = {
		{ 0x6028, 0x4000 }, { 0x0000, 0x0005 }, { 0x0000, 0x3142 },
		{ 0x6010, 0x0001 },
	};
	static const struct s5k3t2_reg pre_init[] = {
		{ 0x6214, 0xff7d }, { 0x6218, 0x0000 }, { 0x0a02, 0x003f },
	};
	u8 fc = 0xff;
	int ret, i;

	ret = s5k3t2_write_table(s, unlock, ARRAY_SIZE(unlock));
	if (ret)
		return ret;
	usleep_range(3000, 3500);
	ret = s5k3t2_write_table(s, pre_init, ARRAY_SIZE(pre_init));
	if (!ret)
		ret = s5k3t2_write_table(s, s5k3t2_init, ARRAY_SIZE(s5k3t2_init));
	if (!ret)
		ret = s5k3t2_write_table(s, s5k3t2_preview,
					 ARRAY_SIZE(s5k3t2_preview));
	if (ret)
		return ret;
	s->streaming = true;		/* let the control writes through */
	ret = __v4l2_ctrl_handler_setup(&s->ctrls);
	if (ret)
		return ret;
	usleep_range(5000, 5500);
	ret = s5k3t2_write(s, S5K3T2_REG_STREAM, 0x01, 1);
	if (ret)
		return ret;
	/* frame counter leaves 0xff once the first frame is out */
	for (i = 0; i < 100; i++) {
		if (!s5k3t2_read8(s, S5K3T2_REG_FRAME_CNT, &fc) && fc != 0xff)
			break;
		usleep_range(1000, 1200);
	}
	dev_dbg(&s->client->dev, "streaming, frame count %u after %d ms\n",
		fc, i);
	return 0;
}

static int s5k3t2_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5k3t2 *s = container_of(ctrl->handler, struct s5k3t2, ctrls);

	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 max = S5K3T2_HEIGHT + ctrl->val - S5K3T2_EXP_MARGIN;

		__v4l2_ctrl_modify_range(s->exposure, S5K3T2_EXP_MIN, max, 1,
					 min_t(s64, s->exposure->default_value, max));
	}
	if (!s->streaming)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		return s5k3t2_write(s, S5K3T2_REG_EXPOSURE, ctrl->val, 2);
	case V4L2_CID_ANALOGUE_GAIN:
		return s5k3t2_write(s, S5K3T2_REG_AGAIN, ctrl->val, 2);
	case V4L2_CID_VBLANK:
		return s5k3t2_write(s, S5K3T2_REG_FRAME_LEN,
				    S5K3T2_HEIGHT + ctrl->val, 2);
	case V4L2_CID_TEST_PATTERN:
		return s5k3t2_write(s, S5K3T2_REG_TEST_PATTERN, ctrl->val, 2);
	default:
		return 0;
	}
}

static const struct v4l2_ctrl_ops s5k3t2_ctrl_ops = {
	.s_ctrl = s5k3t2_set_ctrl,
};

static int s5k3t2_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct s5k3t2 *s = to_s5k3t2(sd);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	if (enable && !s->streaming) {
		ret = s5k3t2_power_on(s);
		if (!ret) {
			ret = s5k3t2_start(s);
			if (ret) {
				s->streaming = false;
				s5k3t2_power_off(s);
			}
		}
	} else if (!enable && s->streaming) {
		s5k3t2_write(s, S5K3T2_REG_STREAM, 0x00, 1);
		usleep_range(5000, 5500);
		s5k3t2_power_off(s);
		s->streaming = false;
	}
	v4l2_subdev_unlock_state(state);
	return ret;
}

static void s5k3t2_fill_fmt(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = S5K3T2_WIDTH;
	fmt->height = S5K3T2_HEIGHT;
	fmt->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int s5k3t2_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_rect *crop = v4l2_subdev_state_get_crop(state, 0);

	s5k3t2_fill_fmt(v4l2_subdev_state_get_format(state, 0));
	*crop = (struct v4l2_rect){ 0, 0, S5K3T2_WIDTH, S5K3T2_HEIGHT };
	return 0;
}

static int s5k3t2_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	return 0;
}

static int s5k3t2_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;
	fse->min_width = fse->max_width = S5K3T2_WIDTH;
	fse->min_height = fse->max_height = S5K3T2_HEIGHT;
	return 0;
}

/* one mode: any request gets it */
static int s5k3t2_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	s5k3t2_fill_fmt(&fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	return 0;
}

static int s5k3t2_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r = (struct v4l2_rect){ 0, 0, S5K3T2_WIDTH, S5K3T2_HEIGHT };
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_subdev_video_ops s5k3t2_video_ops = {
	.s_stream = s5k3t2_s_stream,
};

static const struct v4l2_subdev_pad_ops s5k3t2_pad_ops = {
	.enum_mbus_code = s5k3t2_enum_mbus_code,
	.enum_frame_size = s5k3t2_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = s5k3t2_set_fmt,
	.get_selection = s5k3t2_get_selection,
};

static const struct v4l2_subdev_ops s5k3t2_ops = {
	.video = &s5k3t2_video_ops,
	.pad = &s5k3t2_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5k3t2_internal_ops = {
	.init_state = s5k3t2_init_state,
};

static int s5k3t2_init_controls(struct s5k3t2 *s)
{
	struct v4l2_ctrl_handler *h = &s->ctrls;
	const struct v4l2_ctrl_ops *ops = &s5k3t2_ctrl_ops;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int vblank = S5K3T2_FRAME_LEN - S5K3T2_HEIGHT;
	int ret;

	v4l2_ctrl_handler_init(h, 10);
	ctrl = v4l2_ctrl_new_std(h, ops, V4L2_CID_PIXEL_RATE, S5K3T2_PIXEL_RATE,
				 S5K3T2_PIXEL_RATE, 1, S5K3T2_PIXEL_RATE);
	ctrl = v4l2_ctrl_new_int_menu(h, ops, V4L2_CID_LINK_FREQ, 0, 0,
				      s5k3t2_link_freqs);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	ctrl = v4l2_ctrl_new_std(h, ops, V4L2_CID_HBLANK,
				 S5K3T2_LINE_LEN - S5K3T2_WIDTH,
				 S5K3T2_LINE_LEN - S5K3T2_WIDTH, 1,
				 S5K3T2_LINE_LEN - S5K3T2_WIDTH);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	s->vblank = v4l2_ctrl_new_std(h, ops, V4L2_CID_VBLANK, vblank,
				      S5K3T2_FRAME_LEN_MAX - S5K3T2_HEIGHT, 1,
				      vblank);
	s->exposure = v4l2_ctrl_new_std(h, ops, V4L2_CID_EXPOSURE,
					S5K3T2_EXP_MIN,
					S5K3T2_FRAME_LEN - S5K3T2_EXP_MARGIN,
					1, 1000);
	v4l2_ctrl_new_std(h, ops, V4L2_CID_ANALOGUE_GAIN, S5K3T2_AGAIN_MIN,
			  S5K3T2_AGAIN_MAX, 1, S5K3T2_AGAIN_MIN);
	v4l2_ctrl_new_std_menu_items(h, ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5k3t2_test_patterns) - 1, 0, 0,
				     s5k3t2_test_patterns);

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

static int s5k3t2_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct s5k3t2 *s;
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->client = client;

	s->mclk = devm_clk_get(dev, NULL);
	if (IS_ERR(s->mclk))
		return dev_err_probe(dev, PTR_ERR(s->mclk), "mclk\n");
	ret = clk_set_rate(s->mclk, S5K3T2_MCLK);
	if (ret)
		dev_warn(dev, "mclk rate: %d\n", ret);
	s->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(s->reset))
		return dev_err_probe(dev, PTR_ERR(s->reset), "reset gpio\n");
	s->dvdd = devm_regulator_get(dev, "dvdd");
	s->avdd = devm_regulator_get(dev, "avdd");
	s->dovdd = devm_regulator_get(dev, "dovdd");
	if (IS_ERR(s->dvdd) || IS_ERR(s->avdd) || IS_ERR(s->dovdd))
		return dev_err_probe(dev, -EPROBE_DEFER, "supplies\n");

	ret = s5k3t2_power_on(s);
	if (ret)
		return dev_err_probe(dev, ret, "power on\n");
	ret = s5k3t2_read16(s, S5K3T2_REG_CHIP_ID, &s->chip_id);
	s5k3t2_power_off(s);
	if (ret)
		return dev_err_probe(dev, ret, "chip id read\n");
	if (s->chip_id != S5K3T2_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "chip id 0x%04x\n",
				     s->chip_id);

	v4l2_i2c_subdev_init(&s->sd, client, &s5k3t2_ops);
	s->sd.internal_ops = &s5k3t2_internal_ops;
	s->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = s5k3t2_init_controls(s);
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

	dev_info(dev, "S5K3T2, chip id 0x%04x (mclk %lu Hz)\n", s->chip_id,
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

static void s5k3t2_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k3t2 *s = to_s5k3t2(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s->ctrls);
	if (s->streaming)
		s5k3t2_power_off(s);
}

static const struct of_device_id s5k3t2_of_match[] = {
	{ .compatible = "samsung,s5k3t2" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5k3t2_of_match);

static struct i2c_driver s5k3t2_driver = {
	.driver = {
		.name = "s5k3t2",
		.of_match_table = s5k3t2_of_match,
	},
	.probe = s5k3t2_probe,
	.remove = s5k3t2_remove,
};
module_i2c_driver(s5k3t2_driver);

MODULE_DESCRIPTION("Samsung S5K3T2 image sensor");
MODULE_LICENSE("GPL");
