/*
 * cezanne-screencast: the shell's screen recorder, as GNOME's screencast
 * service: a ScreenCast session on the compositor's org.gnome.Mutter.
 * ScreenCast API (niri serves it) for a monitor or a window, its PipeWire
 * stream through GStreamer into an MP4.
 *
 *   cezanne-screencast FILE monitor CONNECTOR [X Y W H]   (crop, buffer px)
 *   cezanne-screencast FILE window ID
 *
 * Pipelines as GNOME's (gnome-shell overlay 0002): VA-API H.264 on
 * cezanne-venc, zero copy from the compositor's dma-bufs, constant QP 26;
 * else openh264 on the CPU. A selection: crop meta on the compositor's
 * buffers, which vapostproc gives the VA driver as the source region
 * (videocrop refuses DMA_DRM caps), output at the selection's size. niri's
 * stream repeats one PTS on every frame (only basesrc's do-timestamp DTS
 * advances): the encoder then sends frames without a PTS and mp4mux stops,
 * so the arrival time becomes the PTS.
 *
 * Stops on "stop" on stdin, its end, SIGINT or SIGTERM, or when the
 * compositor ends the cast. Prints "recording PIPELINE" once frames flow and
 * "saved FILE" at the end, "error MESSAGE" on failure (exit 1).
 */
#define _GNU_SOURCE
#include <glib-unix.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SCREENCAST	"org.gnome.Mutter.ScreenCast"
#define SESSION_IFACE	SCREENCAST ".Session"
#define FRAMERATE	30

static const struct {
	const char *name, *fmt;
} pipelines[] = {
	{ "va", "capsfilter caps=video/x-raw(memory:DMABuf),format=DMA_DRM,max-framerate=%d/1 ! "
		"vapostproc ! %svah264enc rate-control=cqp qpi=26 qpp=26 ! queue ! h264parse ! "
		"mp4mux fragment-duration=500 fragment-mode=first-moov-then-finalise" },
	{ "sw", "capsfilter caps=video/x-raw,max-framerate=%d/1 ! videoconvert chroma-mode=none "
		"dither=none matrix-mode=output-only n-threads=4 ! %squeue ! "
		"openh264enc complexity=high rate-control=quality qp-min=26 qp-max=26 multi-thread=4 ! "
		"queue ! h264parse ! mp4mux fragment-duration=500 fragment-mode=first-moov-then-finalise" },
};

static GDBusConnection *bus;
static GMainLoop *loop;
static char *path, *session;
static gboolean crop;
static guint cx, cy, cw, ch;
static GstElement *pipeline;
static const char *pipeline_name;
static guint node;
static int tried, status;
static gboolean started, stopping, finished;

static void say(const char *what, const char *msg)
{
	printf("%s %s\n", what, msg);
	fflush(stdout);
}

static GVariant *call(const char *obj, const char *iface, const char *method,
		      GVariant *args, const char *reply, GError **err)
{
	return g_dbus_connection_call_sync(bus, SCREENCAST, obj, iface, method, args,
					   reply ? G_VARIANT_TYPE(reply) : NULL,
					   G_DBUS_CALL_FLAGS_NONE, -1, NULL, err);
}

static void end_session(void)
{
	GVariant *r;

	if (!session)
		return;
	r = call(session, SESSION_IFACE, "Stop", NULL, NULL, NULL);
	if (r)
		g_variant_unref(r);
	g_clear_pointer(&session, g_free);
}

static gboolean finish(gpointer data)
{
	if (finished)
		return G_SOURCE_REMOVE;
	finished = TRUE;
	if (pipeline) {
		gst_element_set_state(pipeline, GST_STATE_NULL);
		g_clear_object(&pipeline);
	}
	end_session();
	if (status == 0) {
		if (started && g_file_test(path, G_FILE_TEST_EXISTS)) {
			say("saved", path);
		} else {
			say("error", "nothing recorded");
			status = 1;
		}
	}
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static void fail(const char *msg)
{
	say("error", msg);
	status = 1;
	stopping = TRUE;
	finish(NULL);
}

static gboolean stop(gpointer data)
{
	if (stopping)
		return G_SOURCE_REMOVE;
	stopping = TRUE;
	if (pipeline && started) {
		gst_element_send_event(pipeline, gst_event_new_eos());
		g_timeout_add_seconds(5, finish, NULL);		/* no EOS: give up */
	} else {
		finish(NULL);
	}
	return G_SOURCE_REMOVE;
}

/* pipewiresrc's own buffers, just made */
static GstPadProbeReturn on_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
	GstBuffer *buf = gst_pad_probe_info_get_buffer(info);
	gboolean add_crop = GPOINTER_TO_INT(data);

	buf = gst_buffer_make_writable(buf);
	if (GST_BUFFER_DTS_IS_VALID(buf))
		GST_BUFFER_PTS(buf) = GST_BUFFER_DTS(buf);
	if (add_crop) {
		GstVideoCropMeta *meta = gst_buffer_get_video_crop_meta(buf);

		if (!meta)
			meta = gst_buffer_add_video_crop_meta(buf);
		meta->x = cx;
		meta->y = cy;
		meta->width = cw;
		meta->height = ch;
	}
	GST_PAD_PROBE_INFO_DATA(info) = buf;
	return GST_PAD_PROBE_OK;
}

/* the CPU pipeline's videocrop takes edges: right / bottom from the size */
static void on_crop_caps(GObject *pad, GParamSpec *pspec, gpointer data)
{
	GstCaps *caps = gst_pad_get_current_caps(GST_PAD(pad));
	GstElement *vc = data;
	int w, h;

	if (!caps)
		return;
	if (gst_structure_get_int(gst_caps_get_structure(caps, 0), "width", &w) &&
	    gst_structure_get_int(gst_caps_get_structure(caps, 0), "height", &h)) {
		g_object_set(vc, "right", MAX(0, w - (int)(cx + cw)),
			     "bottom", MAX(0, h - (int)(cy + ch)), NULL);
	}
	gst_caps_unref(caps);
}

static void build(void);

/* a pipeline that could not start: the next one */
static void drop(const char *why)
{
	g_autofree char *msg = g_strdup_printf("%s: %s", pipeline_name, why);

	say("log", msg);
	if (pipeline) {
		gst_element_set_state(pipeline, GST_STATE_NULL);
		g_clear_object(&pipeline);
	}
	unlink(path);
	if (!stopping)
		build();
}

static gboolean on_message(GstBus *gbus, GstMessage *msg, gpointer data)
{
	GstElement *owner = data;

	if (owner != pipeline)
		return G_SOURCE_REMOVE;		/* a dropped pipeline's bus */
	switch (GST_MESSAGE_TYPE(msg)) {
	case GST_MESSAGE_STATE_CHANGED:
		if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
			GstState old, new;

			gst_message_parse_state_changed(msg, &old, &new, NULL);
			if (new == GST_STATE_PLAYING && !started) {
				started = TRUE;
				say("recording", pipeline_name);
			}
		}
		break;
	case GST_MESSAGE_ERROR: {
		g_autoptr(GError) err = NULL;
		g_autofree char *dbg = NULL;

		gst_message_parse_error(msg, &err, &dbg);
		if (!started)
			drop(err->message);
		else
			fail(err->message);
		return G_SOURCE_REMOVE;
	}
	case GST_MESSAGE_EOS:
		finish(NULL);
		return G_SOURCE_REMOVE;
	default:
		break;
	}
	return G_SOURCE_CONTINUE;
}

static void build(void)
{
	const char *only = g_getenv("CEZANNE_SCREENCAST_PIPELINE");	/* tests */
	g_autofree char *size = NULL, *vcrop = NULL, *middle = NULL, *desc = NULL;
	g_autoptr(GError) err = NULL;
	GstElement *src;
	GstPad *pad;
	gboolean va;

	while (only && tried < (int)G_N_ELEMENTS(pipelines) && strcmp(pipelines[tried].name, only))
		tried++;
	if (tried >= (int)G_N_ELEMENTS(pipelines)) {
		fail("no pipeline works");
		return;
	}
	pipeline_name = pipelines[tried].name;
	va = !strcmp(pipeline_name, "va");
	tried++;
	size = crop && va ? g_strdup_printf("video/x-raw(memory:VAMemory),width=%u,height=%u ! ", cw, ch)
			  : g_strdup("");
	vcrop = crop && !va ? g_strdup_printf("videocrop name=crop left=%u top=%u ! ", cx, cy)
			    : g_strdup("");
	middle = g_strdup_printf(pipelines[tried - 1].fmt, FRAMERATE, va ? size : vcrop);
	desc = g_strdup_printf("pipewiresrc name=src path=%u do-timestamp=true keepalive-time=1000 ! "
			       "%s ! filesink location=\"%s\"", node, middle, path);
	pipeline = gst_parse_launch(desc, &err);
	if (!pipeline) {
		drop(err->message);
		return;
	}
	src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
	pad = gst_element_get_static_pad(src, "src");
	gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_buffer, GINT_TO_POINTER(crop && va), NULL);
	gst_object_unref(pad);
	gst_object_unref(src);
	if (crop && !va) {
		GstElement *vc = gst_bin_get_by_name(GST_BIN(pipeline), "crop");
		GstPad *sink = gst_element_get_static_pad(vc, "sink");

		g_signal_connect(sink, "notify::caps", G_CALLBACK(on_crop_caps), vc);
		gst_object_unref(sink);
		gst_object_unref(vc);
	}
	{
		GstBus *pbus = gst_element_get_bus(pipeline);

		gst_bus_add_watch(pbus, on_message, pipeline);
		gst_object_unref(pbus);
	}
	if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
		drop("does not start");
}

static void on_stream(GDBusConnection *c, const char *sender, const char *obj, const char *iface,
		      const char *signal, GVariant *params, gpointer data)
{
	g_variant_get(params, "(u)", &node);
	build();
}

static void on_closed(GDBusConnection *c, const char *sender, const char *obj, const char *iface,
		      const char *signal, GVariant *params, gpointer data)
{
	stop(NULL);
}

static gboolean on_stdin(int fd, GIOCondition cond, gpointer data)
{
	char line[64];
	ssize_t n = read(fd, line, sizeof(line) - 1);

	if (n > 0)
		line[n] = 0;
	if (n <= 0 || g_str_has_prefix(line, "stop")) {
		stop(NULL);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
	g_autoptr(GError) err = NULL;
	g_autofree char *stream = NULL, *dir = NULL;
	GVariant *r, *props;
	GVariantBuilder b;

	if (argc < 4 || (strcmp(argv[2], "monitor") && strcmp(argv[2], "window"))) {
		fprintf(stderr, "usage: cezanne-screencast FILE monitor CONNECTOR [X Y W H] | FILE window ID\n");
		return 2;
	}
	path = argv[1];
	if (!strcmp(argv[2], "monitor") && argc >= 8) {
		crop = TRUE;
		cx = atoi(argv[4]);
		cy = atoi(argv[5]);
		cw = atoi(argv[6]) & ~1;	/* 4:2:0 */
		ch = atoi(argv[7]) & ~1;
	}
	dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0755);

	g_setenv("GST_VA_ALL_DRIVERS", "1", FALSE);
	gst_init(&argc, &argv);
	loop = g_main_loop_new(NULL, FALSE);
	bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (!bus)
		goto error;

	r = call("/org/gnome/Mutter/ScreenCast", SCREENCAST, "CreateSession",
		 g_variant_new("(a{sv})", NULL), "(o)", &err);
	if (!r)
		goto error;
	g_variant_get(r, "(o)", &session);
	g_variant_unref(r);

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "cursor-mode", g_variant_new_uint32(0));
	if (!strcmp(argv[2], "window")) {
		g_variant_builder_add(&b, "{sv}", "window-id",
				      g_variant_new_uint64(g_ascii_strtoull(argv[3], NULL, 10)));
		props = g_variant_new("(a{sv})", &b);
		r = call(session, SESSION_IFACE, "RecordWindow", props, "(o)", &err);
	} else {
		props = g_variant_new("(sa{sv})", argv[3], &b);
		r = call(session, SESSION_IFACE, "RecordMonitor", props, "(o)", &err);
	}
	if (!r)
		goto error;
	g_variant_get(r, "(o)", &stream);
	g_variant_unref(r);

	g_dbus_connection_signal_subscribe(bus, SCREENCAST, SCREENCAST ".Stream", "PipeWireStreamAdded",
					   stream, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_stream, NULL, NULL);
	g_dbus_connection_signal_subscribe(bus, SCREENCAST, SESSION_IFACE, "Closed", session, NULL,
					   G_DBUS_SIGNAL_FLAGS_NONE, on_closed, NULL, NULL);
	r = call(session, SESSION_IFACE, "Start", NULL, NULL, &err);
	if (!r)
		goto error;
	g_variant_unref(r);

	g_unix_signal_add(SIGINT, stop, NULL);
	g_unix_signal_add(SIGTERM, stop, NULL);
	g_unix_fd_add(STDIN_FILENO, G_IO_IN | G_IO_HUP, on_stdin, NULL);
	g_main_loop_run(loop);
	return status;

error:
	say("error", err ? err->message : "failed");
	end_session();
	return 1;
}
