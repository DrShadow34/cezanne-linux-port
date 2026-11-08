// SPDX-License-Identifier: GPL-2.0
/*
 * Awinic AW8697 LRA haptic driver (Redmi K30 Ultra / cezanne).
 *
 * Exposes an input force-feedback device:
 *  - FF_RUMBLE / FF_CONSTANT: buzz for replay.length ms (RAM loop mode,
 *    waveform 11 of the library, battery-compensated gain)
 *  - FF_PERIODIC + FF_CUSTOM: custom_data[0] = effect id 0..9, plays one of
 *    the short library waveforms (click, tick, ...) once; other periodic
 *    waveforms are treated like rumble
 *  - FF_GAIN: global strength
 * The waveform library is loaded from firmware "aw8697_haptic.bin"
 * (u16 checksum, u16 RAM base address, RAM image).
 *
 * Register map and sequences follow the GPL Awinic/Xiaomi vendor driver
 * (drivers/input/misc/aw8697_haptic in the cezanne kernel source).
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_wakeup.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define AW8697_REG_ID		0x00
#define AW8697_REG_SYSINT	0x02
#define AW8697_REG_SYSINTM	0x03
#define AW8697_REG_SYSCTRL	0x04
#define AW8697_REG_GO		0x05
#define AW8697_REG_WAVSEQ1	0x07
#define AW8697_REG_WAVLOOP1	0x0f
#define AW8697_REG_MAIN_LOOP	0x13
#define AW8697_REG_DBGCTRL	0x20
#define AW8697_REG_BASE_ADDRH	0x21
#define AW8697_REG_BASE_ADDRL	0x22
#define AW8697_REG_FIFO_AEH	0x23
#define AW8697_REG_FIFO_AEL	0x24
#define AW8697_REG_FIFO_AFH	0x25
#define AW8697_REG_FIFO_AFL	0x26
#define AW8697_REG_DATCTRL	0x2b
#define AW8697_REG_PWMPRC	0x2d
#define AW8697_REG_PWMDBG	0x2e
#define AW8697_REG_BSTDBG1	0x31
#define AW8697_REG_BSTDBG2	0x32
#define AW8697_REG_BSTDBG3	0x33
#define AW8697_REG_BSTCFG	0x34
#define AW8697_REG_ANADBG	0x35
#define AW8697_REG_ANACTRL	0x36
#define AW8697_REG_DATDBG	0x39
#define AW8697_REG_BSTDBG4	0x3a
#define AW8697_REG_PRLVL	0x3e
#define AW8697_REG_RAMADDRH	0x40
#define AW8697_REG_RAMADDRL	0x41
#define AW8697_REG_RAMDATA	0x42
#define AW8697_REG_GLB_STATE	0x46
#define AW8697_REG_BST_AUTO	0x47
#define AW8697_REG_CONT_CTRL	0x48
#define AW8697_REG_F_PRE_H	0x49
#define AW8697_REG_F_PRE_L	0x4a
#define AW8697_REG_TSET		0x4d
#define AW8697_REG_TRIM_LRA	0x5b
#define AW8697_REG_R_SPARE	0x5d
#define AW8697_REG_DETCTRL	0x5f
#define AW8697_REG_VBATDET	0x62
#define AW8697_REG_ADCTEST	0x66
#define AW8697_REG_F_LRA_F0_H	0x68
#define AW8697_REG_F_LRA_F0_L	0x69
#define AW8697_REG_BEMF_VTHH_H	0x74
#define AW8697_REG_BEMF_VTHH_L	0x75
#define AW8697_REG_BEMF_VTHL_H	0x76
#define AW8697_REG_BEMF_VTHL_L	0x77
#define AW8697_REG_DRV_LVL	0x7b
#define AW8697_REG_NUM_F0_1	0x7d
#define AW8697_REG_NUM_F0_2	0x7e
#define AW8697_REG_NUM_F0_3	0x7f
#define AW8697_REG_MAX		0xff

#define AW8697_CHIPID		0x97

#define SYSCTRL_RAMINIT		BIT(5)
#define SYSCTRL_PLAY_MODE	GENMASK(3, 2)
#define SYSCTRL_PLAY_RAM	0
#define SYSCTRL_PLAY_CONT	2
#define SYSCTRL_BST_BOOST	BIT(1)
#define SYSCTRL_STANDBY		BIT(0)
#define SYSINTM_UVLO_OFF	BIT(5)
#define SYSINT_DONE		BIT(0)
#define DBGCTRL_INT_EDGE	BIT(2)
#define PWMDBG_MODE		GENMASK(6, 5)
#define PWMDBG_24K		(2 << 5)
#define PWMPRC_PRC_EN		BIT(7)
#define PRLVL_PR_EN		BIT(7)
#define BSTCFG_PEAKCUR		GENMASK(2, 0)
#define BSTCFG_PEAKCUR_3P5A	5
#define ANADBG_IOC		GENMASK(3, 2)
#define ANADBG_IOC_4P65A	(3 << 2)
#define ANACTRL_LRA_SRC_REG	BIT(5)
#define BSTDBG4_BSTVOL		GENMASK(5, 1)
#define BST_AUTO_AUTOSW		BIT(2)
#define DETCTRL_PROTECT_NOACT	BIT(5)
#define DETCTRL_VBAT_GO		BIT(1)
#define DETCTRL_DIAG_GO		BIT(0)
#define ADCTEST_VBAT_HW_COMP	BIT(6)
#define DATCTRL_FC		GENMASK(7, 6)
#define DATCTRL_FC_1000HZ	(3 << 6)
#define DATCTRL_LPF_EN		BIT(5)
#define CONT_CTRL_CLOSE_PLAY	BIT(3)
#define CONT_CTRL_F0_DETECT	BIT(2)

/* cezanne tuning, from the vendor device tree (aw8697_haptic@5a) */
#define AW_F0_PRE		1700	/* 0.1 Hz */
#define AW_F0_COEFF		260
#define AW_F0_CALI_PERCENT	7
#define AW_CONT_DRV_LVL		0x6a
#define AW_TSET			0x1a
#define AW_R_SPARE		0x68
#define AW_BST_VOL_RAM		0x11
/* vib_f0_trace_parameter */
#define AW_F0_PRE_NUM		9
#define AW_F0_WAIT_NUM		3
#define AW_F0_REPEAT_NUM	1
#define AW_F0_TRACE_NUM		0x1f
static const u8 aw_bstdbg[3] = { 0x30, 0xeb, 0xd4 };
static const u8 aw_bemf[4] = { 0x00, 0x08, 0x03, 0xf8 };

#define AW_NUM_SHORT_EFFECTS	10	/* library waves 1..10 */
#define AW_LOOP_WAVE		11
#define AW_VBAT_REFER		4200
#define AW_VBAT_MIN		3000
#define AW_VBAT_MAX		4500
#define AW_LEVEL_MAX		0x80
#define AW_LEVEL_MIN		0x1e
#define AW_MAX_EFFECTS		16

#define AW_FW_NAME		"aw8697_haptic.bin"

struct aw8697_play {
	bool on;
	int wave;		/* 0: loop for duration, else library wave */
	unsigned int level;	/* AW_LEVEL_MIN..AW_LEVEL_MAX */
	unsigned int length_ms;
};

struct aw8697 {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset;
	struct input_dev *input;
	struct mutex lock;		/* chip access */
	struct work_struct play_work;
	struct hrtimer stop_timer;
	spinlock_t play_lock;		/* pending */
	struct aw8697_play pending;
	u16 gain;
	bool ram_ready;
	int trim;			/* -1: factory (efuse) */
	unsigned int f0;		/* 0.1 Hz, last measured */
};

static const struct regmap_config aw8697_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AW8697_REG_MAX,
	.cache_type = REGCACHE_NONE,
};

static int aw_write(struct aw8697 *aw, unsigned int reg, unsigned int val)
{
	return regmap_write(aw->regmap, reg, val);
}

static int aw_update(struct aw8697 *aw, unsigned int reg, unsigned int mask,
		     unsigned int val)
{
	return regmap_update_bits(aw->regmap, reg, mask, val);
}

static unsigned int aw_read(struct aw8697 *aw, unsigned int reg)
{
	unsigned int val = 0;

	regmap_read(aw->regmap, reg, &val);
	return val;
}

static void aw8697_standby(struct aw8697 *aw)
{
	aw_update(aw, AW8697_REG_SYSINTM, SYSINTM_UVLO_OFF, SYSINTM_UVLO_OFF);
	aw_update(aw, AW8697_REG_SYSCTRL, SYSCTRL_STANDBY, SYSCTRL_STANDBY);
}

static void aw8697_active(struct aw8697 *aw, unsigned int mode, bool boost)
{
	aw_update(aw, AW8697_REG_SYSCTRL, SYSCTRL_PLAY_MODE,
		  FIELD_PREP(SYSCTRL_PLAY_MODE, mode));
	aw_update(aw, AW8697_REG_SYSCTRL, SYSCTRL_STANDBY, 0);
	aw_read(aw, AW8697_REG_SYSINT);		/* clear */
	aw_update(aw, AW8697_REG_SYSINTM, SYSINTM_UVLO_OFF, 0);
	aw_update(aw, AW8697_REG_SYSCTRL, SYSCTRL_BST_BOOST,
		  boost ? SYSCTRL_BST_BOOST : 0);
}

static void aw8697_stop(struct aw8697 *aw)
{
	int i;

	aw_write(aw, AW8697_REG_GO, 0);
	for (i = 0; i < 100; i++) {
		if (!(aw_read(aw, AW8697_REG_GLB_STATE) & 0x0f))
			break;
		usleep_range(2000, 2500);
	}
	aw8697_standby(aw);
}

/* battery voltage in mV, clamped */
static unsigned int aw8697_vbat(struct aw8697 *aw)
{
	unsigned int mv;
	int i;

	aw_update(aw, AW8697_REG_SYSCTRL, SYSCTRL_RAMINIT, SYSCTRL_RAMINIT);
	aw_update(aw, AW8697_REG_DETCTRL, DETCTRL_VBAT_GO, DETCTRL_VBAT_GO);
	for (i = 0; i < 2000; i++)
		if (!(aw_read(aw, AW8697_REG_DETCTRL) & DETCTRL_VBAT_GO))
			break;
	mv = 6100 * aw_read(aw, AW8697_REG_VBATDET) / 256;
	aw_update(aw, AW8697_REG_SYSCTRL, SYSCTRL_RAMINIT, 0);
	return clamp(mv, AW_VBAT_MIN, AW_VBAT_MAX);
}

static void aw8697_upload_trim(struct aw8697 *aw)
{
	if (aw->trim < 0)
		return;
	aw_update(aw, AW8697_REG_ANACTRL, ANACTRL_LRA_SRC_REG,
		  ANACTRL_LRA_SRC_REG);
	aw_write(aw, AW8697_REG_TRIM_LRA, aw->trim);
}

static void aw8697_play_wave(struct aw8697 *aw, const struct aw8697_play *p)
{
	unsigned int level = p->level;

	aw8697_upload_trim(aw);
	if (p->wave) {
		/* one shot from RAM, boosted */
		aw_write(aw, AW8697_REG_WAVSEQ1, p->wave);
		aw_write(aw, AW8697_REG_WAVSEQ1 + 1, 0);
		aw_write(aw, AW8697_REG_WAVLOOP1, 0);
		aw_write(aw, AW8697_REG_MAIN_LOOP, 0);
		aw8697_active(aw, SYSCTRL_PLAY_RAM, true);
		aw_update(aw, AW8697_REG_BSTDBG4, BSTDBG4_BSTVOL,
			  FIELD_PREP(BSTDBG4_BSTVOL, AW_BST_VOL_RAM));
	} else {
		/* loop wave 11 forever (stopped by the timer), boost bypassed */
		level = level * AW_VBAT_REFER / aw8697_vbat(aw);
		level = min(level, AW_LEVEL_MAX * AW_VBAT_REFER / AW_VBAT_MIN);
		aw_write(aw, AW8697_REG_WAVSEQ1, AW_LOOP_WAVE);
		aw_write(aw, AW8697_REG_WAVSEQ1 + 1, 0);
		aw_write(aw, AW8697_REG_WAVLOOP1, 0xf0);	/* infinite */
		aw8697_active(aw, SYSCTRL_PLAY_RAM, false);
	}
	aw_write(aw, AW8697_REG_DATDBG, level);
	aw_write(aw, AW8697_REG_GO, 1);
}

static void aw8697_play_work(struct work_struct *work)
{
	struct aw8697 *aw = container_of(work, struct aw8697, play_work);
	struct aw8697_play p;
	unsigned long flags;

	spin_lock_irqsave(&aw->play_lock, flags);
	p = aw->pending;
	spin_unlock_irqrestore(&aw->play_lock, flags);

	mutex_lock(&aw->lock);
	if (!aw->ram_ready) {
		mutex_unlock(&aw->lock);
		return;
	}
	aw8697_stop(aw);
	if (p.on) {
		aw8697_play_wave(aw, &p);
		if (!p.wave) {
			pm_stay_awake(aw->dev);
			hrtimer_start(&aw->stop_timer,
				      ms_to_ktime(p.length_ms),
				      HRTIMER_MODE_REL);
		}
	} else {
		pm_relax(aw->dev);
	}
	mutex_unlock(&aw->lock);
}

static enum hrtimer_restart aw8697_stop_timer(struct hrtimer *t)
{
	struct aw8697 *aw = container_of(t, struct aw8697, stop_timer);
	unsigned long flags;

	spin_lock_irqsave(&aw->play_lock, flags);
	aw->pending.on = false;
	spin_unlock_irqrestore(&aw->play_lock, flags);
	schedule_work(&aw->play_work);
	return HRTIMER_NORESTART;
}

/* 0..0xffff magnitude -> chip level, then global gain */
static unsigned int aw8697_level(struct aw8697 *aw, unsigned int mag)
{
	unsigned int level;

	level = AW_LEVEL_MIN + (AW_LEVEL_MAX - AW_LEVEL_MIN) * mag / 0xffff;
	level = level * aw->gain / 0xffff;
	return max(level, AW_LEVEL_MIN);
}

static int aw8697_upload(struct input_dev *dev, struct ff_effect *effect,
			 struct ff_effect *old)
{
	s16 id;

	if (effect->type != FF_PERIODIC ||
	    effect->u.periodic.waveform != FF_CUSTOM)
		return 0;
	/* custom_data is a user pointer here; keep the effect id in a copy */
	if (effect->u.periodic.custom_len < 1 ||
	    copy_from_user(&id, effect->u.periodic.custom_data, sizeof(id)))
		return -EFAULT;
	if (id < 0 || id >= AW_NUM_SHORT_EFFECTS)
		return -EINVAL;
	effect->u.periodic.custom_len = id + 1;	/* stash for playback */
	return 0;
}

/* called with dev->event_lock held */
static int aw8697_playback(struct input_dev *dev, int effect_id, int value)
{
	struct aw8697 *aw = input_get_drvdata(dev);
	struct ff_effect *e = &dev->ff->effects[effect_id];
	struct aw8697_play p = { .on = value > 0 };
	unsigned int mag = 0xffff;

	switch (e->type) {
	case FF_RUMBLE:
		mag = max(e->u.rumble.strong_magnitude,
			  e->u.rumble.weak_magnitude);
		break;
	case FF_CONSTANT:
		mag = abs(e->u.constant.level) * 2;
		break;
	case FF_PERIODIC:
		mag = abs(e->u.periodic.magnitude) * 2;
		if (e->u.periodic.waveform == FF_CUSTOM)
			p.wave = e->u.periodic.custom_len;	/* id + 1 */
		break;
	}
	p.level = aw8697_level(aw, min(mag, 0xffffu));
	p.length_ms = e->replay.length ?: 1000;

	hrtimer_try_to_cancel(&aw->stop_timer);
	spin_lock(&aw->play_lock);
	aw->pending = p;
	spin_unlock(&aw->play_lock);
	schedule_work(&aw->play_work);
	return 0;
}

static void aw8697_set_gain(struct input_dev *dev, u16 gain)
{
	struct aw8697 *aw = input_get_drvdata(dev);

	aw->gain = gain;
}

/*
 * F0 measurement: open-loop drive with F0 detection (the Awinic method;
 * Xiaomi's closed-loop variant leaves F_LRA_CONT at 0 here); ~110 ms buzz
 */
static int aw8697_measure_f0(struct aw8697 *aw)
{
	unsigned int f0_reg, t_f0_ms;

	aw8697_stop(aw);
	aw8697_active(aw, SYSCTRL_PLAY_CONT, false);
	aw_update(aw, AW8697_REG_CONT_CTRL,
		  CONT_CTRL_CLOSE_PLAY | CONT_CTRL_F0_DETECT, CONT_CTRL_F0_DETECT);
	aw_update(aw, AW8697_REG_DATCTRL, DATCTRL_FC | DATCTRL_LPF_EN,
		  DATCTRL_FC_1000HZ | DATCTRL_LPF_EN);
	aw_update(aw, AW8697_REG_ANACTRL, ANACTRL_LRA_SRC_REG,
		  ANACTRL_LRA_SRC_REG);
	f0_reg = 1000000000 / (AW_F0_PRE * AW_F0_COEFF);
	aw_write(aw, AW8697_REG_F_PRE_H, f0_reg >> 8);
	aw_write(aw, AW8697_REG_F_PRE_L, f0_reg & 0xff);
	aw_write(aw, AW8697_REG_DRV_LVL, AW_CONT_DRV_LVL);
	aw_write(aw, AW8697_REG_NUM_F0_1, AW_F0_PRE_NUM << 4 | AW_F0_WAIT_NUM);
	aw_write(aw, AW8697_REG_NUM_F0_2, AW_F0_REPEAT_NUM);
	aw_write(aw, AW8697_REG_NUM_F0_3, AW_F0_TRACE_NUM);
	aw_read(aw, AW8697_REG_SYSINT);
	aw_write(aw, AW8697_REG_GO, 1);

	t_f0_ms = 1000 * 10 / AW_F0_PRE;
	msleep(t_f0_ms * (AW_F0_PRE_NUM + AW_F0_WAIT_NUM +
			  (AW_F0_TRACE_NUM + AW_F0_WAIT_NUM) *
			  (AW_F0_REPEAT_NUM - 1)) + 50);

	f0_reg = aw_read(aw, AW8697_REG_F_LRA_F0_H) << 8 |
		 aw_read(aw, AW8697_REG_F_LRA_F0_L);
	aw_update(aw, AW8697_REG_CONT_CTRL,
		  CONT_CTRL_CLOSE_PLAY | CONT_CTRL_F0_DETECT, CONT_CTRL_CLOSE_PLAY);
	aw8697_stop(aw);
	aw_update(aw, AW8697_REG_SYSCTRL, SYSCTRL_PLAY_MODE,
		  FIELD_PREP(SYSCTRL_PLAY_MODE, SYSCTRL_PLAY_RAM));
	if (!f0_reg) {
		dev_warn(aw->dev, "F0 measurement: no result\n");
		return -EIO;
	}
	aw->f0 = 1000000000 / (f0_reg * AW_F0_COEFF);
	dev_info(aw->dev, "F0 %u.%u Hz\n", aw->f0 / 10, aw->f0 % 10);
	return 0;
}

/* trim the LRA oscillator so library waves play at the motor's F0 */
static int aw8697_calibrate(struct aw8697 *aw)
{
	int old = aw->trim, step, ret;
	unsigned int f0;

	aw_write(aw, AW8697_REG_TRIM_LRA, 0);
	ret = aw8697_measure_f0(aw);
	if (ret) {
		aw->trim = old;
		if (old < 0)	/* back to the factory trim */
			aw_update(aw, AW8697_REG_ANACTRL, ANACTRL_LRA_SRC_REG, 0);
		aw8697_upload_trim(aw);
		return ret;
	}

	f0 = aw->f0;
	if (f0 * 100 < AW_F0_PRE * (100 - AW_F0_CALI_PERCENT) ||
	    f0 * 100 > AW_F0_PRE * (100 + AW_F0_CALI_PERCENT))
		f0 = AW_F0_PRE;
	/* 0.25 % per step, 6-bit two's complement */
	step = 100000 * ((int)f0 - AW_F0_PRE) / ((int)f0 * 25);
	step = DIV_ROUND_CLOSEST(step, 10);
	aw->trim = step & 0x3f;
	aw_write(aw, AW8697_REG_TRIM_LRA, aw->trim);
	return 0;
}

static void aw8697_ram_load(const struct firmware *fw, void *ctx)
{
	struct aw8697 *aw = ctx;
	unsigned int base, ae, i;
	u16 sum = 0;

	if (!fw) {
		dev_err(aw->dev, "%s not found\n", AW_FW_NAME);
		return;
	}
	for (i = 2; i < fw->size; i++)
		sum += fw->data[i];
	if (fw->size < 5 || sum != (fw->data[0] << 8 | fw->data[1])) {
		dev_err(aw->dev, "%s: bad checksum\n", AW_FW_NAME);
		goto out;
	}
	base = fw->data[2] << 8 | fw->data[3];

	mutex_lock(&aw->lock);
	aw_update(aw, AW8697_REG_SYSCTRL, SYSCTRL_RAMINIT, SYSCTRL_RAMINIT);
	aw_write(aw, AW8697_REG_BASE_ADDRH, base >> 8);
	aw_write(aw, AW8697_REG_BASE_ADDRL, base & 0xff);
	/* FIFO thresholds for RTP: empty at 1/4, full at 3/4 of the base */
	ae = base >> 2;
	aw_write(aw, AW8697_REG_FIFO_AEH, ae >> 8);
	aw_write(aw, AW8697_REG_FIFO_AEL, ae & 0xff);
	aw_write(aw, AW8697_REG_FIFO_AFH, (base - ae) >> 8);
	aw_write(aw, AW8697_REG_FIFO_AFL, (base - ae) & 0xff);
	aw_write(aw, AW8697_REG_RAMADDRH, base >> 8);
	aw_write(aw, AW8697_REG_RAMADDRL, base & 0xff);
	for (i = 4; i < fw->size; i++)
		aw_write(aw, AW8697_REG_RAMDATA, fw->data[i]);
	aw_update(aw, AW8697_REG_SYSCTRL, SYSCTRL_RAMINIT, 0);
	aw->ram_ready = true;
	mutex_unlock(&aw->lock);
	dev_info(aw->dev, "waveform library loaded (%zu bytes at 0x%x)\n",
		 fw->size - 4, base);
out:
	release_firmware(fw);
}

static void aw8697_hw_init(struct aw8697 *aw)
{
	int i;

	aw8697_standby(aw);
	aw_update(aw, AW8697_REG_PWMDBG, PWMDBG_MODE, PWMDBG_24K);
	aw_write(aw, AW8697_REG_BSTDBG1, aw_bstdbg[0]);
	aw_write(aw, AW8697_REG_BSTDBG2, aw_bstdbg[1]);
	aw_write(aw, AW8697_REG_BSTDBG3, aw_bstdbg[2]);
	aw_write(aw, AW8697_REG_TSET, AW_TSET);
	aw_write(aw, AW8697_REG_R_SPARE, AW_R_SPARE);
	aw_update(aw, AW8697_REG_ANADBG, ANADBG_IOC, ANADBG_IOC_4P65A);
	aw_update(aw, AW8697_REG_BSTCFG, BSTCFG_PEAKCUR, BSTCFG_PEAKCUR_3P5A);
	/* motor protection off, manual boost */
	aw_update(aw, AW8697_REG_DETCTRL, DETCTRL_PROTECT_NOACT,
		  DETCTRL_PROTECT_NOACT);
	aw_update(aw, AW8697_REG_PWMPRC, PWMPRC_PRC_EN, 0);
	aw_update(aw, AW8697_REG_PRLVL, PRLVL_PR_EN, 0);
	aw_update(aw, AW8697_REG_BST_AUTO, BST_AUTO_AUTOSW, 0);

	/* offset calibration */
	aw_update(aw, AW8697_REG_SYSCTRL, SYSCTRL_RAMINIT, SYSCTRL_RAMINIT);
	aw_update(aw, AW8697_REG_DETCTRL, DETCTRL_DIAG_GO, DETCTRL_DIAG_GO);
	for (i = 0; i < 2000; i++)
		if (!(aw_read(aw, AW8697_REG_DETCTRL) & DETCTRL_DIAG_GO))
			break;
	aw_update(aw, AW8697_REG_SYSCTRL, SYSCTRL_RAMINIT, 0);

	aw_update(aw, AW8697_REG_ADCTEST, ADCTEST_VBAT_HW_COMP,
		  ADCTEST_VBAT_HW_COMP);
	aw_write(aw, AW8697_REG_BEMF_VTHH_H, aw_bemf[0]);
	aw_write(aw, AW8697_REG_BEMF_VTHH_L, aw_bemf[1]);
	aw_write(aw, AW8697_REG_BEMF_VTHL_H, aw_bemf[2]);
	aw_write(aw, AW8697_REG_BEMF_VTHL_L, aw_bemf[3]);
	aw_update(aw, AW8697_REG_DBGCTRL, DBGCTRL_INT_EDGE, DBGCTRL_INT_EDGE);
}

/*
 * sysfs: f0 (0.1 Hz, last measurement), trim (LRA oscillator trim; -1 =
 * factory), calibrate (write 1: measure F0 and set trim; buzzes ~110 ms)
 */
static ssize_t f0_show(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	struct aw8697 *aw = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", aw->f0);
}
static DEVICE_ATTR_RO(f0);

static ssize_t trim_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct aw8697 *aw = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", aw->trim);
}

static ssize_t trim_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct aw8697 *aw = dev_get_drvdata(dev);
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;
	if (val < 0 || val > 0x3f)
		return -EINVAL;
	mutex_lock(&aw->lock);
	aw->trim = val;
	aw8697_upload_trim(aw);
	mutex_unlock(&aw->lock);
	return count;
}
static DEVICE_ATTR_RW(trim);

static ssize_t calibrate_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct aw8697 *aw = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&aw->lock);
	ret = aw8697_calibrate(aw);
	mutex_unlock(&aw->lock);
	return ret ?: count;
}
static DEVICE_ATTR_WO(calibrate);

static struct attribute *aw8697_attrs[] = {
	&dev_attr_f0.attr,
	&dev_attr_trim.attr,
	&dev_attr_calibrate.attr,
	NULL
};
ATTRIBUTE_GROUPS(aw8697);

static int aw8697_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw8697 *aw;
	unsigned int id = 0;
	int ret;

	aw = devm_kzalloc(dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;
	aw->dev = dev;
	aw->gain = 0xffff;
	aw->trim = -1;
	mutex_init(&aw->lock);
	spin_lock_init(&aw->play_lock);
	INIT_WORK(&aw->play_work, aw8697_play_work);
	hrtimer_setup(&aw->stop_timer, aw8697_stop_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	i2c_set_clientdata(client, aw);

	aw->regmap = devm_regmap_init_i2c(client, &aw8697_regmap_config);
	if (IS_ERR(aw->regmap))
		return PTR_ERR(aw->regmap);

	aw->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(aw->reset))
		return dev_err_probe(dev, PTR_ERR(aw->reset), "reset gpio\n");
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(aw->reset, 0);
	usleep_range(5000, 6000);

	ret = regmap_read(aw->regmap, AW8697_REG_ID, &id);
	if (ret || id != AW8697_CHIPID)
		return dev_err_probe(dev, ret ?: -ENODEV,
				     "no AW8697 (id 0x%02x)\n", id);
	aw_write(aw, AW8697_REG_ID, 0xaa);	/* soft reset */
	usleep_range(1000, 2000);
	aw8697_hw_init(aw);

	aw->input = devm_input_allocate_device(dev);
	if (!aw->input)
		return -ENOMEM;
	aw->input->name = "aw8697-haptics";
	aw->input->id.bustype = BUS_I2C;
	input_set_drvdata(aw->input, aw);
	input_set_capability(aw->input, EV_FF, FF_RUMBLE);
	input_set_capability(aw->input, EV_FF, FF_CONSTANT);
	input_set_capability(aw->input, EV_FF, FF_PERIODIC);
	input_set_capability(aw->input, EV_FF, FF_CUSTOM);
	input_set_capability(aw->input, EV_FF, FF_SINE);
	input_set_capability(aw->input, EV_FF, FF_GAIN);
	ret = input_ff_create(aw->input, AW_MAX_EFFECTS);
	if (ret)
		return ret;
	aw->input->ff->upload = aw8697_upload;
	aw->input->ff->playback = aw8697_playback;
	aw->input->ff->set_gain = aw8697_set_gain;
	ret = input_register_device(aw->input);
	if (ret)
		return ret;

	device_init_wakeup(dev, true);
	return request_firmware_nowait(THIS_MODULE, FW_ACTION_UEVENT,
				       AW_FW_NAME, dev, GFP_KERNEL, aw,
				       aw8697_ram_load);
}

static void aw8697_remove(struct i2c_client *client)
{
	struct aw8697 *aw = i2c_get_clientdata(client);

	hrtimer_cancel(&aw->stop_timer);
	cancel_work_sync(&aw->play_work);
	mutex_lock(&aw->lock);
	aw8697_stop(aw);
	mutex_unlock(&aw->lock);
	gpiod_set_value_cansleep(aw->reset, 1);
}

static int aw8697_suspend(struct device *dev)
{
	struct aw8697 *aw = dev_get_drvdata(dev);

	hrtimer_cancel(&aw->stop_timer);
	cancel_work_sync(&aw->play_work);
	mutex_lock(&aw->lock);
	aw8697_stop(aw);
	mutex_unlock(&aw->lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(aw8697_pm, aw8697_suspend, NULL);

static const struct of_device_id aw8697_of_match[] = {
	{ .compatible = "awinic,aw8697" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw8697_of_match);

static struct i2c_driver aw8697_driver = {
	.driver = {
		.name = "aw8697",
		.of_match_table = aw8697_of_match,
		.dev_groups = aw8697_groups,
		.pm = pm_sleep_ptr(&aw8697_pm),
	},
	.probe = aw8697_probe,
	.remove = aw8697_remove,
};
module_i2c_driver(aw8697_driver);

MODULE_DESCRIPTION("Awinic AW8697 LRA haptic driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(AW_FW_NAME);
