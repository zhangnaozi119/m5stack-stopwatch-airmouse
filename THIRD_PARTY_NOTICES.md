# Third-party notices

本项目的 BLE HID 层为自研实现，**不再包含任何第三方 HID 库**。

The BLE HID layer is implemented in-house (`src/ble_mouse.cpp`); no third-party
HID library is bundled.

---

## 自研 HID 层说明

`include/ble_mouse.h` / `src/ble_mouse.cpp` 基于 **NimBLE-Arduino** 的公共 API
实现，只做三件事：

1. 组装标准 3 键鼠标的 HID 报告描述符（4 字节，无 Report ID）
2. 创建 Input Report 特征并发送相对位移
3. 管理连接、配对与广播

实现中固化了三个曾经踩坑的点，写在 `ble_mouse.h` 的注释里：

| 坑 | 现象 | 处理 |
|---|---|---|
| `NimBLEHIDDevice::startServices()` 在 NimBLE 2.5.1 是空函数 | 主机连上立刻断开（HCI 0x13） | 显式调用 `NimBLEServer::start()` |
| `getInputReport()` 创建的特征带 `READ_ENC` | 未配对时主机无法订阅 | 自建特征，只保留 `READ \| NOTIFY` |
| 主机不主动发起配对 | 链路一直未加密 | `onConnect` 里调用 `startSecurity()` |

> v0.0.x 曾内置 [HijelHID_BLEMouse](https://github.com/HijelHub/HijelHID_BLEMouse)
> （Apache-2.0）并打了 4 处补丁；v0.1.0 起完全移除，改为自研。

---

## 依赖（PlatformIO 拉取，不随仓库分发）

| 库 | 用途 | 许可证 |
|---|---|---|
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | BLE 协议栈 | Apache-2.0 |
| [M5Unified](https://github.com/m5stack/M5Unified) | M5Stack 硬件抽象 | MIT |
| [M5GFX](https://github.com/m5stack/M5GFX) | 显示驱动 | MIT |
| [M5PM1](https://github.com/m5stack/M5PM1) | 电源管理 | MIT |
| [M5IOE1](https://github.com/m5stack/M5IOE1) | IO 扩展 | MIT |

这些库由 PlatformIO 在构建时下载，不包含在本仓库中。
