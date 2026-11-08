/* SPDX-License-Identifier: GPL-2.0 */
/* mbox pin sizes (in 32-bit slots), MIUI 14 SCP firmware; see scp_ipi_pin.h */
#ifndef _SCP_MBOX_LAYOUT_H_
#define _SCP_MBOX_LAYOUT_H_

#define SCP_MBOX_TOTAL 5

/* mbox0 */
#define PIN_OUT_SIZE_AUDIO_VOW_1	 9
#define PIN_IN_SIZE_AUDIO_VOW_ACK_1	 2
#define PIN_IN_SIZE_AUDIO_VOW_1		26
/* mbox1 */
#define PIN_OUT_SIZE_APCCCI_0		 2
#define PIN_OUT_SIZE_DVFS_SET_FREQ_0	 1
#define PIN_OUT_C_SIZE_SLEEP_0		 2
#define PIN_OUT_R_SIZE_SLEEP_0		 1
#define PIN_OUT_SIZE_TEST_0		 1
#define PIN_OUT_SIZE_XM_26		 9
#define PIN_OUT_SIZE_XM_33		10
#define PIN_IN_SIZE_APCCCI_0		 2
#define PIN_IN_SIZE_SCP_ERROR_INFO_0	10
#define PIN_IN_SIZE_SCP_READY_0		 1
#define PIN_IN_SIZE_SCP_RAM_DUMP_0	 2
#define PIN_IN_SIZE_XM_27		 2
#define PIN_IN_SIZE_XM_28		 5
#define PIN_IN_SIZE_XM_34		10
/* mbox3 */
#define PIN_OUT_SIZE_DVFS_SET_FREQ_1	 1
#define PIN_OUT_C_SIZE_SLEEP_1		 2
#define PIN_OUT_R_SIZE_SLEEP_1		 1
#define PIN_OUT_SIZE_TEST_1		 1
#define PIN_OUT_SIZE_LOGGER_CTRL	 6
#define PIN_OUT_SIZE_SCPCTL_1		 2
#define PIN_IN_SIZE_SCP_ERROR_INFO_1	10
#define PIN_IN_SIZE_LOGGER_CTRL		 6
#define PIN_IN_SIZE_SCP_READY_1		 1
#define PIN_IN_SIZE_SCP_RAM_DUMP_1	 2
/* mbox2, mbox4 */
#define PIN_OUT_SIZE_SCP_MPOOL		34
#define PIN_IN_SIZE_SCP_MPOOL		30

#define PIN_OUT_SIZE_CHRE_0		34
#define PIN_OUT_SIZE_CHREX_0		14
#define PIN_OUT_SIZE_SENSOR_0		14
#define PIN_IN_SIZE_CHRE_0		14
#define PIN_IN_SIZE_SENSOR_0		14

#endif
