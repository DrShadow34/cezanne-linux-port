/*
 * cezanne-touch: the touchscreen's raw multitouch, read beside the
 * compositor (no grab: niri keeps getting every touch), for cezanne-shell.
 * niri has no touchscreen gestures; the shell gets three-finger swipes
 * from here, and the fingers' positions while "show taps" is on.
 *
 * stdout, one line per event, coordinates 0..1 in the panel's native
 * (portrait) orientation; the shell turns them with the output transform:
 *   g3 DX DY            a three-finger swipe ended: centroid travel
 *   d SLOT X Y          finger down      (only while taps are on)
 *   m SLOT X Y          finger moved     (at most every 16 ms per finger)
 *   u SLOT              finger up
 * stdin: "taps 1" / "taps 0".
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SLOTS 10
#define BIT(a, n) ((a)[(n) / 8] & (1 << ((n) % 8)))

struct finger {
	int active, x, y, changed, down, up;
	long last_ms;
};

static struct finger f[SLOTS];
static int slot, taps;
static double xmin, xspan, ymin, yspan;

/* three-finger swipe: started with exactly three fingers, cancelled by a
 * fourth; decided when the last finger lifts */
static int gesture, cancelled;
static double gx0, gy0, gx, gy;

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static double nx(int x) { return (x - xmin) / xspan; }
static double ny(int y) { return (y - ymin) / yspan; }

static int open_touchscreen(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	char path[300];
	int fd = -1;

	if (!d)
		return -1;
	while (fd < 0 && (e = readdir(d))) {
		unsigned char props[INPUT_PROP_CNT / 8 + 1] = { 0 }, abs[ABS_CNT / 8 + 1] = { 0 };
		struct input_absinfo ax, ay;

		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGPROP(sizeof(props)), props) < 0 ||
		    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0 ||
		    !BIT(props, INPUT_PROP_DIRECT) || !BIT(abs, ABS_MT_POSITION_X) ||
		    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) < 0 ||
		    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) < 0) {
			close(fd);
			fd = -1;
			continue;
		}
		xmin = ax.minimum;
		xspan = ax.maximum > ax.minimum ? ax.maximum - ax.minimum : 1;
		ymin = ay.minimum;
		yspan = ay.maximum > ay.minimum ? ay.maximum - ay.minimum : 1;
	}
	closedir(d);
	return fd;
}

/* evdev sends only what changed, per slot: start from the kernel's slot
 * state, or a touch whose X or Y matches the slot's last one keeps a stale
 * coordinate (the show-taps spot landed beside the finger) */
static void sync_slots(int fd)
{
	struct { __u32 code; __s32 v[SLOTS]; } mt;
	struct input_absinfo cur;
	int i;

	if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &cur) == 0 && cur.value >= 0 && cur.value < SLOTS)
		slot = cur.value;
	mt.code = ABS_MT_TRACKING_ID;
	if (ioctl(fd, EVIOCGMTSLOTS(sizeof(mt)), &mt) == 0)
		for (i = 0; i < SLOTS; i++)
			f[i].active = mt.v[i] >= 0;
	mt.code = ABS_MT_POSITION_X;
	if (ioctl(fd, EVIOCGMTSLOTS(sizeof(mt)), &mt) == 0)
		for (i = 0; i < SLOTS; i++)
			f[i].x = mt.v[i];
	mt.code = ABS_MT_POSITION_Y;
	if (ioctl(fd, EVIOCGMTSLOTS(sizeof(mt)), &mt) == 0)
		for (i = 0; i < SLOTS; i++)
			f[i].y = mt.v[i];
}

static void frame(void)
{
	long t = now_ms();
	int n = 0, i;
	double cx = 0, cy = 0;

	for (i = 0; i < SLOTS; i++) {
		if (taps) {
			/* a tap shorter than a frame: both, so no spot is left behind */
			if (f[i].down)
				printf("d %d %.4f %.4f\n", i, nx(f[i].x), ny(f[i].y));
			if (f[i].up)
				printf("u %d\n", i);
			else if (!f[i].down && f[i].changed && f[i].active && t - f[i].last_ms >= 16) {
				printf("m %d %.4f %.4f\n", i, nx(f[i].x), ny(f[i].y));
				f[i].last_ms = t;
			}
		}
		f[i].down = f[i].up = f[i].changed = 0;
		if (f[i].active) {
			n++;
			cx += nx(f[i].x);
			cy += ny(f[i].y);
		}
	}
	if (n > 0) {
		cx /= n;
		cy /= n;
	}

	if (n > 3)
		cancelled = 1;
	if (!gesture && n == 3 && !cancelled) {
		gesture = 1;
		gx0 = gx = cx;
		gy0 = gy = cy;
	} else if (gesture && n == 3) {
		gx = cx;
		gy = cy;
	}
	if (n == 0) {
		if (gesture && !cancelled)
			printf("g3 %.4f %.4f\n", gx - gx0, gy - gy0);
		gesture = cancelled = 0;
	}
	fflush(stdout);
}

static void event(const struct input_event *ev)
{
	if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
		frame();
		return;
	}
	if (ev->type != EV_ABS)
		return;
	switch (ev->code) {
	case ABS_MT_SLOT:
		slot = ev->value >= 0 && ev->value < SLOTS ? ev->value : 0;
		break;
	case ABS_MT_TRACKING_ID:
		if (ev->value < 0) {
			if (f[slot].active || f[slot].down)
				f[slot].up = 1;
			f[slot].active = 0;
		} else {
			f[slot].active = f[slot].down = 1;
			f[slot].last_ms = 0;
		}
		break;
	case ABS_MT_POSITION_X:
		f[slot].x = ev->value;
		f[slot].changed = 1;
		break;
	case ABS_MT_POSITION_Y:
		f[slot].y = ev->value;
		f[slot].changed = 1;
		break;
	}
}

int main(void)
{
	struct pollfd p[2];
	struct input_event evs[64];
	char line[64];
	int fd = open_touchscreen();

	if (fd < 0) {
		fprintf(stderr, "cezanne-touch: no touchscreen\n");
		return 1;
	}
	sync_slots(fd);
	p[0] = (struct pollfd){ .fd = fd, .events = POLLIN };
	p[1] = (struct pollfd){ .fd = 0, .events = POLLIN };
	for (;;) {
		if (poll(p, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			return 1;
		}
		if (p[1].revents & (POLLIN | POLLHUP)) {
			ssize_t r = read(0, line, sizeof(line) - 1);
			if (r <= 0)
				return 0;	/* the shell went away */
			line[r] = 0;
			if (!strncmp(line, "taps ", 5))
				taps = line[5] == '1';
		}
		if (p[0].revents & (POLLERR | POLLHUP))
			return 1;
		if (p[0].revents & POLLIN) {
			ssize_t r = read(fd, evs, sizeof(evs));
			if (r < 0 && errno != EAGAIN)
				return 1;
			for (ssize_t i = 0; r > 0 && i < r / (ssize_t)sizeof(evs[0]); i++)
				event(&evs[i]);
		}
	}
}
