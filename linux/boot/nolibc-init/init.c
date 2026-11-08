/*
 * Minimal /init for mainline bring-up, built against the kernel's nolibc:
 * proves userspace runs without depending on any libc or busybox.
 */
#include <linux/reboot.h>

static int kmsg = -1;

static void say(const char *s)
{
	if (kmsg >= 0)
		write(kmsg, s, strlen(s));
}

int main(void)
{
	char buf[64];
	int i;

	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	kmsg = open("/dev/kmsg", O_WRONLY);
	say("NOLIBC: init running (pid 1)\n");

	for (i = 1; i <= 5; i++) {
		sleep(2);
		strcpy(buf, "NOLIBC: heartbeat ");
		buf[18] = '0' + i;
		buf[19] = '\n';
		buf[20] = 0;
		say(buf);
	}
	say("NOLIBC: requesting restart\n");
	reboot(LINUX_REBOOT_CMD_RESTART);
	for (;;)
		sleep(60);
}
