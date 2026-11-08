/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT6885 SCP (tinysys, RV33) for mainline: the API the vendor sensor hub
 * (mtk_nanohub) and other SCP clients link against, matching the BSP
 * scp/v02 + scp/mt6885 headers.
 */
#ifndef __CEZANNE_SCP_H__
#define __CEZANNE_SCP_H__

#include <linux/notifier.h>
#include <linux/types.h>
#include <mt-plat/mtk-mbox.h>
#include <mt-plat/mtk_tinysys_ipi.h>
#include "scp_ipi_pin.h"
#include "scp_ipi_wrapper.h"
#include "scp_feature_define.h"

#ifndef ROUNDUP
#define ROUNDUP(a, b)		(((a) + ((b)-1)) & ~((b)-1))
#endif

enum SCP_NOTIFY_EVENT {
	SCP_EVENT_READY = 0,
	SCP_EVENT_STOP,
};

enum scp_ipi_status {
	SCP_IPI_NOT_READY = -2,
	SCP_IPI_ERROR = -1,
	SCP_IPI_DONE,
	SCP_IPI_BUSY,
};

struct scp_ipi_wrapper {
	uint32_t out_id_0;
	uint32_t out_id_1;
	uint32_t in_id_0;
	uint32_t in_id_1;
	uint32_t out_size;
	uint32_t in_size;
	void *msg_0;
	void *msg_1;
};

/* shared DRAM carve-outs, BSP scp_reservedmem_define.h without VOW/audio */
enum scp_reserve_mem_id_t {
	SENS_MEM_ID,
	SCP_A_LOGGER_MEM_ID,
	NUMS_MEM_ID,
};

extern struct mtk_ipi_device scp_ipidev;

enum scp_ipi_status scp_ipi_registration(enum ipi_id id,
	void (*ipi_handler)(int id, void *data, unsigned int len),
	const char *name);
enum scp_ipi_status scp_ipi_unregistration(enum ipi_id id);
enum scp_ipi_status scp_ipi_send(enum ipi_id id, void *buf,
	unsigned int len, unsigned int wait, enum scp_core_id scp_id);

unsigned int is_scp_ready(enum scp_core_id scp_id);
void scp_A_register_notify(struct notifier_block *nb);
void scp_A_unregister_notify(struct notifier_block *nb);

phys_addr_t scp_get_reserve_mem_phys(enum scp_reserve_mem_id_t id);
phys_addr_t scp_get_reserve_mem_virt(enum scp_reserve_mem_id_t id);
phys_addr_t scp_get_reserve_mem_size(enum scp_reserve_mem_id_t id);

int scp_awake_lock(void *_scp_id);
int scp_awake_unlock(void *_scp_id);
void scp_wdt_reset(int cpu_id);

/* power monitor hooks used by mtk_nanohub; no-ops here */
struct scp_power_monitor {
	const char *name;
	struct list_head list;
	void (*suspend)(void);
	void (*resume)(void);
};
static inline int scp_power_monitor_register(struct scp_power_monitor *m)
{
	return 0;
}
static inline void scp_power_monitor_unregister(struct scp_power_monitor *m)
{
}

#endif
