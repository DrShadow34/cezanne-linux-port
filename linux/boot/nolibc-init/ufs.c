/*
 * nolibc /init for mainline UFS bring-up: list the GPT partitions the kernel
 * found (by PARTNAME), mount userdata read-only and look for the Arch rootfs
 * image, then restart. Everything is logged to /dev/kmsg (-> ramoops).
 */
#include <linux/reboot.h>

#define ROOTIMG "/mnt/unencrypted/arch/rootfs.img"

static int kmsg = -1;

/* nolibc has no strcat */
static char *append(char *dst, const char *src)
{
	strcpy(dst + strlen(dst), src);
	return dst;
}

static void say(const char *s)
{
	if (kmsg >= 0)
		write(kmsg, s, strlen(s));
}

static void say_num(const char *pre, long v, const char *post)
{
	char buf[64];

	say(pre);
	say(ltoa_r(v, buf));
	say(post);
}

/* read the PARTNAME= line of /sys/class/block/<dev>/uevent into out */
static int partname(const char *dev, char *out, int len)
{
	char path[128], buf[512];
	int fd, n;
	char *p, *e;

	strcpy(path, "/sys/class/block/");
	append(path, dev);
	append(path, "/uevent");
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	p = strstr(buf, "PARTNAME=");
	if (!p)
		return -1;
	p += 9;
	e = strchr(p, '\n');
	if (e)
		*e = 0;
	strncpy(out, p, len - 1);
	out[len - 1] = 0;
	return 0;
}

int main(void)
{
	char ents[4096], name[64], userdata[32] = "";
	int fd, n, pos, parts = 0, i;
	struct stat st;

	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	kmsg = open("/dev/kmsg", O_WRONLY);
	say("UFS: init running, waiting for block devices\n");

	for (i = 0; i < 20 && !userdata[0]; i++) {
		sleep(1);
		fd = open("/sys/class/block", O_RDONLY | O_DIRECTORY);
		if (fd < 0)
			continue;
		parts = 0;
		while ((n = getdents64(fd, (void *)ents, sizeof(ents))) > 0) {
			for (pos = 0; pos < n; ) {
				struct linux_dirent64 *d = (void *)(ents + pos);

				pos += d->d_reclen;
				if (d->d_name[0] == '.' || partname(d->d_name, name, sizeof(name)))
					continue;
				parts++;
				if (!strcmp(name, "userdata"))
					strcpy(userdata, d->d_name);
				if (i == 0 || !strcmp(name, "userdata") || !strcmp(name, "super") ||
				    !strcmp(name, "boot") || !strcmp(name, "persist")) {
					say("UFS: part ");
					say(d->d_name);
					say(" = ");
					say(name);
					say("\n");
				}
			}
		}
		close(fd);
	}
	say_num("UFS: partitions with PARTNAME: ", parts, "\n");

	if (userdata[0]) {
		char dev[48] = "/dev/";

		append(dev, userdata);
		mkdir("/mnt", 0755);
		if (mount(dev, "/mnt", "f2fs", MS_RDONLY, NULL) == 0) {
			say("UFS: userdata (f2fs) mounted read-only\n");
			if (stat(ROOTIMG, &st) == 0)
				say_num("UFS: found " ROOTIMG ", MiB: ", st.st_size >> 20, "\n");
			else
				say("UFS: rootfs image NOT found\n");
			umount2("/mnt", 0);
		} else {
			say_num("UFS: mount userdata failed, errno ", errno, "\n");
		}
	} else {
		say("UFS: userdata partition not found\n");
	}

	say("UFS: restarting\n");
	reboot(LINUX_REBOOT_CMD_RESTART);
	for (;;)
		sleep(60);
}
