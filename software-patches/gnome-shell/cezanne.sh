# St tested every selector of every stylesheet for each theme node; first
# opens of menus after a shell start were mostly CSS matching (NOTES "GNOME
# startup stalls")
cz_patch gnome-shell 0001-st-theme-index-selectors.patch
# screen recordings through VA-API at constant QP 26 instead of VBR 2 Mbit/s,
# which starved moving scenes at 2400x1080 (NOTES "Screen recording: frame
# pacing and quality")
cz_patch gnome-shell 0002-screencast-constant-quality-for-va-api.patch
