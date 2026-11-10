# Kernel patches

Base: mainline Linux **v7.3.1**. The patches apply in numeric order with
`patch -Np1` on a clean `git archive v7.3.1` tree (that is what
`packaging/linux-cezanne/PKGBUILD` does); most are plain diffs without a
mail header. Kernel configuration: `packaging/linux-cezanne/config`.

Not in the patches:
- the device tree (`dts/`): built separately with the kernel's
  dt-bindings headers by `boot/mkboot-mainline.sh`, which also wraps the
  kernel in the boot shim (`boot/shim`) for the phone's lk bootloader;
- the out-of-tree drivers (`drivers/`), built with `make M=...` against
  the patched tree, and the MediaTek vendor drivers for Wi-Fi, Bluetooth,
  GNSS and FM, which are patches to MediaTek's public GPL repositories
  (`drivers/connectivity/README`).

## SoC and system

| Patch | What it does |
|---|---|
| 0002 | MT6885 clock driver: topckgen, apmixedsys, infracfg, peripheral gates and resets (new `clk-mt6885.c`, dt-bindings) |
| 0008 | MediaTek clock core: size the clock table by the highest id (sparse MT6885 ids) |
| 0020 | Camera power domains and camsys clock gates (`clk-mt6885-cam.c`) |
| 0022 | Display platform: disp power domain, multimedia clocks (`clk-mt6885-mm.c`), mmsys routing and mutex |
| 0042 | MDPSYS and SMI clock gates |
| 0044 | Keep MAINPLL always on: the bus runs from it, and the first consumer that dropped it hung the SoC |
| 0046 | Video codec, MDP and ADSP power domains, vcodec clocks (`clk-mt6885-vcodec.c`), clock flags, unused-clock gating for low power |
| 0053 | Low-power node: apply its assigned clocks (mdp_sel off TVDPLL) before dropping unused clocks |
| 0006 | MT6885 pin controller (`pinctrl-mt6885.c`) |
| 0009 | MT6885 power domains: GPU (MFG) domains and the domain table (`mt6885-pm-domains.h`) |
| 0013 | CPU frequency scaling through the MCUPM coprocessor (`mediatek-cpufreq-mcupm.c`) |
| 0032 | Energy model for the MCUPM cpufreq driver (EAS) |
| 0060 | MCUPM cpufreq: OPP tables, so SVS can program per-chip voltages |
| 0033 | PSCI cpuidle states and CPU hotplug through the SPM (`mt6885-cpuhp.c`) |
| 0059 | SVS / EEM voltage trimming for the CPU clusters, CCI and GPU |
| 0064 | SVS: complete init02 after a temperature read error, keep the clock |
| 0077 | SVS: retry the probe while the MCUPM still holds the CPU bucks at lk's voltage (otherwise the CPU banks stay untrimmed) |
| 0012 | LVTS thermal sensors and controllers for MT6885 |
| 0062 | LVTS: wait a full measurement cycle before trusting the first reading |
| 0063 | LVTS: reprogram the controllers after deep suspend (it wipes them) |
| 0045 | Arm the MCUPM low-power control for the SPM (`mt6885-lpm.c`); without it deep suspend ends in an SPM watchdog reset |
| 0047 | `/proc/cpuinfo` model name and DRAM information (`mt6885-dram-info.c`) |
| 0015 | RTC: the MediaTek BSP's year encoding (years since 1968) |
| 0016 | Watchdog: stop a watchdog left running by the bootloader across suspend |
| 0019 | I2C: the AP channel offset of the multi-channel controllers |
| 0055 | I2C: restore the multi-channel DMA mode through the ATF after deep suspend |
| 0043 | IOMMU: camera larb 13 in region 0 |
| 0071 | SoC AUXADC: MT6885 compatible with its efuse gain / offset calibration (board thermistors) |
| 0072 | Hardware RNG through the phone ATF's SMC (`MTK_SIP_KERNEL_GET_RND`) |
| 0076 | `ARCH_MEDIATEK` selects `HAVE_SHARED_GPIOS` (the board shares GPIOs between consumers) |
| 0003 | `earlycon=ramlog`: early console into a RAM buffer that survives a reset (the phone has no UART) |
| 9999 | Debug only: early ramlog markers in `head.S` (only with the ramlog earlycon configured) |

## Display and GPU

| Patch | What it does |
|---|---|
| 0005 | Bring-up: refresh the command-mode panel from the boot framebuffer (`mtk-dsi-cmd-kick.c`, before mediatek-drm drove the panel) |
| 0011 | Bring-up: cache maintenance and refresh hooks in the sysfb framebuffer |
| 0021 | Bring-up: recover a stuck display pipe in the refresh driver |
| 0023 | mediatek-drm on MT6885: command-mode DSI with DSC, the Xiaomi J10 panel driver (`panel-xiaomi-j10-38-08-0a.c`) |
| 0024 | Dual display pipe for 120 Hz |
| 0025 | Display suspend: SMI larb bypass, display domain kept on |
| 0026 | Display domain power-off: bus protection, golden settings |
| 0027 | Display IOMMU ports, TE interrupt as the frame trigger |
| 0028 | J10 panel: TE at a fixed scanline |
| 0029 | HDR: PQ / BT.2020 output, HDR metadata, high brightness mode (`mtk_hdr_tables.h`) |
| 0030 | HDR: 10-bit overlay path, 1200 nits peak |
| 0031 | Panel features: power-on timing, HBM, DC dimming (CCORR gain), idle refresh 120 -> 60 Hz, colour modes, always-on display |
| 0034 | Complete the page flip at frame start |
| 0065 | Keep the boot logo on screen until plymouth takes over |
| 0066 | `dirtyfb` for command mode (damage-driven updates) |
| 0075 | Command-mode frames through the GCE command queue (configure, wait for TE, trigger in one packet; CPU fallback), CMDQ physical-address writes |
| 0078 | DSI: keep the interrupt masked while the engine is unclocked (an unackable TE stormed CPU0 and hung suspend entry) |
| 0010 | panfrost: Mali-G77 MC9 on MT6885 (GPU id, power domains, clocks, devfreq) |
| 0054 | panfrost: unprepare the clocks in system suspend (the MFGPLL only stops when unprepared) |
| 0061 | panfrost devfreq: follow OPPs enabled / disabled by SVS |

## Multimedia

| Patch | What it does |
|---|---|
| 0017 | ASoC: the MT8192 audio front end on MT6885 and the cezanne sound card (MT6359 codec + 2x CS35L41) |
| 0018 | CS35L41: the legacy CSPL firmware has no pause / resume |
| 0037 | CS35L41: start the DSP only once the amp is clocked |
| 0038 | USB-C headset jack from the Type-C port's audio accessory mode |
| 0049 | Hostless speech DAI for voice calls (modem on PCM 2) |
| 0074 | mtk-jpeg on MT6885: 16 MP pictures, encoder stall IRQ, drain / LAST handling, unsupported headers rejected, timestamps |

## USB, Type-C and charging

| Patch | What it does |
|---|---|
| 0001 | T-PHY: `mediatek,force-vbus` (VBUS is sensed by the charger IC, not the PHY) |
| 0004 | mtu3: MAC-level VBUS force (`mediatek,force-vbus`) |
| 0041 | mtu3: let xHCI finish the disconnect before the port goes to the device MAC |
| 0051 | mtu3: no IP sleep wait without a Type-C partner (deep suspend on battery) |
| 0067 | mtu3: read control-OUT data once a request is queued (UVC) |
| 0068 | mtu3: GPD ring for UVC isochronous requests |
| 0069 | UVC gadget: disable the endpoint on alternate setting 0 |
| 0070 | mtu3: suspend the device MAC without a bound gadget |
| 0035 | TCPCI MT6360: enable the alerts |
| 0040 | TCPCI MT6360: source VBUS (OTG) from the MT6360 boost |
| 0039 | FSA4480: MIC / GND swap fallback when detection misses a headset |
| 0014 | MT6359 fuel gauge (`mt6359-gauge.c`) |
| 0036 | TI BQ25970 charge pump for direct charging (`bq25970_charger.c`) |
| 0073 | Charge limit: gauge start / end thresholds and the MT6360's `charge_behaviour` |

## Power management fixes

| Patch | What it does |
|---|---|
| 0050 | MDP powers off at every suspend: SMI noirq clocks per node |
| 0052 | Keep the MDP domain on (superseded by 0056) |
| 0056 | VDEC, VDEC2 and MDP domains off when idle |

## Input and NFC

| Patch | What it does |
|---|---|
| 0007 | Goodix GT9896 ("yellowstone") touchscreen driver (`goodix_yellowstone.c`) |
| 0057 | GT9896: sleep in system suspend |
| 0058 | GT9896: wake late, sleep only once its firmware has booted (AOD standby current) |
| 0048 | nxp-nci: don't unregister under `info_lock` (deadlock when neard keeps the device up) |
