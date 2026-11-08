/* SPDX-License-Identifier: GPL-2.0 */
/* Mainline stand-in: no device APC violation reporting */
#ifndef __DEVAPC_PUBLIC_H__
#define __DEVAPC_PUBLIC_H__

enum devapc_subsys {
	INFRA_SUBSYS_CONN = 3,
};

struct devapc_vio_callbacks {
	int id;
	void (*debug_dump)(void);
};

static inline void register_devapc_vio_callback(struct devapc_vio_callbacks *cb) {}

#endif
