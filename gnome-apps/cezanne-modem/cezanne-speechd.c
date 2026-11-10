// SPDX-License-Identifier: MIT
/*
 * cezanne-speechd: call audio for the cezanne modem. While ModemManager has
 * a call dialing or up, it switches the modem's speech path on over
 * /dev/ccci_aud (cezanne-ccci) and holds the sound card's hostless speech
 * stream open, which powers the AFE route modem PCM 2 <-> microphones /
 * earpiece (or the headset when the sound server has it selected).
 *
 * Speech control protocol (as MIUI's audio HAL speaks it, see NOTES.md
 * "Call audio"): 16-byte CCCI messages on channel 5 / 4. Mailbox: data0
 * 0xffffffff, u16 param, u16 message id, channel, u32 param. Payload: data0
 * 0, length, channel, u16 size + 6, u16 id, then u16 0xa2a2, u16 type, u16
 * size and the data (modem payloads: 0x1234, type, size, index, total).
 * The modem acks a message with its id | 0x8000.
 *
 *   cezanne-speechd [-v] [--loopback SECONDS [--no-stream]] [--alive-ack]
 *
 * --loopback: the modem returns the uplink on the downlink for a while (no
 * call); --no-stream leaves the PCMs to the caller (digital tests);
 * --alive-ack answers a modem "alive" that came before we listened.
 *
 * During a call it follows the desktop user's controls: callaudiod's speaker
 * and mic state (GNOME Calls' buttons) switch earpiece / speakerphone and the
 * modem's uplink mute; the default sink's volume (the volume keys) is the call
 * volume, kept apart from the media volume.
 *
 * With cezanne-voice installed the call's audio goes through the AP (SPH_AP=0:
 * the direct path): the AFE connects PCM 2 to the AP's DL2 (uplink) and
 * UL_MONO_1 (downlink) instead of the mics and amps, and the desktop user's
 * sound server runs cezanne-voice's call chain: mic -> echo cancelling ->
 * RNNoise -> modem, modem -> the default sink (the echo reference). The
 * volume then works in the sound server, the modem's DL gain stays 0 dB.
 * Without it (or if that chain does not come up) the mics and amps talk to
 * the modem directly and the sink's volume sets the DL gain.
 *
 * The C version of the former Python script: same messages, routes,
 * commands and log lines. Mixer controls go through alsa-lib's control API
 * (what amixer cget / cset did); the desktop user's tools (pactl, gdbus,
 * pw-cli, pw-link, pw-dump, pipewire) still run as that user's processes.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>

extern char **environ;

#define DEV		"/dev/ccci_aud"
#define STATE		"/sys/devices/platform/cezanne-ccci/state"
#define SHM_SIZE	0xd000
#define MAGIC		0xffffffffu
#define ACK		0x8000

#define MSG_DL_GAIN		0x2f00	/* param16: gain, 0 = 0 dB, 0x8000 = mute */
#define MSG_UL_GAIN		0x2f01
#define MSG_UL_MUTE		0x2f02
#define MSG_DL_MUTE		0x2f03
#define MSG_UL_SOURCE_MUTE	0x2f08
#define MSG_DL_MUTE_CODEC	0x2f09
#define MSG_DL_MUTE_DYNAMIC	0x2f0a
#define MSG_SPH_ENH		0x2f23	/* param16: speech enhancement on / off */
#define MSG_SPH_ON		0x2f20
#define MSG_SPH_OFF		0x2f21
#define MSG_SPH_INFO		0x2f7b	/* the info again, after the call setup */
#define MSG_MD_ALIVE_ACK	0x2fa0	/* answers the modem's "alive" (0xafa0) */
#define TYPE_SPH_INFO		0x19

static const struct {
	unsigned int msg;
	const char *name;
} md_names[] = {
	{ 0xaf70, "speech status" }, { 0xaf75, "RAT info" }, { 0xaf78, "EPOF" },
	{ 0xafa0, "MD alive" },
};

/*
 * 1 = MIUI's info (its audio DSP does the speech processing): the downlink
 * then comes through too, but hot (clipping); 0 = the modem processes it
 */
static long opendsp, ul_gain, enhance;
#define PCM	"hw:0,22"	/* Hostless_Speech */
#define RATE	48000
/*
 * mics -> modem; modem -> amplifiers (I2S3) or, with the headset in use (the
 * sound server's Headphones device sets HP Mux), the MT6359 headphone path
 */
static const char *const ul_routes[] = { "PCM_2_PB_CH1 ADDA_UL_CH1", "PCM_2_PB_CH2 ADDA_UL_CH2", NULL };
static const char *const amp_routes[] = { "I2S3_CH1 PCM_2_CAP_CH1", "I2S3_CH2 PCM_2_CAP_CH1", NULL };
static const char *const hp_routes[] = { "ADDA_DL_CH1 PCM_2_CAP_CH1", "ADDA_DL_CH2 PCM_2_CAP_CH1", NULL };
/*
 * the UCM Earpiece device (top amplifier alone, ASP, amp gain 0); the
 * sound server's own settings come back after the call
 */
static const char *const earpiece[][2] = {
	{ "Speakers Switch", "off" }, { "Earpiece Switch", "on" },
	{ "Top PCM Source", "ASP" }, { "Top Analog PCM Volume", "0" },
	{ "Top DSP RX2 Source", "ASPRX1" }, { NULL, NULL },
};
/*
 * speakerphone: both amplifiers, as MIUI's voice speaker path (the same
 * settings as the UCM Speaker2 device: the top DSP takes both inputs from
 * its own slot), with MIUI's handsfree tuning deltas while it is on
 */
static const char *const speakers[][2] = {
	{ "Speakers Switch", "on" }, { "Earpiece Switch", "on" },
	{ "Top PCM Source", "DSP" }, { "Top Analog PCM Volume", "17" },
	{ "Top DSP RX2 Source", "ASPRX1" }, { NULL, NULL },
};
#define AMP_USECASE	"/usr/local/bin/cezanne-amp-usecase"
/*
 * call volume (percent of the default sink) -> DL gain in 0.25 dB as the
 * sink's own cubic volume curve, never muted; kept between calls
 */
static char *call_volume_path;
#define CALL_VOLUME_DEFAULT	80
#define DL_GAIN_MIN	(-40 * 4)
#define DL_GAIN_MAX	(6 * 4)
#define CALLAUDIO_NAME	"org.mobian_project.CallAudio"
#define CALLAUDIO_PATH	"/org/mobian_project/CallAudio"
/*
 * the AP path: uplink from DL2 (Playback_2), downlink to UL_MONO_1
 * (Capture_Mono_1); the two PCMs become nodes in the user's sound server
 * (name, description, factory, ALSA device, channels) for call.conf.in
 */
#define CALL_CONF	"/usr/share/cezanne-voice/call.conf.in"
static gboolean ap_path;
static const char *const ap_routes[] = { "PCM_2_PB_CH1 DL2_CH1", "PCM_2_PB_CH2 DL2_CH2", "UL_MONO_1_CH1 PCM_2_CAP_CH1", NULL };
static const struct node {
	const char *name, *desc, *factory, *path;
	int channels;
} uplink = { "cezanne_modem.uplink", "Modem uplink", "api.alsa.pcm.sink", "hw:0,2", 2 },
  downlink = { "cezanne_modem.downlink", "Modem downlink", "api.alsa.pcm.source", "hw:0,18", 1 };

#define MM		"org.freedesktop.ModemManager1"
#define CALL_IFACE	MM ".Call"
#define VOICE_IFACE	MM ".Modem.Voice"

/* MMCallState: dialing, ringing out, active, held */
static gboolean speech_state(int s)
{
	return s == 1 || s == 2 || s == 4 || s == 5;
}

static gboolean verbose;

static void log_msg(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void log_msg(const char *fmt, ...)
{
	struct timespec ts;
	va_list ap;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	printf("%.1f ", ts.tv_sec + ts.tv_nsec / 1e9);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

static double mono(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Python's repr() of a str and of bytes, for the log lines */
static void repr_add(GString *s, const char *p, size_t n, gboolean bytes)
{
	gboolean sq = memchr(p, '\'', n) != NULL, dq = memchr(p, '"', n) != NULL;
	char q = sq && !dq ? '"' : '\'';
	size_t i;

	if (bytes)
		g_string_append_c(s, 'b');
	g_string_append_c(s, q);
	for (i = 0; i < n; i++) {
		unsigned char c = p[i];

		if (c == q || c == '\\')
			g_string_append_printf(s, "\\%c", c);
		else if (c == '\t')
			g_string_append(s, "\\t");
		else if (c == '\n')
			g_string_append(s, "\\n");
		else if (c == '\r')
			g_string_append(s, "\\r");
		else if (c < 0x20 || c == 0x7f || (bytes && c > 0x7f))
			g_string_append_printf(s, "\\x%02x", c);
		else
			g_string_append_c(s, c);
	}
	g_string_append_c(s, q);
}

/* str(list) of strings */
static void repr_strv(GString *s, char **v)
{
	int i;

	g_string_append_c(s, '[');
	for (i = 0; v && v[i]; i++) {
		if (i)
			g_string_append(s, ", ");
		repr_add(s, v[i], strlen(v[i]), FALSE);
	}
	g_string_append_c(s, ']');
}

/* OSError's text for errno and an optional file name */
static char *os_error(int e, const char *path)
{
	if (path)
		return g_strdup_printf("[Errno %d] %s: '%s'", e, g_strerror(e), path);
	return g_strdup_printf("[Errno %d] %s", e, g_strerror(e));
}

/* ---- the speech channel ------------------------------------------------ */

/*
 * sph_info_t (128 bytes) as MIUI sends it for a handset call; app 0 phone
 * call, 2 loopback
 */
static void sph_info(uint8_t b[128], int app)
{
	static const uint16_t common[5] = { 6, 0xdabd, 0x7918, 0x2a00, 0x8001 };
	int i;

	memset(b, 0, 128);
	b[0] = app;
	b[1] = 0;		/* no Bluetooth */
	b[2] = 3;		/* 48 kHz */
	b[3] = opendsp;		/* enhancement in the audio DSP (MIUI) instead of the modem */
	b[0x12] = 3;		/* dynamic enhancement control */
	/* common parameters (MIUI's SpeechGeneral values), debug info */
	for (i = 0; i < 5; i++) {
		b[0x1c + 2 * i] = common[i] & 0xff;
		b[0x1d + 2 * i] = common[i] >> 8;
	}
	b[0x34] = 3;
	b[0x54] = 2;		/* two smart amplifiers */
	if (app == 2) {
		b[0xc] = 2;	/* loopback without the speech codec */
		b[0xd] = 0;	/* delay frames */
	}
}

/* a message from the modem: id, param16 (or the payload type), param32 or data */
struct md_msg {
	unsigned int msg, a;
	uint32_t p32;
	gboolean payload;
	uint8_t data[4096];
	size_t size;
};

struct modem {
	int fd;
	uint8_t *shm;
};

static void put16(uint8_t *p, unsigned int v)
{
	p[0] = v;
	p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, v & 0xffff);
	put16(p + 2, v >> 16);
}

static unsigned int get16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

static uint32_t get32(const uint8_t *p)
{
	return get16(p) | (uint32_t)get16(p + 2) << 16;
}

/*
 * sph_shm_t: guards, AP / MD flags, speech parameter, AP data and MD data
 * regions (offset, size, read / write index), checksum field
 */
static void format_shm(uint8_t *s)
{
	static const uint32_t regions[3][2] = {
		{ 0x80, 0x3000 }, { 0x3080, 0x2000 }, { 0x5080, 0x7f60 },
	};
	int i;

	memset(s, 0, SHM_SIZE);
	memset(s, '\n', 0x20);
	put32(s + 0x20, 1);
	put32(s + 0x24, 0);
	for (i = 0; i < 3; i++) {
		put32(s + 0x28 + 0x10 * i, regions[i][0]);
		put32(s + 0x2c + 0x10 * i, regions[i][1]);
		put32(s + 0x30 + 0x10 * i, 0);
		put32(s + 0x34 + 0x10 * i, 0);
	}
	put32(s + 0x7c, 0x7c);
	memset(s + 0xcfe0, '\n', 0x20);
}

static struct modem *modem_new(char **err)
{
	struct modem *md = g_new0(struct modem, 1);

	md->fd = open(DEV, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (md->fd < 0) {
		*err = os_error(errno, DEV);
		g_free(md);
		return NULL;
	}
	md->shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, md->fd, 0);
	if (md->shm == MAP_FAILED) {
		*err = os_error(errno, NULL);
		close(md->fd);
		g_free(md);
		return NULL;
	}
	format_shm(md->shm);
	return md;
}

static gboolean md_send(struct modem *md, const uint8_t *pkt, size_t n)
{
	if (write(md->fd, pkt, n) < 0) {
		char *e = os_error(errno, NULL);

		log_msg("speech channel write: %s", e);
		g_free(e);
		return FALSE;
	}
	return TRUE;
}

static size_t mailbox_pkt(uint8_t pkt[16], unsigned int msg, unsigned int p16, uint32_t p32)
{
	put32(pkt, MAGIC);
	put16(pkt + 4, p16);
	put16(pkt + 6, msg);
	put32(pkt + 8, 0);
	put32(pkt + 12, p32);
	return 16;
}

static size_t payload_pkt(uint8_t *pkt, unsigned int msg, unsigned int dtype,
			  const uint8_t *data, size_t n)
{
	put32(pkt, 0);
	put32(pkt + 4, 0);
	put32(pkt + 8, 0);
	put16(pkt + 12, n + 6);
	put16(pkt + 14, msg);
	put16(pkt + 16, 0xa2a2);
	put16(pkt + 18, dtype);
	put16(pkt + 20, n);
	memcpy(pkt + 22, data, n);
	return 22 + n;
}

static gboolean md_mailbox(struct modem *md, unsigned int msg, unsigned int p16, uint32_t p32)
{
	uint8_t pkt[16];

	return md_send(md, pkt, mailbox_pkt(pkt, msg, p16, p32));
}

/* one message from the modem, FALSE if none */
static gboolean md_receive(struct modem *md, struct md_msg *m)
{
	uint8_t pkt[4096];
	ssize_t n = read(md->fd, pkt, sizeof(pkt));

	if (n < 16)
		return FALSE;
	if (get32(pkt) == MAGIC) {
		m->payload = FALSE;
		m->a = get16(pkt + 4);
		m->msg = get16(pkt + 6);
		m->p32 = get32(pkt + 12);
		m->size = 0;
		return TRUE;
	}
	if (n < 22)
		return FALSE;
	m->payload = TRUE;
	m->msg = get32(pkt + 12) >> 16;
	m->a = get16(pkt + 18);
	m->size = get16(pkt + 20);
	if (n <= 26)
		m->size = 0;
	else if (m->size > (size_t)(n - 26))
		m->size = n - 26;
	memcpy(m->data, pkt + 26, m->size);
	return TRUE;
}

/* a modem message outside a request (acks share the 0xafxx ids) */
static void md_handle(struct modem *md, const struct md_msg *m)
{
	const char *name = NULL;
	unsigned int i;

	for (i = 0; i < G_N_ELEMENTS(md_names); i++)
		if (md_names[i].msg == m->msg)
			name = md_names[i].name;
	if (name)
		log_msg("modem: %s (0x%x)", name, m->msg);
	if (m->msg == 0xafa0) {
		uint8_t pkt[16];

		if (write(md->fd, pkt, mailbox_pkt(pkt, MSG_MD_ALIVE_ACK, 0, 0)) < 0) {
			char *e = os_error(errno, NULL);

			log_msg("alive ack: %s", e);
			g_free(e);
		}
	} else if (verbose) {
		GString *b = g_string_new(NULL);

		if (m->payload)
			repr_add(b, (const char *)m->data, m->size, TRUE);
		else
			g_string_append_printf(b, "%u", m->p32);
		log_msg("modem: 0x%x %u %s", m->msg, m->a, b->str);
		g_string_free(b, TRUE);
	}
}

/* send, then process modem messages until msg's ack (or a second) */
static gboolean md_request(struct modem *md, unsigned int msg, const uint8_t *pkt, size_t n)
{
	double end;

	md_send(md, pkt, n);
	end = mono() + 1.0;
	for (;;) {
		double left = end - mono();
		struct pollfd p = { md->fd, POLLIN, 0 };
		struct md_msg m;

		if (left <= 0) {
			log_msg("no ack for 0x%x", msg);
			return FALSE;
		}
		poll(&p, 1, (int)ceil(left * 1000));
		if (!md_receive(md, &m))
			continue;
		if (m.msg == (msg | ACK))
			return TRUE;
		md_handle(md, &m);
	}
}

static gboolean md_request_mailbox(struct modem *md, unsigned int msg, unsigned int p16)
{
	uint8_t pkt[16];

	return md_request(md, msg, pkt, mailbox_pkt(pkt, msg, p16, 0));
}

static gboolean md_request_info(struct modem *md, unsigned int msg, const uint8_t info[128])
{
	uint8_t pkt[22 + 128];

	return md_request(md, msg, pkt, payload_pkt(pkt, msg, TYPE_SPH_INFO, info, 128));
}

/* ---- the hostless speech stream ---------------------------------------- */

/*
 * opened and started, it powers the path; no data moves (stop threshold =
 * boundary, so playback may run empty)
 */
static snd_pcm_t *pcms[2];
static int npcms;

static gboolean hostless_open(snd_pcm_stream_t stream, char **err)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t boundary;
	snd_pcm_t *pcm;
	int ret;

	if (snd_pcm_open(&pcm, PCM, stream, 0) < 0) {
		*err = g_strdup("snd_pcm_open " PCM);
		return FALSE;
	}
	snd_pcm_hw_params_malloc(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
	snd_pcm_hw_params_set_channels(pcm, hw, 2);
	snd_pcm_hw_params_set_rate(pcm, hw, RATE, 0);
	ret = snd_pcm_hw_params(pcm, hw);
	snd_pcm_hw_params_free(hw);
	if (ret < 0) {
		*err = g_strdup_printf("hw_params %d", ret);
		snd_pcm_close(pcm);
		return FALSE;
	}
	snd_pcm_sw_params_malloc(&sw);
	snd_pcm_sw_params_current(pcm, sw);
	snd_pcm_sw_params_get_boundary(sw, &boundary);
	snd_pcm_sw_params_set_stop_threshold(pcm, sw, boundary);
	snd_pcm_sw_params(pcm, sw);
	snd_pcm_sw_params_free(sw);
	if (snd_pcm_prepare(pcm) < 0 || snd_pcm_start(pcm) < 0) {
		*err = g_strdup("start");
		snd_pcm_close(pcm);
		return FALSE;
	}
	pcms[npcms++] = pcm;
	return TRUE;
}

static gboolean hostless_start(char **err)
{
	/* playback: powers modem DL -> amps, mics -> modem UL */
	if (!hostless_open(SND_PCM_STREAM_PLAYBACK, err))
		return FALSE;
	/* capture: modem PCM 2 and ADDA capture */
	return hostless_open(SND_PCM_STREAM_CAPTURE, err);
}

static void hostless_stop(void)
{
	while (npcms)
		snd_pcm_close(pcms[--npcms]);
}

/* ---- mixer controls (amixer -c0 cget / cset) ----------------------------- */

static snd_ctl_t *ctl_open(void)
{
	snd_ctl_t *ctl;

	if (snd_ctl_open(&ctl, "hw:0", 0) < 0)
		return NULL;
	return ctl;
}

/* the control's info, NULL if there is none of that name */
static snd_ctl_elem_info_t *ctl_info(snd_ctl_t *ctl, const char *name)
{
	snd_ctl_elem_info_t *info;

	snd_ctl_elem_info_malloc(&info);
	snd_ctl_elem_info_set_interface(info, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_info_set_name(info, name);
	if (snd_ctl_elem_info(ctl, info) < 0) {
		snd_ctl_elem_info_free(info);
		return NULL;
	}
	return info;
}

/* a control's raw values as amixer prints them ('1', 'on,on', ...) */
static char *cget(const char *name)
{
	snd_ctl_t *ctl = ctl_open();
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *val;
	snd_ctl_elem_id_t *id;
	GString *s;
	unsigned int i, count;

	if (!ctl)
		return NULL;
	info = ctl_info(ctl, name);
	if (!info) {
		snd_ctl_close(ctl);
		return NULL;
	}
	snd_ctl_elem_id_malloc(&id);
	snd_ctl_elem_info_get_id(info, id);
	snd_ctl_elem_value_malloc(&val);
	snd_ctl_elem_value_set_id(val, id);
	snd_ctl_elem_id_free(id);
	if (snd_ctl_elem_read(ctl, val) < 0) {
		snd_ctl_elem_value_free(val);
		snd_ctl_elem_info_free(info);
		snd_ctl_close(ctl);
		return NULL;
	}
	s = g_string_new(NULL);
	count = snd_ctl_elem_info_get_count(info);
	for (i = 0; i < count; i++) {
		if (i)
			g_string_append_c(s, ',');
		switch (snd_ctl_elem_info_get_type(info)) {
		case SND_CTL_ELEM_TYPE_BOOLEAN:
			g_string_append(s, snd_ctl_elem_value_get_boolean(val, i) ? "on" : "off");
			break;
		case SND_CTL_ELEM_TYPE_INTEGER:
			g_string_append_printf(s, "%li", snd_ctl_elem_value_get_integer(val, i));
			break;
		case SND_CTL_ELEM_TYPE_INTEGER64:
			g_string_append_printf(s, "%lli", snd_ctl_elem_value_get_integer64(val, i));
			break;
		case SND_CTL_ELEM_TYPE_ENUMERATED:
			g_string_append_printf(s, "%u", snd_ctl_elem_value_get_enumerated(val, i));
			break;
		case SND_CTL_ELEM_TYPE_BYTES:
			g_string_append_printf(s, "0x%02x", snd_ctl_elem_value_get_byte(val, i));
			break;
		default:
			g_string_append(s, "?");
		}
	}
	snd_ctl_elem_value_free(val);
	snd_ctl_elem_info_free(info);
	snd_ctl_close(ctl);
	return g_string_free(s, FALSE);
}

/* as amixer cset: the value parsed against the control (names, numbers, on/off) */
static void cset(const char *name, const char *value)
{
	snd_ctl_t *ctl = ctl_open();
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *val;
	snd_ctl_elem_id_t *id;
	int ret;

	if (!ctl) {
		fprintf(stderr, "cset %s: no card\n", name);
		return;
	}
	info = ctl_info(ctl, name);
	if (!info) {
		fprintf(stderr, "cset %s: no such control\n", name);
		snd_ctl_close(ctl);
		return;
	}
	snd_ctl_elem_id_malloc(&id);
	snd_ctl_elem_info_get_id(info, id);
	snd_ctl_elem_value_malloc(&val);
	snd_ctl_elem_value_set_id(val, id);
	ret = snd_ctl_elem_read(ctl, val);
	if (ret >= 0)
		ret = snd_ctl_ascii_value_parse(ctl, val, info, value);
	if (ret >= 0)
		ret = snd_ctl_elem_write(ctl, val);
	if (ret < 0)
		fprintf(stderr, "cset %s %s: %s\n", name, value, snd_strerror(ret));
	snd_ctl_elem_value_free(val);
	snd_ctl_elem_id_free(id);
	snd_ctl_elem_info_free(info);
	snd_ctl_close(ctl);
}

/* ---- commands ---------------------------------------------------------- */

struct child {
	pid_t pid;
	int out;		/* stdout pipe, -1 if none */
	guint watch;
	GString *buf;
	void (*line)(const char *line);
};

/*
 * starts argv; out / err: pipes for its stdout / stderr (else /dev/null).
 * The error text as Python's OSError, or NULL.
 */
static pid_t spawn(char **argv, int *out, int *err, char **error)
{
	posix_spawn_file_actions_t fa;
	int po[2] = { -1, -1 }, pe[2] = { -1, -1 };
	pid_t pid;
	int ret;

	if ((out && pipe2(po, O_CLOEXEC)) || (err && pipe2(pe, O_CLOEXEC))) {
		*error = os_error(errno, NULL);
		return -1;
	}
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	if (out)
		posix_spawn_file_actions_adddup2(&fa, po[1], 1);
	else
		posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
	if (err)
		posix_spawn_file_actions_adddup2(&fa, pe[1], 2);
	else
		posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	ret = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	if (out)
		close(po[1]);
	if (err)
		close(pe[1]);
	if (ret) {
		*error = os_error(ret, argv[0]);
		if (out)
			close(po[0]);
		if (err)
			close(pe[0]);
		return -1;
	}
	if (out)
		*out = po[0];
	if (err)
		*err = pe[0];
	return pid;
}

/*
 * argv's output and exit status (subprocess.run with capture_output);
 * timeout_ms 0: none. FALSE with the error text if it did not start or ran
 * too long (then killed).
 */
static gboolean run_argv(char **argv, int timeout_ms, GString *out, GString *err,
			 int *status, char **error)
{
	int fo, fe, open = 2, st;
	double end = mono() + timeout_ms / 1000.0;
	pid_t pid = spawn(argv, &fo, &fe, error);

	if (pid < 0)
		return FALSE;
	while (open) {
		struct pollfd p[2] = { { fo, POLLIN, 0 }, { fe, POLLIN, 0 } };
		int wait = -1, i;

		if (timeout_ms) {
			double left = end - mono();

			if (left <= 0) {
				GString *s = g_string_new(NULL);

				kill(pid, SIGKILL);
				waitpid(pid, NULL, 0);
				repr_strv(s, argv);
				*error = g_strdup_printf("Command '%s' timed out after %d seconds",
							 s->str, timeout_ms / 1000);
				g_string_free(s, TRUE);
				if (fo >= 0)
					close(fo);
				if (fe >= 0)
					close(fe);
				return FALSE;
			}
			wait = (int)ceil(left * 1000);
		}
		if (poll(p, 2, wait) < 0 && errno != EINTR)
			break;
		for (i = 0; i < 2; i++) {
			char b[4096];
			ssize_t n;

			if (p[i].fd < 0 || !(p[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			n = read(p[i].fd, b, sizeof(b));
			if (n > 0) {
				GString *dst = i ? err : out;

				if (dst)
					g_string_append_len(dst, b, n);
			} else if (n == 0 || errno != EINTR) {
				close(p[i].fd);
				if (i)
					fe = -1;
				else
					fo = -1;
				open--;
			}
		}
	}
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	if (status)
		*status = WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st);
	return TRUE;
}

/* the amps' tuning deltas (music / handsfree) */
static void usecase(const char *mode)
{
	char *argv[] = { (char *)AMP_USECASE, (char *)mode, NULL };
	GString *out = g_string_new(NULL), *err = g_string_new(NULL);
	char *error = NULL;
	int status = 0;

	if (!run_argv(argv, 0, out, err, &status, &error)) {
		/* Python: the OSError ended the daemon; log it instead */
		log_msg("amp tuning %s: %s", mode, error);
		g_free(error);
	} else if (status) {
		GString *t = err->len ? err : out;

		log_msg("amp tuning %s: %s", mode, g_strstrip(t->str));
	}
	g_string_free(out, TRUE);
	g_string_free(err, TRUE);
}

/* ---- routes ------------------------------------------------------------ */

/* the AFE interconnects and output device for a call */
static struct {
	const char *const *on[2];	/* route lists switched on */
	GPtrArray *saved;		/* name, value pairs */
	gboolean headset, speaker;
} routes;

static void routes_switch(const char *value)
{
	int i, j;

	for (i = 0; i < 2; i++)
		for (j = 0; routes.on[i] && routes.on[i][j]; j++)
			cset(routes.on[i][j], value);
}

/* earpiece or speakerphone (the headset stays) */
static void routes_output(gboolean speaker, gboolean force)
{
	const char *const (*set)[2] = speaker ? speakers : earpiece;
	int i;

	if (!routes.on[0] || routes.headset || (speaker == routes.speaker && !force))
		return;
	for (i = 0; set[i][0]; i++)
		cset(set[i][0], set[i][1]);
	if (speaker != routes.speaker)
		usecase(speaker ? "handsfree" : "music");
	routes.speaker = speaker;
	log_msg("call audio to %s", speaker ? "speakers" : "earpiece");
}

/*
 * ap: PCM 2 to the AP's memories, the downlink then plays through the
 * default sink (the amps' earpiece / speaker settings as below)
 */
static void routes_start(gboolean speaker, gboolean ap)
{
	char *hp = cget("HP Mux");
	int i;

	routes.headset = hp && strcmp(hp, "0");
	g_free(hp);
	routes.on[0] = ap ? ap_routes : ul_routes;
	routes.on[1] = ap ? NULL : routes.headset ? hp_routes : amp_routes;
	routes_switch("on");
	if (!routes.saved)
		routes.saved = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_set_size(routes.saved, 0);
	if (routes.headset) {
		log_msg("call audio to headset");
		return;
	}
	for (i = 0; earpiece[i][0]; i++) {
		char *old = cget(earpiece[i][0]);

		if (old) {
			g_ptr_array_add(routes.saved, g_strdup(earpiece[i][0]));
			g_ptr_array_add(routes.saved, old);
		}
	}
	routes_output(speaker, TRUE);
}

/* from the AP path to the direct one (the call chain did not come up) */
static void routes_direct(void)
{
	routes_switch("off");
	routes.on[0] = ul_routes;
	routes.on[1] = routes.headset ? hp_routes : amp_routes;
	routes_switch("on");
}

static void routes_stop(void)
{
	int i;

	routes_switch("off");
	for (i = (int)(routes.saved ? routes.saved->len : 0) - 2; i >= 0; i -= 2)
		cset(routes.saved->pdata[i], routes.saved->pdata[i + 1]);
	if (routes.speaker)
		usecase("music");
	routes.on[0] = routes.on[1] = NULL;
	if (routes.saved)
		g_ptr_array_set_size(routes.saved, 0);
	routes.speaker = FALSE;
}

/* ---- speech state ------------------------------------------------------ */

static struct {
	struct modem *modem;
	guint modem_watch;
	gboolean on;
	int dl;			/* DL gain */
	gboolean muted, speaker_on;
	gboolean ap;		/* the call runs through the AP (cezanne-voice) */
	gboolean have_session;
} speech;

static int dl_gain(int percent)
{
	if (percent <= 0)
		return DL_GAIN_MIN;
	return CLAMP((int)rint(240 * log10(percent / 100.0)), DL_GAIN_MIN, DL_GAIN_MAX);
}

/*
 * the DL gain for the sink's volume; the AP path keeps 0 dB, the sink's
 * volume works on the downlink itself
 */
static void speech_volume(int gain)
{
	if (speech.ap)
		return;
	if (gain != speech.dl && speech.on && speech.modem) {
		md_mailbox(speech.modem, MSG_DL_GAIN, gain & 0xffff, 0);
		log_msg("DL gain %.2f dB", gain / 4.0);
	}
	speech.dl = gain;
}

/* the user's call controls; before speech on they only set what it starts with */
static void speech_speaker(gboolean on)
{
	speech.speaker_on = on;
	routes_output(on, FALSE);
}

static void speech_mute(gboolean muted)
{
	if (muted != speech.muted && speech.on && speech.modem) {
		md_request_mailbox(speech.modem, MSG_UL_MUTE, muted);
		log_msg("mic %s", muted ? "muted" : "on");
	}
	speech.muted = muted;
}

/* ---- the desktop user's session ---------------------------------------- */

/*
 * the desktop user's call controls, while speech is on: callaudiod's state
 * (gdbus monitor) and the default sink's volume (pactl subscribe), both run
 * as the user of the graphical session
 */
static struct {
	GPtrArray *cmd;		/* setpriv ... env ... prefix, NULL: no user */
	GPtrArray *procs;	/* struct child */
	char *media_sink;	/* the volume to restore after the call */
	char **media;
	int volume;		/* -1: none */
	guint check;
	pid_t graph;		/* the AP path's call chain (its process) */
	char *rundir;		/* the user's XDG_RUNTIME_DIR */
} session = { .volume = -1 };

static int cmp_str(gconstpointer a, gconstpointer b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static GPtrArray *session_user(void)
{
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free), *cmd = NULL;
	DIR *d = opendir("/run/user");
	struct dirent *e;
	unsigned int i;

	while (d && (e = readdir(d)))
		g_ptr_array_add(names, g_strdup(e->d_name));
	if (d)
		closedir(d);
	g_ptr_array_sort(names, cmp_str);
	for (i = 0; i < names->len && !cmd; i++) {
		const char *n = names->pdata[i];
		char *run, *bus;
		struct passwd *pw;
		gboolean digits = *n != 0;
		const char *c;

		for (c = n; *c; c++)
			digits &= g_ascii_isdigit(*c);
		if (!digits || atol(n) < 1000)
			continue;
		run = g_strconcat("/run/user/", n, NULL);
		bus = g_strconcat(run, "/bus", NULL);
		pw = access(bus, F_OK) ? NULL : getpwuid(atol(n));
		if (pw) {
			g_free(session.rundir);
			session.rundir = g_strdup(run);
			cmd = g_ptr_array_new_with_free_func(g_free);
			g_ptr_array_add(cmd, g_strdup("setpriv"));
			g_ptr_array_add(cmd, g_strdup_printf("--reuid=%u", pw->pw_uid));
			g_ptr_array_add(cmd, g_strdup_printf("--regid=%u", pw->pw_gid));
			g_ptr_array_add(cmd, g_strdup("--clear-groups"));
			g_ptr_array_add(cmd, g_strdup("env"));
			g_ptr_array_add(cmd, g_strconcat("HOME=", pw->pw_dir, NULL));
			g_ptr_array_add(cmd, g_strconcat("XDG_RUNTIME_DIR=", run, NULL));
			g_ptr_array_add(cmd, g_strdup_printf("DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus", run));
		}
		g_free(run);
		g_free(bus);
	}
	g_ptr_array_free(names, TRUE);
	return cmd;
}

/* the user's command line: the prefix, then args */
static char **user_argv_strv(char *const *args)
{
	GPtrArray *v = g_ptr_array_new();
	unsigned int i;

	for (i = 0; i < session.cmd->len; i++)
		g_ptr_array_add(v, g_strdup(session.cmd->pdata[i]));
	for (i = 0; args[i]; i++)
		g_ptr_array_add(v, g_strdup(args[i]));
	g_ptr_array_add(v, NULL);
	return (char **)g_ptr_array_free(v, FALSE);
}

/* the same with args up to NULL */
static char **user_argv(const char *first, va_list ap)
{
	GPtrArray *v = g_ptr_array_new();
	const char *a;
	char **argv;

	for (a = first; a; a = va_arg(ap, const char *))
		g_ptr_array_add(v, (char *)a);
	g_ptr_array_add(v, NULL);
	argv = user_argv_strv((char *const *)v->pdata);
	g_ptr_array_free(v, TRUE);
	return argv;
}

/* a command's stdout as the user ("" if it failed to run) */
static char *session_run(const char *first, ...) G_GNUC_NULL_TERMINATED;

static char *session_run(const char *first, ...)
{
	GString *out = g_string_new(NULL);
	char *error = NULL, **argv;
	va_list ap;

	va_start(ap, first);
	argv = user_argv(first, ap);
	va_end(ap);
	if (!run_argv(argv, 5000, out, NULL, NULL, &error)) {
		log_msg("%s: %s", first, error);
		g_free(error);
		g_string_truncate(out, 0);
	}
	g_strfreev(argv);
	return g_string_free(out, FALSE);
}

/* the same with the args as an array (pactl set-sink-volume SINK V...) */
static char *session_runv(char **args)
{
	char **argv = user_argv_strv(args), *error = NULL;
	GString *out = g_string_new(NULL);

	if (!run_argv(argv, 5000, out, NULL, NULL, &error)) {
		log_msg("%s: %s", args[0], error);
		g_free(error);
		g_string_truncate(out, 0);
	}
	g_strfreev(argv);
	return g_string_free(out, FALSE);
}

static gboolean child_readable(int fd, GIOCondition cond, gpointer data)
{
	struct child *c = data;
	char b[4096];
	ssize_t n = read(fd, b, sizeof(b));
	char *nl;

	if (n < 0 && errno == EAGAIN)
		return G_SOURCE_CONTINUE;
	if (n <= 0) {
		c->watch = 0;
		return G_SOURCE_REMOVE;
	}
	g_string_append_len(c->buf, b, n);
	while ((nl = memchr(c->buf->str, '\n', c->buf->len))) {
		*nl = 0;
		c->line(c->buf->str);
		g_string_erase(c->buf, 0, nl - c->buf->str + 1);
	}
	return G_SOURCE_CONTINUE;
}

/* a long running command (as the user), its output lines to line() */
static gboolean session_watch(void (*line)(const char *), char **error, const char *first, ...)
{
	struct child *c;
	char **argv;
	va_list ap;
	int out;
	pid_t pid;

	va_start(ap, first);
	argv = user_argv(first, ap);
	va_end(ap);
	pid = spawn(argv, &out, NULL, error);
	g_strfreev(argv);
	if (pid < 0)
		return FALSE;
	g_unix_set_fd_nonblocking(out, TRUE, NULL);
	c = g_new0(struct child, 1);
	c->pid = pid;
	c->out = out;
	c->buf = g_string_new(NULL);
	c->line = line;
	c->watch = g_unix_fd_add(out, G_IO_IN | G_IO_HUP, child_readable, c);
	g_ptr_array_add(session.procs, c);
	return TRUE;
}

/* pactl get-sink-volume's percent (channel average), -1 if none */
static int parse_volume(char *out)
{
	GRegex *re = g_regex_new("(\\d+)%", 0, 0, NULL);
	GMatchInfo *mi;
	long sum = 0;
	int n = 0;

	out[strcspn(out, "\n")] = 0;
	g_regex_match(re, out, 0, &mi);
	while (g_match_info_matches(mi)) {
		char *d = g_match_info_fetch(mi, 1);

		sum += atol(d);
		n++;
		g_free(d);
		g_match_info_next(mi, NULL);
	}
	g_match_info_free(mi);
	g_regex_unref(re);
	/* Python's // : floor */
	return n ? (int)(sum / n) : -1;
}

/* the default (or a) sink's volume in percent, -1 if none */
static int sink_volume(const char *sink)
{
	char *out = session_run("pactl", "get-sink-volume", sink, NULL);
	int v = parse_volume(out);

	g_free(out);
	return v;
}

/* pactl get-sink-volume's raw volume per channel, NULL if none */
static char **parse_volume_raw(char *out)
{
	GRegex *re = g_regex_new("(\\d+) / ", 0, 0, NULL);
	GPtrArray *v = g_ptr_array_new();
	GMatchInfo *mi;

	out[strcspn(out, "\n")] = 0;
	g_regex_match(re, out, 0, &mi);
	while (g_match_info_matches(mi)) {
		g_ptr_array_add(v, g_match_info_fetch(mi, 1));
		g_match_info_next(mi, NULL);
	}
	g_match_info_free(mi);
	g_regex_unref(re);
	if (!v->len) {
		g_ptr_array_free(v, TRUE);
		return NULL;
	}
	g_ptr_array_add(v, NULL);
	return (char **)g_ptr_array_free(v, FALSE);
}

static char **sink_volume_raw(const char *sink)
{
	char *out = session_run("pactl", "get-sink-volume", sink, NULL);
	char **v = parse_volume_raw(out);

	g_free(out);
	return v;
}

/* the first "<pattern> <uint32 N>" in text, -1 if none */
static long uint32_field(GRegex *re, const char *text)
{
	GMatchInfo *mi;
	long v = -1;

	if (g_regex_match(re, text, 0, &mi)) {
		char *d = g_match_info_fetch(mi, 1);

		v = atol(d);
		g_free(d);
	}
	g_match_info_free(mi);
	return v;
}

static void callaudio(const char *text)
{
	static GRegex *speaker_re, *mic_re;
	long v;

	if (!speaker_re) {
		speaker_re = g_regex_new("'SpeakerState': <uint32 (\\d+)>", 0, 0, NULL);
		mic_re = g_regex_new("'MicState': <uint32 (\\d+)>", 0, 0, NULL);
	}
	v = uint32_field(speaker_re, text);
	if (v >= 0)
		speech_speaker(v == 1);
	v = uint32_field(mic_re, text);
	if (v >= 0)
		speech_mute(v == 0);
}

static gboolean volume_changed(gpointer data)
{
	int v;

	session.check = 0;
	v = sink_volume("@DEFAULT_SINK@");
	if (v >= 0 && v != session.volume) {
		session.volume = v;
		speech_volume(dl_gain(v));
	}
	return G_SOURCE_REMOVE;
}

static void pulse_event(const char *text)
{
	if ((strstr(text, " on sink ") || strstr(text, " on server ")) && !session.check)
		session.check = g_timeout_add(50, volume_changed, NULL);
}

/* before speech on: the speaker, mic and call volume to start with */
static void session_start(void)
{
	char *error = NULL, *text, *sink, *media = NULL;
	gsize len;

	if (session.cmd)
		g_ptr_array_free(session.cmd, TRUE);
	session.cmd = session_user();
	if (!session.cmd)
		return;
	if (!session.procs)
		session.procs = g_ptr_array_new();
	if (!session_watch(callaudio, &error, "gdbus", "monitor", "--session", "-d",
			   CALLAUDIO_NAME, NULL) ||
	    !session_watch(pulse_event, &error, "pactl", "subscribe", NULL)) {
		log_msg("session: %s", error);
		g_clear_pointer(&error, g_free);
	}
	text = session_run("gdbus", "call", "--session", "-d", CALLAUDIO_NAME, "-o",
			   CALLAUDIO_PATH, "-m", "org.freedesktop.DBus.Properties.GetAll",
			   CALLAUDIO_NAME, NULL);
	callaudio(text);
	g_free(text);
	sink = g_strstrip(session_run("pactl", "get-default-sink", NULL));
	g_clear_pointer(&session.media_sink, g_free);
	g_clear_pointer(&session.media, g_strfreev);
	if (*sink) {
		session.media = sink_volume_raw(sink);
		if (session.media)
			session.media_sink = g_strdup(sink);
	}
	session.volume = CALL_VOLUME_DEFAULT;
	if (g_file_get_contents(call_volume_path, &text, &len, NULL)) {
		char *t = g_strstrip(text), *end;
		long v = strtol(t, &end, 10);

		if (*t && !*end)
			session.volume = v;
		g_free(text);
	}
	if (*sink) {
		char *pct = g_strdup_printf("%d%%", session.volume);
		char *r = session_run("pactl", "set-sink-volume", sink, pct, NULL);

		g_free(r);
		g_free(pct);
	}
	speech_volume(dl_gain(session.volume));
	if (session.media)
		media = g_strjoinv("/", session.media);
	log_msg("call volume %d%% (media %s)", session.volume, media ? media : "None");
	g_free(media);
	g_free(sink);
}

/* the modem PCMs' node ids in pw-dump's output */
static GPtrArray *parse_node_ids(const char *out)
{
	GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
	JsonParser *p = json_parser_new();
	JsonArray *objs;
	unsigned int i;

	if (!json_parser_load_from_data(p, *out ? out : "[]", -1, NULL) ||
	    !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(p)))
		goto out;
	objs = json_node_get_array(json_parser_get_root(p));
	for (i = 0; i < json_array_get_length(objs); i++) {
		JsonNode *n = json_array_get_element(objs, i);
		JsonObject *o, *info, *props;
		const char *name;

		if (!JSON_NODE_HOLDS_OBJECT(n))
			continue;
		o = json_node_get_object(n);
		info = json_object_has_member(o, "info") ? json_object_get_object_member(o, "info") : NULL;
		props = info && json_object_has_member(info, "props") ?
			json_object_get_object_member(info, "props") : NULL;
		name = props && json_object_has_member(props, "node.name") ?
			json_object_get_string_member(props, "node.name") : NULL;
		if (name && (!strcmp(name, uplink.name) || !strcmp(name, downlink.name)))
			g_ptr_array_add(ids, g_strdup_printf("%" G_GINT64_FORMAT,
							      json_object_get_int_member(o, "id")));
	}
out:
	g_object_unref(p);
	return ids;
}

/* the modem PCMs' nodes in the sound server */
static GPtrArray *node_ids(void)
{
	char *out = session_run("pw-dump", NULL);
	GPtrArray *ids = parse_node_ids(out);

	g_free(out);
	return ids;
}

static void destroy_nodes(void)
{
	GPtrArray *ids = node_ids();
	unsigned int i;

	for (i = 0; i < ids->len; i++)
		g_free(session_run("pw-cli", "destroy", ids->pdata[i], NULL));
	g_ptr_array_free(ids, TRUE);
}

static void stop_graph(void)
{
	pid_t pid = session.graph;
	int i;

	session.graph = 0;
	if (pid > 0) {
		kill(pid, SIGTERM);
		for (i = 0; i < 300 && waitpid(pid, NULL, WNOHANG) == 0; i++)
			g_usleep(10000);
		if (i == 300) {
			kill(pid, SIGKILL);
			waitpid(pid, NULL, 0);
		}
	}
	if (session.cmd)
		destroy_nodes();
}

/* whitespace-split words of a command's output */
static char **words(char *out)
{
	char **w = g_strsplit_set(g_strstrip(out), " \t\n\r\f\v", -1);
	GPtrArray *v = g_ptr_array_new();
	int i;

	for (i = 0; w[i]; i++)
		if (*w[i])
			g_ptr_array_add(v, g_strdup(w[i]));
	g_strfreev(w);
	g_ptr_array_add(v, NULL);
	return (char **)g_ptr_array_free(v, FALSE);
}

/*
 * the AP path's call chain, as the user: the modem PCMs as nodes in the
 * sound server (the AFE routes are up: a DPCM front end without a back end
 * cannot be opened), then cezanne-voice's call.conf.in in a PipeWire client,
 * its mic capture linked to the hardware source by hand (WirePlumber would
 * put the mic's noise filter in front of it)
 */
static gboolean start_graph(void)
{
	const struct node *nodes[2] = { &uplink, &downlink };
	char *mic, *conf, *text = NULL, *error = NULL, *src0 = NULL;
	GPtrArray *ids;
	gboolean linked = FALSE;
	int i;

	if (!session.cmd)
		return FALSE;
	mic = g_strstrip(session_run("pactl", "get-default-source", NULL));
	destroy_nodes();	/* left over from an interrupted call */
	for (i = 0; i < 2; i++) {
		const struct node *n = nodes[i];
		char *props = g_strdup_printf(
			"{ factory.name = %s node.name = %s node.description = \"%s\" "
			"api.alsa.path = \"%s\" audio.rate = %d audio.channels = %d "
			"audio.position = %s priority.session = 1 priority.driver = 1 "
			"node.latency = 480/%d object.linger = true }",
			n->factory, n->name, n->desc, n->path, RATE, n->channels,
			n->channels == 2 ? "[ FL FR ]" : "[ MONO ]", RATE);

		g_free(session_run("pw-cli", "create-node", "adapter", props, NULL));	/* prints nothing */
		g_free(props);
	}
	ids = node_ids();
	conf = g_build_filename(session.rundir, "cezanne-call.conf", NULL);
	session.graph = -1;
	if (g_file_get_contents(CALL_CONF, &text, NULL, NULL)) {
		GString *s = g_string_new(text);

		g_string_replace(s, "@UPLINK@", uplink.name, 0);
		g_string_replace(s, "@DOWNLINK@", downlink.name, 0);
		if (g_file_set_contents(conf, s->str, s->len, NULL) && chmod(conf, 0644) == 0) {
			char *args[] = { "pipewire", "-c", conf, NULL };
			char **argv = user_argv_strv(args);

			session.graph = spawn(argv, NULL, NULL, &error);
			g_strfreev(argv);
		} else {
			error = os_error(errno, conf);
		}
		g_string_free(s, TRUE);
		g_free(text);
	} else {
		error = os_error(ENOENT, CALL_CONF);
	}
	if (session.graph < 0) {
		log_msg("call chain: %s", error);
		g_clear_pointer(&error, g_free);
		session.graph = 0;
	}
	for (i = 0; i < 30; i++) {	/* its ports show up within ~0.2 s */
		char *o = session_run("pw-link", "-o", NULL), *in = session_run("pw-link", "-i", NULL);
		char **ports = words(o), **inputs = words(in);
		char *prefix = g_strconcat(mic, ":", NULL);
		GPtrArray *src = g_ptr_array_new(), *fl = g_ptr_array_new();
		gboolean done = FALSE;
		int k;

		for (k = 0; ports[k]; k++)
			if (g_str_has_prefix(ports[k], prefix)) {
				g_ptr_array_add(src, ports[k]);
				if (g_str_has_suffix(ports[k], "_FL"))
					g_ptr_array_add(fl, ports[k]);
			}
		if (session.graph > 0 && ids->len == 2 && src->len &&
		    g_strv_contains((const char *const *)inputs, "cezanne_call.mic:input_FL")) {
			char *out = fl->len ? fl->pdata[0] : src->pdata[0];
			char *args[] = { "pw-link", out, "cezanne_call.mic:input_FL", NULL };
			char **argv = user_argv_strv(args);
			int status = -1;

			if (!run_argv(argv, 5000, NULL, NULL, &status, &error)) {
				/* Python: an uncaught TimeoutExpired; here a failed link */
				log_msg("pw-link: %s", error);
				g_clear_pointer(&error, g_free);
			}
			linked = status == 0;
			src0 = g_strdup(out);
			g_strfreev(argv);
			done = TRUE;
		}
		if (!done && (session.graph <= 0 || waitpid(session.graph, NULL, WNOHANG) != 0)) {
			if (session.graph > 0)
				session.graph = 0;	/* reaped */
			done = TRUE;
		}
		g_ptr_array_free(src, TRUE);
		g_ptr_array_free(fl, TRUE);
		g_free(prefix);
		g_strfreev(ports);
		g_strfreev(inputs);
		g_free(o);
		g_free(in);
		if (done)
			break;
		g_usleep(100000);
	}
	if (!linked) {
		GString *m = g_string_new(NULL), *n = g_string_new(NULL);

		repr_add(m, mic, strlen(mic), FALSE);
		g_ptr_array_add(ids, NULL);
		repr_strv(n, (char **)ids->pdata);
		log_msg("call chain did not come up (mic %s, nodes %s)", m->str, n->str);
		g_string_free(m, TRUE);
		g_string_free(n, TRUE);
		stop_graph();
	} else {
		log_msg("call chain up: %s -> echo cancelling -> RNNoise -> modem", src0);
	}
	g_ptr_array_free(ids, TRUE);
	g_free(src0);
	g_free(conf);
	g_free(mic);
	return linked;
}

/* after speech off: keep the call volume, the media volume back */
static void session_stop(void)
{
	GError *err = NULL;
	char *tmp;
	unsigned int i;

	if (!session.cmd)
		return;
	for (i = 0; session.procs && i < session.procs->len; i++) {
		struct child *c = session.procs->pdata[i];

		if (c->watch)
			g_source_remove(c->watch);
		kill(c->pid, SIGTERM);
		waitpid(c->pid, NULL, 0);
		close(c->out);
		g_string_free(c->buf, TRUE);
		g_free(c);
	}
	if (session.procs)
		g_ptr_array_set_size(session.procs, 0);
	if (session.check) {
		g_source_remove(session.check);
		session.check = 0;
	}
	tmp = g_strdup_printf("%d\n", session.volume);
	if (!g_file_set_contents(call_volume_path, tmp, -1, &err)) {
		log_msg("call volume: %s", err->message);
		g_clear_error(&err);
	}
	g_free(tmp);
	if (session.media) {
		GPtrArray *v = g_ptr_array_new();

		g_ptr_array_add(v, "pactl");
		g_ptr_array_add(v, "set-sink-volume");
		g_ptr_array_add(v, session.media_sink);
		for (i = 0; session.media[i]; i++)
			g_ptr_array_add(v, session.media[i]);
		g_ptr_array_add(v, NULL);
		g_free(session_runv((char **)v->pdata));
		g_ptr_array_free(v, TRUE);
	}
	g_clear_pointer(&session.media, g_strfreev);
	g_clear_pointer(&session.media_sink, g_free);
	g_ptr_array_free(session.cmd, TRUE);
	session.cmd = NULL;
}

/* ---- speech on / off --------------------------------------------------- */

static gboolean modem_readable(int fd, GIOCondition cond, gpointer data)
{
	struct md_msg m;

	if (speech.modem && md_receive(speech.modem, &m))
		md_handle(speech.modem, &m);
	return G_SOURCE_CONTINUE;
}

static gboolean open_modem(gboolean watch)
{
	char state[64], *err = NULL;
	int fd;
	ssize_t n;

	if (speech.modem)
		return TRUE;
	fd = open(STATE, O_RDONLY | O_CLOEXEC);
	n = fd < 0 ? -1 : read(fd, state, sizeof(state) - 1);
	if (n < 0) {
		err = os_error(errno, STATE);
	} else {
		state[n] = 0;
		if (strcmp(g_strstrip(state), "ready")) {
			close(fd);
			return FALSE;
		}
		speech.modem = modem_new(&err);
	}
	if (fd >= 0)
		close(fd);
	if (!speech.modem) {
		log_msg("speech channel: %s", err);
		g_free(err);
		return FALSE;
	}
	if (watch)
		speech.modem_watch = g_unix_fd_add(speech.modem->fd, G_IO_IN, modem_readable, NULL);
	log_msg("speech channel open");
	return TRUE;
}

static void speech_set(gboolean on, int app, gboolean stream)
{
	struct modem *md;

	if (on == speech.on)
		return;
	if (on) {
		uint8_t info[128];
		double t0;
		gboolean ok;
		char *err = NULL;

		if (!open_modem(TRUE)) {
			log_msg("modem not ready, no call audio");
			return;
		}
		t0 = mono();
		sph_info(info, app);
		md = speech.modem;
		speech.dl = 0;
		speech.muted = speech.speaker_on = FALSE;
		speech.ap = stream && ap_path && speech.have_session;
		if (speech.have_session)
			session_start();
		/* as MIUI: the AFE path first, then speech on */
		if (stream) {
			routes_start(speech.speaker_on, speech.ap);
			if (!hostless_start(&err)) {
				log_msg("hostless stream: %s", err);
				g_free(err);
			}
		}
		ok = md_request_info(md, MSG_SPH_ON, info);
		if (!opendsp) {
			static const unsigned int mutes[] = {
				MSG_DL_MUTE, MSG_DL_MUTE_DYNAMIC, MSG_UL_MUTE, MSG_UL_SOURCE_MUTE,
			};
			unsigned int i;

			/*
			 * the modem processes speech itself: its gains and mutes start
			 * muted (the downlink then carries only a -70 dBFS floor), no
			 * parameters for its enhancement yet. Gains and the codec mute
			 * are not acked.
			 */
			md_mailbox(md, MSG_DL_GAIN, speech.dl & 0xffff, 0);
			md_mailbox(md, MSG_UL_GAIN, ul_gain, 0);
			md_mailbox(md, MSG_DL_MUTE_CODEC, 0, 0);
			md_request_mailbox(md, MSG_SPH_ENH, enhance);
			for (i = 0; i < G_N_ELEMENTS(mutes); i++)
				md_request_mailbox(md, mutes[i], speech.muted && mutes[i] == MSG_UL_MUTE);
		}
		md_request_info(md, MSG_SPH_INFO, info);
		if (speech.ap && !start_graph()) {
			speech.ap = FALSE;
			routes_direct();
			if (session.volume >= 0)
				speech_volume(dl_gain(session.volume));
		}
		log_msg("speech on (%s, %s path, %.0f ms)", ok ? "acked" : "no ack",
			speech.ap ? "AP" : "direct", (mono() - t0) * 1000);
	} else if (speech.modem) {
		md = speech.modem;
		md_request_mailbox(md, MSG_UL_MUTE, 1);
		md_request_mailbox(md, MSG_SPH_OFF, 0);
		md_request_mailbox(md, MSG_UL_MUTE, 0);
		if (speech.have_session)
			stop_graph();	/* its PCMs before their routes */
		if (npcms)
			hostless_stop();
		routes_stop();
		if (speech.have_session)
			session_stop();
		speech.ap = FALSE;
		log_msg("speech off");
	}
	speech.on = on;
}

/* ---- ModemManager's calls ---------------------------------------------- */

/*
 * Modem.Voice "Calls", one proxy per call: speech on while one is dialing,
 * alerting, active or held
 */
static struct {
	GDBusObjectManager *om;
	GHashTable *voices;	/* modem path -> Voice proxy */
	GHashTable *calls;	/* call path -> Call proxy */
} calls;

static void calls_sync(void);

static void calls_update(void)
{
	GHashTableIter it;
	gpointer proxy;
	GString *s = g_string_new("[");
	gboolean any = FALSE;
	int n = 0;

	g_hash_table_iter_init(&it, calls.calls);
	while (g_hash_table_iter_next(&it, NULL, &proxy)) {
		GVariant *v = g_dbus_proxy_get_cached_property(proxy, "State");
		int st = v && g_variant_is_of_type(v, G_VARIANT_TYPE_INT32) ? g_variant_get_int32(v) : 0;

		if (v)
			g_variant_unref(v);
		g_string_append_printf(s, "%s%d", n++ ? ", " : "", st);
		any |= speech_state(st);
	}
	g_string_append_c(s, ']');
	if (verbose)
		log_msg("call states %s", s->str);
	g_string_free(s, TRUE);
	speech_set(any, 0, TRUE);
}

static void on_voice_changed(void)
{
	calls_sync();
}

static void on_call_changed(void)
{
	calls_update();
}

static void drop_proxy(gpointer p)
{
	g_signal_handlers_disconnect_by_func(p, on_voice_changed, NULL);
	g_signal_handlers_disconnect_by_func(p, on_call_changed, NULL);
	g_object_unref(p);
}

/* track every modem's Voice interface and its calls */
static void calls_sync(void)
{
	GHashTable *modems = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GHashTable *paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GList *objs = g_dbus_object_manager_get_objects(calls.om), *l;
	GHashTableIter it;
	gpointer key, value;

	for (l = objs; l; l = l->next) {
		const char *path = g_dbus_object_get_object_path(l->data);
		GDBusInterface *iface = g_dbus_object_get_interface(l->data, VOICE_IFACE);
		GDBusProxy *v;
		GError *err = NULL;

		if (!iface)
			continue;
		g_object_unref(iface);
		g_hash_table_add(modems, g_strdup(path));
		if (g_hash_table_contains(calls.voices, path))
			continue;
		v = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, 0, NULL, MM, path,
						  VOICE_IFACE, NULL, &err);
		if (!v) {
			log_msg("voice %s: %s", path, err->message);
			g_error_free(err);
			continue;
		}
		g_signal_connect(v, "g-signal", G_CALLBACK(on_voice_changed), NULL);
		g_signal_connect(v, "g-properties-changed", G_CALLBACK(on_voice_changed), NULL);
		g_hash_table_insert(calls.voices, g_strdup(path), v);
	}
	g_list_free_full(objs, g_object_unref);
	g_hash_table_iter_init(&it, calls.voices);
	while (g_hash_table_iter_next(&it, &key, NULL))
		if (!g_hash_table_contains(modems, key))
			g_hash_table_iter_remove(&it);
	g_hash_table_iter_init(&it, calls.voices);
	while (g_hash_table_iter_next(&it, NULL, &value)) {
		GVariant *c = g_dbus_proxy_get_cached_property(value, "Calls");
		GVariantIter ci;
		const char *p;

		if (!c)
			continue;
		if (g_variant_is_of_type(c, G_VARIANT_TYPE("ao"))) {
			g_variant_iter_init(&ci, c);
			while (g_variant_iter_next(&ci, "&o", &p))
				g_hash_table_add(paths, g_strdup(p));
		}
		g_variant_unref(c);
	}
	g_hash_table_iter_init(&it, paths);
	while (g_hash_table_iter_next(&it, &key, NULL)) {
		GDBusProxy *c;
		GError *err = NULL;

		if (g_hash_table_contains(calls.calls, key))
			continue;
		c = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, 0, NULL, MM, key,
						  CALL_IFACE, NULL, &err);
		if (!c) {
			log_msg("call %s: %s", (char *)key, err->message);
			g_error_free(err);
			continue;
		}
		g_signal_connect(c, "g-signal", G_CALLBACK(on_call_changed), NULL);
		g_signal_connect(c, "g-properties-changed", G_CALLBACK(on_call_changed), NULL);
		g_hash_table_insert(calls.calls, g_strdup(key), c);
	}
	g_hash_table_iter_init(&it, calls.calls);
	while (g_hash_table_iter_next(&it, &key, NULL))
		if (!g_hash_table_contains(paths, key))
			g_hash_table_iter_remove(&it);
	g_hash_table_unref(modems);
	g_hash_table_unref(paths);
	calls_update();
}

static void on_om_changed(void)
{
	calls_sync();
}

static gboolean calls_init(void)
{
	GError *err = NULL;

	calls.om = g_dbus_object_manager_client_new_for_bus_sync(
		G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, MM,
		"/org/freedesktop/ModemManager1", NULL, NULL, NULL, NULL, &err);
	if (!calls.om) {
		fprintf(stderr, "ModemManager: %s\n", err->message);
		g_error_free(err);
		return FALSE;
	}
	calls.voices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, drop_proxy);
	calls.calls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, drop_proxy);
	g_signal_connect(calls.om, "object-added", G_CALLBACK(on_om_changed), NULL);
	g_signal_connect(calls.om, "object-removed", G_CALLBACK(on_om_changed), NULL);
	g_signal_connect(calls.om, "notify::name-owner", G_CALLBACK(on_om_changed), NULL);
	calls_sync();
	return TRUE;
}

/* ---- main -------------------------------------------------------------- */

/* int(os.environ.get(name, default), base) */
static long env_int(const char *name, const char *def, int base)
{
	const char *s = g_getenv(name);
	char *end;
	long v;

	if (!s)
		s = def;
	v = strtol(s, &end, base);
	while (g_ascii_isspace(*end))
		end++;
	if (end == s || *end) {
		fprintf(stderr, "%s: invalid literal %s\n", name, s);
		exit(1);
	}
	return v;
}

static gboolean on_sigint(gpointer loop)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static void usage(void)
{
	fprintf(stderr, "usage: cezanne-speechd [-h] [-v] [--loopback SECONDS] [--no-stream] [--alive-ack]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	double loopback = 0;
	gboolean no_stream = FALSE, alive_ack = FALSE;
	const char *state_dir;
	GMainLoop *loop;
	int i;

	mallopt(M_ARENA_MAX, 1);
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "-v"))
			verbose = TRUE;
		else if (!strcmp(a, "--no-stream"))
			no_stream = TRUE;
		else if (!strcmp(a, "--alive-ack"))
			alive_ack = TRUE;
		else if (!strcmp(a, "--loopback") || g_str_has_prefix(a, "--loopback=")) {
			const char *v = a[10] == '=' ? a + 11 : (++i < argc ? argv[i] : NULL);
			char *end;

			if (!v)
				usage();
			loopback = g_ascii_strtod(v, &end);
			if (end == v || *end)
				usage();
		} else {
			usage();
		}
	}
	opendsp = env_int("SPH_OPENDSP", "0", 10);
	ul_gain = env_int("SPH_UL_GAIN", "0", 0);
	enhance = env_int("SPH_ENH", "0", 10);
	ap_path = g_strcmp0(g_getenv("SPH_AP") ? g_getenv("SPH_AP") : "1", "0") &&
		  access(CALL_CONF, F_OK) == 0;
	state_dir = g_getenv("STATE_DIRECTORY");
	call_volume_path = g_build_filename(state_dir ? state_dir : "/var/lib/cezanne-speechd",
					    "call-volume", NULL);

	if (!open_modem(FALSE)) {
		fprintf(stderr, "modem not ready\n");
		return 1;
	}
	if (alive_ack)
		md_mailbox(speech.modem, MSG_MD_ALIVE_ACK, 0, 0);
	if (loopback) {
		speech_set(TRUE, 2, !no_stream);
		g_usleep((gulong)(loopback * 1e6));
		speech_set(FALSE, 0, TRUE);
		return 0;
	}
	speech.modem_watch = g_unix_fd_add(speech.modem->fd, G_IO_IN, modem_readable, NULL);
	speech.have_session = TRUE;
	if (!calls_init())
		return 1;
	loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGINT, on_sigint, loop);
	g_main_loop_run(loop);
	if (speech.on)
		speech_set(FALSE, 0, TRUE);
	return 0;
}
