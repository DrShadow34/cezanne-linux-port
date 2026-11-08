// SPDX-License-Identifier: GPL-2.0
/*
 * Boot chooser support for the Redmi K30 Ultra boot-slot shim
 * (mainline/shim): `reboot miui` (LINUX_REBOOT_CMD_RESTART2 with "miui")
 * sets the one-shot RAM flag the shim checks, so the next boot starts
 * MIUI; any other restart leaves it clear and the phone comes back to Arch.
 * Writing 1 to the boot_ok parameter (cezanne-boot-ok.service, once
 * userspace is up) resets the shim's failed-boot counter; after
 * ARCH_TRIES boots without that the shim falls back to MIUI.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/string.h>

#include "bootsel.h"

static void __iomem *flag;
static bool force_miui_pending;
static bool force_miui_armed;
static int force_miui_set(const char *val, const struct kernel_param *kp);

static int bootsel_notify(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	const char *cmd = data;
	bool miui = (action == SYS_RESTART && cmd && !strcmp(cmd, "miui")) ||
		    force_miui_armed;

	pr_info("cezanne-bootsel: restart cmd=%s armed=%d -> %s\n",
		cmd ? cmd : "(null)", force_miui_armed,
		miui ? "MIUI" : "Arch");
	writeq(miui ? BOOTSEL_MIUI : 0, flag);
	writeq(miui ? ~BOOTSEL_MIUI : 0, flag + 8);
	pr_info("cezanne-bootsel: flag now %016llx %016llx\n",
		readq(flag), readq(flag + 8));
	if (miui)
		pr_info("cezanne-bootsel: next boot: MIUI\n");
	return NOTIFY_DONE;
}

static int boot_ok_set(const char *val, const struct kernel_param *kp)
{
	bool ok;
	int ret = kstrtobool(val, &ok);

	if (ret)
		return ret;
	if (ok)
		writeq(0, flag + 16);
	return 0;
}

static const struct kernel_param_ops boot_ok_ops = {
	.set = boot_ok_set,
};
module_param_cb(boot_ok, &boot_ok_ops, NULL, 0200);
MODULE_PARM_DESC(boot_ok, "write 1 once booted: reset the failed-boot counter");

static int force_miui_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(val, &on);

	if (ret)
		return ret;
	if (!flag) {	/* insmod-time param: init will apply it */
		force_miui_pending = on;
		return 0;
	}
	force_miui_armed = on;
	writeq(on ? BOOTSEL_MIUI : 0, flag);
	writeq(on ? ~BOOTSEL_MIUI : 0, flag + 8);
	pr_info("cezanne-bootsel: forced next boot: %s\n",
		on ? "MIUI" : "Arch");
	return 0;
}

static const struct kernel_param_ops force_miui_ops = {
	.set = force_miui_set,
};
module_param_cb(force_miui, &force_miui_ops, NULL, 0200);
MODULE_PARM_DESC(force_miui, "write 1 to arm the one-shot MIUI boot flag");

static struct notifier_block bootsel_nb = {
	.notifier_call = bootsel_notify,
};

static int __init bootsel_init(void)
{
	/* inside the no-map pstore carve-out: not System RAM, so ioremap */
	flag = ioremap(BOOTSEL_ADDR, 24);
	if (!flag)
		return -ENOMEM;
	pr_info("cezanne-bootsel: flag %016llx %016llx %016llx\n",
		readq(flag), readq(flag + 8), readq(flag + 16));
	if (force_miui_pending) {
		writeq(BOOTSEL_MIUI, flag);
		writeq(~BOOTSEL_MIUI, flag + 8);
		pr_info("cezanne-bootsel: armed MIUI boot (pending param)\n");
	}
	return register_reboot_notifier(&bootsel_nb);
}

static void __exit bootsel_exit(void)
{
	unregister_reboot_notifier(&bootsel_nb);
	iounmap(flag);
}

module_init(bootsel_init);
module_exit(bootsel_exit);
MODULE_DESCRIPTION("Redmi K30 Ultra boot chooser flag");
MODULE_LICENSE("GPL");
