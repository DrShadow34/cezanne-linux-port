# SCP sensor hub (MT6885) for mainline

All motion/environment sensors of the Redmi K30 Ultra sit behind the SCP
(tinysys, 2x RISC-V) on its private buses: ICM-40607 accel/gyro, AK09918
magnetometer, BU27030 front ALS, TSL2540 rear ALS, TMD2755 proximity.

## Modules (load order handled by modules-load.d/cezanne-sensors.conf)

| module | source | role |
|---|---|---|
| scp-cezanne.ko | scp_cezanne.c, mtk-mbox.c, mtk_tinysys_ipi.c | loads scp.img (does lk's job: lk skips the SCP in recovery boot), mailbox IPI, ready handshake, ULPOSC cali, logger (debugfs scp/) |
| hf-manager.ko | sensor/hf_manager.c | BSP sensor core, /dev/hf_manager |
| nanohub.ko | sensor/{main,bl,comms,nanohub-mtk}.c | CHRE transport (sensor config) |
| mtk-nanohub.ko | sensor/mtk_nanohub*.c, sensor_list.c | hub protocol, shared DRAM FIFO |
| scp-iio.ko | sensor/scp_iio.c | IIO devices accel, gyro, magn, als, proximity, compass |

Build: `./build.sh` then `./build-sensor.sh` (against src/out-mainline).

## Userspace

- `/usr/lib/firmware/mediatek/mt6885/scp.img` = stock scp1 partition.
- `cezanne-sensor-cal.service`: replays MIUI's factory calibration
  (nvcfg:/sensor/*.json) via /dev/hf_manager; without it the ALS reads 0.
- `61-cezanne-sensors.rules`: PROXIMITY_NEAR_LEVEL for iio-sensor-proxy.
- `cezanne-compass`: GTK4 compass + magnetic field meter with its own
  calibration (the hub's fused heading is ~90 deg off; MIUI computes the
  heading in its HAL too).

## Conventions

Hub values are Android axes/units; scp-iio converts units and adds the
mount matrix diag(-1,-1,1) that iio-sensor-proxy expects. See NOTES.md
("SCP / sensor hub") for the bring-up details and gotchas.
