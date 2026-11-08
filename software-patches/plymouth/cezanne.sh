# GDM's `plymouth deactivate` left the last spinner frame on screen: the erase
# of the stopped throbber was drawn but never flushed before master was dropped
# (NOTES "Spinner stops")
cz_patch plymouth 0001-boot-splash-flush-displays-when-becoming-idle.patch
