/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SCP IPI ids for the MIUI 14 (V14.0.5.0) SCP firmware.  The Xiaomi kernel
 * source has an older table; this one is the mt6853-style layout read back
 * from the stock kernel image (scp_mbox_pin_send/recv), which adds five
 * Xiaomi pins on mbox1 (ids 26-28, 33, 34; unused here but they shift the
 * slot offsets).
 */
#ifndef _SCP_IPI_PIN_H_
#define _SCP_IPI_PIN_H_

#include <mt-plat/mtk_tinysys_ipi.h>

#define SCP_AWAKE_TIMEOUT 100000

enum scp_core_id {
	SCP_A_ID = 0,
	SCP_CORE_TOTAL = 1,
};

enum {
	SCP_CORE0_ID = 0,
	SCP_CORE1_ID = 1,
};

enum {
	/* mbox0 */
	IPI_OUT_AUDIO_VOW_1       =  0,
	IPI_IN_AUDIO_VOW_ACK_1    =  1,
	IPI_IN_AUDIO_VOW_1        =  2,
	/* mbox1 (core0) */
	IPI_OUT_APCCCI_0          =  3,
	IPI_OUT_DVFS_SET_FREQ_0   =  4,
	IPI_OUT_C_SLEEP_0         =  5,
	IPI_OUT_TEST_0            =  6,
	IPI_IN_APCCCI_0           =  7,
	IPI_IN_SCP_ERROR_INFO_0   =  8,
	IPI_IN_SCP_READY_0        =  9,
	IPI_IN_SCP_RAM_DUMP_0     = 10,
	/* mbox2 */
	IPI_OUT_SCP_MPOOL_0       = 11,
	IPI_IN_SCP_MPOOL_0        = 12,
	/* mbox3 (core1) */
	IPI_OUT_AUDIO_ULTRA_SND_1 = 13,	/* no pin in this firmware */
	IPI_OUT_DVFS_SET_FREQ_1   = 14,
	IPI_OUT_C_SLEEP_1         = 15,
	IPI_OUT_TEST_1            = 16,
	IPI_OUT_LOGGER_CTRL       = 17,
	IPI_OUT_SCPCTL_1          = 18,
	IPI_IN_AUDIO_ULTRA_SND_1  = 19,	/* no pin in this firmware */
	IPI_IN_SCP_ERROR_INFO_1   = 20,
	IPI_IN_LOGGER_CTRL        = 21,
	IPI_IN_SCP_READY_1        = 22,
	IPI_IN_SCP_RAM_DUMP_1     = 23,
	/* mbox4 */
	IPI_OUT_SCP_MPOOL_1       = 24,
	IPI_IN_SCP_MPOOL_1        = 25,
	/* mbox1, Xiaomi additions (names unknown) */
	IPI_OUT_XM_26             = 26,
	IPI_IN_XM_27              = 27,
	IPI_IN_XM_28              = 28,
	IPI_OUT_XM_33             = 33,
	IPI_IN_XM_34              = 34,	/* ack of a send-only pin */
	SCP_IPI_COUNT
};

extern struct mtk_mbox_device scp_mboxdev;
extern struct mtk_ipi_device scp_ipidev;
extern char *core_ids[SCP_CORE_TOTAL];
extern int scp_awake_lock(void *_scp_id);
extern int scp_awake_unlock(void *_scp_id);
extern int scp_clr_spm_reg(void *unused);
extern unsigned int is_scp_ready(enum scp_core_id scp_id);

#endif
