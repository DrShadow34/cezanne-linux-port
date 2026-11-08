# PanVK v9 for DXVK

Proton runs Direct3D 9-11 through DXVK, which refuses a Vulkan device
without these features (dxvk_device_info.cpp, required = true). PanVK on
the G77 (Valhall v9) had them all false (2026-11-01):

| Feature | Plan | State |
|---|---|---|
| multiViewport | 16 viewports, the rasterizer uses viewport / scissor 0: correct while no shader writes ViewportIndex | done (0004) |
| robustBufferAccess2 | v9 bounds-checks buffer loads in hardware (descriptor access) and SSBO stores in software; upstream claims it from v11 only. Texel buffers (LEA_BUF + converting load / store on Valhall) get a bounds check against the view (0013): past it, reads load from zero-filled memory (a common sysval, the device's null index buffer) so the format conversion supplies (0,0,0,1) for formats without alpha, writes / atomics go to the OOB address and are dropped. robustness2 1/20: 699 pass, 0 fail | done (0013) |
| shaderClipDistance | no clip distances in hardware: the vertex shader writes them as generic varyings, the fragment shader interpolates them and discards where one is negative; which ones the vertex shader writes is a draw-time sysval, so separately compiled fragment shaders (DXVK uses graphics pipeline libraries) work. Draws whose vertex shader writes none keep early ZS / FPK (second early ZS table without the discard) | done (0007) |
| clip distances, cost | DXVK's D3D9 vertex shaders write all six user clip planes, 0.0 when disabled: the fragment discard then ran in 97% of the draws (no early ZS / FPK). 0015 drops components only ever set to a constant >= 0 (DXVK's optimized pipelines): Ziggurat 14.7 -> 25 fps | done (0015) |
| shaderCullDistance | as clip distance, per fragment (should be per primitive: a partly negative primitive is cut instead of kept); exact only with a geometry stage. CTS: the 14 clipping.user_defined.clip_cull_distance.vert.N_M cases fail on this, pure clip distance passes | approximated (0007) |
| fillModeNonSolid | POINT: the same vertices as a point list (0006). LINE: the edges of each triangle as a line list written by a precomp job ahead of the draw (panlib_rewrite_indices_v9, 0012); the same job drops adjacent vertices for adjacency topologies without a GS. No face culling of the edges (CTS rasterization.culling.*_line fail); indirect draws / primitive restart in strips draw filled | done (0006, 0012) |
| geometryShader | Mali has none. Mesa's common emulation src/poly (from Asahi, as Honeykrisp): the vertex shader runs as a compute job storing its outputs to memory (PANVK_VS_VARIANT_SW, attributes through load_attribute_pan with explicit IDs), the geometry shader as a compute job writing the index buffer of dynamic topologies (PANVK_GS_VARIANT_MAIN, skipped for static ones) and as the hardware vertex shader of the draw (PANVK_GS_VARIANT_RAST, runs the GS once per output vertex, linked with the FS). Both compute jobs go into the draw's vertex/tiler chain ahead of the IDVS job. CTS geometry.*: 188 pass, 1 fail (primitive restart), 10 skip (side effects, GS point size, more than 32 invocations: features not claimed); clip distances through a GS pass. Indirect draws and primitive restart in strips (0012): poly's GPU-side setup from a device heap (restart unrolled into it first), a setup job patching the compute and IDVS jobs' counts; all of geometry.* passes forced through that path too. Missing: transform feedback; adjacency without a GS with indirect draws / restart (skipped) | done (0011, 0012) |

All are claimed on v9 since 0014 (0005's `PANVK_DEBUG=dxvk`, which
claimed them unimplemented for testing, is gone), and the Steam launcher
(apps/steam-arm64, pkgrel 4) leaves Proton on DXVK; `PROTON_USE_WINED3D=1
%command%` per game goes back to WineD3D.

Test: dEQP-VK groups per feature (tests/cts-run.sh), then Proton games.
Known gaps: viewport 0 only (gl_ViewportIndex ignored), cull distance per
fragment, LINE fill without face culling, LINE fill / adjacency with
indirect draws or restart, no transform feedback, no tessellation.

Source: src/mesa/mesa-g77 on the host is a git tree, mesa 26.2.5 + one
commit per patch here; `git format-patch` exports them.

0010: layered rendering into cube views (only possible with a GS) loaded
the previous content of layers past the first out of bounds: the frame
shaders sample cube attachments as 2D arrays, the descriptor now matches.

Also needed beyond the feature list: 32 vertex attributes (0009; DXVK's D3D9
fixed-function vertex shader declares 18), unbinding geometry / tessellation
shaders (0008, shader objects). 0009 first moved the VS driver set's dummy
sampler and dynamic buffers to MAX_VS_ATTRIBS (32) in the draw only; the
descriptor lowering kept 16, so vertex shaders read dynamic UBOs from unused
attribute descriptors (vkQuake drew no models, 2026-11-04): fixed in 0009.

First game, 2026-11-01: Ziggurat (Unity, D3D9) through Proton 11 ARM64 +
DXVK 2.7.1 with PANVK_DEBUG=dxvk, launched from Steam: device created,
swapchain 2400x1080 FIFO, the intro renders and runs. Started outside Steam
the game quits by itself ("Error initializing Steam", Ziggurat_Data/
output_log.txt), which first looked like a presentation problem.
