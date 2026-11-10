# gnome-shell segfault on xdg_toplevel.move with a cancelled touch's serial
# (NOTES "gnome-shell segfault on a touch drag in Firefox")
cz_patch mutter 0001-wayland-touch-skip-cancelled-touches-in-grab-lookup.patch
# screencasts at 24 fps (120 Hz) / 20-26 fps (60 Hz) instead of 30: the max
# framerate check skipped frames a hair early (NOTES "Screen recording:
# frame pacing and quality")
cz_patch mutter 0002-screen-cast-allow-slack-in-the-max-framerate-check.patch
