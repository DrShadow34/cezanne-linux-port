# cezanne packages: everything the port changes comes from pacman

Goal: the phone updates with a plain `pacman -Syu`. Every file the port adds
or changes belongs to a package built from a PKGBUILD in this project, and
patched Arch packages follow Arch's updates by themselves.

## How it fits together

- **[cezanne] repo** on the phone: `/var/lib/cezanne/repo` (file://, local
  only, `SigLevel = Optional TrustAll`), listed **before [core]** in
  `/etc/pacman.conf`. pacman takes a package from the first repo that has its
  name, so our builds of patched packages (mutter, libcamera, ...) shadow
  ALARM's instead of being held back with IgnorePkg.
- **packages.list**: every package, in build order, as `NAME TYPE DIR`:
  - `own`: our PKGBUILD in DIR (our software, firmware, rootfs integration).
    Rebuilt when DIR's contents change. Bump pkgrel (or pkgver) with every
    change, or pacman keeps the old build (cezanne-build warns).
    cezanne-base and cezanne-pkgtools take their version from the build time
    (pkgver()). A PKGBUILD sees only its own directory (`$startdir`, bound
    into the chroot as /startdir).
  - `overlay`: a patched Arch package. DIR has `upstream` (`UPSTREAM=` Arch
    pkgbase, `SUFFIX=` our revision) and `cezanne.sh`, appended to the
    *current* upstream PKGBUILD together with `overlay-lib.sh` (`cz_patch`,
    `cz_source`, `cz_after`, `cz_before`). Upstream = ALARM's PKGBUILDs repo
    if ALARM modifies the package, else Arch's packaging GitLab at the tag of
    the version in the ALARM sync db. Our pkgrel = upstream pkgrel + `.SUFFIX`:
    bump SUFFIX with every overlay change while upstream's version stays, or
    the rebuild has the published file name and is not published.
    Rebuilt when ALARM ships a new version or the overlay changes: no
    hand-merging of upstream PKGBUILDs. `FROM=arch` in `upstream`: a package
    ALARM does not carry (mangohud); its version is Arch's x86_64 [core] /
    [extra] one (archlinux.org package search) and the PKGBUILD Arch's at
    that tag, so it follows Arch's releases the same way.
  - `track`: our PKGBUILD for something built against an upstream package
    (`upstream`: `PROBE=` that package); the PKGBUILD gets its version as
    `$CZ_UPVER` (a line cezanne-build puts at the top of the build copy).
    gst-plugins-cezanne: the patched v4l2codecs / va plugins in
    `/usr/lib/gstreamer-1.0-cezanne`, preferred through `GST_PLUGIN_PATH`
    (GStreamer keeps the first plugin of a file name), so Arch's 73 GStreamer
    packages stay as they are.
  - `follow`: our PKGBUILD for software released outside the distro repos
    (steam-arm64: Valve's arm64 client, runtime and launcher). DIR has an
    executable `upstream-version` (prints what is followed, one line) and
    `update-sources.sh` (pins the build copy's PKGBUILD to the current
    release: versions, sources, checksums). Rebuilt when that line or the
    directory changes. Steam follows the runtime and the launcher, not every
    client release (the client updates itself in ~/.local/share/Steam).

Nothing is pinned by hand any more: own packages are our software, every
other package follows its upstream (ALARM, Arch via FROM=arch, or Valve).
- **cezanne-build** (phone, root; `pkgtools/cezanne-build`, package
  cezanne-pkgtools): prepares stale packages in
  `/home/alarm/cezanne-build/NAME` (logs `NAME.log`, `chroot.log`) and
  builds each in a clean copy of the chroot `/var/lib/cezanne/chroot/root`
  (a btrfs subvolume, so the copy is a snapshot; created by mkarchroot when
  missing) (devtools-alarm `makechrootpkg`, as alarm; the system's pacman.conf without
  IgnorePkg; [cezanne] bound in read-only, so our packages are build
  dependencies too). Build dependencies never touch the installed system.
  Upstream versions come from sync dbs of its own (`/var/cache/cezanne-build/db`,
  as checkupdates): the system's sync dbs are only refreshed by your
  `pacman -Syu`. Downloads are kept in `/var/cache/cezanne-build/sources`
  (git mirrors, tarballs, FEX's 1 GB image); packages are zstd. A build
  needs 3 GB free on the root file system, or what the package dir's
  `.minfree` says (fex-guestos-mesa: 16), or it is skipped. Publishes with
  `repo-add -R` (never over a version already in the repo) and records each
  package's key in `/var/lib/cezanne/built/NAME`. `--status` shows ok /
  STALE, `-f` forces, `-n` is a dry run.
- **cezanne-build.timer**: daily 03:30 (on AC power only), so a new ALARM
  mutter / libcamera / GStreamer is followed by our rebuild before or soon
  after `pacman -Syu` wants it. Until then pacman keeps our older build (or,
  if an Arch package needs the newer one, `-Syu` stops with a dependency
  error instead of breaking the patch).
- **cezanne-pkg** (laptop): `sync` pushes `packaging/` and every package dir
  to `/home/alarm/cezanne-pkg`, after copying the Mali-G77 patches of
  mainline/mesa-g77 into their two users (the mesa overlay and
  fex-guestos-mesa, a track of mesa: guest and host Mesa stay the same
  release with the same patches); `build [NAMES]` syncs and builds (with the
  synced cezanne-build, so changes to it apply at once; the timer runs the
  packaged one); `status`; `host NAME` builds packaging/NAME here
  (linux-cezanne) and adds it to the phone's repo.

## Day to day

    packaging/cezanne-pkg build cezanne-base     # after editing mainline/rootfs
    packaging/cezanne-pkg status
    ssh phone pacman -Syu                        # installs what was built

New patched Arch package: `packaging/overlays/NAME/{upstream,cezanne.sh,*.patch}`
and a line in packages.list. New own package: a PKGBUILD in its directory
and a line in packages.list.

## State (2026-11-02)

Installed on the phone from [cezanne], all built in the clean chroot: the
cezanne-* apps, cezanne-base, cezanne-firmware, cezanne-pkgtools (timer
enabled), cezanne-va (VA-API driver), neard, steam-arm64 (follow);
overlays mutter, xorg-xwayland (20:9 modes), mangohud and mission-center
(Arch's, FROM=arch; MediaTek sensors / cezanne-top), libcamera, mesa (PanVK +
the G77 patches,
trimmed driver set), modemmanager (mtk-ccmni plugin), snapshot (the camera
app fork, + cezanne-photo helpers), realesrgan-ncnn-vulkan (+ models),
cezanne-upscale (x4plus on the APU, its int16 model built from
realesrgan-ncnn-vulkan's; cezanne-upscaled.socket enabled by its install);
gst-plugins-cezanne and fex-guestos-mesa (tracks); linux-cezanne (host build: `cezanne-pkg host linux-cezanne` after a
pkgrel bump; the hook flashes the boot slot, the old image goes to
/var/lib/cezanne/boot-prev.img). No IgnorePkg: `pacman -Syu` takes everything.

The cezanne-* apps include cezanne-gallery and cezanne-storage (2026-11-02):
new packages are installed once by name (`pacman -S`), then follow -Syu.
An "own" package with a fixed pkgver needs a pkgrel bump for every change:
a version already in the repo is never republished.

Not yet:
- devtools (/usr/local/bin test tools).
- leftovers removed 2026-11-02 (/opt, the /usr/local MangoHud copy and
  bwrap wrapper, Android-era drop-ins; backup in
  build/backups/phone-leftovers-20261102.tar.zst); the files the packages
  replaced are kept in /var/lib/cezanne/leftovers-20261102.
