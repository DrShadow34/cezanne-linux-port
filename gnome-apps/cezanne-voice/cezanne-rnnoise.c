// SPDX-License-Identifier: MIT
/*
 * cezanne-rnnoise: LADSPA mono noise suppression with RNNoise (Xiph /
 * Mozilla, the system librnnoise 0.2 and its built-in model) for PipeWire's
 * filter-chain: the microphone filter and the call chain (cezanne-voice).
 *
 * RNNoise works on 10 ms frames at 48 kHz (480 samples) and its output lags
 * its input by two more (measured: 1440 samples in all, 30 ms); the
 * "latency" output port says so. Off, the plugin passes the input through
 * with the same delay, so switching never disturbs a stream.
 *
 * On / off: the file $XDG_CONFIG_HOME/cezanne/noise-reduction ("0" = off;
 * missing = on), written by the Power app. Each instance has a thread that
 * watches it with inotify; the audio thread only reads an atomic flag.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ladspa.h>
#include <rnnoise.h>

#define FRAME		480
#define RNN_DELAY	2		/* frames rnnoise_process_frame() lags */
#define RATE		48000
#define STATE_NAME	"noise-reduction"

enum { PORT_IN, PORT_OUT, PORT_LATENCY, PORT_VAD, N_PORTS };

struct rnn {
	LADSPA_Data *port[N_PORTS];
	DenoiseState *st;		/* NULL: wrong rate, pass through */
	float in[FRAME], out[FRAME];
	float past[RNN_DELAY][FRAME];	/* the last input frames, for bypass */
	unsigned pos, next;
	float vad;
	atomic_int enabled;
	char dir[PATH_MAX];		/* the state file's directory */
	int stop;			/* eventfd: ends the watcher */
	pthread_t watcher;
	int watching;
};

static int read_enabled(const char *dir)
{
	char path[PATH_MAX + sizeof(STATE_NAME) + 1], buf[8] = "";
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, STATE_NAME);
	f = fopen(path, "r");
	if (!f)
		return 1;
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = '1';
	fclose(f);
	return buf[0] != '0';
}

static void *watch(void *arg)
{
	struct rnn *r = arg;
	char ev[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	int fd = inotify_init1(IN_CLOEXEC);

	if (fd < 0 || inotify_add_watch(fd, r->dir, IN_CLOSE_WRITE | IN_MOVED_TO |
					 IN_DELETE | IN_MOVED_FROM) < 0) {
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	atomic_store(&r->enabled, read_enabled(r->dir));	/* after the watch: no gap */
	for (;;) {
		struct pollfd p[2] = { { fd, POLLIN, 0 }, { r->stop, POLLIN, 0 } };

		if (poll(p, 2, -1) < 0 && errno != EINTR)
			break;
		if (p[1].revents)
			break;
		if (p[0].revents && read(fd, ev, sizeof(ev)) > 0)
			atomic_store(&r->enabled, read_enabled(r->dir));
	}
	close(fd);
	return NULL;
}

static void state_dir(char *dir, size_t n)
{
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");

	if (xdg && *xdg)
		snprintf(dir, n, "%s/cezanne", xdg);
	else
		snprintf(dir, n, "%s/.config/cezanne", home ? home : "/tmp");
}

static LADSPA_Handle instantiate(const LADSPA_Descriptor *d __attribute__((unused)), unsigned long rate)
{
	struct rnn *r = calloc(1, sizeof(*r));
	char parent[PATH_MAX];

	if (!r)
		return NULL;
	if (rate == RATE)
		r->st = rnnoise_create(NULL);
	else
		fprintf(stderr, "cezanne-rnnoise: %lu Hz, needs %d: passing through\n", rate, RATE);
	atomic_init(&r->enabled, 1);
	state_dir(r->dir, sizeof(r->dir));
	snprintf(parent, sizeof(parent), "%s", r->dir);
	*strrchr(parent, '/') = 0;
	mkdir(parent, 0700);
	mkdir(r->dir, 0700);
	r->stop = eventfd(0, EFD_CLOEXEC);
	if (r->stop >= 0)
		r->watching = !pthread_create(&r->watcher, NULL, watch, r);
	if (!r->watching)
		atomic_store(&r->enabled, read_enabled(r->dir));
	return r;
}

static void connect_port(LADSPA_Handle h, unsigned long port, LADSPA_Data *data)
{
	if (port < N_PORTS)
		((struct rnn *)h)->port[port] = data;
}

static void activate(LADSPA_Handle h)
{
	struct rnn *r = h;

	memset(r->in, 0, sizeof(r->in));
	memset(r->out, 0, sizeof(r->out));
	memset(r->past, 0, sizeof(r->past));
	r->pos = 0;
	r->next = 0;
	r->vad = 0;
}

static void run(LADSPA_Handle h, unsigned long n)
{
	struct rnn *r = h;
	const LADSPA_Data *in = r->port[PORT_IN];
	LADSPA_Data *out = r->port[PORT_OUT];
	unsigned long i;
	int k;

	for (i = 0; i < n; i++) {
		float x = in ? in[i] : 0;

		if (out)
			out[i] = r->out[r->pos];
		r->in[r->pos] = x;
		if (++r->pos < FRAME)
			continue;
		r->pos = 0;
		if (r->st && atomic_load_explicit(&r->enabled, memory_order_relaxed)) {
			/* RNNoise works in the 16-bit sample range */
			float x[FRAME];

			for (k = 0; k < FRAME; k++)
				x[k] = r->in[k] * 32768.0f;
			r->vad = rnnoise_process_frame(r->st, r->out, x);
			for (k = 0; k < FRAME; k++)
				r->out[k] *= 1.0f / 32768.0f;
		} else {
			/* the input RNN_DELAY frames ago: RNNoise's own lag */
			memcpy(r->out, r->past[r->next], sizeof(r->out));
			r->vad = -1;
		}
		memcpy(r->past[r->next], r->in, sizeof(r->in));
		r->next = (r->next + 1) % RNN_DELAY;
	}
	if (r->port[PORT_LATENCY])
		*r->port[PORT_LATENCY] = (1 + RNN_DELAY) * FRAME;
	if (r->port[PORT_VAD])
		*r->port[PORT_VAD] = r->vad;
}

static void cleanup(LADSPA_Handle h)
{
	struct rnn *r = h;
	uint64_t one = 1;

	if (r->watching) {
		if (write(r->stop, &one, sizeof(one)) < 0)
			pthread_cancel(r->watcher);
		pthread_join(r->watcher, NULL);
	}
	if (r->stop >= 0)
		close(r->stop);
	if (r->st)
		rnnoise_destroy(r->st);
	free(r);
}

static const LADSPA_PortDescriptor port_desc[N_PORTS] = {
	LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
	LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
	LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL,
	LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL,
};
static const char *const port_names[N_PORTS] = { "In", "Out", "latency", "VAD" };
static const LADSPA_PortRangeHint port_hints[N_PORTS] = {
	{ 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
	{ LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, -1, 1 },
};

static const LADSPA_Descriptor descriptor = {
	.UniqueID = 5061,
	.Label = "cezanne_rnnoise",
	.Properties = LADSPA_PROPERTY_HARD_RT_CAPABLE,
	.Name = "RNNoise noise suppression (mono, 48 kHz)",
	.Maker = "cezanne",
	.Copyright = "MIT",
	.PortCount = N_PORTS,
	.PortDescriptors = port_desc,
	.PortNames = port_names,
	.PortRangeHints = port_hints,
	.instantiate = instantiate,
	.connect_port = connect_port,
	.activate = activate,
	.run = run,
	.cleanup = cleanup,
};

const LADSPA_Descriptor *ladspa_descriptor(unsigned long index)
{
	return index == 0 ? &descriptor : NULL;
}
