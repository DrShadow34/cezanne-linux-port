# cezanne-remote

Remote control app (GTK4 / libadwaita, Python) for the phone's IR blaster
(`gpio-ir-tx` on GPIO135, `/dev/lirc0`; NOTES "IR blaster").

- TV: brand (searchable) and remote variant (previous / next, "Test" sends
  volume up), power, input, mute, navigation, back / home / menu / settings,
  volume, number keys.
- Air conditioner: brand and remote variant; power, temperature, mode, fan,
  swing, limited to what the remote supports. The whole state goes out with
  every change (as AC remotes do); the state shown is the one last sent,
  saved per remote in `~/.config/cezanne-remote/state.json`.
- Main menu (header bar): Preferences (Ctrl+,) with button size (100-175 %,
  default 150 %) and spacing (50-175 %, default 125 %), applied live and
  saved in the state file; About. Sizes are CSS generated from the factors
  (text grows at 0.7 of the button factor); the widest rows fit a 540 px
  wide (logical) screen at 175 % / 175 %, a wider page scrolls instead of
  widening the window.
- Keys a remote has no code for are greyed out. IRext stores them as "0"
  (2164 placeholders, e.g. LG remote 1 "settings"), which LIRC refuses with
  EINVAL; `build-db.py` drops them and merges zero-length entries, and the
  app checks every code (no zeros, pulse first and last, <= 500 ms an
  entry, <= 1023 entries) before writing it.

Codes: [IRext](https://github.com/irext) (MIT). `build-db.py` turns IRext's
offline database into `remotes.db` (TV keys decoded, AC protocol binaries;
identical variants within a brand merged): 238 TV brands / 2085 remotes,
245 AC brands / 1485 remotes. AC codes come from IRext's decoder, built as
`libirdecode.so` (`BOARD_PC_DLL`, no debug output) and called with ctypes.
Carrier 38 kHz for everything (IRext's decoded data has none).
LG: IRext's "menu" on remote 1 is NEC 04 FB 43, LG's gear / Settings key
(remote 2 has it as "home" and Q.Menu 45 as "menu").

Lean Remote (Android) ships its own pre-generated tables that match IRext:
its "AuxAC5" is IRext AUX remote `new_ac_2807` (AUX, 3 of 12 here); the
frames are identical except the vane field (Lean: fixed position 2, IRext:
auto), neither changes it with swing.

Build / install: `makepkg` with the PKGBUILD here (fetches IRext core and the
database's LFS files at pinned commits, checksums), then `pacman -U`.
