/*
 * cezanne-home: an empty, transparent window that niri keeps as the first
 * column of the workspace. Three-finger swipes past the first or last app
 * slide to it, within the workspace (the wallpaper and the dock show
 * there), as GNOME's empty workspace did; niri has no empty space beside
 * its columns, and its own empty workspace lies below, not to the side.
 * One 1x1 transparent buffer scaled to the window by wp_viewporter: about
 * a megabyte, where a Qt window costs ~45 MB.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_viewporter *viewporter;
static struct wl_surface *surface;
static struct wp_viewport *viewport;
static struct wl_buffer *buffer;
static int width, height;

static void global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_compositor_interface.name))
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name))
		wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
	else if (!strcmp(iface, wp_viewporter_interface.name))
		viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
}

static void global_remove(void *data, struct wl_registry *reg, uint32_t name) {}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static void ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { ping };

static void surface_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	wl_surface_attach(surface, buffer, 0, 0);
	wp_viewport_set_destination(viewport, width > 0 ? width : 1, height > 0 ? height : 1);
	wl_surface_damage_buffer(surface, 0, 0, 1, 1);
	wl_surface_commit(surface);
}

static const struct xdg_surface_listener surface_listener = { surface_configure };

static void toplevel_configure(void *data, struct xdg_toplevel *tl, int32_t w, int32_t h, struct wl_array *states)
{
	width = w;
	height = h;
}

/* closing home is a mistake: it stays */
static void toplevel_close(void *data, struct xdg_toplevel *tl) {}

static void toplevel_bounds(void *data, struct xdg_toplevel *tl, int32_t w, int32_t h) {}

static void toplevel_caps(void *data, struct xdg_toplevel *tl, struct wl_array *caps) {}

static const struct xdg_toplevel_listener toplevel_listener = {
	toplevel_configure, toplevel_close, toplevel_bounds, toplevel_caps
};

static struct wl_buffer *transparent_pixel(void)
{
	int fd = memfd_create("cezanne-home", MFD_CLOEXEC);
	struct wl_shm_pool *pool;
	struct wl_buffer *b;

	if (fd < 0 || ftruncate(fd, 4) < 0)
		return NULL;
	/* a zero ARGB pixel: fully transparent */
	pool = wl_shm_create_pool(shm, fd, 4);
	b = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

int main(void)
{
	struct wl_display *display = wl_display_connect(NULL);
	struct wl_registry *registry;
	struct xdg_surface *xs;
	struct xdg_toplevel *toplevel;

	if (!display) {
		fprintf(stderr, "cezanne-home: no Wayland display\n");
		return 1;
	}
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !wm_base || !viewporter) {
		fprintf(stderr, "cezanne-home: compositor lacks a needed global\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	buffer = transparent_pixel();
	if (!buffer)
		return 1;

	surface = wl_compositor_create_surface(compositor);
	viewport = wp_viewporter_get_viewport(viewporter, surface);
	xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "Home");
	xdg_toplevel_set_app_id(toplevel, "cezanne-home");
	wl_surface_commit(surface);

	while (wl_display_dispatch(display) != -1)
		;
	return 0;
}
