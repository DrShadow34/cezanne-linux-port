# Redmi K30 Ultra (cezanne) hardware under Arch Linux

Phone: Redmi K30 Ultra, M2006J10C, MediaTek Dimensity 1000+ (MT6885).
Kernel: mainline 7.3.1 + linux/patches 0001-0083 + the out-of-tree drivers
in linux/drivers, package linux-cezanne (linux/packaging); userspace: Arch
Linux ARM + niri with the port's own shell (shell/cezanne-shell, since
2026-11-13; GNOME before, see SHELL.md), its own software (gnome-apps) and
patched packages (software-patches), all built from PKGBUILDs so the phone
updates with a plain `pacman -Syu`. State as of 2026-11-14 (linux-cezanne
7.3.1-33); open items at the end. Hardware gaps
found by comparing the stock MIUI device tree with the running phone are
marked "(2026-11-04)"; a second pass on 2026-11-08 also compared MIUI's
active interrupts and every enabled MMIO block / I2C device by address.

Origin of each driver:
- **mainline**: existing upstream driver, used as is or with small fixes;
- **adapted**: upstream driver or framework extended for the MT6885 / this
  phone (new SoC data, missing features), usually modelled on the MT8192;
- **ported**: MediaTek / Xiaomi GPL vendor driver moved to mainline 7.x
  (out of tree, patched);
- **new**: written for this port (register layouts from vendor GPL
  sources, MIUI captures or reverse engineering of MIUI's blobs).

Status: **works**, **partial** (works with gaps), **open** (not working /
not started).

## SoC and system

| Hardware | Driver / stack | Origin | Status | Firmware |
|---|---|---|---|---|
| CPU 4x Cortex-A55 2.0 GHz + 4x Cortex-A77 2.6 GHz | cpufreq through MCUPM (0013, 0060), energy model (0032), PSCI cpuidle + hotplug (0033), uclamp touch boost | new (cpufreq), mainline (idle) | works; under sustained game load the big cores hold 1.75-1.93 GHz (power_allocator, see thermal sensors) | MCUPM firmware (`mcupm` partition, loaded by lk) |
| Voltage trimming (EEM / SVS: CPU clusters, CCI, GPU) | mtk-svs + MT6885 data (0059), MCUPM OPP tables (0060), panfrost OPP notifier (0061), init02 on a temp error (0064) | adapted (MT8192 / MT8186 SVS) | works: per-chip voltages, 30-90 mV below the signoff tables, re-run on every resume | none (per-chip efuse data) |
| Clocks, resets (topckgen, apmixed, subsystems) | clk-mt6885 (0002, 0008, 0042, 0044) | adapted (MT8192) | works | none |
| Pin control / GPIO | pinctrl-mt6885 (0006) | adapted (MediaTek paris) | works | none |
| Power domains (SCPSYS) | mtk-pm-domains + MT6885 domains (0009, 0020, 0022, 0026, 0046, 0050, 0052, 0056) | adapted | works | none |
| IOMMU / SMI larbs | mtk_iommu, mtk-smi + MT6885 data (0043) | adapted | works | none |
| Thermal sensors (LVTS) | lvts_thermal + MT6885 (0012, 0062), reprogrammed after deep suspend (0063), DT trips | adapted | works (before 0063 every zone was dead after the first deep standby until reboot); big-core zones on power_allocator (switch-on 55 C, control 80 C, 1.1 W; cezanne-base udev rule 69-cezanne-thermal.rules, 7.3.1-32), step_wise on the others; no LVTS hardware protection (RGU) | none (efuse calibration) |
| PMIC MT6359 (regulators, keys, RTC, ADC) | mt6359 regulators / keys / auxadc, rtc-mt6397 (0015) | mainline | works | none |
| Fuel gauge (MT6359) | mt6359 gauge (0014) | new | works | none |
| Watchdog | mtk_wdt (0016) | mainline | works | none |
| DRAM 8 GB LPDDR4X | info only (0047: /proc/cpuinfo model, DRAM info) | new | works; at the bootloader's rate (no DRAM / Vcore DVFS, only the GPU has devfreq); DVFSRC measured 2026-11-08: no idle gain over this state, display stalls / GPU loss at low levels, not pursued | none (set up by the preloader) |
| AI accelerator APU 3.0 (2x MDLA 1.5 + 3x VPU, EDMA, reviser) | cezanne-apu (/dev/cezanne-apu: power-up through SPM + APU RPC + ATF, DMA from a 1 GiB CMA pool above 4 GiB, timeout recovery) + cezanne-upscaled (own MDLA command builder, int16 networks) | new | partial: both MDLA cores at 800 MHz run standard / dilated / strided / depthwise convs + PReLU in int16, exact to the integer models: camera and gallery AI upscaling (Real-ESRGAN x4plus, compact for video), portrait masks (U^2-Net objects / people), portrait depth (MiDaS small); done interrupts (1 ms look first, 7.3.1-13), int8 unused, int16 depthwise adds no bias (CPU), no general NN runtime; VPU and EDMA not started | none (the MDLA runs the command streams cezanne-upscaled builds) |
| GCE command queue (CMDQ mailbox, 0x10228000 / 0x10318000) | mtk-cmdq-mailbox (mt6779 data), mt6885-gce.h; PA writes for blocks without a subsys ID (0075) | adapted | works (2026-11-08, linux-cezanne 7.3.1-11): command-mode display frames go through it (mediatek_drm.cmd_gce, default on: config + wait for the TE event + trigger in one packet, CPU path as fallback); MDP3 (scale / rotate) not set up yet | none |
| CQDMA (general DMA engine, 0x10212000) | none (mtk-cqdma exists upstream) | - | not needed (memcpy offload only) | - |
| Hardware RNG (TRNG at 0x1020f000, behind ATF) | mtk-rng with the phone ATF's call (0072: MTK_SIP_KERNEL_GET_RND 0xc200026a, 16 bytes a call, as the BSP's mt67xx-rng), DT node rng | adapted | works (2026-11-08): /dev/hwrng (rng_current "rng", quality 900); probe checks the call answers | the stock ATF (`tee` partitions) |
| Board thermistors (4 NTCs on the SoC AUXADC, MIUI mtboard-thermistor1-4) | SoC AUXADC mt6577_auxadc + MT6885 compatible and its efuse gain / offset calibration (0071), generic-adc-thermal with the NCP15WF104F03RC table at the BSP's 390 kOhm pull-up to 1.8 V | adapted | works (2026-11-08): thermal zones ap-ntc (AP board, ~SoC infra temperature), mdpa-ntc (4G PA), nrpa-ntc (5G PA), board4-ntc (channel 4, role unknown); monitoring only (no trips); MIUI's thermal charge-current levels not built on them yet | none (efuse calibration) |
| UFS storage | ufs-mediatek | mainline | works | none |
| Suspend / SPM / low-power | s2idle and deep suspend through the ATF / SPM firmware, MCUPM LPM (0045, 0053), the AOD's own sleep cycle (cezanne-aod); research modules (not published) | new | partial: suspend works, standby ~38 mA vs MIUI 5-10 mA (AOD panel ~18 mA) | SPM and SSPM firmware (`spmfw`, `sspm` partitions, loaded by lk), MCUPM (`mcupm`) |
| SCP sensor coprocessor (tinysys) | scp-cezanne, mtk-nanohub, hf-manager (linux/drivers/scp) | ported | works | `mediatek/mt6885/scp.img` = the `scp1` partition (cezanne-firmware); loaded by scp-cezanne, as lk skips the SCP in this boot mode |
| Boot chain (lk -> shim -> kernel) | linux/boot (shim, nolibc init), cezanne-bootsel module | new | works | stock preloader and lk (`preloader`, `lk` partitions) |
| UART / early console | 8250_mtk, earlycon ramlog (0003) | mainline + new | works | none |

## Graphics and display

| Hardware | Driver / stack | Origin | Status | Firmware |
|---|---|---|---|---|
| GPU Mali-G77 MC9 (kernel) | panfrost + Mali-G77 / MT6885 (0010, 0054, 0061), G57 id for containers (0080), implicit sync only on shared BOs for render-pass overlap (0081, 0082) | adapted | works; 350-836 MHz; MFGPLL is hardware-capped at ~854 MHz, CON1 and FHCTL alike (no overclock) | none |
| GPU OpenGL ES / OpenGL | Mesa panfrost + G77 (mesa-g77 0001) | adapted | works (GL 3.1, GLES 3.1) | none |
| GPU Vulkan | Mesa PanVK for Valhall on the job manager (mesa-g77 0002) + DXVK features 0004-0015 (geometry shaders through Mesa's poly from Asahi, clip / cull, wireframe, robustness2), compressed textures (0017), render-pass overlap (0018, on by default) | new (v9 JM), adapted (poly) | works (Vulkan 1.4, DXVK); gaps in software-patches/mesa/DXVK.md | none |
| Display pipeline (OVL, RDMA, DSC, DSI command mode, dual pipe 120 Hz) | mediatek-drm + MT6885 (0005, 0011, 0021-0027, 0029-0031, 0034) | adapted | works (120 Hz, HDR10, AOD, DC dimming + MIUI colour schemes via cezanne-dc-dimming); CRTC has CTM / GAMMA_LUT (no night light: GNOME's was never tested, niri has none); AAL (content-adaptive brightness) and the display resizer unused | none |
| Panel 6.67" AMOLED 1080x2400 (j10) | panel-xiaomi-j10 (0028) | new (vendor panel tables) | works; 60 / 120 Hz only | none (init tables in the driver) |
| Touchscreen Goodix GT9896 | goodix-yellowstone (0007, 0057, 0058) | new | works; no gestures / double-tap wake | none (the GT9896 runs from its own flash) |
| Under-display fingerprint Goodix GF3658 | goodix_fp + cezanne_teei (Microtrust TEE) + cezanne-fp | new | partial: authentication works (MIUI-enrolled fingers; cezanne-shell's lock screen through fprintd's API and pam_fprintd), no enrolment | Goodix's trusted app in the Microtrust TEE (`tee` partitions); the TEE's storage in RPMB |

## Multimedia

| Hardware | Driver / stack | Origin | Status | Firmware |
|---|---|---|---|---|
| Video decoder (H.264, HEVC, VP8, VP9, AV1, 10 bit, AV1 film grain) | cezanne-vdec (V4L2 stateless) + own VA-API driver (libva-v4l2-request fork) | new (RE of MIUI's vpud / libvcodecdrv) | works | none |
| Video encoder (H.264) | cezanne-venc (V4L2 stateful) + VA-API EncSlice | new | works; no HEVC / B frames | none |
| Audio front end (AFE) | ASoC mt8192 adapted to MT6885 (0017, 0049) | adapted | works | none |
| Audio DSP (ADSP, 2 HiFi cores: offload, VoIP / call processing, voice wakeup) | none (lk loads its firmware) | - | open (2026-11-04); not needed for playback / calls | ADSP firmware (`audio_dsp` partition, loaded by lk) |
| Bluetooth SCO audio (BT CVSD block, mtk-btcvsd-snd at 0x18830000) | none: CONFIG_SND_SOC_MTK_BTCVSD off, no DT node | - | open (2026-11-04): BlueZ offers Handsfree, but headset call audio / mic likely silent | - |
| Speaker amps 2x Cirrus CS35L41 | cs35l41 (0018, 0037) | mainline + fixes | works (stereo) | `cirrus/cs35l41-dsp1-spk-prot-cezanne-{bottom,top}.{wmfw,bin}` + `cs35l41-delta-*.txt` (vendor `/firmware`, renamed per amp; cezanne-firmware); factory calibration read from the `persist` partition (`audio/cs35l41_cal.txt`) at boot |
| Codec MT6359 (mics, earpiece) | mt6359 ASoC | mainline | works | none |
| USB-C analog audio (FSA4480) | fsa4480 (0039), Type-C headset jack (0038) | mainline + fixes | partial: stereo out and the headset mic work (checked 2026-10-25 with a Xiaomi analog USB-C adapter); headset buttons and mic detection (3- vs 4-pole) not implemented and not tested: no headset with buttons to test with (2026-11-08). The hardware for it: the MT6359's accdet (its MFD cell exists, unbound; mainline mt6359-accdet is CONFIG off and expects a 3.5 mm jack, MIUI tied it to the FSA4480 with 3 / 4-key thresholds) | none |
| Main camera Sony IMX682 64 MP | imx682 | new (vendor tables) | works | none (register tables in the driver) |
| Front pop-up camera Samsung S5K3T2 20 MP | s5k3t2 | new | works | none (register tables in the driver) |
| Ultrawide HI1337, macro S5K5E9, mono OV02B1B | cezanne_sensor (table driven) | new | works | none (register tables in the driver) |
| Camera front end (seninf CSI-2 RX, CAMSV raw DMA) | cezanne_camsv, v4l2-cci | new | works (raw capture; frames the DMA did not write are dropped, 7.3.1-12; processing on CPU / GPU / APU, no hardware ISP) | none |
| Camera flash (2 LEDs on the MT6360) | leds-mt6360 flash, Snapshot flash modes, cezanne-flashlight (Quick Settings torch) | mainline | works | none |
| JPEG encoder / decoder (0x17030000 / 0x17040000, VENC domain) | mtk-jpeg (0074: 16 MP bitstream size, stall IRQ, drain / LAST, preset size, header checks, timestamps) | adapted | works (2026-11-08): v4l2jpegenc (Snapshot's pictures, 16 MP in ~40 ms + NV12 conversion vs ~250 ms jpegenc) and v4l2jpegdec (baseline 4:2:0 / 4:2:2, ranked below jpegdec); the second encoder core unused | none |
| Camera processing blocks (ISP P1 raw cam1-3 / DIP, CCU, MFB multi-frame blend, WPE warp, MSF / MSS multi-scale, FE / RSC, FDVT face detection, MDP scale / rotate) | none (MDP3 built, no DT node) | - | open: all camera processing on CPU / GPU (2026-11-04: FDVT / WPE / MFB listed from the MIUI DT; 2026-11-08: MIUI used CCU, FDVT, RSC, MSS, JPEG during capture) | - |
| Stereo depth engine DPE (DVS / DVP at 0x1b100000) | none | - | open (2026-11-08): hardware disparity; the stereo portrait runs on the CPU (minutes), the APU's depth path (MiDaS) replaces it for most pictures | - |
| Camera calibration EEPROMs (main 0x50, ultrawide 0x51, macro 0x52, 0x54) | none at runtime: dumped once (eedump), shading and the AWB point in the libcamera tuning files | new (tools) | works for this phone | none (data read once into the libcamera tuning files) |
| Focus VCM DW9719 | dw9719 | ported | works | none |
| Camera LDO FAN53870 | fan53870 | new | works | none |
| Camera app | Snapshot fork (gnome-apps/snapshot) with libcamera backend, cezanne-photo post-processing, cezanne-upscaled on the APU | adapted | works (RAW / DNG, HDR, portrait with APU masks and depth, low light, AI upscaling, hardware JPEG and video) | none |
| Pop-up camera motor (TI DRV8846) | popup-motor | new | works | none |
| Pop-up position hall sensor AK09970 | ak09970 (IIO) | new | works (cezanne-popupd: retracts on free fall) | factory calibration from the `persist` partition (`sensors/hall_calidata.txt`, copied by cezanne-popupd on first start) |

## Connectivity

| Hardware | Driver / stack | Origin | Status | Firmware |
|---|---|---|---|---|
| Wi-Fi 6 (CONNSYS CONNAC2 + MT6635) | conninfra, wlan-adaptor, wlan gen4m (linux/drivers/connectivity: MediaTek's repositories + patches) | ported (MediaTek out-of-tree, Motorola GPL release) | works; throughput / tx power tuning open; AP (hotspot) and P2P modes offered by the driver, untested (2026-11-04) | `WIFI_RAM_CODE_soc3_0_1_1.bin`, `soc3_0_ram_{wifi,mcu,wmmcu}_*_hdr.bin`, `soc3_0_patch_wmmcu_1_1_hdr.bin`, `wifi.cfg`, `conninfra.cfg` (vendor `/firmware`; cezanne-firmware); calibration and MAC address from the `nvdata` partition (`APCFG/APRDEB/WIFI`, `WIFI_CUSTOM`) at start |
| Bluetooth 5.1 | mtk_btif, bt-mt66xx | ported | works (reconnect fix); A2DP / HID untested, call audio needs BT CVSD (Multimedia) | `soc3_0_ram_bt_1_1_hdr.bin`, `soc3_0_ram_bt_1a_1_hdr.bin`, `BT_FW.cfg` (vendor `/firmware`; cezanne-firmware); address from the `nvdata` partition (`APCFG/APRDEB/BT_Addr`) |
| GNSS (L1 + L5) | gps data link driver + cezanne-gnss receiver + GeoClue | ported (kernel), new (userspace) | works | `mediatek/cezanne/gnss-l{1,5}.bin`: the DSP RAM code inside vendor `/lib64/libmnl.so` (extracted by cezanne-firmware) |
| Cellular modem (5G, MT6885 md1) | cezanne_ccci (CCIF, DPMAIF, ports) + cezanne-modem | new (modelled on the BSP) | partial: SMS, data, call downlink; uplink audio untested; SIM not seen at MD cold boot until re-inserted | modem image (`md1img` partition, loaded by lk); NVRAM (IMEI, calibration) copied from the `nvdata`, `nvcfg`, `protect1`, `protect2` partitions (cezanne-modem-nvram-import); OTA configs `mediatek/cezanne/mdota/` (vendor `/etc/mdota`; cezanne-firmware) |
| SIM tray switch (GPIO 43, MIUI xiaomi,simtray-status) | none | - | open (2026-11-04): unused; a lead for the SIM cold-boot detection | - |
| NFC NXP SN100U | nxp-nci (0048) + neard + cezanne-nfc | mainline + fix | partial: tag read / write, no card emulation | none (the controller runs from its own flash) |
| IR blaster | gpio-ir-tx + Remote Control app (IRext) | mainline (driver), new (app) | works | none |
| USB (device, host / OTG) | mtu3 (0004, 0041, 0051, 0067-0070, 0083), tphy (0001); device modes from cezanne-usb: UVC webcam (screen / cameras), MTP (uMTP-Responder), RNDIS developer network | mainline + fixes | works (high speed only; isoc 1024 B per microframe, no high-bandwidth); deep suspend resets the SSUSB IP, mtu3 sets it up again on resume (0083, 2026-11-14); a host that autosuspended the phone misses mode switches; the RNDIS network does not share the phone's internet (no NAT) | none |
| USB-C / PD (MT6360 TCPC) | tcpci_mt6360 (0035, 0040), cezanne-tcpc-guard | mainline + fixes | works: PD / PPS sink, source (OTG); a power bank whose USB-C port only takes power gets charged by the phone (DRP, Try.SNK) | none |
| FM radio (MT6635) | fmradio_drv (linux/drivers/connectivity: the 4.14 BSP driver + patches/fmradio.patch), DT fm node, cezanne-fm.service, FM Radio app (gnome-apps/cezanne-fm) | ported (4.14 BSP driver, CONNAC2 / conninfra), app new | driver + app work (tune, scan, audio into PipeWire); no usable reception: RSSI floor -114 dBm with or without a wired USB-C headset, the board likely has no FM antenna connection | `mt6635_fm_v1_patch.bin`, `mt6635_fm_v1_coeff.bin`, `fm_cust.cfg` (vendor `/firmware`; cezanne-firmware) |

## Power and charging

| Hardware | Driver / stack | Origin | Status | Firmware |
|---|---|---|---|---|
| Charger MT6360 | mt6360 charger + mt6360-chg-setup | mainline | works (its own BC1.2 reports SDP for dedicated chargers: the XMUSB350's type is used) | none |
| Charge pump TI BQ25970 (33 W direct charging) | bq25970_charger (0036) + cezanne-dcharge | new | partial: USB PD PPS works (vendor PD policy); 4.8 A phase below 50 % untested | none |
| Charger type detection XMUSB350 (Weltrend WT6670F: BC1.2, QC 2 / 3 / 3+) | xmusb350 (linux/drivers/xmusb350) + mt6360-chg-setup / cezanne-dcharge | new | works on a QC 3.0 power bank: QC 2 / 3 detection, 9 V into the MT6360 (2 A in, 3 A charge), QC3 direct charging through the pump (regulates, hands back); QC3+ (Xiaomi 33 W charger) and the high-current phases (< 50 %) not yet tested | none (its own firmware) |
| Battery | MT6359 gauge (above), JEITA in mt6360-chg-setup, cezanne-power app | new | works; no MIUI thermal charge-current levels | none |
| Charge limit (GNOME "Preserve Battery Health") | gauge charge_control_start / end_threshold + MT6360 charge_behaviour inhibit-charge (CHG_EN; 0073), UPower via a hwdb CHARGE_LIMIT=75,80 entry (cezanne-base), cezanne-dcharge stops the charge pump too | new | works (2026-11-08): charging stops at 80 %, resumes at 75 %, VBUS keeps powering the phone; toggled through UPower as GNOME does (the Settings switch itself not looked at); the 0..50 C guard uses the same switch now (mt6360-chg-setup used to undo it) | none |
| Battery authentication DS28E16 (1-Wire on GPIO 28) | none | - | not needed: only MIUI's charging policy used it | - |

## Sensors and misc

| Hardware | Driver / stack | Origin | Status | Firmware |
|---|---|---|---|---|
| Accel / gyro ICM-40607, magnetometer AK09918, ALS BU27030 / TSL2540, proximity TMD2755 | behind the SCP: nanohub / scp-iio (IIO) -> iio-sensor-proxy | ported | works (rotation, auto brightness, proximity); nothing blanks the screen from the proximity sensor during a call (2026-11-04) | the SCP image (above); factory calibration read from the `nvcfg` partition (`sensor/*.json`, cezanne-sensor-cal) at boot |
| Vibrator AW8697 (LRA) | aw8697 | new (vendor register map) | works | `aw8697_haptic.bin`, the waveform library (vendor `/firmware`; cezanne-firmware); the LRA trim is measured on the phone |
| Notification / pop-up RGB lights AW2015 | leds-aw2015 | new | works | none |
| Buttons (power, volume) | PMIC keys, gpio-keys | mainline | works | none |

## Not on this board

The SoC (and MIUI's generic device tree) has these, the phone does not:

| Hardware | Why not | Status |
|---|---|---|
| microSD card | the K30 Ultra has no card slot (MIUI's DT enables the SoC's SD controller msdc1 at 0x11240000 all the same) | not applicable |
| DisplayPort / video out over USB-C | the SoC has a DP transmitter (dp_tx 0x14800000, dp_intf 0x14125000), but the phone's USB-C port is USB 2.0: no high-speed lanes, so no DP alt mode; no HDMI either | not applicable |
| LCD bias, DP switch, a second haptics part (AW8624) | generic nodes in MIUI's DT, not populated on this board | not applicable |

## Firmware and the stock partitions

No firmware is distributed with the port. The Firmware column above names
what each driver loads and where it comes from:
- **cezanne-firmware** (linux/firmware): the blobs Linux loads
  from /usr/lib/firmware, extracted from the phone's own stock partitions by
  `collect-stock-firmware.sh` (the vendor image inside `super`, `scp1`;
  GNSS DSP code out of vendor `/lib64/libmnl.so`), then built with makepkg.
  Collect it before `super` is reused (the port's fstab makes it swap); images of `super`
  (also the fastboot ROM's) and `scp1` work too. Made for MIUI
  V14.0.5.0.SJNCNXM: the GNSS images are checked by sha256.
- **Read at runtime**, mounted read-only and never written: Wi-Fi
  calibration / MAC and the Bluetooth address (`nvdata`), sensor factory
  calibration (`nvcfg`), speaker and pop-up hall calibration (`persist`);
  the modem's NVRAM is copied once from `nvdata`, `nvcfg`, `protect1`,
  `protect2` (cezanne-modem-nvram-import).
- **linux-cezanne**'s boot image carries the stock boot image's dtb
  section (lk's; the kernel gets the port's DTB from the shim): the build
  takes it from a dd of the phone's `boot` partition (`stock-boot.img`).
- **Loaded by the stock bootloaders**, so these partitions stay MIUI's:
  `preloader`, `lk`, `tee`, `gz`, `sspm`, `spmfw`, `mcupm`, `md1img`
  (modem), `audio_dsp`, `scp1` (loaded by Linux here, from the firmware
  package), plus the data partitions above and `nvram`.
The TEE (Microtrust) runs Goodix's fingerprint TA; Linux only relays.

## Open items (2026-11-08)

Ordered roughly by how much they matter day to day.

- Calls: the screen stays on at the ear (no proximity blanking in a call).
- Charging: QC3+ (Xiaomi 33 W charger) untested; QC / PPS pump phases at
  high current (battery < 50 %) never exercised; MIUI's thermal
  charge-current levels (the board thermistors read since 2026-11-08; no
  policy on them yet).
- Bluetooth call audio: BT CVSD (mtk-btcvsd) not enabled, no DT node.
- Wi-Fi hotspot / P2P untested; USB network without internet sharing.
- Power while awake: DRAM fixed at 3733 MT/s, no DRAM / Vcore DVFS
  (DVFSRC measured: no idle gain, display stalls at low levels).
- AI accelerator (APU): MDLA used for upscaling (photos, video), portrait
  masks and depth (done interrupts since 7.3.1-13), int8 unused, no general NN runtime;
  VPU and EDMA not started.
- MDP3 (scale / rotate): driver built, no DT node (the GCE it needs works
  since 7.3.1-11).
- Audio DSP (offload, voice wakeup): not driven.
- Standby: ~38 mA vs MIUI 5-10 mA (AOD panel ~18 mA, rest ~20 mA asleep);
  MCUSYS-off idle disabled; notifications that arrive during suspend do
  not light the LED.
- Fingerprint: enrolment (only MIUI-enrolled fingers work).
- Modem: call uplink audio / speaker path untested (needs a second phone);
  SIM not seen at MD cold boot (lead: the SIM tray switch on GPIO 43).
- USB-C headset: buttons and mic detection (accdet) not implemented and
  not tested: no headset with buttons to test with (parked 2026-11-08).
- Touchscreen: no gesture mode (double-tap to wake, screen-off gestures).
- NFC: card emulation.
- Video encoder: HEVC, B frames.
- Camera: no hardware ISP / MFB / WPE / face detection / DPE stereo depth
  (CPU / GPU / APU processing); JPEG in hardware for Snapshot's own
  pictures, cezanne-photo's post-processed ones still through libjpeg.
- Wi-Fi: throughput and tx power tables (txpowerctrl.cfg) not tuned;
  Bluetooth headset / HID profiles not tested.
- Display: the 2026-10-22 stalls under memory load (kick-driver era) were
  not reproduced since mediatek-drm, not re-tested on purpose.
- GPU: no tessellation (D3D11 games at feature level 10_1); Drag'n Wash
  renders, with texture artefacts at native resolution only (open).
- Hardware limits, not fixable: FM has no antenna path; no HDMI /
  DisplayPort out (USB 2.0 board) and no microSD slot ("Not on this
  board"); GPU PLL capped at ~854 MHz; the panel's vendor tables only have
  60 / 120 Hz (no 90 Hz).
