# Mesa for the Mali-G77 (Valhall v9, job manager)

Upstream Mesa 26.2 builds PanVK only for v6/v7 (JM, Bifrost) and v10+ (CSF).
The G77 is Valhall v9 with a job manager: Valhall descriptors and compiler,
JM job chains. Patches against mesa-26.2.5:

- `0001-panfrost-add-mali-g77.patch`: G77 model for the GL driver.
- `0003-vulkan-wsi-wayland-prerotate.patch`: Wayland WSI pre-rotation (below).
- `0002-panvk-valhall-jm-v9.patch`: PanVK v9. Vulkan 1.4, no longer behind
  `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER`, reports conformanceVersion 1.4.6.2 (the
  VK-GL-CTS release it passes, as RADV does; not a Khronos submission).
- `0004`-`0015`: what DXVK needs on v9 and the D3D9 clip plane cost (DXVK.md).
- `0016-pan-perf-...`: hardware counters (panquick, pan_perf) on the G77: the
  dump buffer was sized for 9 cores while the kernel dumps 23 blocks (sparse
  shader_present 0x660067), and the shader core / L2 block counts were
  swapped. Profiling recipe in NOTES "GPU profiling: Risk of Rain 2".
- `0017-panvk-v9-compressed-images-...`: BC / ETC2 / ASTC images with transfer
  usage were LINEAR (panvk added the uncompressed-view flags for vk_meta
  copies); now U-interleaved, copies through a libpan block-copy kernel.
- `0018-panvk-v9-render-pass-overlap-...` (default on, `PANVK_JM_OVERLAP=0`: off):
  vertex/tiler/compute chains of later batches run while earlier fragment
  chains still do (JS1 and JS0 in parallel), with linux-cezanne patch 0081
  (PANFROST_JD_REQ_SHARED_IMPLICIT_SYNC: implicit dependencies only on
  dma-buf shared BOs). Two tiler heaps alternating per batch; a vertex/
  tiler/compute chain waits for earlier fragment chains for query reset /
  end / copy and fragment-stage timestamps, and after a barrier from
  fragment-side to vertex/compute/transfer stages only if it can see what
  they touched (compute jobs, a vertex shader with textures / images /
  storage buffers: vs.reads_memory, or fragment shader stores); a skipped
  barrier stays pending. Not covered: an image a fragment job wrote,
  aliased as a vertex / index buffer. PANVK_DEBUG=startup logs the mode.
  Debug: debug/jmstats.patch (PANVK_JM_STATS=<file> wait statistics,
  /tmp/panvk-serial = serial at runtime). Test: tests/vkoverlap (below).
- `debug/pvtrace.patch` (not part of the build): per-render-pass and
  per-image trace to /tmp/pvtrace.log while /tmp/pvtrace(-img) exists.

## Development tree
src/mesa/mesa-g77, branch `g77-current` (since 2026-11-10): mesa 26.2.5, then
0001-0003 applied as plain diffs and 0004-0018 with git am, one commit each;
`git format-patch -1 --start-number N` writes a patch back here. The older
branch `main` lacks the 2026-11-04 change of 0009 (one line in
panvk_vX_nir_lower_descriptors.c: vertex shader samplers): a build from it
reads 0 from every texture in a vertex shader (vkoverlap's vs check). The
phone's dev build (/home/alarm/mesa-g77/{src,build}, release buildtype,
user alarm) is that tree on mesa 26.2.6 like the package; /home/alarm is
private, so tests copy libvulkan_panfrost.so and an ICD file to /tmp.

## v9 port (jm/panvk_vX_cmd_draw_v9.c and friends)
- Draws: one MALLOC_VERTEX_JOB per draw and layer (IDVS), CSF-style descriptor
  prep (resource tables, SPD, FAU, DCD), tiler context per layer.
- Layers: each layer has a single-layer tiler context with hardware
  `layer_offset = -layer` (the tiler adds it to gl_Layer and keeps [0, 1)); the
  FBD selects internal layer `layer - valhall.layer_offset` = 0. Multiview:
  offset 0, one job per view, gl_Layer writes dropped from multiview-compiled
  vertex shaders (only vk_meta's identical per-view clear rects use them),
  vertex gl_ViewIndex = per-view `layer_id` sysval, fragment shaders use the
  hardware frame argument.
- Indirect draws (incl. KHR_draw_indirect_count, multiDrawIndirect up to
  65535 draws): placeholder job + `panlib_draw_indirect_v9_helper` precomp job
  that patches counts/offsets/index pointer, the VS FAU sysvals (first vertex,
  base instance, draw id - v9 has no hardware draw ID) and the per-instance
  attribute offsets of the draw's own driver set copy.
- Re-issued command buffers: the GPU writes into malloc vertex jobs, so the
  payload is restored from a pristine copy (batch->job_copies) and tiler
  context layer offsets are re-applied before re-submission.
- Compute / precomp / indirect dispatch: COMPUTE_JOB with v9 payload.
- Timestamps: WRITE_VALUE "System Timestamp" job in its own batch, batches
  with timestamps submitted with PANFROST_JD_REQ_CYCLE_COUNT.
- Global priority: LOW listed and accepted on JM (runs at medium).

## MT6885 GPU timestamp (hardware + kernel hold)
GPU_TIMESTAMP only counts while the cycle counter is on and resets with MFG
power (MediaTek: "a mistake by DE"; the BSP gives apps CNTVCT instead), runs at
26 MHz, and needs MFG_TIMESTAMP (mfgcfg+0x130 bit 0) set after every GPU
power-on. No TIMESTAMP_OFFSET register on v9. Kernel (patch 0010,
`timestamp_hold`): the first CYCLE_COUNT submit or SYSTEM_TIMESTAMP query of a
DRM client takes a runtime-PM reference and a cycle counter reference until the
client closes the device, so timestamps are monotonic and comparable for the
client's lifetime (dmesg: "timestamp hold by <comm>"). GNOME Shell/GL don't
trigger it; the GPU still runtime-suspends when no Vulkan timestamp user is
open. The counter still restarts at 0 after system suspend or a GPU reset.
Needed for Vulkan 1.4 (timestamps are required).

- Input attachment local reads: v9 has no "wait for resource read" flow, so
  read-only colour tile loads use the plain LD_TILE wait.
- Re-issued simultaneous-use command buffers (e.g. the same one twice in
  a vkQueueSubmit) wait for the queue before their jobs and tiler state are
  reset, the GPU may still be running the previous instance.
- Index buffers: v9 has no index buffer size. A null index buffer
  (maintenance6) binds a zeroed 256 KiB device buffer, and direct and
  indirect draws clamp the index count to the bound range.
- Tiler heap 512 MiB on v9 (grown on fault): JM has no tiler OOM handling
  and the heap also holds the IDVS varyings; 1M-primitive draws ran out of
  the 128 MiB heap (OOM job fault). All VkDevices of an instance share one
  DRM file (dup'ed fd) and so one GPU VA space: the kernel (patch 0010)
  extends it to 64G on 64-bit page table GPUs and maps heap BOs above 4G,
  regular BOs stay below 4G.
- MSAA image copies go through the graphics path on v9 (no multisample
  storage images; the compute copy only wrote sample 0).
- Lines: strictLines = false. Rectangular lines exactly on a pixel edge
  vanish with some viewport widths (258: x = 16k + 1). Default lines are drawn
  with aligned ends (parallelograms, width along the minor axis) and moved up
  by 1/256 px (viewport offset y) to break ties like the reference rasterizer.
- Flat shading: the tiler context needs first_provoking_vertex too.
- Vertex shaders: v9 IDVS shades one vertex past the end of a draw (vertex
  blocks); its out of bounds attributes read 0 and a shader looping on one
  never ends (job timeout). The vertex index is clamped to the last element of
  the per-vertex buffers (vs.last_vertex sysval, also patched for indirect
  draws). tests/vktrap.vert reproduces it.
- Compute: consecutive dispatches share one compute-only batch (job chain,
  barrier flag, a CACHE_FLUSH job for pipeline barriers) instead of a batch
  and kernel submission each.
- Indirect draws: the draw job waits for its patch job through dependency 1
  (pan_jc_add_job replaces dependency 2 of tiling jobs with the previous one).
- Multiview queries: one query index per view; each view's draw job counts into
  its own occlusion query, Begin/End reset/make available all of them.

## Conformance (VK-GL-CTS 1.4.6.2, vk-default, 2026-10-24)
1/5 sample (650k cases): 153510 pass, 98 fail, no GPU job faults. 96 are the
Arm-waived mirrored_repeat texture offset tests (waiver 4936, upstream skips
them on all Mali); the other 2 (conformance_version, non-strict wide lines)
are fixed since and pass on their own, the sample wasn't rerun yet. Full groups with 0 fail: rasterization 5612, copy_and_blit 92417,
synchronization + synchronization2 + memory 19919, draw 6078, indexed /
maintenance5/6 4164, glsl.loops 624, multiview 571, timestamp/calibrated 389.
Run on the phone: tests/cts-build.sh (VK-GL-CTS + deqp-runner in /var/tmp/cts),
tests/cts-run.sh <name> <fraction> <overlap 0|1> [caselist] (deqp-runner, 5
jobs: ~3 min for 1/50, ~28 min for 1/5).

2026-11-11 (0001-0018, PANVK_JM_OVERLAP=1): ordering groups in full (654k)
202884 pass / 279 fail, 1/5 sample 159458 pass / 322 fail / 75 crash; all
of them also fail with overlap off (NOTES "Render-pass overlap"). New since
10-24: DXVK feature gaps (multi-viewport, adjacency + indirect, LINE / POINT
depth bias, GS + multiview), fast-linked GPL multisample min_sample_shading,
and a host image copy crash on compressed optimal images.

## Render-pass overlap test (tests/vkoverlap)
`build.sh <dir>` (glslangValidator, cc -lvulkan), `vkoverlap <dir>`, run as
arch with VK_DRIVER_FILES. Default: per frame, a slow fragment pass and then
what depends on it on the vertex/tiler/compute side (vertex shader read,
compute read, copy, occlusion query copy, write after read, a barrier a
pass without memory access skips and the dispatch after it, a fragment
shader store read as a vertex buffer), checked per frame. BAD=1 / BAD2=1 /
BAD3=1 use an insufficient barrier for the vs / vin / cs2 check: with
overlap every frame must fail (the check sees the race), without none.
ONE=1 submits all frames at once. PERF=1: heavy vertex + heavy fragment
passes, ms per frame; BROAD=1 with DXVK-style barriers. Overlap is on by
default; PANVK_JM_OVERLAP=0 gives the serial reference. 2026-11-11, kernel
7.3.1-30 (overlap then opt-in): all checks pass with and
without overlap, each BAD fails 40/40 only with overlap, PERF 59 -> 49 ms
per frame (also BROAD).

## Install (phone)
Packaged since 2026-11-02: the overlay packaging/overlays/mesa applies these
patches to ALARM's mesa (packages mesa, vulkan-panfrost, ...; ICD
/usr/share/vulkan/icd.d/panfrost_icd.json). The old dev install
(/opt/mesa-dev, ICD panfrost_icd.aarch64.json) is gone; a dev build to test
goes to a prefix of its own and is picked with ICD=<its icd.json> in the
tests/ scripts (VK_ICD_FILENAMES), never over the packaged files.

## Tested applications
- vkcube, vkmark 2025.01 (headless: 800x600 score 3284, 1920x1080 score 1281).
- vkQuake 1.37.0 (Arch package; requests Vulkan 1.1, uses compute lightmaps
  and indirect draws): LibreQuake and Quake Brutalist Jam III (77 maps,
  /home/arch/quake, app grid "Brutalist Jam 3 (vkQuake)" with MangoHud). Several
  levels played with a Bluetooth DualShock 4, no issues, no GPU faults.
  (Since 2026-10-27 the data is in ~/Documents/Cezanne-Development/quake.)
  2026-11-04: no models (monsters, items, the weapon) from 0009 until the fix
  of its VS dummy sampler / dynamic buffer index: a vertex shader's dynamic
  UBOs (vkQuake's per-entity data) came from unused attribute descriptors.

## Performance notes (2026-10-24, vkQuake + Quake Brutalist Jam III)
- qbj3_chadquad at native resolution: 24.9 FPS under GNOME. GPU busy 96%
  (kernel job timeline): vkQuake vertex/tiler/compute 67%, fragment 27%,
  gnome-shell compositing 25% (~5 ms per composite, never direct scanout:
  the panel is portrait and rotated, also no scanout in portrait).
- Not the limit: resolution (r_scale 1/2/4 all 24.9), FIFO vs mailbox, linear
  vs AFBC WSI buffers, CPU idle states, batch/job serialization. LibreQuake
  e1m1: 61 FPS, so the jam's geometry is heavy.
- Without a compositor (gdm stopped, SDL kmsdrm + VK_KHR_display with the
  mediatek-drm card0 as display fd, test-only hack): 40.6 FPS. panvk passes no
  display fd to wsi_device_init, so VK_KHR_display finds no displays.
- Ironwail (GL) doesn't run: needs vertex shader storage blocks, panfrost GL
  exposes none.

## Wayland pre-rotation (0003, all Vulkan drivers' common WSI)
The panel is portrait and runs rotated 90 degrees; the display engine (MTK
OVL) can't rotate by 90, so mutter composited every frame of a landscape
fullscreen game (rotation in GL, ~5 ms GPU per frame). With a single output
rotated by 90/270 (queried over a connection of our own: wl_output objects
bound on the app's connection would appear in its wl_surface.enter events and
crash SDL), the swapchain presents through a buffer blit that rotates the
image (wsi-rotate/wsi_rotate.comp, SPIR-V embedded in wsi_rotate_spv.h) into
a linear buffer of the transposed size, attached with set_buffer_transform.
mutter then scans it out directly ("Assigning scanout to stage view") and
does no GPU work for the game. 8-bit RGBA/BGRA (UNORM/SRGB) formats only,
default on, MESA_VK_WSI_PREROTATE=0 disables it. vkQuake (same settings):
composited 50.4 FPS, pre-rotated 54.5. tests/vkrot.c checks scanout of a
pre-rotated buffer.
