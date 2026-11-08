/* SPDX-License-Identifier: GPL-2.0 */
/* Mainline stand-in: no connectivity power throttling (it reacts to low battery) */
#ifndef CONN_POWER_THROTTLING_H
#define CONN_POWER_THROTTLING_H

struct conn_pwr_plat_info {
	int chip_id;
	int adie_id;
	int (*get_temp)(int *temp);
};

static inline int conn_pwr_init(struct conn_pwr_plat_info *data) { return 0; }
static inline int conn_pwr_deinit(void) { return 0; }
static inline int conn_pwr_suspend(void) { return 0; }
static inline int conn_pwr_resume(void) { return 0; }

/* used by the BT driver: never reports low battery, so BT keeps full TX power */
enum conn_pwr_drv_type { CONN_PWR_DRV_BT, CONN_PWR_DRV_FM, CONN_PWR_DRV_GPS, CONN_PWR_DRV_WIFI, CONN_PWR_DRV_MAX };
enum conn_pwr_low_battery_level {
	CONN_PWR_THR_LV_0, CONN_PWR_THR_LV_1, CONN_PWR_THR_LV_2, CONN_PWR_THR_LV_3,
	CONN_PWR_THR_LV_4, CONN_PWR_THR_LV_5, CONN_PWR_LOW_BATTERY_MAX
};
enum conn_pwr_event_type { CONN_PWR_EVENT_LEVEL, CONN_PWR_EVENT_MAX_TEMP, CONN_PWR_EVENT_MAX };
typedef int (*CONN_PWR_EVENT_CB)(enum conn_pwr_event_type type, void *data);
static inline int conn_pwr_drv_pre_on(enum conn_pwr_drv_type drv, enum conn_pwr_low_battery_level *lv) { return 0; }
static inline int conn_pwr_drv_post_off(enum conn_pwr_drv_type drv) { return 0; }
static inline int conn_pwr_register_event_cb(enum conn_pwr_drv_type drv, CONN_PWR_EVENT_CB cb) { return 0; }

#endif
