// SPDX-License-Identifier: GPL-2.0
/*
 * AKM AK09970 3-axis hall sensor (Redmi K30 Ultra pop-up camera position).
 *
 * Polled IIO device: the first read starts continuous 100 Hz measurement,
 * which stops again after IDLE_OFF_MS without reads.  Raw counts are
 * 1.1 uT each; MIUI's position calibration (persist hall_calidata.txt) is
 * in these counts.  Register layout from the vendor akm09970 driver.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define AK09970_REG_WIA		0x00
#define AK09970_REG_ST_XYZ	0x17
#define AK09970_REG_CNTL2	0x21
#define AK09970_WIA1		0x48
#define AK09970_WIA2		0xC0
#define AK09970_MODE_OFF	0x00
#define AK09970_MODE_100HZ	0x0E
#define AK09970_ST_DRDY		BIT(0)
#define AK09970_ST_ERRADC	BIT(8)
#define AK09970_ST_ERRXY	BIT(7)
#define IDLE_OFF_MS		3000

struct ak09970 {
	struct i2c_client *client;
	struct gpio_desc *reset;
	struct mutex lock;
	bool running;
	unsigned long last_read;
	struct delayed_work idle_work;
};

static int ak09970_start(struct ak09970 *ak)
{
	int ret;

	/* vendor power-up: pulse reset, then continuous mode */
	gpiod_set_value_cansleep(ak->reset, 1);
	udelay(50);
	gpiod_set_value_cansleep(ak->reset, 0);
	udelay(100);
	ret = i2c_smbus_write_byte_data(ak->client, AK09970_REG_CNTL2,
					AK09970_MODE_100HZ);
	if (ret)
		return ret;
	ak->running = true;
	msleep(12);		/* first sample */
	return 0;
}

static void ak09970_stop(struct ak09970 *ak)
{
	i2c_smbus_write_byte_data(ak->client, AK09970_REG_CNTL2,
				  AK09970_MODE_OFF);
	gpiod_set_value_cansleep(ak->reset, 1);
	ak->running = false;
}

static void ak09970_idle_work(struct work_struct *work)
{
	struct ak09970 *ak = container_of(work, struct ak09970, idle_work.work);

	mutex_lock(&ak->lock);
	if (ak->running && time_after(jiffies, ak->last_read +
				      msecs_to_jiffies(IDLE_OFF_MS)))
		ak09970_stop(ak);
	else if (ak->running)
		schedule_delayed_work(&ak->idle_work,
				      msecs_to_jiffies(IDLE_OFF_MS));
	mutex_unlock(&ak->lock);
}

static int ak09970_read(struct ak09970 *ak, int axis, int *val)
{
	u8 buf[8];
	int ret, tries;
	u16 st;

	mutex_lock(&ak->lock);
	ak->last_read = jiffies;
	if (!ak->running) {
		ret = ak09970_start(ak);
		if (ret)
			goto out;
		schedule_delayed_work(&ak->idle_work,
				      msecs_to_jiffies(IDLE_OFF_MS));
	}
	for (tries = 0; tries < 3; tries++) {
		ret = i2c_smbus_read_i2c_block_data(ak->client,
						    AK09970_REG_ST_XYZ,
						    sizeof(buf), buf);
		if (ret < 0)
			goto out;
		st = get_unaligned_be16(buf);
		if (!(st & (AK09970_ST_ERRADC | AK09970_ST_ERRXY)))
			break;
		usleep_range(10000, 11000);
	}
	if (tries == 3) {
		ret = -EIO;
		goto out;
	}
	*val = (s16)get_unaligned_be16(&buf[2 + 2 * axis]);
	ret = 0;
out:
	mutex_unlock(&ak->lock);
	return ret;
}

static int ak09970_read_raw(struct iio_dev *indio,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct ak09970 *ak = iio_priv(indio);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = ak09970_read(ak, chan->address, val);
		return ret ? ret : IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		/* 1.1 uT = 0.011 G per count */
		*val = 0;
		*val2 = 11000;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

#define AK09970_CHAN(ax, idx) {						\
	.type = IIO_MAGN,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##ax,					\
	.address = idx,							\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
}

static const struct iio_chan_spec ak09970_channels[] = {
	AK09970_CHAN(X, 0), AK09970_CHAN(Y, 1), AK09970_CHAN(Z, 2),
};

static const struct iio_info ak09970_info = {
	.read_raw = ak09970_read_raw,
};

static void ak09970_cleanup(void *data)
{
	struct ak09970 *ak = data;

	cancel_delayed_work_sync(&ak->idle_work);
	mutex_lock(&ak->lock);
	if (ak->running)
		ak09970_stop(ak);
	mutex_unlock(&ak->lock);
}

static int ak09970_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio;
	struct ak09970 *ak;
	u8 wia[2];
	int ret;

	indio = devm_iio_device_alloc(dev, sizeof(*ak));
	if (!indio)
		return -ENOMEM;
	ak = iio_priv(indio);
	ak->client = client;
	mutex_init(&ak->lock);
	INIT_DELAYED_WORK(&ak->idle_work, ak09970_idle_work);

	ak->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ak->reset))
		return dev_err_probe(dev, PTR_ERR(ak->reset), "reset gpio\n");
	gpiod_set_value_cansleep(ak->reset, 0);
	udelay(100);
	ret = i2c_smbus_read_i2c_block_data(client, AK09970_REG_WIA, 2, wia);
	gpiod_set_value_cansleep(ak->reset, 1);
	if (ret < 0)
		return dev_err_probe(dev, ret, "no response\n");
	if (wia[0] != AK09970_WIA1 || wia[1] != AK09970_WIA2)
		dev_warn(dev, "unexpected WIA %02x %02x\n", wia[0], wia[1]);

	ret = devm_add_action_or_reset(dev, ak09970_cleanup, ak);
	if (ret)
		return ret;

	indio->name = "popup_hall";
	indio->info = &ak09970_info;
	indio->modes = INDIO_DIRECT_MODE;
	indio->channels = ak09970_channels;
	indio->num_channels = ARRAY_SIZE(ak09970_channels);
	return devm_iio_device_register(dev, indio);
}

static const struct of_device_id ak09970_of_match[] = {
	{ .compatible = "asahi-kasei,ak09970" },
	{ }
};
MODULE_DEVICE_TABLE(of, ak09970_of_match);

static struct i2c_driver ak09970_driver = {
	.driver = {
		.name = "ak09970",
		.of_match_table = ak09970_of_match,
	},
	.probe = ak09970_probe,
};
module_i2c_driver(ak09970_driver);

MODULE_DESCRIPTION("AKM AK09970 hall sensor");
MODULE_LICENSE("GPL");
