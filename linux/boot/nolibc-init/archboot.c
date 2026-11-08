/*
 * nolibc /init for booting the Arch rootfs under mainline:
 *   userdata (btrfs) -> /		(installed: Arch owns the partition;
 *					default subvolume, zstd compression)
 *   userdata (ext4) -> /			(installed, before 2026-11-02)
 *   userdata (f2fs) : /unencrypted/arch/rootfs.img (ext4) -> /
 *					(dev layout inside MIUI's data, as on 4.14)
 * On any failure it logs and restarts (the shim counts failed boots).
 * Build: build.sh
 */
#include <linux/loop.h>
#include <linux/reboot.h>

#define ROOTIMG "/data/unencrypted/arch/rootfs.img"

#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif

static int kmsg = -1;

static void say(const char *s)
{
	if (kmsg >= 0)
		write(kmsg, s, strlen(s));
}

static void fail(const char *what)
{
	char buf[32];

	say("ARCHBOOT: FAILED: ");
	say(what);
	say(" errno=");
	say(ltoa_r(errno, buf));
	say("\n");
	sleep(2);
	reboot(LINUX_REBOOT_CMD_RESTART);
	for (;;)
		sleep(60);
}

/* find the block device whose GPT name is `want`; fills "/dev/sdXN" */
static int find_part(const char *want, char *dev)
{
	char ents[4096], path[128], buf[512];
	int dfd, fd, n, pos, r;
	char *p, *e;

	dfd = open("/sys/class/block", O_RDONLY | O_DIRECTORY);
	if (dfd < 0)
		return -1;
	while ((n = getdents64(dfd, (void *)ents, sizeof(ents))) > 0) {
		for (pos = 0; pos < n; ) {
			struct linux_dirent64 *d = (void *)(ents + pos);

			pos += d->d_reclen;
			if (d->d_name[0] == '.')
				continue;
			strcpy(path, "/sys/class/block/");
			strcpy(path + strlen(path), d->d_name);
			strcpy(path + strlen(path), "/uevent");
			fd = open(path, O_RDONLY);
			if (fd < 0)
				continue;
			r = read(fd, buf, sizeof(buf) - 1);
			close(fd);
			if (r <= 0)
				continue;
			buf[r] = 0;
			p = strstr(buf, "PARTNAME=");
			if (!p)
				continue;
			p += 9;
			e = strchr(p, '\n');
			if (e)
				*e = 0;
			if (strcmp(p, want))
				continue;
			strcpy(dev, "/dev/");
			strcpy(dev + 5, d->d_name);
			close(dfd);
			return 0;
		}
	}
	close(dfd);
	return -1;
}

int main(void)
{
	struct loop_config cfg;
	char dev[48], loopdev[32], buf[32];
	int i, ctl, nr, lfd, ifd;

	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	kmsg = open("/dev/kmsg", O_WRONLY);
	say("ARCHBOOT: mainline initramfs\n");

	for (i = 0; i < 100; i++) {
		if (find_part("userdata", dev) == 0)
			break;
		usleep(100000);
	}
	if (i == 100)
		fail("userdata partition not found");
	say("ARCHBOOT: userdata is ");
	say(dev);
	say("\n");

	mkdir("/newroot", 0755);
	/* btrfs: its default subvolume (@) is the root; quiet on other fs */
	if (mount(dev, "/newroot", "btrfs", MS_NOATIME, "compress=zstd:1") == 0) {
		say("ARCHBOOT: root is userdata (btrfs)\n");
		goto switch_root;
	}
	if (mount(dev, "/newroot", "ext4", MS_NOATIME, NULL) == 0) {
		say("ARCHBOOT: root is userdata (ext4)\n");
		goto switch_root;
	}

	mkdir("/data", 0755);
	if (mount(dev, "/data", "f2fs", MS_NOATIME, NULL))
		fail("mount userdata");

	ctl = open("/dev/loop-control", O_RDWR);
	if (ctl < 0)
		fail("open loop-control");
	nr = ioctl(ctl, LOOP_CTL_GET_FREE, 0);
	if (nr < 0)
		fail("LOOP_CTL_GET_FREE");
	strcpy(loopdev, "/dev/loop");
	strcpy(loopdev + 9, ltoa_r(nr, buf));

	ifd = open(ROOTIMG, O_RDWR);
	if (ifd < 0)
		fail("open " ROOTIMG);
	lfd = open(loopdev, O_RDWR);
	if (lfd < 0)
		fail("open loop device");
	memset(&cfg, 0, sizeof(cfg));
	cfg.fd = ifd;
	if (ioctl(lfd, LOOP_CONFIGURE, &cfg))
		fail("LOOP_CONFIGURE");

	if (mount(loopdev, "/newroot", "ext4", MS_NOATIME, NULL))
		fail("mount rootfs image");

	/* keep userdata reachable from the new root (as on 4.14) */
	mkdir("/newroot/mnt/userdata", 0755);
	if (mount("/data", "/newroot/mnt/userdata", NULL, MS_MOVE, NULL))
		fail("move userdata");

switch_root:
	say("ARCHBOOT: switching root to Arch (systemd)\n");
	umount2("/proc", MNT_DETACH);
	umount2("/sys", MNT_DETACH);
	if (mount("/dev", "/newroot/dev", NULL, MS_MOVE, NULL))
		fail("move /dev");

	/* switch_root: overmount / with the new root and chroot into it */
	if (chdir("/newroot") || mount(".", "/", NULL, MS_MOVE, NULL) || chroot("."))
		fail("switch root");
	chdir("/");
	{
		char *argv[] = { "/usr/lib/systemd/systemd", NULL };
		char *envp[] = { "HOME=/", "TERM=linux", NULL };

		execve(argv[0], argv, envp);
	}
	fail("exec systemd");
	return 0;
}
