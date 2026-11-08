/*
 * nolibc /init: bring up a USB NCM gadget on the mtu3 UDC and stay up for a
 * minute so the host can see the device, then restart.
 */
#include <linux/reboot.h>

static int kmsg = -1;

static void say(const char *s)
{
	if (kmsg >= 0)
		write(kmsg, s, strlen(s));
}

static void put(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0 || write(fd, val, strlen(val)) < 0) {
		say("GADGET: write failed: ");
		say(path);
		say("\n");
	}
	if (fd >= 0)
		close(fd);
}

static void mk(const char *path)
{
	if (mkdir(path, 0755) < 0) {
		say("GADGET: mkdir failed: ");
		say(path);
		say("\n");
	}
}

int main(void)
{
	const char *g = "/sys/kernel/config/usb_gadget/g1";
	int i;

	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	kmsg = open("/dev/kmsg", O_WRONLY);
	say("GADGET: init running\n");

	mount("configfs", "/sys/kernel/config", "configfs", 0, NULL);
	mk("/sys/kernel/config/usb_gadget/g1");
	put("/sys/kernel/config/usb_gadget/g1/idVendor", "0x1d6b");
	put("/sys/kernel/config/usb_gadget/g1/idProduct", "0x0104");
	mk("/sys/kernel/config/usb_gadget/g1/strings/0x409");
	put("/sys/kernel/config/usb_gadget/g1/strings/0x409/manufacturer", "Arch Linux ARM");
	put("/sys/kernel/config/usb_gadget/g1/strings/0x409/product", "K30 Ultra mainline");
	put("/sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber", "cezanne-mainline");
	mk("/sys/kernel/config/usb_gadget/g1/functions/ncm.usb0");
	mk("/sys/kernel/config/usb_gadget/g1/configs/c.1");
	if (symlink("/sys/kernel/config/usb_gadget/g1/functions/ncm.usb0",
		    "/sys/kernel/config/usb_gadget/g1/configs/c.1/ncm.usb0") < 0)
		say("GADGET: symlink failed\n");
	put("/sys/kernel/config/usb_gadget/g1/UDC", "11201000.usb");
	say("GADGET: bound to UDC 11201000.usb\n");
	(void)g;

	for (i = 1; i <= 12; i++) {
		sleep(5);
		say("GADGET: alive\n");
	}
	say("GADGET: restarting\n");
	reboot(LINUX_REBOOT_CMD_RESTART);
	for (;;)
		sleep(60);
}
