// SPDX-License-Identifier: GPL-2.0
/*
 * One module for the Microtrust driver pieces, initialised in the order the
 * vendor kernel's initcalls ran them (tee core first, the clients after).
 */
#include <linux/module.h>

int cz_init_tee_init(void);
int cz_init_soter_driver_init(void);
int cz_init_teei_client_init(void);
int cz_init_fp_init(void);
int cz_init_vfs_init(void);
int cz_init_keymaster_init(void);
void cz_exit_tee_exit(void);
void cz_exit_soter_driver_exit(void);
void cz_exit_teei_client_exit(void);
void cz_exit_fp_exit(void);
void cz_exit_vfs_exit(void);
void cz_exit_keymaster_exit(void);

static int __init cezanne_teei_init(void)
{
	int ret;

	ret = cz_init_tee_init();
	if (ret)
		return ret;
	ret = cz_init_soter_driver_init();
	if (ret)
		goto err_tee;
	ret = cz_init_teei_client_init();
	if (ret)
		goto err_soter;
	/* the rest log their own failures and are not needed by the others */
	cz_init_fp_init();
	cz_init_vfs_init();
	cz_init_keymaster_init();
	return 0;

err_soter:
	cz_exit_soter_driver_exit();
err_tee:
	cz_exit_tee_exit();
	return ret;
}
module_init(cezanne_teei_init);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Microtrust TEE client driver (cezanne port)");
MODULE_IMPORT_NS("DMA_BUF");
