// SPDX-License-Identifier: GPL-2.0
/*
 * IIO front end for the MT6885 SCP sensor hub: an in-kernel hf_manager client
 * (instead of the Android HAL on /dev/hf_manager) that exposes the physical
 * sensors as polled IIO devices for iio-sensor-proxy.
 *
 * A sensor is enabled on the hub when one of its channels is read and
 * disabled again after IDLE_OFF_MS without reads, so it only runs while
 * something (iio-sensor-proxy with a client) is polling it. Suspend turns
 * them all off: the idle timer doesn't run while suspended, and a sensor
 * left on makes the hub wake the 26 MHz clock for every sample (the AOD
 * reads the light sensor at each minute wake and sleeps again within 1 s).
 */
#include <linux/iio/iio.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>

#include "hf_manager.h"

#define IDLE_OFF_MS		5000
#define FIRST_SAMPLE_MS		500
#define PROBE_RETRY_MS		1000
#define PROBE_RETRIES		60

struct sh_desc {
	u8 type;
	const char *name;
	enum iio_chan_type chan_type;
	int nchan;			/* 3 = x/y/z, 1 = single */
	bool processed;			/* in_*_input instead of raw+scale */
	bool invert;			/* report !word[0] (proximity) */
	bool bias;			/* word[3..5]: calibration bias */
	bool orientation;		/* azimuth, pitch, roll */
	int scale_val, scale_val2;	/* IIO_VAL_INT_PLUS_NANO */
	s64 delay_ns;
};

/*
 * hf_manager gains (mtk_nanohub_init_sensor_info): accel m/s^2 * 1000,
 * gyro rad/s * 1e6, magnetic uT * 100 (IIO wants gauss: 1 uT = 0.01 G),
 * light lux, proximity raw, orientation degrees * 1000.
 */
static const struct sh_desc sh_descs[] = {
	{ SENSOR_TYPE_ACCELEROMETER, "accel", IIO_ACCEL, 3, false, false,
	  false, false, 0, 1000000, 20 * NSEC_PER_MSEC },
	{ SENSOR_TYPE_GYROSCOPE, "gyro", IIO_ANGL_VEL, 3, false, false,
	  false, false, 0, 1000, 50 * NSEC_PER_MSEC },
	{ SENSOR_TYPE_MAGNETIC_FIELD, "magn", IIO_MAGN, 3, false, false, true,
	  false, 0, 100000, 20 * NSEC_PER_MSEC },
	{ SENSOR_TYPE_LIGHT, "als", IIO_LIGHT, 1, true, false, false, false,
	  1, 0, 200 * NSEC_PER_MSEC },
	/* hub reports distance: 0 near, 1 far; IIO wants bigger = nearer */
	{ SENSOR_TYPE_PROXIMITY, "proximity", IIO_PROXIMITY, 1, false, true,
	  false, false, 1, 0, 200 * NSEC_PER_MSEC },
	/*
	 * Android orientation (hub fusion with the AKM library): heading as
	 * in_rot_from_north_magnetic_tilt_comp, pitch/roll as in_incli_x/y.
	 */
	{ SENSOR_TYPE_ORIENTATION, "compass", IIO_ROT, 3, false, false, false,
	  true, 0, 1000000, 100 * NSEC_PER_MSEC },
};

struct sh_sensor {
	const struct sh_desc *desc;
	struct iio_dev *indio;
	struct iio_chan_spec chans[3];
	struct mutex lock;		/* enable state */
	bool enabled;
	unsigned long last_read;	/* jiffies */
	spinlock_t val_lock;
	s32 val[6];
	u8 accuracy;			/* Android SENSOR_STATUS_*, 0..3 */
	u64 seq;			/* bumped per sample */
	wait_queue_head_t wait;
};

/*
 * The hub reports Android axes (upright portrait: +y, landscape with the
 * top to the left: +x); iio-sensor-proxy expects the opposite sign on x/y.
 */
static const struct iio_mount_matrix sh_mount_matrix = {
	.rotation = { "-1", "0", "0", "0", "-1", "0", "0", "0", "1" },
};

static const struct iio_mount_matrix *
sh_get_mount_matrix(const struct iio_dev *indio,
		    const struct iio_chan_spec *chan)
{
	return &sh_mount_matrix;
}

static const struct iio_chan_spec_ext_info sh_ext_info[] = {
	IIO_MOUNT_MATRIX(IIO_SHARED_BY_TYPE, sh_get_mount_matrix),
	{ }
};

static struct {
	struct platform_device *pdev;
	struct hf_client *client;
	struct task_struct *thread;
	struct delayed_work idle_work;
	struct delayed_work probe_work;
	int retries;
	bool registered;
	bool suspending;		/* no enables until resumed */
	struct sh_sensor *sensors[ARRAY_SIZE(sh_descs)];
} sh;

static struct sh_sensor *sh_find(u8 type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sh_descs); i++)
		if (sh.sensors[i] && sh.sensors[i]->desc->type == type)
			return sh.sensors[i];
	return NULL;
}

static int sh_control(struct sh_sensor *s, bool on)
{
	struct hf_manager_cmd cmd = {
		.sensor_type = s->desc->type,
		.action = on ? HF_MANAGER_SENSOR_ENABLE :
			       HF_MANAGER_SENSOR_DISABLE,
		.delay = s->desc->delay_ns,
		.latency = 0,
	};

	return hf_client_control_sensor(sh.client, &cmd);
}

static int sh_read(struct sh_sensor *s, int idx, int *out)
{
	unsigned long flags;
	u64 seq;
	int ret = 0;

	mutex_lock(&s->lock);
	s->last_read = jiffies;
	/* userspace runs on after the suspend notifier: the last value */
	if (!s->enabled && !READ_ONCE(sh.suspending)) {
		spin_lock_irqsave(&s->val_lock, flags);
		seq = s->seq;
		spin_unlock_irqrestore(&s->val_lock, flags);
		ret = sh_control(s, true);
		if (ret < 0) {
			mutex_unlock(&s->lock);
			return ret;
		}
		s->enabled = true;
		mod_delayed_work(system_wq, &sh.idle_work,
				 msecs_to_jiffies(IDLE_OFF_MS));
		/* on-change sensors may take a while; return the last value */
		wait_event_timeout(s->wait, READ_ONCE(s->seq) != seq,
				   msecs_to_jiffies(FIRST_SAMPLE_MS));
	}
	mutex_unlock(&s->lock);

	spin_lock_irqsave(&s->val_lock, flags);
	*out = s->val[idx];
	spin_unlock_irqrestore(&s->val_lock, flags);
	return 0;
}

static void sh_idle_work(struct work_struct *work)
{
	bool pending = false;
	int i;

	for (i = 0; i < ARRAY_SIZE(sh_descs); i++) {
		struct sh_sensor *s = sh.sensors[i];

		if (!s)
			continue;
		mutex_lock(&s->lock);
		if (s->enabled) {
			if (time_after(jiffies, s->last_read +
				       msecs_to_jiffies(IDLE_OFF_MS))) {
				sh_control(s, false);
				s->enabled = false;
			} else {
				pending = true;
			}
		}
		mutex_unlock(&s->lock);
	}
	if (pending)
		mod_delayed_work(system_wq, &sh.idle_work,
				 msecs_to_jiffies(IDLE_OFF_MS / 2));
}

static void sh_all_off(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sh_descs); i++) {
		struct sh_sensor *s = sh.sensors[i];

		if (!s)
			continue;
		mutex_lock(&s->lock);
		if (s->enabled) {
			sh_control(s, false);
			s->enabled = false;
		}
		mutex_unlock(&s->lock);
	}
}

/*
 * Off from the suspend notifier on, not just at it: readers (iio-sensor-
 * proxy polling) aren't frozen yet and would turn a sensor on again. The
 * next read after resume enables it.
 */
static int sh_pm_notify(struct notifier_block *nb, unsigned long action,
			void *data)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
		WRITE_ONCE(sh.suspending, true);
		sh_all_off();
		break;
	case PM_POST_SUSPEND:
		WRITE_ONCE(sh.suspending, false);
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block sh_pm_nb = { .notifier_call = sh_pm_notify };

static int sh_read_raw(struct iio_dev *indio, struct iio_chan_spec const *chan,
		       int *val, int *val2, long mask)
{
	struct sh_sensor *s = iio_priv(indio);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
	case IIO_CHAN_INFO_PROCESSED:
		ret = sh_read(s, chan->scan_index, val);
		return ret ? ret : IIO_VAL_INT;
	case IIO_CHAN_INFO_CALIBBIAS:
		ret = sh_read(s, chan->scan_index + 3, val);
		return ret ? ret : IIO_VAL_INT;
	case IIO_CHAN_INFO_INT_TIME:
		/* iio-sensor-proxy polls at this interval (default 0.8 s) */
		*val = 0;
		*val2 = s->desc->delay_ns / NSEC_PER_USEC;
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_SCALE:
		*val = s->desc->scale_val;
		*val2 = s->desc->scale_val2;
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return -EINVAL;
	}
}

/* Android sensor status: 0 unreliable (needs calibration) .. 3 high */
static ssize_t accuracy_show(struct device *dev, struct device_attribute *a,
			     char *buf)
{
	struct sh_sensor *s = iio_priv(dev_to_iio_dev(dev));
	int dummy, ret = sh_read(s, 0, &dummy);

	return ret ? ret : sysfs_emit(buf, "%u\n", READ_ONCE(s->accuracy));
}
static DEVICE_ATTR_RO(accuracy);

static struct attribute *sh_attrs[] = {
	&dev_attr_accuracy.attr,
	NULL
};

static const struct attribute_group sh_attr_group = {
	.attrs = sh_attrs,
};

static const struct iio_info sh_iio_info = {
	.read_raw = sh_read_raw,
	.attrs = &sh_attr_group,
};

static int sh_event_thread(void *unused)
{
	struct hf_manager_event ev[16];
	unsigned long flags;
	int n, i, k;

	while (!kthread_should_stop()) {
		n = hf_client_poll_sensor_timeout(sh.client, ev, ARRAY_SIZE(ev),
						  msecs_to_jiffies(500));
		for (i = 0; i < n; i++) {
			struct sh_sensor *s;

			if (ev[i].action != DATA_ACTION)
				continue;
			s = sh_find(ev[i].sensor_type);
			if (!s)
				continue;
			spin_lock_irqsave(&s->val_lock, flags);
			for (k = 0; k < ARRAY_SIZE(s->val); k++)
				s->val[k] = ev[i].word[k];
			s->accuracy = ev[i].accurancy;
			if (s->desc->invert)
				s->val[0] = !s->val[0];
			s->seq++;
			spin_unlock_irqrestore(&s->val_lock, flags);
			wake_up(&s->wait);
		}
	}
	return 0;
}

static int sh_add_sensor(int i)
{
	const struct sh_desc *d = &sh_descs[i];
	struct iio_dev *indio;
	struct sh_sensor *s;
	int k;

	if (hf_client_find_sensor(sh.client, d->type) < 0)
		return 0;

	indio = devm_iio_device_alloc(&sh.pdev->dev, sizeof(*s));
	if (!indio)
		return -ENOMEM;
	s = iio_priv(indio);
	s->desc = d;
	s->indio = indio;
	mutex_init(&s->lock);
	spin_lock_init(&s->val_lock);
	init_waitqueue_head(&s->wait);

	for (k = 0; k < d->nchan; k++) {
		struct iio_chan_spec *c = &s->chans[k];

		c->type = d->chan_type;
		c->scan_index = k;
		if (d->orientation) {
			c->modified = 1;
			c->type = k ? IIO_INCLI : IIO_ROT;
			c->channel2 = k ? IIO_MOD_X + k - 1 :
					  IIO_MOD_NORTH_MAGN_TILT_COMP;
		} else if (d->nchan == 3) {
			c->modified = 1;
			c->channel2 = IIO_MOD_X + k;
			c->ext_info = sh_ext_info;
		}
		if (d->processed) {
			c->info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED) |
						BIT(IIO_CHAN_INFO_INT_TIME);
		} else {
			c->info_mask_separate = BIT(IIO_CHAN_INFO_RAW);
			c->info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE);
			if (d->bias)
				c->info_mask_separate |=
					BIT(IIO_CHAN_INFO_CALIBBIAS);
		}
	}

	indio->name = d->name;
	indio->info = &sh_iio_info;
	indio->modes = INDIO_DIRECT_MODE;
	indio->channels = s->chans;
	indio->num_channels = d->nchan;
	sh.sensors[i] = s;
	return devm_iio_device_register(&sh.pdev->dev, indio);
}

/* the hub's manager comes up asynchronously once the SCP reports in */
static void sh_probe_work(struct work_struct *work)
{
	int i, ret;

	if (hf_client_find_sensor(sh.client, SENSOR_TYPE_ACCELEROMETER) < 0) {
		if (++sh.retries < PROBE_RETRIES)
			schedule_delayed_work(&sh.probe_work,
					      msecs_to_jiffies(PROBE_RETRY_MS));
		else
			pr_err("scp-iio: sensor hub did not come up\n");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(sh_descs); i++) {
		ret = sh_add_sensor(i);
		if (ret)
			pr_err("scp-iio: %s: %d\n", sh_descs[i].name, ret);
	}
	sh.registered = true;
}

static int __init scp_iio_init(void)
{
	int ret;

	sh.client = hf_client_create();
	if (!sh.client)
		return -ENOMEM;
	INIT_DELAYED_WORK(&sh.idle_work, sh_idle_work);
	INIT_DELAYED_WORK(&sh.probe_work, sh_probe_work);

	sh.pdev = platform_device_register_simple("scp-sensors", -1, NULL, 0);
	if (IS_ERR(sh.pdev)) {
		ret = PTR_ERR(sh.pdev);
		goto err_client;
	}
	if (!devres_open_group(&sh.pdev->dev, NULL, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto err_pdev;
	}

	sh.thread = kthread_run(sh_event_thread, NULL, "scp-iio");
	if (IS_ERR(sh.thread)) {
		ret = PTR_ERR(sh.thread);
		goto err_devres;
	}
	schedule_delayed_work(&sh.probe_work, 0);
	register_pm_notifier(&sh_pm_nb);
	return 0;

err_devres:
	devres_release_group(&sh.pdev->dev, NULL);
err_pdev:
	platform_device_unregister(sh.pdev);
err_client:
	hf_client_destroy(sh.client);
	return ret;
}

static void __exit scp_iio_exit(void)
{
	unregister_pm_notifier(&sh_pm_nb);
	cancel_delayed_work_sync(&sh.probe_work);
	cancel_delayed_work_sync(&sh.idle_work);
	sh_all_off();
	kthread_stop(sh.thread);
	devres_release_group(&sh.pdev->dev, NULL);
	platform_device_unregister(sh.pdev);
	hf_client_destroy(sh.client);
}

module_init(scp_iio_init);
module_exit(scp_iio_exit);
MODULE_DESCRIPTION("IIO devices for the MT6885 SCP sensor hub");
MODULE_LICENSE("GPL");
