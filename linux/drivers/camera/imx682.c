// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX682 64 MP quad-Bayer image sensor (Redmi K30 Ultra main camera).
 *
 * V4L2 subdev with the vendor preview mode: 4624x3472 RAW10 (on-sensor
 * 2x2 binning, full field of view), 3-trio C-PHY, 30 fps.
 * Rails (vendor imgsensor_cfg_table.c IMX682SUNNY + kd_camera_hw1 DT, read
 * back in MIUI while streaming): FAN53870 L5 1.8 V ("AVDD"), L3 2.9 V
 * ("DVDD" in the vendor table, the analog rail), vcamio DOVDD, GPIO133 for
 * the 1.1 V core ("PDN"), reset GPIO145.  Order: RST low (+1 ms), avdd2,
 * dovdd, avdd (+1 ms), dvdd (+2 ms), MCLK 24 MHz (+3 ms), RST high (+5 ms).
 * 16-bit register addresses, 8-bit values, chip ID 0x0682 at 0x0016.
 *
 * Timing (vendor .pre): pixel clock 1029 MHz, line length 9432, frame
 * length 3636.  Exposure 0x0202 (lines, frame length - 48 at most),
 * analogue gain 0x0204 = 1024 - 1024 / gain (1x..64x), frame length
 * 0x0340, all under group hold 0x0104.
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

#include "imx682_regs.h"

#define IMX682_REG_CHIP_ID	0x0016
#define IMX682_CHIP_ID		0x0682
#define IMX682_MCLK		24000000

#define IMX682_REG_FRAME_CNT	0x0005
#define IMX682_REG_STREAM	0x0100
#define IMX682_REG_ORIENT	0x0101
#define IMX682_REG_HOLD		0x0104
#define IMX682_REG_EXPOSURE	0x0202
#define IMX682_REG_AGAIN	0x0204
#define IMX682_REG_FRAME_LEN	0x0340
#define IMX682_REG_TEMP_CTL	0x0138
#define IMX682_REG_TEST_PATTERN	0x0600

#define IMX682_WIDTH		4624
#define IMX682_HEIGHT		3472
#define IMX682_PIXEL_RATE	1029000000
#define IMX682_LINK_FREQ	492000000
#define IMX682_LINE_LEN		9432
#define IMX682_FRAME_LEN	3636
#define IMX682_FRAME_LEN_MAX	0xffff
#define IMX682_EXP_MARGIN	48
#define IMX682_EXP_MIN		9
#define IMX682_AGAIN_MIN	0
#define IMX682_AGAIN_MAX	1008	/* 64x */

static const s64 imx682_link_freqs[] = { IMX682_LINK_FREQ };

static const char * const imx682_test_patterns[] = {
	"Disabled",
	"Solid Colour",
	"Colour Bars",
	"Colour Bars With Fade",
	"Pseudorandom Sequence (PN9)",
};

struct imx682 {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct clk *mclk;
	struct gpio_desc *reset;
	struct regulator *avdd, *avdd2, *dovdd, *dvdd;
	u16 chip_id;
	bool streaming;
};

static inline struct imx682 *to_imx682(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx682, sd);
}

static int imx682_read(struct imx682 *s, u16 reg, u8 *val, int len)
{
	u8 addr[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{ .addr = s->client->addr, .len = 2, .buf = addr },
		{ .addr = s->client->addr, .flags = I2C_M_RD, .len = len, .buf = val },
	};
	int ret = i2c_transfer(s->client->adapter, msgs, 2);

	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static int imx682_write(struct imx682 *s, u16 reg, u16 val, int len)
{
	u8 buf[4] = { reg >> 8, reg & 0xff, val >> 8, val & 0xff };
	int ret;

	if (len == 1)
		buf[2] = val & 0xff;
	ret = i2c_master_send(s->client, buf, 2 + len);
	return ret == 2 + len ? 0 : (ret < 0 ? ret : -EIO);
}

/* consecutive addresses go out as one burst, like the vendor table writer */
static int imx682_write_table(struct imx682 *s, const struct imx682_reg *t,
			      int n)
{
	u8 buf[2 + 64];
	int i = 0, len, ret;

	while (i < n) {
		buf[0] = t[i].addr >> 8;
		buf[1] = t[i].addr & 0xff;
		len = 0;
		do {
			buf[2 + len++] = t[i++].val;
		} while (i < n && len < 64 && t[i].addr == t[i - 1].addr + 1);
		ret = i2c_master_send(s->client, buf, 2 + len);
		if (ret != 2 + len)
			return ret < 0 ? ret : -EIO;
	}
	return 0;
}

static int imx682_power_on(struct imx682 *s)
{
	int ret;

	gpiod_set_value_cansleep(s->reset, 1);		/* in reset */
	usleep_range(1000, 1500);
	ret = regulator_enable(s->avdd2);
	if (ret)
		return ret;
	ret = regulator_enable(s->dovdd);
	if (ret)
		goto off_avdd2;
	ret = regulator_enable(s->avdd);
	if (ret)
		goto off_dovdd;
	usleep_range(1000, 1500);
	ret = regulator_enable(s->dvdd);
	if (ret)
		goto off_avdd;
	usleep_range(2000, 2500);
	ret = clk_prepare_enable(s->mclk);
	if (ret)
		goto off_dvdd;
	usleep_range(3000, 3500);
	gpiod_set_value_cansleep(s->reset, 0);
	usleep_range(5000, 5500);
	return 0;

off_dvdd:
	regulator_disable(s->dvdd);
off_avdd:
	regulator_disable(s->avdd);
off_dovdd:
	regulator_disable(s->dovdd);
off_avdd2:
	regulator_disable(s->avdd2);
	return ret;
}

static void imx682_power_off(struct imx682 *s)
{
	gpiod_set_value_cansleep(s->reset, 1);
	clk_disable_unprepare(s->mclk);
	regulator_disable(s->dvdd);
	regulator_disable(s->avdd);
	regulator_disable(s->dovdd);
	regulator_disable(s->avdd2);
}

/* vendor sensor_init() + preview_setting(), then the current controls */
static int imx682_start(struct imx682 *s)
{
	u8 fc = 0xff;
	int ret, i;

	ret = imx682_write_table(s, imx682_init, ARRAY_SIZE(imx682_init));
	if (!ret)
		ret = imx682_write(s, IMX682_REG_TEMP_CTL, 0x01, 1);
	if (!ret)
		ret = imx682_write(s, IMX682_REG_ORIENT, 0x00, 1);
	if (!ret)
		ret = imx682_write_table(s, imx682_preview,
					 ARRAY_SIZE(imx682_preview));
	if (ret)
		return ret;
	s->streaming = true;		/* let the control writes through */
	ret = __v4l2_ctrl_handler_setup(&s->ctrls);
	if (ret)
		return ret;
	ret = imx682_write(s, IMX682_REG_STREAM, 0x01, 1);
	if (ret)
		return ret;
	/* frame counter leaves 0xff once the first frame is out */
	for (i = 0; i < 100; i++) {
		if (!imx682_read(s, IMX682_REG_FRAME_CNT, &fc, 1) && fc != 0xff)
			break;
		usleep_range(1000, 1200);
	}
	dev_dbg(&s->client->dev, "streaming, frame count %u after %d ms\n",
		fc, i);
	return 0;
}

static int imx682_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx682 *s = container_of(ctrl->handler, struct imx682, ctrls);
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 max = IMX682_HEIGHT + ctrl->val - IMX682_EXP_MARGIN;

		__v4l2_ctrl_modify_range(s->exposure, IMX682_EXP_MIN, max, 1,
					 min_t(s64, s->exposure->default_value, max));
	}
	if (!s->streaming)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
	case V4L2_CID_ANALOGUE_GAIN:
	case V4L2_CID_VBLANK:
		break;
	case V4L2_CID_TEST_PATTERN:
		return imx682_write(s, IMX682_REG_TEST_PATTERN, ctrl->val, 2);
	default:
		return 0;
	}

	ret = imx682_write(s, IMX682_REG_HOLD, 0x01, 1);
	if (ret)
		return ret;
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = imx682_write(s, IMX682_REG_EXPOSURE, ctrl->val, 2);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = imx682_write(s, IMX682_REG_AGAIN, ctrl->val, 2);
		break;
	case V4L2_CID_VBLANK:
		ret = imx682_write(s, IMX682_REG_FRAME_LEN,
				   IMX682_HEIGHT + ctrl->val, 2);
		break;
	}
	imx682_write(s, IMX682_REG_HOLD, 0x00, 1);
	return ret;
}

static const struct v4l2_ctrl_ops imx682_ctrl_ops = {
	.s_ctrl = imx682_set_ctrl,
};

static int imx682_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx682 *s = to_imx682(sd);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	if (enable && !s->streaming) {
		ret = imx682_power_on(s);
		if (!ret) {
			ret = imx682_start(s);
			if (ret) {
				s->streaming = false;
				imx682_power_off(s);
			}
		}
	} else if (!enable && s->streaming) {
		imx682_write(s, IMX682_REG_STREAM, 0x00, 1);
		usleep_range(5000, 5500);
		imx682_power_off(s);
		s->streaming = false;
	}
	v4l2_subdev_unlock_state(state);
	return ret;
}

static void imx682_fill_fmt(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = IMX682_WIDTH;
	fmt->height = IMX682_HEIGHT;
	fmt->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int imx682_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_rect *crop = v4l2_subdev_state_get_crop(state, 0);

	imx682_fill_fmt(v4l2_subdev_state_get_format(state, 0));
	*crop = (struct v4l2_rect){ 0, 0, IMX682_WIDTH, IMX682_HEIGHT };
	return 0;
}

static int imx682_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	return 0;
}

static int imx682_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SRGGB10_1X10)
		return -EINVAL;
	fse->min_width = fse->max_width = IMX682_WIDTH;
	fse->min_height = fse->max_height = IMX682_HEIGHT;
	return 0;
}

/* one mode: any request gets it */
static int imx682_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	imx682_fill_fmt(&fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	return 0;
}

static int imx682_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r = (struct v4l2_rect){ 0, 0, IMX682_WIDTH, IMX682_HEIGHT };
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_subdev_video_ops imx682_video_ops = {
	.s_stream = imx682_s_stream,
};

static const struct v4l2_subdev_pad_ops imx682_pad_ops = {
	.enum_mbus_code = imx682_enum_mbus_code,
	.enum_frame_size = imx682_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = imx682_set_fmt,
	.get_selection = imx682_get_selection,
};

static const struct v4l2_subdev_ops imx682_ops = {
	.video = &imx682_video_ops,
	.pad = &imx682_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx682_internal_ops = {
	.init_state = imx682_init_state,
};

static int imx682_init_controls(struct imx682 *s)
{
	struct v4l2_ctrl_handler *h = &s->ctrls;
	const struct v4l2_ctrl_ops *ops = &imx682_ctrl_ops;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int vblank = IMX682_FRAME_LEN - IMX682_HEIGHT;
	int ret;

	v4l2_ctrl_handler_init(h, 10);
	ctrl = v4l2_ctrl_new_std(h, ops, V4L2_CID_PIXEL_RATE, IMX682_PIXEL_RATE,
				 IMX682_PIXEL_RATE, 1, IMX682_PIXEL_RATE);
	ctrl = v4l2_ctrl_new_int_menu(h, ops, V4L2_CID_LINK_FREQ, 0, 0,
				      imx682_link_freqs);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	ctrl = v4l2_ctrl_new_std(h, ops, V4L2_CID_HBLANK,
				 IMX682_LINE_LEN - IMX682_WIDTH,
				 IMX682_LINE_LEN - IMX682_WIDTH, 1,
				 IMX682_LINE_LEN - IMX682_WIDTH);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	s->vblank = v4l2_ctrl_new_std(h, ops, V4L2_CID_VBLANK, vblank,
				      IMX682_FRAME_LEN_MAX - IMX682_HEIGHT, 1,
				      vblank);
	s->exposure = v4l2_ctrl_new_std(h, ops, V4L2_CID_EXPOSURE,
					IMX682_EXP_MIN,
					IMX682_FRAME_LEN - IMX682_EXP_MARGIN,
					1, 0x3d0);
	v4l2_ctrl_new_std(h, ops, V4L2_CID_ANALOGUE_GAIN, IMX682_AGAIN_MIN,
			  IMX682_AGAIN_MAX, 1, IMX682_AGAIN_MIN);
	v4l2_ctrl_new_std_menu_items(h, ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx682_test_patterns) - 1, 0, 0,
				     imx682_test_patterns);

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

static int imx682_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct imx682 *s;
	u8 id[2];
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->client = client;

	s->mclk = devm_clk_get(dev, NULL);
	if (IS_ERR(s->mclk))
		return dev_err_probe(dev, PTR_ERR(s->mclk), "mclk\n");
	ret = clk_set_rate(s->mclk, IMX682_MCLK);
	if (ret)
		dev_warn(dev, "mclk rate: %d\n", ret);
	s->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(s->reset))
		return dev_err_probe(dev, PTR_ERR(s->reset), "reset gpio\n");
	s->avdd = devm_regulator_get(dev, "avdd");
	s->avdd2 = devm_regulator_get(dev, "avdd2");
	s->dovdd = devm_regulator_get(dev, "dovdd");
	s->dvdd = devm_regulator_get(dev, "dvdd");
	if (IS_ERR(s->avdd) || IS_ERR(s->avdd2) || IS_ERR(s->dovdd) ||
	    IS_ERR(s->dvdd))
		return dev_err_probe(dev, -EPROBE_DEFER, "supplies\n");

	ret = imx682_power_on(s);
	if (ret)
		return dev_err_probe(dev, ret, "power on\n");
	ret = imx682_read(s, IMX682_REG_CHIP_ID, id, 2);
	imx682_power_off(s);
	if (ret)
		return dev_err_probe(dev, ret, "chip id read\n");
	s->chip_id = (id[0] << 8) | id[1];
	if (s->chip_id != IMX682_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "chip id 0x%04x\n",
				     s->chip_id);

	v4l2_i2c_subdev_init(&s->sd, client, &imx682_ops);
	s->sd.internal_ops = &imx682_internal_ops;
	s->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = imx682_init_controls(s);
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

	dev_info(dev, "IMX682, chip id 0x%04x (mclk %lu Hz)\n", s->chip_id,
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

static void imx682_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx682 *s = to_imx682(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s->ctrls);
	if (s->streaming)
		imx682_power_off(s);
}

static const struct of_device_id imx682_of_match[] = {
	{ .compatible = "sony,imx682" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx682_of_match);

static struct i2c_driver imx682_driver = {
	.driver = {
		.name = "imx682",
		.of_match_table = imx682_of_match,
	},
	.probe = imx682_probe,
	.remove = imx682_remove,
};
module_i2c_driver(imx682_driver);

MODULE_DESCRIPTION("Sony IMX682 image sensor");
MODULE_LICENSE("GPL");
