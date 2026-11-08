// SPDX-License-Identifier: GPL-2.0
/*
 * onsemi FAN53870: 7-channel I2C LDO (camera rails on the Redmi K30 Ultra).
 *
 * Register map from the vendor driver (drivers/regulator/fan53870.c):
 * 0x00 PID, 0x01 RID, 0x03 enable (bit n = LDOn+1), 0x04 + n VSET.
 * VSET = (mV - 800) / 8 + 99 for LDO1/2, (mV - 1500) / 8 + 16 for LDO3-7.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define FAN53870_REG_PID	0x00
#define FAN53870_REG_ENABLE	0x03
#define FAN53870_REG_VSET(n)	(0x04 + (n))
#define FAN53870_NUM_LDO	7

static const struct regulator_ops fan53870_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
};

#define FAN53870_LDO(_n, _min, _minsel)						\
	{								\
		.name = "ldo" #_n,					\
		.of_match = of_match_ptr("ldo" #_n),			\
		.regulators_node = of_match_ptr("regulators"),		\
		.id = (_n) - 1,						\
		.ops = &fan53870_ops,					\
		.type = REGULATOR_VOLTAGE,				\
		.owner = THIS_MODULE,					\
		.min_uV = (_min),					\
		.linear_min_sel = (_minsel),				\
		.uV_step = 8000,					\
		.n_voltages = 256,					\
		.vsel_reg = FAN53870_REG_VSET((_n) - 1),		\
		.vsel_mask = 0xff,					\
		.enable_reg = FAN53870_REG_ENABLE,			\
		.enable_mask = BIT((_n) - 1),				\
		.enable_time = 1000,					\
	}

static const struct regulator_desc fan53870_regulators[FAN53870_NUM_LDO] = {
	FAN53870_LDO(1, 800000, 99),
	FAN53870_LDO(2, 800000, 99),
	FAN53870_LDO(3, 1500000, 16),
	FAN53870_LDO(4, 1500000, 16),
	FAN53870_LDO(5, 1500000, 16),
	FAN53870_LDO(6, 1500000, 16),
	FAN53870_LDO(7, 1500000, 16),
};

static const struct regmap_config fan53870_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x10,
};

static int fan53870_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config cfg = { .dev = dev };
	struct gpio_desc *reset;
	struct regmap *map;
	unsigned int pid;
	int i, ret;

	/* vendor pinctrl drives the reset line high (out of reset) */
	reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset))
		return PTR_ERR(reset);
	if (reset)
		usleep_range(1000, 1500);

	map = devm_regmap_init_i2c(client, &fan53870_regmap);
	if (IS_ERR(map))
		return PTR_ERR(map);
	ret = regmap_read(map, FAN53870_REG_PID, &pid);
	if (ret)
		return dev_err_probe(dev, ret, "no response\n");
	dev_info(dev, "FAN53870 PID %#x\n", pid);

	cfg.regmap = map;
	for (i = 0; i < FAN53870_NUM_LDO; i++) {
		struct regulator_dev *rdev;

		rdev = devm_regulator_register(dev, &fan53870_regulators[i],
					       &cfg);
		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev), "ldo%d\n",
					     i + 1);
	}
	return 0;
}

static const struct of_device_id fan53870_of_match[] = {
	{ .compatible = "onsemi,fan53870" },
	{ }
};
MODULE_DEVICE_TABLE(of, fan53870_of_match);

static struct i2c_driver fan53870_driver = {
	.driver = {
		.name = "fan53870",
		.of_match_table = fan53870_of_match,
	},
	.probe = fan53870_probe,
};
module_i2c_driver(fan53870_driver);

MODULE_DESCRIPTION("onsemi FAN53870 LDO regulator");
MODULE_LICENSE("GPL");
