# M5 Air Mouse

> Turn the M5Stack StopWatch into a wireless air mouse with a thumb touchpad.

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange)](https://platformio.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.2.2-blue)](CHANGELOG.md)

English | [简体中文](README.zh-CN.md)

---

## What it is

A BLE HID mouse firmware for the **M5Stack StopWatch Dev Kit (ESP32-S3)**.
The round AMOLED watch becomes a two-mode pointing device, and in both modes
the screen doubles as a thumb touchpad:

| Mode | Sensor | Feel |
|---|---|---|
| **Air Mouse** | Gyroscope (angular rate) | Like waving a mouse — stop moving, cursor stops |
| **Level Mouse** | Accelerometer (tilt) | Like a joystick — tilt to move, level to stop |

Switch modes by holding **A + B** for 0.6 s. No drivers, no dongle — it pairs
as a standard Bluetooth mouse.

---

## Features

- **Two pointing modes** driven by different sensors
- **World-frame projection** in air-mouse mode: the gyro vector is projected
  onto world axes using gravity, so left/right and up/down stay correct in any grip
- **Eight-way axis calibration** for level mode (4 rotations × 2 mirrors),
  cycled at runtime with a long press of B
- **Thumb touchpad** (available in both modes)
  - swipe up/down = vertical scroll, with inertia
  - swipe left/right = horizontal scroll (AC Pan, with inertia)
  - tap = left click
  - fast swipe left/right = back / forward
  - **axis lock**: one gesture drives one axis only, so diagonal swipes don't jitter
- **Five-button mouse**: left / right / middle / back / forward
- **Drag lock**: double-click to latch the left button, move to drag, click to release
- **Freeze** to re-centre the device without moving the cursor
- **Haptic + audio feedback** on every mode and axis change
- **On-screen diagnostics**: pairing state, sent-report counter, and a rolling
  3-second gyro peak, so you can debug without a serial cable
- **Battery level** reported over the standard HID battery service

---

## Hardware

| Item | Spec |
|---|---|
| Board | M5Stack StopWatch Dev Kit (SKU C152) |
| SoC | ESP32-S3R8, 16 MB flash, 8 MB PSRAM |
| Display | 1.75" round AMOLED, 466×466 (CO5300) |
| Touch | CST820B capacitive |
| IMU | BMI270 6-axis |
| Buttons | A, B + power |
| Audio | ES8311 codec, speaker |
| Haptics | Vibration motor |
| USB | USB-Serial/JTAG (VID:PID `303A:1001`) |

---

## Build & flash

Requires [PlatformIO](https://platformio.org/).

```bash
git clone https://github.com/zhangnaozi119/m5stack-stopwatch-airmouse.git
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
2. On your computer: **Bluetooth → Add device → M5 AirMouse**.
3. Once paired, the screen shows `已连接` (connected).

### Buttons

| Action | Function |
|---|---|
| **A** short | Left click |
| **A** double | Latch / release the left button (drag) |
| **A** long | Hold left button (drag) |
| **B** short | Right click |
| **B** double | Freeze / unfreeze movement |
| **B** long | Air Mouse: vertical axis Y/X · Level Mouse: orientation (0–7) |
| **A + B** short | Middle click (released within 50–600 ms) |
| **A + B** long 0.6 s | Switch Air Mouse ↔ Level Mouse |
| **A + B** hold 10 s | Clear pairing and reboot (switching hosts) |

### Thumb touchpad

Rest your thumb on the screen. **The pointer pauses while the screen is
touched**, so scrolling never flings the cursor around.

| Gesture | Function |
|---|---|
| Swipe up / down | Vertical scroll, with inertia after release |
| Swipe left / right | Horizontal scroll, with inertia after release |
| Tap | Left click |
| Fast swipe left | Back (browser / File Explorer) |
| Fast swipe right | Forward |

Slow swipes scroll; fast flicks trigger back/forward. The two are separated by
instantaneous velocity. Once a gesture starts, its axis is locked, so a diagonal
swipe will not flicker between horizontal and vertical.

The gyro pointer resumes **120 ms** after the thumb leaves the screen. Ending a
flick often flings the finger off; taking over immediately would send the cursor
flying with the device.

### Tuning

Pointer speed lives in `include/air_mouse.h`:

| Constant | Default | Effect |
|---|---|---|
| `_gain` | 22.0 | Air-mouse speed (px per dps·s) |
| `_levelGain` | 60.0 | Level-mode speed (px per degree·s) |
| `kDeadzoneDps` | 0.8 | Air-mouse dead zone |
| `kDeadzoneDeg` | 1.5 | Level-mode dead zone |
| Smoothing | 0.20–0.60 | Adaptive: smoother when slow, more responsive when fast |

Touchpad sensitivity lives in `include/touchpad.h`:

| Constant | Default | Effect |
|---|---|---|
| `kScrollStep` | 6.0 | Vertical: pixels per scroll notch (lower = more sensitive) |
| `kPanStep` | 3.0 | Horizontal: pixels per notch |
| `kSwipeThreshold` | 5.0 | Pixels before scrolling starts (lower = snappier) |
| `kTapMoveMax` | 8.0 | Max travel for a tap |
| `kAxisLockRatio` | 1.6 | How far the other axis must exceed the locked one to flip |
| `kMaxStepPerFrame` | 2 | Max scroll notches per frame (prevents jumps after a dropped frame) |
| `kFlickVx` | 1.8 | Flick velocity threshold (px/ms) |
| `kFlickDist` | 120.0 | Minimum travel for a flick (px) |
| `kFlickMaxMs` | 350 | Maximum duration for a flick |

The send cadence in `src/main.cpp` (30 ms) is matched to a typical BLE
connection interval; lowering it further can cause queuing on some hosts.

---

## How it works

```
BMI270 ──► AirMouse (rate / tilt) ──► accumulate ──► HID report (30 ms)
CST820B ─► Touchpad (gesture FSM) ──► accumulate ──┘
                                                      │
                                        BLE HID over GATT
                                                      │
                                                  Host cursor
```

Three details matter more than the sensor math:

1. **Rate control, not integration.** The air-mouse mode maps angular *velocity*
   to cursor *velocity*. Integrating the gyro into an angle would drift as the
   bias accumulates.
2. **Accumulate, don't overwrite.** The HID layer overwrites pending deltas
   between sends; the application accumulates them and submits on a fixed
   cadence so no motion is lost.
3. **Flick detection must use instantaneous velocity.** An early version used
   the peak horizontal velocity of the whole gesture, so the jitter at
   touch-down raised the peak and suppressed scrolling for the entire gesture —
   the root cause of horizontal scroll never working.

The HID report is **5 bytes with no Report ID**: `[buttons, X, Y, wheel, AC Pan]`.
Horizontal scrolling uses the Consumer-page AC Pan field.

See [CHANGELOG.md](CHANGELOG.md) for the bug hunt — including the GATT server
that was never started and a stray `0xC0` in the descriptor that made Windows
connect but never respond.

---

## Project layout

```
include/
  air_mouse.h        sensor → pointer math
  touchpad.h         touchpad gesture state machine
  ble_mouse.h        BLE HID layer interface
  version.h
src/
  main.cpp           wiring, buttons, UI
  air_mouse.cpp      sensor → pointer mapping
  touchpad.cpp       gesture recognition and scroll accumulation
  ble_mouse.cpp      self-contained BLE HID layer
docs/
  touchpad-design.md touchpad design notes
```

---

## Known limitations

- **A gyroscope measures rotation, not translation.** Sliding the device sideways
  without rotating it produces no signal in any gyro-based air mouse. Rotate the
  wrist (yaw) instead of translating the arm.
- **Horizontal scroll relies on AC Pan.** Windows browsers, Excel and VS Code
  support it; File Explorer and some other apps do not support horizontal
  wheels at all — an application-side limitation.
- **Media keys (volume/play) and voice input (Ctrl+Win) are not supported.**
  Both require keyboard or consumer HID reports, which in turn require Report
  IDs; Windows rejects such descriptors on this setup (pairing and encryption
  succeed, but the host never subscribes). See the v0.2.2 entry in
  [CHANGELOG.md](CHANGELOG.md).
- Absolute (laser-pointer) positioning is not implemented; level mode is
  velocity-based so it needs no calibration against the host screen.
- The BLE connection interval is negotiated by the host; a very slow interval
  makes the pointer feel laggy.
- macOS may reject the HID descriptor more readily than Windows/Linux.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Paired but the cursor never moves | Invalid HID descriptor, or the host cached an old one. Remove the device and pair again |
| Descriptor changes have no effect | Windows caches GATT per MAC address; the device must be removed and re-paired |
| New host / pairing trouble | Hold **A + B for 10 s** on the device to clear pairing and reboot |
| Pointer feels laggy | The host negotiated a long connection interval; move closer or reduce 2.4 GHz interference |
| Horizontal scroll does nothing | Try a browser or Excel first — some apps ignore AC Pan entirely |

---

## Credits

The BLE HID layer is implemented in-house on top of NimBLE-Arduino's public API,
with no third-party HID library. Three pitfalls are documented in
`include/ble_mouse.h`: the no-op `startServices()` in NimBLE 2.5.1, the
`READ_ENC` flag on NimBLE's default input report, and hosts not initiating
pairing on their own.

- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) — BLE stack
- [M5Unified](https://github.com/m5stack/M5Unified) / [M5GFX](https://github.com/m5stack/M5GFX)
  — M5Stack hardware abstraction

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

MIT — see [LICENSE](LICENSE).
