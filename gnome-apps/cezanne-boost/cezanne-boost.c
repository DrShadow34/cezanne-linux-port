// SPDX-License-Identifier: MIT
/*
 * cezanne-boost: touch boost for the phone's shell. While the screen is
 * touched, and for a short time after the last touch event or a power /
 * volume key press, the threads of gnome-shell get a high uclamp.min: EAS
 * then places them on the big cores and schedutil clocks those up. After
 * that the clamp goes back to the default. Without it short UI bursts (a
 * popup opening) start on a slow little core and the first frames are late.
 *
 * cezanne-boost [-m util] [-t ms] [-p comm]... [-v]
 *   -m  uclamp.min while boosted, 1..1024 (default 768)
 *   -t  hold time after the last touch event / key press (default 1000)
 *   -p  boost the processes with this comm (repeatable; default gnome-shell)
 *   -v  log every boost / release
 *
 * Input: every /dev/input/event* with multitouch positions and BTN_TOUCH
 * (the touchscreen, also uinput test devices) or a power / volume key; new
 * devices are picked up through inotify. Realtime and deadline threads are
 * left alone (they already run at full capacity). Threads created while
 * boosted inherit the clamp; the release resets every thread it finds.
 * Needs CONFIG_UCLAMP_TASK and CAP_SYS_NICE.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/input.h>
#include <linux/sched.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVS	16
#define MAX_COMMS	8
#define MAX_PIDS	16
#define STUCK_MS	5000	/* finger "down" without events: release anyway */
#define PID_CACHE_MS	10000	/* rescan /proc for the target processes */
#define CLAMP_RESET	((uint32_t)-1)

#define BITS_LONG	(8 * sizeof(long))
#define NLONGS(n)	(((n) + BITS_LONG - 1) / BITS_LONG)
#define TEST_BIT(b, a)	(((a)[(b) / BITS_LONG] >> ((b) % BITS_LONG)) & 1)

/* struct sched_attr (uapi linux/sched/types.h), named apart from glibc's */
struct boost_attr {
	uint32_t size;
	uint32_t sched_policy;
	uint64_t sched_flags;
	int32_t sched_nice;
	uint32_t sched_priority;
	uint64_t sched_runtime;
	uint64_t sched_deadline;
	uint64_t sched_period;
	uint32_t sched_util_min;
	uint32_t sched_util_max;
};

enum kind { TOUCH, KEYS };

struct dev {
	int fd;
	char name[32];
	enum kind kind;
	bool down;
};

static struct dev devs[MAX_DEVS];
static int ndevs;
static const char *comms[MAX_COMMS];
static int ncomms;
static int pids[MAX_PIDS], npids;
static int64_t pids_at = -PID_CACHE_MS;
static uint32_t boost_min = 768;
static int hold_ms = 1000;
static bool verbose, boosted;
static int64_t boosted_at, last_event;

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int set_clamp(int tid, uint32_t min)
{
	struct boost_attr a = { .size = sizeof(a) };

	if (syscall(SYS_sched_getattr, tid, &a, sizeof(a), 0))
		return -errno;
	if (a.sched_policy == SCHED_FIFO || a.sched_policy == SCHED_RR ||
	    a.sched_policy == SCHED_DEADLINE)
		return 0;
	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.sched_flags = SCHED_FLAG_KEEP_ALL | SCHED_FLAG_UTIL_CLAMP_MIN;
	a.sched_util_min = min;
	if (syscall(SYS_sched_setattr, tid, &a, 0))
		return -errno;
	return 1;
}

static bool is_target(int pid)
{
	char path[64], comm[32];
	int fd, n, i;

	snprintf(path, sizeof(path), "/proc/%d/comm", pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	n = read(fd, comm, sizeof(comm) - 1);
	close(fd);
	if (n <= 0)
		return false;
	comm[n] = 0;
	comm[strcspn(comm, "\n")] = 0;
	for (i = 0; i < ncomms; i++)
		if (!strcmp(comm, comms[i]))
			return true;
	return false;
}

static void scan_pids(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;

	npids = 0;
	if (!d)
		return;
	while ((e = readdir(d)) && npids < MAX_PIDS) {
		int pid = atoi(e->d_name);

		if (pid > 0 && is_target(pid))
			pids[npids++] = pid;
	}
	closedir(d);
	pids_at = now_ms();
}

/* clamp every thread of the target processes; returns the thread count */
static int clamp_all(uint32_t min)
{
	char path[64];
	struct dirent *e;
	int i, n = 0;
	DIR *d;

	if (now_ms() - pids_at > PID_CACHE_MS)
		scan_pids();
	for (i = 0; i < npids; i++) {
		snprintf(path, sizeof(path), "/proc/%d/task", pids[i]);
		d = opendir(path);
		if (!d) {		/* gone: rescan on the next boost */
			pids_at = -PID_CACHE_MS;
			continue;
		}
		while ((e = readdir(d)))
			if (e->d_name[0] != '.' && set_clamp(atoi(e->d_name), min) > 0)
				n++;
		closedir(d);
	}
	return n;
}

static void boost(void)
{
	int n;

	if (boosted)
		return;
	boosted = true;
	boosted_at = now_ms();
	n = clamp_all(boost_min);
	if (verbose)
		fprintf(stderr, "boost: %d threads, %lld ms\n", n,
			(long long)(now_ms() - boosted_at));
}

static void release(void)
{
	int n;

	if (!boosted)
		return;
	boosted = false;
	n = clamp_all(CLAMP_RESET);
	if (verbose)
		fprintf(stderr, "release: %d threads after %lld ms\n", n,
			(long long)(now_ms() - boosted_at));
}

static void add_dev(const char *name)
{
	unsigned long ev[NLONGS(EV_CNT)] = { 0 }, abs[NLONGS(ABS_CNT)] = { 0 },
		      key[NLONGS(KEY_CNT)] = { 0 };
	char path[64];
	enum kind kind;
	int fd, i;

	if (strncmp(name, "event", 5) || ndevs == MAX_DEVS)
		return;
	for (i = 0; i < ndevs; i++)
		if (!strcmp(devs[i].name, name))
			return;
	snprintf(path, sizeof(path), "/dev/input/%s", name);
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
	if (TEST_BIT(EV_ABS, ev) && TEST_BIT(ABS_MT_POSITION_X, abs) &&
	    TEST_BIT(BTN_TOUCH, key)) {
		kind = TOUCH;
	} else if (TEST_BIT(EV_KEY, ev) && (TEST_BIT(KEY_POWER, key) ||
		   TEST_BIT(KEY_VOLUMEUP, key) || TEST_BIT(KEY_VOLUMEDOWN, key))) {
		kind = KEYS;
	} else {
		close(fd);
		return;
	}
	devs[ndevs] = (struct dev){ .fd = fd, .kind = kind };
	snprintf(devs[ndevs].name, sizeof(devs[ndevs].name), "%s", name);
	ndevs++;
	if (verbose)
		fprintf(stderr, "watching %s (%s)\n", path, kind == TOUCH ? "touch" : "keys");
}

static void drop_dev(int i)
{
	if (verbose)
		fprintf(stderr, "lost /dev/input/%s\n", devs[i].name);
	close(devs[i].fd);
	devs[i] = devs[--ndevs];
}

/* reads one device; true if it reported input that should boost */
static bool read_dev(int i)
{
	struct input_event ev[64];
	bool active = false;
	ssize_t n;
	int k;

	while ((n = read(devs[i].fd, ev, sizeof(ev))) > 0) {
		for (k = 0; k < n / (ssize_t)sizeof(ev[0]); k++) {
			if (devs[i].kind == TOUCH) {
				if (ev[k].type == EV_KEY && ev[k].code == BTN_TOUCH)
					devs[i].down = ev[k].value;
				if (ev[k].type != EV_SYN)
					active = true;
			} else if (ev[k].type == EV_KEY && (ev[k].code == KEY_POWER ||
				   ev[k].code == KEY_VOLUMEUP || ev[k].code == KEY_VOLUMEDOWN)) {
				active = true;
			}
		}
	}
	if (n < 0 && errno == ENODEV)
		drop_dev(i);
	return active;
}

static bool any_down(void)
{
	int i;

	for (i = 0; i < ndevs; i++)
		if (devs[i].down)
			return true;
	return false;
}

static void arm(int tfd)
{
	int64_t at = last_event + (any_down() ? STUCK_MS : hold_ms);
	struct itimerspec it = {
		.it_value = { .tv_sec = at / 1000, .tv_nsec = at % 1000 * 1000000 },
	};

	timerfd_settime(tfd, TFD_TIMER_ABSTIME, &it, NULL);
}

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s [-m util] [-t ms] [-p comm]... [-v]\n", argv0);
	exit(2);
}

int main(int argc, char **argv)
{
	struct pollfd pfd[3 + MAX_DEVS];
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	struct boost_attr probe = { .size = sizeof(probe) };
	struct inotify_event *ie;
	struct dirent *e;
	sigset_t sigs;
	int ifd, tfd, sfd, i, c, n;
	DIR *d;

	while ((c = getopt(argc, argv, "m:t:p:v")) != -1) {
		switch (c) {
		case 'm':
			boost_min = atoi(optarg);
			if (boost_min < 1 || boost_min > 1024)
				usage(argv[0]);
			break;
		case 't':
			hold_ms = atoi(optarg);
			break;
		case 'p':
			if (ncomms == MAX_COMMS)
				usage(argv[0]);
			comms[ncomms++] = optarg;
			break;
		case 'v':
			verbose = true;
			break;
		default:
			usage(argv[0]);
		}
	}
	if (!ncomms)
		comms[ncomms++] = "gnome-shell";

	/* a kernel without CONFIG_UCLAMP_TASK: nothing to do, no restart loop */
	probe.sched_flags = SCHED_FLAG_KEEP_ALL | SCHED_FLAG_UTIL_CLAMP_MIN;
	probe.sched_util_min = CLAMP_RESET;
	if (syscall(SYS_sched_setattr, 0, &probe, 0) && errno == EOPNOTSUPP) {
		fprintf(stderr, "kernel without utilization clamping (CONFIG_UCLAMP_TASK)\n");
		return 0;
	}

	sigemptyset(&sigs);
	sigaddset(&sigs, SIGTERM);
	sigaddset(&sigs, SIGINT);
	sigprocmask(SIG_BLOCK, &sigs, NULL);
	sfd = signalfd(-1, &sigs, SFD_CLOEXEC);
	tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (sfd < 0 || tfd < 0 || ifd < 0 ||
	    inotify_add_watch(ifd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
		perror("cezanne-boost");
		return 1;
	}
	d = opendir("/dev/input");
	while (d && (e = readdir(d)))
		add_dev(e->d_name);
	if (d)
		closedir(d);
	fprintf(stderr, "uclamp.min %u for %s, hold %d ms, %d input devices\n",
		boost_min, comms[0], hold_ms, ndevs);

	for (;;) {
		pfd[0] = (struct pollfd){ .fd = sfd, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = tfd, .events = POLLIN };
		pfd[2] = (struct pollfd){ .fd = ifd, .events = POLLIN };
		for (i = 0; i < ndevs; i++)
			pfd[3 + i] = (struct pollfd){ .fd = devs[i].fd, .events = POLLIN };
		n = ndevs;
		if (poll(pfd, 3 + n, -1) < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}
		if (pfd[0].revents)
			break;
		if (pfd[2].revents) {
			ssize_t len;

			while ((len = read(ifd, buf, sizeof(buf))) > 0)
				for (char *p = buf; p < buf + len; p += sizeof(*ie) + ie->len) {
					ie = (struct inotify_event *)p;
					if (ie->len)
						add_dev(ie->name);
				}
		}
		/* devices from the back: drop_dev() moves the last one into a hole */
		for (i = n - 1; i >= 0; i--) {
			if (!(pfd[3 + i].revents & (POLLIN | POLLERR | POLLHUP)))
				continue;
			if (read_dev(i)) {
				last_event = now_ms();
				boost();
				arm(tfd);
			}
		}
		if (pfd[1].revents) {
			uint64_t ticks;

			if (read(tfd, &ticks, sizeof(ticks)) < 0)
				ticks = 0;
			if (now_ms() - last_event >= STUCK_MS)
				for (i = 0; i < ndevs; i++)
					devs[i].down = false;
			if (any_down())
				arm(tfd);
			else
				release();
		}
	}
	release();
	return 0;
}
