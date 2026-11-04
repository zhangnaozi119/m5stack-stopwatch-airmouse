# M5 空中鼠标

> 把 M5Stack StopWatch 变成一只蓝牙空中鼠标。

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange)](https://platformio.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.1.0-blue)](CHANGELOG.md)

[English](README.md) | 简体中文

---

## 这是什么

面向 **M5Stack StopWatch Dev Kit (ESP32-S3)** 的 BLE HID 鼠标固件。
这块圆屏手表可以变成两种模式的指针设备：

| 模式 | 传感器 | 手感 |
|---|---|---|
| **空中鼠** | 陀螺仪（角速度） | 像挥动鼠标——手停，指针停 |
| **水平鼠** | 加速度计（倾角） | 像摇杆——倾斜移动，回正停止 |

同时按住 **A + B** 0.6 秒切换模式。免驱动、免接收器，配对后就是标准蓝牙鼠标。

---

## 功能

- **两种指向模式**，分别由不同传感器驱动
- **世界坐标投影**：用重力方向把陀螺仪向量投影到世界坐标轴，
  任何握姿下左右/上下都保持一致
- **水平鼠 8 向校准**（4 种旋转 × 2 种镜像），运行时按 B 长按循环切换
- **完整鼠标按键**：单击、双击（打开文件）、拖拽、右键
- **冻结**：重新对中设备时不移动指针
- **震动与提示音**：每次切换模式或轴向都有反馈
- **屏幕自诊断**：显示配对状态与已发送报告数，不插线也能排查
- **电量上报**：走标准 HID 电池服务

---

## 硬件

| 项目 | 规格 |
|---|---|
| 板卡 | M5Stack StopWatch Dev Kit（SKU C152） |
| 主控 | ESP32-S3R8，16 MB Flash / 8 MB PSRAM |
| 屏幕 | 1.75" 圆 AMOLED，466×466（CO5300） |
| 传感器 | BMI270 六轴 IMU |
| 按键 | A、B + 电源键 |
| 音频 | ES8311 codec + 喇叭 |
| 振动 | 振动马达 |
| USB | USB-Serial/JTAG（VID:PID `303A:1001`） |

---

## 编译与烧录

需要 [PlatformIO](https://platformio.org/)。

```bash
git clone https://github.com/zhangnaozi119/m5stack-stopwatch-airmouse.git
cd m5stack-stopwatch-airmouse
pio run                 # 编译
pio run -t upload       # 烧录（自动识别端口）
```

板卡配置为 `esp32s3box` + `default_16MB.csv` + `qio_opi`，
与 M5Stack 官方 StopWatch 例程一致。

### 端口问题

```bash
pio device list          # 查看端口，例如 COM4
pio run -t upload --upload-port COM4
```

---

## 使用

### 首次启动

1. 开机后**保持静止约 1 秒**，屏幕显示校准进度条（采集陀螺零偏）。
2. 电脑上：**蓝牙 → 添加设备 → M5 AirMouse**。
3. 配对成功后屏幕显示「已连接」。

### 按键

| 操作 | 功能 |
|---|---|
| **A** 短按 | 左键单击 |
| **A** 双击 | 左键双击（打开文件/文件夹） |
| **A** 长按 | 按住左键拖拽 |
| **B** 短按 | 右键单击 |
| **B** 双击 | 冻结/解冻移动 |
| **B** 长按 | 空中鼠：垂直轴 Y/X · 水平鼠：切换 8 向校准 |
| **A + B** 长按 | 切换 空中鼠 ↔ 水平鼠 |

### 调参

默认值在 `include/air_mouse.h`：

| 常量 | 默认 | 作用 |
|---|---|---|
| `_gain` | 22.0 | 空中鼠速度（像素 / dps·秒） |
| `_levelGain` | 60.0 | 水平鼠速度（像素 / 度·秒） |
| `kDeadzoneDps` | 0.8 | 空中鼠死区 |
| `kDeadzoneDeg` | 1.5 | 水平鼠死区 |
| `kLpfAlpha` | 0.35 | 低通平滑系数 |

`src/main.cpp` 中的提交周期（30 ms）与常见 BLE 连接间隔匹配，
再调小容易在某些主机上造成积压。

---

## 实现要点

```
BMI270 ──► AirMouse（角速度 / 倾角）──► 累加 ──► HID 报告（30 ms）
                                                  │
                                        BLE HID over GATT
                                                  │
                                              主机指针
```

比传感器数学更关键的两点：

1. **用速度映射，不做积分。** 空中鼠模式把角**速度**映射为指针**速度**。
   若把陀螺积分成角度，零偏会累积，手一停指针就自己飘。
2. **累加，不要覆盖。** HID 层在两次发送之间会覆盖待发位移，
   所以应用层先累加、再按固定节奏提交，避免丢步。

排障过程（包括「GATT 服务从未启动」这个导致所有报告失效的根因）
记录在 [CHANGELOG.md](CHANGELOG.md)。

---

## 目录结构

```
include/
  air_mouse.h        传感器 → 指针的换算
  version.h
src/
  main.cpp           按键、界面、主循环
  air_mouse.cpp      传感器 → 指针换算
  ble_mouse.cpp      自研 BLE HID 层
```

---

## 已知限制

- **陀螺仪测旋转，不测平移。** 平放时单纯左右平移手臂不会产生任何角速度信号，
  需要转动手腕（yaw）。这是所有陀螺仪空中鼠标的共同限制。
- 未实现绝对定位（激光笔式）；水平鼠采用速度映射，因此不需要与主机屏幕标定。
- BLE 连接间隔由主机协商，间隔过大时指针会显得迟滞。
- macOS 对 HID 描述符的接受度可能低于 Windows/Linux。

---

## 致谢

- BLE HID 层为自研实现，基于 [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) 的公共 API，不依赖第三方 HID 库。
  三个踩过的坑写在 `include/ble_mouse.h` 的注释里。
- [M5Unified](https://github.com/m5stack/M5Unified) / [M5GFX](https://github.com/m5stack/M5GFX) —— M5Stack 硬件抽象层。

详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 许可证

MIT —— 见 [LICENSE](LICENSE)。
