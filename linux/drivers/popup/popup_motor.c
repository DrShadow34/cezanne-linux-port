// SPDX-License-Identifier: GPL-2.0
/*
 * Redmi K30 Ultra pop-up front camera: TI DRV8846 stepper driver.
 *
 * Step pulses come from a PWM channel, direction / sleep / microstep mode
 * from GPIOs.  A move is MIUI's motor profile (libmivendor_module_motor.so):
 * a fast phase at a 54171 ns step period, then a short slow phase that lets
 * the carriage settle on the end stop.  Timing runs in the kernel, so a
 * crashing or stalled userspace cannot leave the motor running; every move
 * is also capped at MAX_MOVE_MS.  Position feedback (hall sensor) and the
 * policy live in userspace (cezanne-popupd).
 *
 * sysfs (on the platform device):
 *   move   w: "up" | "down"           full travel, MIUI profile
 *              "up <ms>" | "down <ms>" fast phase only, for nudges
 *              "stop"
 *   state  r: "idle" | "up" | "down"  (poll()able, notified when a move ends)
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/workqueue.h>

#define FAST_PERIOD_NS	54171	/* ~18.46 kHz steps */
#define SLOW_PERIOD_NS	625000	/* 1.6 kHz settle */
#define UP_FAST_MS	554
#define DOWN_FAST_MS	559
#define SLOW_MS		50
#define MAX_MOVE_MS	800

enum popup_phase { PHASE_IDLE, PHASE_FAST, PHASE_SLOW };

struct popup_motor {
	struct device *dev;
	struct pwm_device *pwm;
	struct gpio_desc *mode0, *mode1, *sleep, *dir, *power;
	struct mutex lock;		/* move state */
	struct hrtimer timer;
	struct work_struct phase_work;
	enum popup_phase phase;
	bool up;
	bool slow_after;
	ktime_t started;
	struct kernfs_node *state_kn;
};

static int popup_pwm(struct popup_motor *m, u64 period_ns)
{
	struct pwm_state st;

	pwm_init_state(m->pwm, &st);
	st.period = period_ns;
	st.duty_cycle = period_ns / 2;
	st.enabled = period_ns != 0;
	if (!period_ns)
		st.period = FAST_PERIOD_NS;
	return pwm_apply_might_sleep(m->pwm, &st);
}

/* dev_groups appear after probe, so look the attribute up on first use */
static void popup_notify(struct popup_motor *m)
{
	if (!m->state_kn)
		m->state_kn = sysfs_get_dirent(m->dev->kobj.sd, "state");
	if (m->state_kn)
		sysfs_notify_dirent(m->state_kn);
}

static void popup_stop_locked(struct popup_motor *m)
{
	popup_pwm(m, 0);
	gpiod_set_value_cansleep(m->sleep, 1);
	if (m->phase != PHASE_IDLE) {
		dev_dbg(m->dev, "move %s done after %lld ms\n",
			m->up ? "up" : "down",
			ktime_ms_delta(ktime_get(), m->started));
		m->phase = PHASE_IDLE;
		popup_notify(m);
	}
}

static void popup_phase_work(struct work_struct *work)
{
	struct popup_motor *m = container_of(work, struct popup_motor, phase_work);

	mutex_lock(&m->lock);
	if (m->phase == PHASE_FAST && m->slow_after &&
	    ktime_ms_delta(ktime_get(), m->started) + SLOW_MS < MAX_MOVE_MS) {
		m->phase = PHASE_SLOW;
		popup_pwm(m, SLOW_PERIOD_NS);
		hrtimer_start(&m->timer, ms_to_ktime(SLOW_MS), HRTIMER_MODE_REL);
	} else {
		popup_stop_locked(m);
	}
	mutex_unlock(&m->lock);
}

static enum hrtimer_restart popup_timer(struct hrtimer *t)
{
	struct popup_motor *m = container_of(t, struct popup_motor, timer);

	schedule_work(&m->phase_work);
	return HRTIMER_NORESTART;
}

static int popup_move(struct popup_motor *m, bool up, unsigned int fast_ms,
		      bool slow_after)
{
	int ret;

	if (!fast_ms || fast_ms > MAX_MOVE_MS)
		return -EINVAL;

	mutex_lock(&m->lock);
	hrtimer_cancel(&m->timer);
	if (m->phase != PHASE_IDLE)
		popup_stop_locked(m);

	m->up = up;
	m->slow_after = slow_after;
	m->started = ktime_get();
	/* vendor: dir low = up; wake the driver before stepping */
	gpiod_set_value_cansleep(m->dir, up ? 0 : 1);
	gpiod_set_value_cansleep(m->sleep, 0);
	usleep_range(1000, 1200);		/* DRV8846 wake-up time */
	ret = popup_pwm(m, FAST_PERIOD_NS);
	if (ret) {
		gpiod_set_value_cansleep(m->sleep, 1);
		mutex_unlock(&m->lock);
		return ret;
	}
	m->phase = PHASE_FAST;
	popup_notify(m);
	hrtimer_start(&m->timer, ms_to_ktime(fast_ms), HRTIMER_MODE_REL);
	mutex_unlock(&m->lock);
	return 0;
}

static ssize_t move_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct popup_motor *m = dev_get_drvdata(dev);
	char cmd[8];
	unsigned int ms = 0;
	int n, ret;

	n = sscanf(buf, "%7s %u", cmd, &ms);
	if (n < 1)
		return -EINVAL;

	if (!strcmp(cmd, "stop")) {
		mutex_lock(&m->lock);
		hrtimer_cancel(&m->timer);
		popup_stop_locked(m);
		mutex_unlock(&m->lock);
		return count;
	}
	if (strcmp(cmd, "up") && strcmp(cmd, "down"))
		return -EINVAL;
	if (n == 1)
		ret = popup_move(m, cmd[0] == 'u',
				 cmd[0] == 'u' ? UP_FAST_MS : DOWN_FAST_MS, true);
	else
		ret = popup_move(m, cmd[0] == 'u', ms, false);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(move);

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct popup_motor *m = dev_get_drvdata(dev);
	const char *s;

	mutex_lock(&m->lock);
	s = m->phase == PHASE_IDLE ? "idle" : m->up ? "up" : "down";
	mutex_unlock(&m->lock);
	return sysfs_emit(buf, "%s\n", s);
}
static DEVICE_ATTR_RO(state);

static struct attribute *popup_attrs[] = {
	&dev_attr_move.attr,
	&dev_attr_state.attr,
	NULL
};
ATTRIBUTE_GROUPS(popup);

static int popup_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct popup_motor *m;
	u32 step_mode = 2;

	m = devm_kzalloc(dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->dev = dev;
	mutex_init(&m->lock);
	INIT_WORK(&m->phase_work, popup_phase_work);
	hrtimer_setup(&m->timer, popup_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	of_property_read_u32(dev->of_node, "xiaomi,step-mode", &step_mode);

	m->pwm = devm_pwm_get(dev, NULL);
	if (IS_ERR(m->pwm))
		return dev_err_probe(dev, PTR_ERR(m->pwm), "pwm\n");
	m->mode0 = devm_gpiod_get(dev, "mode0", (step_mode & 1) ?
				  GPIOD_OUT_HIGH : GPIOD_OUT_LOW);
	m->mode1 = devm_gpiod_get(dev, "mode1", (step_mode & 2) ?
				  GPIOD_OUT_HIGH : GPIOD_OUT_LOW);
	m->dir = devm_gpiod_get(dev, "dir", GPIOD_OUT_LOW);
	m->sleep = devm_gpiod_get(dev, "sleep", GPIOD_OUT_HIGH);
	m->power = devm_gpiod_get(dev, "power", GPIOD_OUT_HIGH);
	if (IS_ERR(m->mode0) || IS_ERR(m->mode1) || IS_ERR(m->dir) ||
	    IS_ERR(m->sleep) || IS_ERR(m->power))
		return dev_err_probe(dev, -ENODEV, "gpios\n");

	popup_pwm(m, 0);
	platform_set_drvdata(pdev, m);
	dev_info(dev, "pop-up camera motor ready (step mode %u)\n", step_mode);
	return 0;
}

static void popup_remove(struct platform_device *pdev)
{
	struct popup_motor *m = platform_get_drvdata(pdev);

	mutex_lock(&m->lock);
	hrtimer_cancel(&m->timer);
	popup_stop_locked(m);
	mutex_unlock(&m->lock);
	cancel_work_sync(&m->phase_work);
	if (m->state_kn)
		sysfs_put(m->state_kn);
}

static void popup_shutdown(struct platform_device *pdev)
{
	popup_remove(pdev);
}

static const struct of_device_id popup_of_match[] = {
	{ .compatible = "xiaomi,cezanne-popup-motor" },
	{ }
};
MODULE_DEVICE_TABLE(of, popup_of_match);

static struct platform_driver popup_driver = {
	.probe = popup_probe,
	.remove = popup_remove,
	.shutdown = popup_shutdown,
	.driver = {
		.name = "cezanne-popup-motor",
		.of_match_table = popup_of_match,
		.dev_groups = popup_groups,
	},
};
module_platform_driver(popup_driver);

MODULE_DESCRIPTION("Redmi K30 Ultra pop-up camera motor (DRV8846)");
MODULE_LICENSE("GPL");
