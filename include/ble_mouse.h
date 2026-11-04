/**
 * @file ble_mouse.h
 * @brief 自研 BLE HID 鼠标层（替代第三方 HijelHID_BLEMouse）
 *
 * 只用 NimBLE-Arduino 的公共 API，不依赖任何第三方 HID 封装。
 *
 * 报告格式：4 字节，无 Report ID
 *   [buttons, dx, dy, wheel]
 *
 * 关键点（踩过的坑，全部写在这里）：
 *   1. NimBLE 2.5.1 的 NimBLEHIDDevice::startServices() 是空函数，
 *      必须显式调用 NimBLEServer::start()，否则主机连上后发现不了服务，
 *      会以 HCI 0x13 (Remote User Terminated) 主动断开。
 *   2. getInputReport() 创建的特征带 READ_ENC，未配对时主机无法订阅。
 *      这里自己创建特征，只要 READ | NOTIFY。
 *   3. 主机不会主动为 HID 设备发起配对，需要在 onConnect 里
 *      startSecurity()，否则部分主机的链路一直未加密。
 */
#pragma once

#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

#include <stdint.h>

namespace sw {

/// 鼠标按键位（5 键）
constexpr uint8_t kBtnLeft    = 0x01;
constexpr uint8_t kBtnRight   = 0x02;
constexpr uint8_t kBtnMiddle  = 0x04;
constexpr uint8_t kBtnBack    = 0x08;
constexpr uint8_t kBtnForward = 0x10;

class BleMouse {
 public:
  /// 初始化 BLE 栈、HID 服务并开始广播
  bool begin(const char* deviceName);

  bool isConnected() const { return _connected; }
  bool isPaired() const { return _paired; }

  /// 发送一次相对位移（带上当前按键状态）
  bool move(int8_t dx, int8_t dy, int8_t wheel = 0, int8_t pan = 0);

  /// 直接设置按键并立即上报
  void setButtons(uint8_t mask);

  /// 单击：按下 -> holdMs 后抬起
  void click(uint8_t mask, uint16_t holdMs = 30);

  /// 双击：两次 按下-抬起，间隔 gapMs
  void doubleClick(uint8_t mask, uint16_t holdMs = 30, uint16_t gapMs = 45);

  void setBatteryLevel(uint8_t percent);
  void setTxPower(uint8_t level);

  /// 在主循环里调用，推进点击时序
  void tick();

  // 供 NimBLE 回调使用
  void onConnected();
  void onDisconnected();
  void onAuthenticated(bool encrypted);

 private:
  bool sendReport(int8_t dx, int8_t dy, int8_t wheel, int8_t pan);
  void setupAdvertising();   ///< 只在 begin() 里调用一次
  void startAdvertising();   ///< 断开重连时只重新 start()

  NimBLEServer*         _server = nullptr;
  NimBLEHIDDevice*      _hid = nullptr;
  NimBLECharacteristic* _input = nullptr;
  const char*           _name = nullptr;

  volatile bool _connected = false;
  volatile bool _paired = false;

  uint8_t  _buttons = 0;
  uint8_t  _battery = 100;

  // 单击 / 双击时序
  uint8_t  _clickMask = 0;
  uint8_t  _clickRemaining = 0;   // 还要发几次"按下-抬起"
  bool     _clickDown = false;
  uint16_t _clickHoldMs = 30;
  uint16_t _clickGapMs = 45;
  uint32_t _clickNextAt = 0;
};

}  // namespace sw
