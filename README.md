# M5 Air Mouse

> Turn the M5Stack StopWatch into a wireless air mouse over Bluetooth LE.

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange)](https://platformio.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.0.8-blue)](CHANGELOG.md)

English | [简体中文](README.zh-CN.md)

---

## What it is

A BLE HID mouse firmware for the **M5Stack StopWatch Dev Kit (ESP32-S3)**.
The round AMOLED watch becomes a two-mode pointing device:

| Mode | Sensor | Feel |
|---|---|---|
| **Air mouse** | Gyroscope (angular rate) | Like waving a mouse — stop moving, cursor stops |
| **Level** | Accelerometer (tilt) | Like a joystick — tilt to move, level to stop |

Switch modes by holding **A + B** for 0.6 s. No drivers, no dongle — it pairs
as a standard Bluetooth mouse.

---

## Features

- **Two pointing modes** driven by different sensors
- **Eight-way axis calibration** for level mode (4 rotations × 2 mirrors),
  cycled at runtime with a long press of B
- **Full mouse buttons**: click, double-click (opens files), drag, right-click
- **Freeze** to re-centre the device without moving the cursor
- **Haptic + audio feedback** on every mode and axis change
- **On-screen diagnostics**: pairing state and sent-report counter, so you can
  debug without a serial cable
- **Battery level** reported over the standard HID battery service

---

## Hardware

| Item | Spec |
|---|---|
| Board | M5Stack StopWatch Dev Kit (SKU C152) |
| SoC | ESP32-S3R8, 16 MB flash, 8 MB PSRAM |
| Display | 1.75" round AMOLED, 466×466 (CO5300) |
| IMU | BMI270 6-axis |
| Buttons | A, B + power |
| Audio | ES8311 codec, speaker |
| Haptics | Vibration motor |
| USB | USB-Serial/JTAG (VID:PID `303A:1001`) |

---

## Build & flash

Requires [PlatformIO](https://platformio.org/).

```bash
git clone <this-repo>
cd m5stack-stopwatch-airmouse
pio run                 # build
pio run -t upload       # flash (auto-detects the port)
```

The board is configured as `esp32s3box` with `default_16MB.csv` and
`qio_opi` memory type — the same combination used by M5Stack's official
StopWatch examples.

### Troubleshooting the port

```bash
pio device list          # find the port, e.g. COM4
pio run -t upload --upload-port COM4
```

---

## Usage

### First start

1. Power on and **hold the device still for ~1 second** — the screen shows a
   calibration bar while it measures the gyro bias.
2. On your computer: **Bluetooth → Add device → M5 AirMouse 3**.
3. Once paired, the screen shows `已连接` (connected).

### Controls

| Action | Function |
|---|---|
| **A** short | Left click |
| **A** double | Left double-click (open a file/folder) |
| **A** long | Hold left button (drag) |
| **B** short | Right click |
| **B** double | Freeze / unfreeze movement |
| **B** long | Air mouse: switch axis · Level: cycle orientation (0–7) |
| **A + B** long | Switch air mouse ↔ level |

### Tuning

The defaults live in `include/air_mouse.h`:

| Constant | Default | Effect |
|---|---|---|
| `_gain` | 8.0 | Air-mouse speed (px per dps·s) |
| `_levelGain` | 60.0 | Level-mode speed (px per degree·s) |
| `kDeadzoneDps` | 0.8 | Air-mouse dead zone |
| `kDeadzoneDeg` | 1.5 | Level-mode dead zone |
| `kLpfAlpha` | 0.35 | Low-pass smoothing |

The send cadence in `src/main.cpp` (30 ms) is matched to a typical BLE
connection interval; lowering it further can cause queuing on some hosts.

---

## How it works

```
BMI270 ──► AirMouse (rate / tilt) ──► accumulate ──► HID report (30 ms)
                                                        │
                                          BLE HID over GATT
                                                        │
                                                    Host cursor
```

Two details matter more than the sensor math:

1. **Rate control, not integration.** The air-mouse mode maps angular *velocity*
   to cursor *velocity*. Integrating the gyro into an angle would drift as the
   bias accumulates.
2. **Accumulate, don't overwrite.** The HID layer overwrites pending deltas
   between sends; the application accumulates them and submits on a fixed
   cadence so no motion is lost.

See [CHANGELOG.md](CHANGELOG.md) for the bug hunt — including the GATT server
that was never started, which silently broke all reports.

---

## Project layout

```
include/
  air_mouse.h        sensor → pointer math
  version.h
src/
  main.cpp           wiring, buttons, UI
  air_mouse.cpp
lib/
  HijelHID_BLEMouse/ vendored BLE HID mouse library (Apache-2.0)
```

---

## Known limitations

- Absolute (laser-pointer) positioning is not implemented; level mode is
  velocity-based so it needs no calibration against the host screen.
- The BLE connection interval is negotiated by the host; a very slow interval
  makes the pointer feel laggy.
- macOS may reject the HID descriptor more readily than Windows/Linux.

---

## Credits

- [HijelHID_BLEMouse](https://github.com/HijelHub/HijelHID_BLEMouse) — BLE HID
  mouse library (Apache-2.0), vendored under `lib/`.
- [M5Unified](https://github.com/m5stack/M5Unified) / [M5GFX](https://github.com/m5stack/M5GFX)
  — M5Stack hardware abstraction.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

MIT — see [LICENSE](LICENSE).
