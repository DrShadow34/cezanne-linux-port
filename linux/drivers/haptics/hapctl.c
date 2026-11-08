// SPDX-License-Identifier: GPL-2.0
/*
 * hapctl: play force-feedback effects on the aw8697 input device.
 *   hapctl rumble <ms> [strength%]
 *   hapctl effect <id 0..9> [strength%]
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int open_haptics(void)
{
	char path[300], name[64];
	struct dirent *de;
	DIR *d = opendir("/dev/input");
	int fd;

	while (d && (de = readdir(d))) {
		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) > 0 &&
		    !strcmp(name, "aw8697-haptics")) {
			closedir(d);
			return fd;
		}
		close(fd);
	}
	if (d)
		closedir(d);
	return -1;
}

int main(int argc, char **argv)
{
	struct ff_effect e = { .id = -1 };
	struct input_event ev = { .type = EV_FF, .value = 1 };
	int strength = argc > 3 ? atoi(argv[3]) : 100;
	unsigned int mag = 0xffff * strength / 100;
	short custom[1];
	int fd, ms;

	if (argc < 3) {
		fprintf(stderr, "usage: hapctl rumble <ms> [strength%%]\n"
				"       hapctl effect <0..9> [strength%%]\n");
		return 1;
	}
	fd = open_haptics();
	if (fd < 0) {
		perror("aw8697-haptics");
		return 1;
	}
	if (!strcmp(argv[1], "rumble")) {
		ms = atoi(argv[2]);
		e.type = FF_RUMBLE;
		e.u.rumble.strong_magnitude = mag;
		e.replay.length = ms;
	} else {
		custom[0] = atoi(argv[2]);
		e.type = FF_PERIODIC;
		e.u.periodic.waveform = FF_CUSTOM;
		e.u.periodic.magnitude = mag / 2;
		e.u.periodic.custom_len = 1;
		e.u.periodic.custom_data = custom;
		ms = 100;
	}
	if (ioctl(fd, EVIOCSFF, &e) < 0) {
		perror("EVIOCSFF");
		return 1;
	}
	ev.code = e.id;
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) {
		perror("play");
		return 1;
	}
	usleep((ms + 50) * 1000);
	return 0;
}
