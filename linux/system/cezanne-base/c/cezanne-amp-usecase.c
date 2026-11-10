// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cezanne-amp-usecase [--wait SECONDS] music|handsfree: the CS35L41 protection
 * firmware's "fast use case" tuning deltas, as MIUI's audio_device.xml selects
 * them (media = spk-music / rcv-music, speakerphone = *-handsfree) and the
 * vendor driver applies them (cs35l41_do_fast_switch, GPL): the delta file is
 * a comma separated list of s32, the first one the word count; written big
 * endian into CSPL_UPDATE_PARAMS_CONFIG, then CSPL_COMMAND = UPDATE_PARAM (8)
 * and CSPL_STATE must read RUNNING (0) again. Needs the DSPs running (they
 * start with the first clocked stream since boot and are never stopped
 * again: the firmware ignores pause, system suspend keeps the amps powered),
 * so the parameters stay until the next boot. --wait: first wait (up to
 * SECONDS) until both DSP heartbeats count.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#define CARD			"hw:cezanne"
#define FW			"/usr/lib/firmware/cirrus"
#define CSPL_CMD_UPDATE_PARAM	8
#define CSPL_ST_RUNNING		0
#define PARAMS_SIZE		400	/* bytes of the UPDATE_PARAMS_CONFIG control */
#define NAME_MAX_LEN		43	/* ALSA control names end after 43 characters */

static const struct {
	const char *amp, *kind;
} amps[] = { { "Bottom", "spk" }, { "Top", "rcv" } };

static snd_ctl_t *ctl;

/* wm_adsp cuts the firmware's control name from the front (Bottom:
 * ...ATE_PARAMS_CONFIG) */
static void ctl_name(char *buf, const char *amp, const char *name)
{
	int pre = snprintf(buf, 64, "%s DSP1 Protection cd ", amp);
	int len = strlen(name), cut = pre + len - NAME_MAX_LEN;

	snprintf(buf + pre, 64 - pre, "%s", name + (cut > 0 ? cut : 0));
}

static void die(const char *what, const char *name, int err)
{
	fprintf(stderr, "%s %s: %s\n", what, name, snd_strerror(err));
	exit(1);
}

static void elem_id(snd_ctl_elem_id_t *id, const char *name)
{
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, name);
}

static void cset(const char *name, const unsigned char *data, size_t len)
{
	snd_ctl_elem_id_t *id;
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *val;
	int err;

	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_value_alloca(&val);
	elem_id(id, name);
	snd_ctl_elem_info_set_id(info, id);
	err = snd_ctl_elem_info(ctl, info);
	if (err < 0)
		die("no control", name, err);
	if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_BYTES ||
	    snd_ctl_elem_info_get_count(info) < len)
		die("not a bytes control of that size:", name, -EINVAL);
	snd_ctl_elem_value_set_id(val, id);
	snd_ctl_elem_set_bytes(val, (void *)data, len);
	err = snd_ctl_elem_write(ctl, val);
	if (err < 0)
		die("cannot write", name, err);
}

static uint32_t cget_u32(const char *name)
{
	snd_ctl_elem_id_t *id;
	snd_ctl_elem_value_t *val;
	const unsigned char *b;
	int err;

	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_value_alloca(&val);
	elem_id(id, name);
	snd_ctl_elem_value_set_id(val, id);
	err = snd_ctl_elem_read(ctl, val);
	if (err < 0)
		die("no value for", name, err);
	b = snd_ctl_elem_value_get_bytes(val);
	return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

static void put_be32(unsigned char *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static void sleep_s(double s)
{
	struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

static double mono(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int running(const char *amp)
{
	char hb[64];
	uint32_t a;

	snprintf(hb, sizeof(hb), "%s DSP1 Protection 400a4 HALO_HEARTBEAT", amp);
	a = cget_u32(hb);
	sleep_s(0.3);
	return cget_u32(hb) != a;
}

/* the delta file's words; exits on a malformed file */
static int read_delta(const char *kind, const char *mode, int32_t *w, int max)
{
	char path[256], *s = NULL, *p, *end;
	size_t cap = 0;
	ssize_t got;
	FILE *f;
	int n = 0;

	snprintf(path, sizeof(path), "%s/cs35l41-delta-%s-%s.txt", FW, kind, mode);
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		exit(1);
	}
	got = getdelim(&s, &cap, '\0', f);
	fclose(f);
	for (p = got > 0 ? s : ""; ; p = end + 1) {
		long v;

		errno = 0;
		v = strtol(p, &end, 10);
		while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
			end++;
		if (end == p || errno || v < INT32_MIN || v > INT32_MAX ||
		    (*end && *end != ',')) {
			fprintf(stderr, "%s-%s: bad delta file\n", kind, mode);
			exit(1);
		}
		if (n < max)
			w[n] = v;
		n++;
		if (!*end)
			break;
	}
	free(s);
	return n;
}

static int apply(const char *amp, const char *kind, const char *mode)
{
	int32_t words[PARAMS_SIZE / 4 + 1];
	unsigned char data[PARAMS_SIZE] = { 0 }, cmd[4];
	char name[64];
	int n = read_delta(kind, mode, words, PARAMS_SIZE / 4 + 1), i;

	if (words[0] != n || 4 * n > PARAMS_SIZE) {
		fprintf(stderr, "%s-%s: bad delta file (%d words, header %d)\n",
			kind, mode, n, words[0]);
		exit(1);
	}
	for (i = 0; i < n; i++)
		put_be32(data + 4 * i, words[i]);
	ctl_name(name, amp, "CSPL_UPDATE_PARAMS_CONFIG");
	cset(name, data, sizeof(data));
	put_be32(cmd, CSPL_CMD_UPDATE_PARAM);
	ctl_name(name, amp, "CSPL_COMMAND");
	cset(name, cmd, sizeof(cmd));
	ctl_name(name, amp, "CSPL_STATE");
	for (i = 0; i < 50; i++) {
		if (cget_u32(name) == CSPL_ST_RUNNING) {
			printf("%s: %s-%s applied\n", amp, kind, mode);
			fflush(stdout);
			return 1;
		}
		sleep_s(0.01);
	}
	fprintf(stderr, "%s: CSPL not running after %s-%s\n", amp, kind, mode);
	return 0;
}

int main(int argc, char **argv)
{
	double wait = 0, end;
	const char *mode = "";
	int i, ok = 1, err;

	argv++, argc--;
	if (argc > 1 && !strcmp(argv[0], "--wait")) {
		wait = strtod(argv[1], NULL);
		argv += 2, argc -= 2;
	}
	if (argc)
		mode = argv[0];
	if (strcmp(mode, "music") && strcmp(mode, "handsfree")) {
		fputs("usage: cezanne-amp-usecase [--wait SECONDS] music|handsfree\n", stderr);
		return 1;
	}
	err = snd_ctl_open(&ctl, CARD, 0);
	if (err < 0)
		die("cannot open", CARD, err);
	end = mono() + wait;
	for (;;) {
		for (i = 0; i < 2 && running(amps[i].amp); i++)
			;
		if (i == 2)
			break;
		if (mono() > end) {
			fputs("DSPs not running\n", stderr);
			return 1;
		}
		sleep_s(1);
	}
	for (i = 0; i < 2; i++)
		ok &= apply(amps[i].amp, amps[i].kind, mode);
	snd_ctl_close(ctl);
	return ok ? 0 : 1;
}
