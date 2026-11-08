/* SPDX-License-Identifier: GPL-2.0 */
/* mbox pin tables, same order as the stock MIUI 14 kernel (see scp_ipi_pin.h) */
#ifndef _SCP_IPI_TABLE_H_
#define _SCP_IPI_TABLE_H_

#include "scp_mbox_layout.h"
#include "scp_ipi_pin.h"

struct mtk_mbox_info scp_mbox_info[SCP_MBOX_TOTAL] = {
	{0, 0, 0, 64, 0, 1, 1, 0, 0, 0, 0, 0, 0, { { { { 0 } } } }, {0, 0, 0} },
	{0, 0, 1, 64, 0, 1, 1, 0, 0, 0, 0, 0, 0, { { { { 0 } } } }, {0, 0, 0} },
	{0, 0, 2, 64, 0, 1, 1, 0, 0, 0, 0, 0, 0, { { { { 0 } } } }, {0, 0, 0} },
	{0, 0, 3, 64, 0, 1, 1, 0, 0, 0, 0, 0, 0, { { { { 0 } } } }, {0, 0, 0} },
	{0, 0, 4, 64, 0, 1, 1, 0, 0, 0, 0, 0, 0, { { { { 0 } } } }, {0, 0, 0} },
};

struct mtk_mbox_pin_recv scp_mbox_pin_recv[] = {
	{0, 0, 0, 0, 1, 0, PIN_IN_SIZE_AUDIO_VOW_ACK_1, 0,
	 IPI_IN_AUDIO_VOW_ACK_1, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{0, 0, 0, 0, 1, 0, PIN_IN_SIZE_AUDIO_VOW_1, 0,
	 IPI_IN_AUDIO_VOW_1, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{1, 0, 0, 0, 1, 0, PIN_IN_SIZE_APCCCI_0, 0,
	 IPI_IN_APCCCI_0, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{1, 0, 0, 0, 1, 0, PIN_IN_SIZE_SCP_ERROR_INFO_0, 0,
	 IPI_IN_SCP_ERROR_INFO_0, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{1, 0, 0, 0, 1, 0, PIN_IN_SIZE_SCP_READY_0, 0,
	 IPI_IN_SCP_READY_0, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{1, 0, 0, 0, 1, 0, PIN_IN_SIZE_SCP_RAM_DUMP_0, 0,
	 IPI_IN_SCP_RAM_DUMP_0, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{1, 0, 0, 0, 1, 0, PIN_IN_SIZE_XM_27, 0,
	 IPI_IN_XM_27, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{1, 0, 0, 0, 1, 0, PIN_IN_SIZE_XM_28, 0,
	 IPI_IN_XM_28, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{1, 0, 1, 0, 1, 0, PIN_OUT_R_SIZE_SLEEP_0, 0,
	 IPI_OUT_C_SLEEP_0, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{1, 0, 1, 0, 1, 0, PIN_IN_SIZE_XM_34, 0,
	 IPI_IN_XM_34, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{2, 0, 0, 0, 1, 0, PIN_IN_SIZE_SCP_MPOOL, 0,
	 IPI_IN_SCP_MPOOL_0, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{3, 0, 0, 0, 1, 0, PIN_IN_SIZE_SCP_ERROR_INFO_1, 0,
	 IPI_IN_SCP_ERROR_INFO_1, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{3, 0, 0, 0, 1, 0, PIN_IN_SIZE_LOGGER_CTRL, 0,
	 IPI_IN_LOGGER_CTRL, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{3, 0, 0, 0, 1, 0, PIN_IN_SIZE_SCP_READY_1, 0,
	 IPI_IN_SCP_READY_1, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{3, 0, 0, 0, 1, 0, PIN_IN_SIZE_SCP_RAM_DUMP_1, 0,
	 IPI_IN_SCP_RAM_DUMP_1, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{3, 0, 1, 0, 1, 0, PIN_OUT_R_SIZE_SLEEP_1, 0,
	 IPI_OUT_C_SLEEP_1, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
	{4, 0, 0, 0, 1, 0, PIN_IN_SIZE_SCP_MPOOL, 0,
	 IPI_IN_SCP_MPOOL_1, { 0 }, 0, 0, 0, { { { { 0 } } } },
	{0, 0, 0, 0, 0, 0} },
};

struct mtk_mbox_pin_send scp_mbox_pin_send[] = {
	{0, 0, 0, 0, PIN_OUT_SIZE_AUDIO_VOW_1, 0,
	 IPI_OUT_AUDIO_VOW_1, { { 0 } }, { 0 }, { { { { 0 } } } } },
	{1, 0, 0, 0, PIN_OUT_SIZE_APCCCI_0, 0,
	 IPI_OUT_APCCCI_0, { { 0 } }, { 0 }, { { { { 0 } } } } },
	{1, 0, 0, 0, PIN_OUT_SIZE_DVFS_SET_FREQ_0, 0,
	 IPI_OUT_DVFS_SET_FREQ_0, { { 0 } }, { 0 }, { { { { 0 } } } } },
	{1, 0, 0, 0, PIN_OUT_C_SIZE_SLEEP_0, 0,
	 IPI_OUT_C_SLEEP_0, { { 0 } }, { 0 }, { { { { 0 } } } } },
	{1, 0, 0, 0, PIN_OUT_SIZE_TEST_0, 0,
	 IPI_OUT_TEST_0, { { 0 } }, { 0 }, { { { { 0 } } } } },
	{1, 0, 0, 0, PIN_OUT_SIZE_XM_26, 0,
	 IPI_OUT_XM_26, { { 0 } }, { 0 }, { { { { 0 } } } } },
	{1, 0, 0, 0, PIN_OUT_SIZE_XM_33, 0,
	 IPI_OUT_XM_33, { { 0 } }, { 0 }, { { { { 0 } } } } },
	{2, 0, 0, 0, PIN_OUT_SIZE_SCP_MPOOL, 0,
	 IPI_OUT_SCP_MPOOL_0, { { 0 } }, { 0 }, { { { { 0 } } } } },
	{3, 0, 0, 0, PIN_OUT_SIZE_DVFS_SET_FREQ_1, 0,
	 IPI_OUT_DVFS_SET_FREQ_1, { { 0 } }, { 0 }, { { { { 0 } } } } },
	{3, 0, 0, 0, PIN_OUT_C_SIZE_SLEEP_1, 0,
	 IPI_OUT_C_SLEEP_1, { { 0 } }, { 0 }, { { { { 0 } } } } },
	{3, 0, 0, 0, PIN_OUT_SIZE_TEST_1, 0,
	 IPI_OUT_TEST_1, { { 0 } }, { 0 }, { { { { 0 } } } } },
	{3, 0, 0, 0, PIN_OUT_SIZE_LOGGER_CTRL, 0,
	 IPI_OUT_LOGGER_CTRL, { { 0 } }, { 0 }, { { { { 0 } } } } },
	{3, 0, 0, 0, PIN_OUT_SIZE_SCPCTL_1, 0,
	 IPI_OUT_SCPCTL_1, { { 0 } }, { 0 }, { { { { 0 } } } } },
	{4, 0, 0, 0, PIN_OUT_SIZE_SCP_MPOOL, 0,
	 IPI_OUT_SCP_MPOOL_1, { { 0 } }, { 0 }, { { { { 0 } } } } },
};

#define SCP_TOTAL_RECV_PIN	ARRAY_SIZE(scp_mbox_pin_recv)
#define SCP_TOTAL_SEND_PIN	ARRAY_SIZE(scp_mbox_pin_send)

struct mtk_mbox_device scp_mboxdev = {
	.name = "scp_mboxdev",
	.pin_recv_table = &scp_mbox_pin_recv[0],
	.pin_send_table = &scp_mbox_pin_send[0],
	.info_table = &scp_mbox_info[0],
	.count = SCP_MBOX_TOTAL,
	.recv_count = SCP_TOTAL_RECV_PIN,
	.send_count = SCP_TOTAL_SEND_PIN,
	.post_cb = (mbox_rx_cb_t)scp_clr_spm_reg,
};

struct mtk_ipi_device scp_ipidev = {
	.name = "scp_ipidev",
	.id = IPI_DEV_SCP,
	.mbdev = &scp_mboxdev,
	.pre_cb = (ipi_tx_cb_t)scp_awake_lock,
	.post_cb = (ipi_tx_cb_t)scp_awake_unlock,
	.prdata = 0,
};

#endif
