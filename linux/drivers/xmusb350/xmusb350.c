// SPDX-License-Identifier: GPL-2.0
/*
 * XMUSB350: charger type detection and QC signalling on cezanne
 *
 * A Weltrend WT6670F microcontroller with Xiaomi's firmware ("XM35"). It does
 * BC1.2 and QC 2.0 / 3.0 / 3+ detection on D+ / D- by itself and generates
 * the QC mode levels and voltage step pulses on request. Register interface
 * from the Xiaomi GPL driver (charger_cezanne/xmusb350.c). The chip runs from
 * VBUS: it only answers while a charger is plugged in.
 *
 * Power supply "xmusb350": online / usb_type from the detection; sysfs:
 *   qc_type  detected type (none, sdp, cdp, dcp, float, ocp, hvdcp, qc2, qc3,
 *            qc3p-18, qc3p-27, qc3-18, qc3-27, pd, pd-dr, unknown)
 *   qc_mode  write qc2-5v, qc2-9v, qc2-12v, qc3 (continuous mode at 5 V)
 *            or qc3p (QC 3+ at 5 V)
 *   pulses   write N: N > 0 raises VBUS by N steps, N < 0 lowers it (QC 3.0
 *            200 mV, QC 3+ 20 mV per step, in the last selected mode)
 *   qc_voltage  nominal VBUS in mV from the requests above (5000 until a
 *            mode is selected, 0 while unplugged), for the indicators
 *   reset    write 1: hardware reset of the chip
 * Each plug-in gets one detection rerun once the MT6360's BC1.2 is done, and
 * a "hvdcp" result (a QC capable DCP) gets the QC handshake started here, as
 * in the vendor driver; the voltage policy lives in userspace
 * (cezanne-dcharge), as for USB PD PPS.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

/* write only */
#define XM35_RERUN_APSD		0x01	/* command byte only */
#define XM35_QC_MODE		0x02
#define XM35_HVDCP_EN		0x05
#define XM35_QC30_PULSES	0x73	/* {Data1, Data2}: bit 15 D+ (up), 14:0 count */
#define XM35_QC3P_PULSES	0x83
/* read only */
#define XM35_TYPE_STATE		0x11	/* type [4:0], error / state [15:8] */
#define XM35_VERSION		0x14

#define XM35_PULSE_UP		BIT(15)
#define XM35_PULSE_MAX		0x7fff

/* raw types (XMUSB350_REG_CHGT_ERROR [4:0]) */
enum {
	XM35_NONE,
	XM35_OCP,
	XM35_FLOAT,
	XM35_SDP,
	XM35_CDP,
	XM35_DCP,
	XM35_QC2,
	XM35_QC3,
	XM35_QC3P_18,
	XM35_QC3P_27,
	XM35_QC3_18,
	XM35_QC3_27,
	XM35_PD,
	XM35_PD_DR,
	XM35_HVDCP = 0x10,
	XM35_UNKNOWN,
	XM35_NTYPES,
};

static const char *const xm35_type_names[XM35_NTYPES] = {
	[XM35_NONE] = "none",		[XM35_OCP] = "ocp",
	[XM35_FLOAT] = "float",		[XM35_SDP] = "sdp",
	[XM35_CDP] = "cdp",		[XM35_DCP] = "dcp",
	[XM35_QC2] = "qc2",		[XM35_QC3] = "qc3",
	[XM35_QC3P_18] = "qc3p-18",	[XM35_QC3P_27] = "qc3p-27",
	[XM35_QC3_18] = "qc3-18",	[XM35_QC3_27] = "qc3-27",
	[XM35_PD] = "pd",		[XM35_PD_DR] = "pd-dr",
	[0x0e] = "unknown",		[0x0f] = "unknown",
	[XM35_HVDCP] = "hvdcp",		[XM35_UNKNOWN] = "unknown",
};

/* XMUSB350_REG_QC_MS values 1.. */
static const char *const xm35_modes[] = {
	"qc2-5v", "qc2-9v", "qc2-12v", "qc3", "qc3p",
};
#define XM35_MODE_QC3P		5

/* vendor: retries of the QC handshake before settling for QC 2.0 */
#define XM35_HVDCP_TRIES	6
#define XM35_HVDCP_DELAY_MS	500
#define XM35_MODE_DELAY_MS	200
#define XM35_POLL_MS		5000
#define XM35_PSY_DEBOUNCE_MS	300
/*
 * On a plug-in the MT6360 runs its own BC1.2 on the same D+ / D- lines at
 * the same time; the two detections disturb each other (OCP / float for a
 * QC charger). Detect again once the MT6360 is done (vendor: the charger
 * manager's type recheck reruns the XMUSB350's APSD).
 */
#define XM35_APSD_DELAY_MS	2000

struct xm35 {
	struct device *dev;
	struct i2c_client *client;
	struct power_supply *psy;
	struct gpio_desc *reset;
	struct notifier_block psy_nb;
	struct delayed_work work;
	struct mutex lock;		/* chip access, state below */
	int type;			/* XM35_*, XM35_NONE while unpowered */
	int state;			/* error / state byte */
	int hvdcp_tries;
	u8 pulse_reg;
	int mv;				/* nominal VBUS from the requests */
	bool apsd_pending;		/* detection rerun due at apsd_at */
	unsigned long apsd_at;
};

static int xm35_read_type(struct xm35 *xm, int *type, int *state)
{
	int v = i2c_smbus_read_word_data(xm->client, XM35_TYPE_STATE);

	if (v < 0)
		return v;
	*type = min(v & 0x1f, XM35_UNKNOWN);
	*state = (v >> 8) & 0xff;
	return 0;
}

static void xm35_schedule_apsd(struct xm35 *xm)
{
	xm->apsd_pending = true;
	xm->apsd_at = jiffies + msecs_to_jiffies(XM35_APSD_DELAY_MS);
}

static void xm35_hard_reset(struct xm35 *xm)
{
	gpiod_set_value_cansleep(xm->reset, 1);
	msleep(10);
	gpiod_set_value_cansleep(xm->reset, 0);
	msleep(60);
}

static void xm35_work(struct work_struct *work)
{
	struct xm35 *xm = container_of(to_delayed_work(work), struct xm35, work);
	int type, state, next = 0;
	bool changed;

	mutex_lock(&xm->lock);
	if (xm35_read_type(xm, &type, &state)) {
		/* no VBUS: the chip is off */
		type = XM35_NONE;
		state = 0;
		xm->hvdcp_tries = 0;
		xm->pulse_reg = XM35_QC30_PULSES;
		xm->mv = 5000;
		xm->apsd_pending = false;
	} else if (xm->type == XM35_NONE && !xm->apsd_pending) {
		/* just powered up: plugged in */
		xm35_schedule_apsd(xm);
	}

	if (type != XM35_NONE && xm->apsd_pending &&
	    time_after_eq(jiffies, xm->apsd_at)) {
		xm->apsd_pending = false;
		xm->hvdcp_tries = 0;
		i2c_smbus_write_byte(xm->client, XM35_RERUN_APSD);
		next = 1000;
	} else if (type == XM35_HVDCP && !xm->apsd_pending) {
		/* a QC capable DCP: start the QC 2.0 / 3.0 / 3+ handshake */
		if (xm->hvdcp_tries++ < XM35_HVDCP_TRIES) {
			msleep(XM35_HVDCP_DELAY_MS);
			i2c_smbus_write_byte_data(xm->client, XM35_HVDCP_EN, 1);
			next = 1000;
		} else {
			type = XM35_QC2;	/* non standard: vendor forces QC 2.0 */
		}
	}
	if (type != XM35_NONE && !next)
		next = xm->apsd_pending ?
		       max(jiffies_to_msecs(xm->apsd_at - jiffies), 1U) :
		       XM35_POLL_MS;

	changed = type != xm->type;
	if (changed)
		dev_info(xm->dev, "%s -> %s (state %#x)\n",
			 xm35_type_names[xm->type], xm35_type_names[type], state);
	xm->type = type;
	xm->state = state;
	mutex_unlock(&xm->lock);

	if (changed)
		power_supply_changed(xm->psy);
	if (next)
		mod_delayed_work(system_freezable_wq, &xm->work,
				 msecs_to_jiffies(next));
}

/* a charger plugged in or out shows up as a change of the other supplies */
static int xm35_psy_notify(struct notifier_block *nb, unsigned long event,
			   void *data)
{
	struct xm35 *xm = container_of(nb, struct xm35, psy_nb);

	if (event == PSY_EVENT_PROP_CHANGED && data != xm->psy)
		mod_delayed_work(system_freezable_wq, &xm->work,
				 msecs_to_jiffies(XM35_PSY_DEBOUNCE_MS));
	return NOTIFY_OK;
}

static irqreturn_t xm35_irq(int irq, void *data)
{
	struct xm35 *xm = data;

	mod_delayed_work(system_freezable_wq, &xm->work, 0);
	return IRQ_HANDLED;
}

static bool xm35_online(int type)
{
	return type != XM35_NONE && type != XM35_UNKNOWN;
}

static enum power_supply_usb_type xm35_usb_type(int type)
{
	switch (type) {
	case XM35_SDP:
	case XM35_PD_DR:
		return POWER_SUPPLY_USB_TYPE_SDP;
	case XM35_CDP:
		return POWER_SUPPLY_USB_TYPE_CDP;
	case XM35_NONE:
	case XM35_UNKNOWN:
	case XM35_FLOAT:
		return POWER_SUPPLY_USB_TYPE_UNKNOWN;
	default:
		return POWER_SUPPLY_USB_TYPE_DCP;
	}
}

static int xm35_get_property(struct power_supply *psy,
			     enum power_supply_property psp,
			     union power_supply_propval *val)
{
	struct xm35 *xm = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = xm35_online(READ_ONCE(xm->type));
		return 0;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = xm35_usb_type(READ_ONCE(xm->type));
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "XMUSB350";
		return 0;
	default:
		return -EINVAL;
	}
}

static enum power_supply_property xm35_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static const struct power_supply_desc xm35_desc = {
	.name = "xmusb350",
	.type = POWER_SUPPLY_TYPE_USB,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP),
	.properties = xm35_props,
	.num_properties = ARRAY_SIZE(xm35_props),
	.get_property = xm35_get_property,
};

static struct xm35 *xm35_from_dev(struct device *dev)
{
	return power_supply_get_drvdata(to_power_supply(dev));
}

static ssize_t qc_type_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  xm35_type_names[READ_ONCE(xm35_from_dev(dev)->type)]);
}
static DEVICE_ATTR_RO(qc_type);

static ssize_t qc_mode_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct xm35 *xm = xm35_from_dev(dev);
	int mode, ret;

	mode = sysfs_match_string(xm35_modes, buf);
	if (mode < 0)
		return mode;
	mode++;

	mutex_lock(&xm->lock);
	ret = i2c_smbus_write_byte_data(xm->client, XM35_QC_MODE, mode);
	if (!ret) {
		static const int mode_mv[] = { 5000, 9000, 12000, 5000, 5000 };

		xm->pulse_reg = mode == XM35_MODE_QC3P ? XM35_QC3P_PULSES :
							 XM35_QC30_PULSES;
		xm->mv = mode_mv[mode - 1];
		/* vendor: >= 120 ms before the first pulse */
		msleep(XM35_MODE_DELAY_MS);
	}
	mutex_unlock(&xm->lock);
	return ret ?: count;
}
static DEVICE_ATTR_WO(qc_mode);

static ssize_t pulses_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct xm35 *xm = xm35_from_dev(dev);
	u16 word;
	int n, ret;

	ret = kstrtoint(buf, 0, &n);
	if (ret)
		return ret;
	if (!n || abs(n) > XM35_PULSE_MAX)
		return -EINVAL;
	word = (n > 0 ? XM35_PULSE_UP : 0) | abs(n);

	mutex_lock(&xm->lock);
	/* Data1 (bit 15..8) first */
	ret = i2c_smbus_write_word_data(xm->client, xm->pulse_reg, swab16(word));
	if (!ret)
		xm->mv = max(xm->mv + n * (xm->pulse_reg == XM35_QC3P_PULSES ?
					   20 : 200), 3600);
	mutex_unlock(&xm->lock);
	return ret ?: count;
}
static DEVICE_ATTR_WO(pulses);

static ssize_t reset_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct xm35 *xm = xm35_from_dev(dev);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;
	if (!val)
		return count;

	mutex_lock(&xm->lock);
	xm35_hard_reset(xm);
	xm->hvdcp_tries = 0;
	xm->pulse_reg = XM35_QC30_PULSES;
	xm->mv = 5000;
	xm35_schedule_apsd(xm);
	mutex_unlock(&xm->lock);
	mod_delayed_work(system_freezable_wq, &xm->work, msecs_to_jiffies(500));
	return count;
}
static DEVICE_ATTR_WO(reset);

static ssize_t qc_voltage_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct xm35 *xm = xm35_from_dev(dev);

	return sysfs_emit(buf, "%d\n",
			  READ_ONCE(xm->type) == XM35_NONE ? 0 : READ_ONCE(xm->mv));
}
static DEVICE_ATTR_RO(qc_voltage);

static ssize_t version_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct xm35 *xm = xm35_from_dev(dev);
	int v;

	mutex_lock(&xm->lock);
	v = i2c_smbus_read_word_data(xm->client, XM35_VERSION);
	mutex_unlock(&xm->lock);
	if (v < 0)
		return v;
	return sysfs_emit(buf, "%#06x\n", swab16(v));
}
static DEVICE_ATTR_RO(version);

static struct attribute *xm35_attrs[] = {
	&dev_attr_qc_type.attr,
	&dev_attr_qc_mode.attr,
	&dev_attr_pulses.attr,
	&dev_attr_reset.attr,
	&dev_attr_qc_voltage.attr,
	&dev_attr_version.attr,
	NULL
};
ATTRIBUTE_GROUPS(xm35);

static void xm35_stop(void *data)
{
	struct xm35 *xm = data;

	power_supply_unreg_notifier(&xm->psy_nb);
	cancel_delayed_work_sync(&xm->work);
}

static int xm35_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct power_supply_config cfg = {};
	struct xm35 *xm;
	int ret;

	xm = devm_kzalloc(dev, sizeof(*xm), GFP_KERNEL);
	if (!xm)
		return -ENOMEM;
	xm->dev = dev;
	xm->client = client;
	xm->type = XM35_NONE;
	xm->pulse_reg = XM35_QC30_PULSES;
	xm->mv = 5000;
	mutex_init(&xm->lock);
	INIT_DELAYED_WORK(&xm->work, xm35_work);

	/* active high; low = running */
	xm->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(xm->reset))
		return dev_err_probe(dev, PTR_ERR(xm->reset), "reset gpio\n");

	cfg.drv_data = xm;
	cfg.fwnode = dev_fwnode(dev);
	cfg.attr_grp = xm35_groups;
	xm->psy = devm_power_supply_register(dev, &xm35_desc, &cfg);
	if (IS_ERR(xm->psy))
		return dev_err_probe(dev, PTR_ERR(xm->psy), "power supply\n");

	xm->psy_nb.notifier_call = xm35_psy_notify;
	ret = power_supply_reg_notifier(&xm->psy_nb);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, xm35_stop, xm);
	if (ret)
		return ret;

	if (client->irq) {
		ret = devm_request_threaded_irq(dev, client->irq, NULL, xm35_irq,
						IRQF_ONESHOT, dev_name(dev), xm);
		if (ret)
			return dev_err_probe(dev, ret, "irq\n");
	}

	/*
	 * A charger already plugged in: the chip kept its result through the
	 * reboot, but the boot loader's BC1.2 has since dropped the charger
	 * out of QC mode. Detect again from scratch (the vendor driver resets
	 * the chip at probe too).
	 */
	if (xm->reset && i2c_smbus_read_word_data(client, XM35_TYPE_STATE) >= 0)
		xm35_hard_reset(xm);
	mod_delayed_work(system_freezable_wq, &xm->work, 0);
	return 0;
}

static const struct of_device_id xm35_of_match[] = {
	{ .compatible = "xiaomi,xmusb350" },
	{ }
};
MODULE_DEVICE_TABLE(of, xm35_of_match);

static const struct i2c_device_id xm35_i2c_ids[] = {
	{ "xmusb350" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, xm35_i2c_ids);

static struct i2c_driver xm35_driver = {
	.driver = {
		.name = "xmusb350",
		.of_match_table = xm35_of_match,
	},
	.probe = xm35_probe,
	.id_table = xm35_i2c_ids,
};
module_i2c_driver(xm35_driver);

MODULE_DESCRIPTION("XMUSB350 charger type detection / QC signalling");
MODULE_LICENSE("GPL");
