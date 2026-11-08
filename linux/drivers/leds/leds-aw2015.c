// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW2015 3-channel RGB LED driver, as one multicolor LED.
 *
 * Supports steady colour, hardware blinking (timer trigger) and hardware
 * breathing (pattern trigger, hw_pattern "0 rise B hold B fall 0 off").
 * With "awinic,charge-indicator" the chip's own charging indication (a red
 * light driven from the charger state) is left on while the LED is off.
 *
 * Register layout from the vendor (Xiaomi) driver.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>

#define AW2015_RSTR		0x00
#define AW2015_RSTR_RESET	0x55
#define AW2015_CHIP_ID		0x31
#define AW2015_GCR		0x01
#define AW2015_GCR_CHIPEN	BIT(0)
#define AW2015_GCR_CHGDIS	BIT(1)	/* disable the charge indicator */
#define AW2015_IMAX		0x03
#define AW2015_LCFG(x)		(0x04 + (x))
#define AW2015_LCFG_PATTERN	BIT(0)
#define AW2015_LEDEN		0x07
#define AW2015_LEDCTR		0x08
#define AW2015_LEDCTR_SYNC	BIT(3)
#define AW2015_PAT_RUN		0x09
#define AW2015_ILED(x)		(0x10 + (x))	/* ILEDx_1 */
#define AW2015_PWM(x)		(0x1c + (x))
#define AW2015_PAT_T1(x)	(0x30 + (x) * 5)	/* rise << 4 | hold */
#define AW2015_PAT_T2(x)	(0x31 + (x) * 5)	/* fall << 4 | off */
#define AW2015_PAT_T3(x)	(0x32 + (x) * 5)	/* slot << 4 | delay */
#define AW2015_PAT_T4(x)	(0x33 + (x) * 5)	/* 0: repeat forever */
#define AW2015_REG_MAX		0x3e

#define AW2015_CHANNELS		3

/* IMAX codes 0..3 */
static const u32 aw2015_imax_ua[] = { 3187, 6375, 12750, 25500 };

/* pattern phase times (ms), 4-bit codes */
static const u32 aw2015_times_ms[] = {
	40, 130, 260, 380, 510, 770, 1040, 1600,
	2100, 2600, 3100, 4200, 5200, 6200, 7300, 8300,
};

struct aw2015 {
	struct i2c_client *client;
	struct regmap *regmap;
	struct mutex lock;
	struct led_classdev_mc mc;
	struct mc_subled subleds[AW2015_CHANNELS];
	bool charge_indicator;
	u8 imax;	/* IMAX code */
	u8 iled_max;	/* ILED value at full brightness */
	/* hardware pattern (blink / breathing) running, 4-bit time codes */
	bool pattern;
	u8 rise, hold, fall, off;
};

static const struct regmap_config aw2015_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AW2015_REG_MAX,
};

static u8 aw2015_time_code(u32 ms)
{
	u8 i;

	for (i = 0; i < ARRAY_SIZE(aw2015_times_ms) - 1; i++)
		if (ms < (aw2015_times_ms[i] + aw2015_times_ms[i + 1]) / 2)
			break;
	return i;
}

static int aw2015_idle(struct aw2015 *chip)
{
	int ret;

	ret = regmap_write(chip->regmap, AW2015_LEDEN, 0);
	if (!ret)
		ret = regmap_write(chip->regmap, AW2015_LEDCTR, 0);
	if (!ret)
		ret = regmap_write(chip->regmap, AW2015_GCR,
				   chip->charge_indicator ? 0 : AW2015_GCR_CHGDIS);
	return ret;
}

/* Programs the chip from the LED state; called with chip->lock held. */
static int aw2015_update(struct aw2015 *chip, unsigned int brightness)
{
	struct regmap *map = chip->regmap;
	u8 iled[AW2015_CHANNELS], en = 0;
	int i, ret;

	led_mc_calc_color_components(&chip->mc, brightness);
	for (i = 0; i < AW2015_CHANNELS; i++) {
		struct mc_subled *s = &chip->subleds[i];

		iled[s->channel] = DIV_ROUND_UP(s->brightness * chip->iled_max,
						chip->mc.led_cdev.max_brightness);
		if (iled[s->channel])
			en |= BIT(s->channel);
	}
	if (!en)
		return aw2015_idle(chip);

	ret = regmap_write(map, AW2015_GCR, AW2015_GCR_CHIPEN | AW2015_GCR_CHGDIS);
	if (ret)
		return ret;
	usleep_range(100, 200);

	/* stop a running pattern before reprogramming it */
	ret = regmap_write(map, AW2015_LEDEN, 0);
	ret = ret ?: regmap_write(map, AW2015_IMAX, chip->imax);
	for (i = 0; i < AW2015_CHANNELS && !ret; i++) {
		ret = regmap_write(map, AW2015_LCFG(i),
				   chip->pattern ? AW2015_LCFG_PATTERN : 0);
		ret = ret ?: regmap_write(map, AW2015_ILED(i), iled[i]);
		ret = ret ?: regmap_write(map, AW2015_PWM(i), 0xff);
		if (ret || !chip->pattern)
			continue;
		ret = regmap_write(map, AW2015_PAT_T1(i), chip->rise << 4 | chip->hold);
		ret = ret ?: regmap_write(map, AW2015_PAT_T2(i), chip->fall << 4 | chip->off);
		ret = ret ?: regmap_write(map, AW2015_PAT_T3(i), 0);
		ret = ret ?: regmap_write(map, AW2015_PAT_T4(i), 0);
	}
	ret = ret ?: regmap_write(map, AW2015_LEDCTR,
				  chip->pattern ? AW2015_LEDCTR_SYNC : 0);
	ret = ret ?: regmap_write(map, AW2015_LEDEN, en);
	if (ret || !chip->pattern)
		return ret;
	/* in sync (true-colour) mode, channel 1's run bit starts all of them */
	return regmap_write(map, AW2015_PAT_RUN, BIT(0));
}

static struct aw2015 *to_aw2015(struct led_classdev *cdev)
{
	return container_of(lcdev_to_mccdev(cdev), struct aw2015, mc);
}

static int aw2015_brightness_set(struct led_classdev *cdev,
				 enum led_brightness brightness)
{
	struct aw2015 *chip = to_aw2015(cdev);
	int ret;

	mutex_lock(&chip->lock);
	chip->pattern = false;
	ret = aw2015_update(chip, brightness);
	mutex_unlock(&chip->lock);
	return ret;
}

static int aw2015_set_pattern(struct aw2015 *chip, unsigned int brightness,
			      u32 rise, u32 hold, u32 fall, u32 off)
{
	int ret;

	mutex_lock(&chip->lock);
	chip->pattern = true;
	chip->rise = aw2015_time_code(rise);
	chip->hold = aw2015_time_code(hold);
	chip->fall = aw2015_time_code(fall);
	chip->off = aw2015_time_code(off);
	ret = aw2015_update(chip, brightness);
	mutex_unlock(&chip->lock);
	return ret;
}

static int aw2015_blink_set(struct led_classdev *cdev,
			    unsigned long *delay_on, unsigned long *delay_off)
{
	struct aw2015 *chip = to_aw2015(cdev);
	unsigned int brightness;

	if (!*delay_on && !*delay_off) {
		*delay_on = 500;
		*delay_off = 500;
	}
	if (!*delay_on)
		return aw2015_brightness_set(cdev, LED_OFF);

	/* the timer trigger keeps the brightness it found; default to full */
	brightness = cdev->blink_brightness ?: cdev->brightness ?: cdev->max_brightness;

	*delay_on = aw2015_times_ms[aw2015_time_code(*delay_on)];
	*delay_off = aw2015_times_ms[aw2015_time_code(*delay_off)];
	/* shortest ramps: a square-ish blink */
	return aw2015_set_pattern(chip, brightness, 0, *delay_on, 0, *delay_off);
}

/*
 * Hardware pattern: the breathing cycle as the pattern trigger describes it,
 * "0 rise B hold B fall 0 off" (ramp up, stay, ramp down, stay off),
 * repeated forever (repeat -1 or 0). Four equal steps: steady (or off).
 */
static int aw2015_pattern_set(struct led_classdev *cdev,
			      struct led_pattern *p, u32 len, int repeat)
{
	struct aw2015 *chip = to_aw2015(cdev);

	/* repeat stays 0 (forever) unless "repeat" is written first */
	if (len != 4 || repeat > 0)
		return -EINVAL;
	/*
	 * all steps equal: steady (all zero: off), so that users of the trigger
	 * switch between states with one write
	 */
	if (p[0].brightness == p[1].brightness && p[1].brightness == p[2].brightness &&
	    p[2].brightness == p[3].brightness)
		return aw2015_brightness_set(cdev, min_t(u32, p[0].brightness,
							 cdev->max_brightness));
	if (p[0].brightness || p[3].brightness ||
	    p[1].brightness != p[2].brightness)
		return -EINVAL;

	return aw2015_set_pattern(chip, min_t(u32, p[1].brightness, cdev->max_brightness),
				  p[0].delta_t, p[1].delta_t,
				  p[2].delta_t, p[3].delta_t);
}

static int aw2015_pattern_clear(struct led_classdev *cdev)
{
	return aw2015_brightness_set(cdev, LED_OFF);
}

static int aw2015_parse_dt(struct aw2015 *chip)
{
	struct device *dev = &chip->client->dev;
	struct device_node *np;
	u32 max_ua, channel, color;
	int i = 0, ret;

	chip->charge_indicator = device_property_read_bool(dev, "awinic,charge-indicator");

	struct device_node *multi __free(device_node) =
		of_get_child_by_name(dev->of_node, "multi-led");
	if (!multi)
		return dev_err_probe(dev, -EINVAL, "no multi-led node\n");

	if (of_property_read_u32(multi, "led-max-microamp", &max_ua))
		max_ua = aw2015_imax_ua[0];
	for (chip->imax = 0; chip->imax < ARRAY_SIZE(aw2015_imax_ua) - 1; chip->imax++)
		if (max_ua <= aw2015_imax_ua[chip->imax])
			break;
	max_ua = min(max_ua, aw2015_imax_ua[chip->imax]);
	chip->iled_max = max(1u, max_ua * 255 / aw2015_imax_ua[chip->imax]);

	for_each_available_child_of_node(multi, np) {
		if (i == AW2015_CHANNELS) {
			of_node_put(np);
			return dev_err_probe(dev, -EINVAL, "too many LEDs\n");
		}
		ret = of_property_read_u32(np, "reg", &channel);
		if (!ret)
			ret = of_property_read_u32(np, "color", &color);
		if (ret || channel >= AW2015_CHANNELS) {
			of_node_put(np);
			return dev_err_probe(dev, -EINVAL, "bad LED %pOFn\n", np);
		}
		chip->subleds[i].channel = channel;
		chip->subleds[i].color_index = color;
		chip->subleds[i].intensity = 255;
		i++;
	}
	if (!i)
		return dev_err_probe(dev, -EINVAL, "no LEDs\n");

	chip->mc.subled_info = chip->subleds;
	chip->mc.num_colors = i;
	return 0;
}

static int aw2015_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct led_init_data init_data = {};
	struct led_classdev *cdev;
	struct aw2015 *chip;
	unsigned int id;
	int ret;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->client = client;
	i2c_set_clientdata(client, chip);

	ret = devm_mutex_init(dev, &chip->lock);
	if (ret)
		return ret;

	chip->regmap = devm_regmap_init_i2c(client, &aw2015_regmap_config);
	if (IS_ERR(chip->regmap))
		return PTR_ERR(chip->regmap);

	ret = regmap_read(chip->regmap, AW2015_RSTR, &id);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read chip ID\n");
	if (id != AW2015_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "unknown chip ID %#x\n", id);

	ret = aw2015_parse_dt(chip);
	if (ret)
		return ret;

	/* drop what the bootloader or the last OS left running */
	regmap_write(chip->regmap, AW2015_RSTR, AW2015_RSTR_RESET);
	usleep_range(1000, 2000);
	ret = aw2015_idle(chip);
	if (ret)
		return dev_err_probe(dev, ret, "cannot init chip\n");

	cdev = &chip->mc.led_cdev;
	cdev->max_brightness = 255;
	cdev->brightness_set_blocking = aw2015_brightness_set;
	cdev->blink_set = aw2015_blink_set;
	cdev->pattern_set = aw2015_pattern_set;
	cdev->pattern_clear = aw2015_pattern_clear;

	init_data.fwnode = of_fwnode_handle(of_get_child_by_name(dev->of_node, "multi-led"));
	ret = devm_led_classdev_multicolor_register_ext(dev, &chip->mc, &init_data);
	fwnode_handle_put(init_data.fwnode);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register LED\n");

	dev_info(dev, "%s: %u uA max (IMAX %u uA)%s\n", dev_name(cdev->dev),
		 chip->iled_max * aw2015_imax_ua[chip->imax] / 255,
		 aw2015_imax_ua[chip->imax],
		 chip->charge_indicator ? ", charge indicator" : "");
	return 0;
}

static void aw2015_shutdown(struct i2c_client *client)
{
	struct aw2015 *chip = i2c_get_clientdata(client);

	/* back to defaults: the charge indicator works while powered off */
	regmap_write(chip->regmap, AW2015_RSTR, AW2015_RSTR_RESET);
}

static void aw2015_remove(struct i2c_client *client)
{
	aw2015_shutdown(client);
}

static const struct of_device_id aw2015_of_match[] = {
	{ .compatible = "awinic,aw2015" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw2015_of_match);

static struct i2c_driver aw2015_driver = {
	.driver = {
		.name = "leds-aw2015",
		.of_match_table = aw2015_of_match,
	},
	.probe = aw2015_probe,
	.remove = aw2015_remove,
	.shutdown = aw2015_shutdown,
};
module_i2c_driver(aw2015_driver);

MODULE_DESCRIPTION("Awinic AW2015 RGB LED driver");
MODULE_LICENSE("GPL");
