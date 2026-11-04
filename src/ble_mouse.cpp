/**
 * @file ble_mouse.cpp
 * @brief 自研 BLE HID 鼠标层实现
 */
#include "ble_mouse.h"

#include <Arduino.h>

namespace sw {
namespace {

// 5 键鼠标 + 相对位移 + 滚轮 + AC Pan（水平滚动）
// 报告：5 字节 [buttons, dx, dy, wheel, pan]，无 Report ID
const uint8_t kReportMap[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x05,        //     Usage Maximum (5)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x05,        //     Report Count (5)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x03,        //     Report Size (3)  -- 补齐 1 字节
    0x81, 0x03,        //     Input (Const,Var,Abs)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data,Var,Rel)
    // AC Pan 必须与 X/Y 在同一 Physical Collection 内，并切到 Consumer page，
    // 否则 Windows/macOS 不会识别为水平滚动。
    0x05, 0x0C,        //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,  //     Usage (AC Pan)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data,Var,Rel)
    0xC0,              //   End Collection
    0xC0,              // End Collection
};

class ServerCB : public NimBLEServerCallbacks {
 public:
  explicit ServerCB(BleMouse* owner) : _owner(owner) {}

  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    _owner->onConnected();
    // 先协商连接间隔（7.5~15ms），再发起配对。
    // 顺序与已验证的库实现一致：过早请求配对时部分主机会直接断开。
    server->updateConnParams(connInfo.getConnHandle(), 6, 12, 0, 400);
    int rc = 0;
    NimBLEDevice::startSecurity(connInfo.getConnHandle(), &rc);
    Serial.printf("[ble] 已连接 handle=%u interval=%u paired_req_rc=%d\n",
                  connInfo.getConnHandle(), connInfo.getConnInterval(), rc);
  }

  void onDisconnect(NimBLEServer* /*server*/, NimBLEConnInfo& connInfo, int reason) override {
    Serial.printf("[ble] 已断开 handle=%u reason=0x%02X\n",
                  connInfo.getConnHandle(), reason);
    _owner->onDisconnected();
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    const bool enc = connInfo.isEncrypted();
    Serial.printf("[ble] 配对结果 encrypted=%d bonded=%d\n",
                  enc ? 1 : 0, connInfo.isBonded() ? 1 : 0);
    _owner->onAuthenticated(enc);
  }

  // 数值比较（Numeric Comparison）配对：主机显示 6 位码，设备必须确认，
  // 否则主机等到超时后主动断开（HCI 0x13）。库版本实现了这两个回调，
  // 自研层漏掉它们会导致每一步配对都失败。
  uint32_t onPassKeyDisplay() override { return 0; }

  void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pin) override {
    Serial.printf("[ble] 配对码 %06lu，自动确认\n", static_cast<unsigned long>(pin));
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }

 private:
  BleMouse* _owner;
};

}  // namespace

bool BleMouse::begin(const char* deviceName) {
  _name = deviceName;

  NimBLEDevice::init(deviceName);

  // 每次开机清空绑定：主机按 MAC 记住配对密钥，设备侧密钥一旦与之不一致，
  // 主机会用旧密钥加密 -> 失败 -> 主动断开 (HCI 0x13)。
  // 清空后由 onConnect 里的 startSecurity 重新配对。
  if (NimBLEDevice::getNumBonds() > 0) {
    NimBLEDevice::deleteAllBonds();
    Serial.println("[ble] 已清空旧绑定");
  }

  NimBLEDevice::setPower(ESP_PWR_LVL_P3);
  NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_SC);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  _server = NimBLEDevice::createServer();
  _server->setCallbacks(new ServerCB(this));
  _server->advertiseOnDisconnect(false);  // 广播由我们自己控制

  _hid = new NimBLEHIDDevice(_server);
  _hid->setManufacturer("M5Stack");
  _hid->setPnp(0x02, 0x303A, 0x0000, 0x0001);
  _hid->setHidInfo(0x00, 0x01);
  _hid->setReportMap(const_cast<uint8_t*>(kReportMap), sizeof(kReportMap));

  // 试验：改用 NimBLE 自带的 getInputReport
  _input = _hid->getInputReport(0);
  if (_input == nullptr) {
    Serial.println("[ble] Input Report 特征创建失败");
    return false;
  }

  const uint8_t idle[5] = {0, 0, 0, 0, 0};
  _input->setValue(idle, sizeof(idle));

  _hid->setBatteryLevel(_battery);

  // 必须显式启动 GATT server
  if (!_server->start()) {
    Serial.println("[ble] GATT server 启动失败");
    return false;
  }

  setupAdvertising();
  startAdvertising();
  Serial.printf("[ble] 广播中: %s\n", deviceName);
  Serial.printf("[ble] GATT: hidSvc=%u hidInfo=%u reportMap=%u control=%u input=%u\n",
                _hid->getHidService()->getHandle(),
                _hid->getHidInfo()->getHandle(),
                _hid->getReportMap()->getHandle(),
                _hid->getHidControl()->getHandle(),
                _input->getHandle());
  return true;
}

void BleMouse::setupAdvertising() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(HID_MOUSE);
  adv->setName(_name ? _name : "M5 AirMouse");
  // 只放 HID 服务 UUID：31 字节广播包放不下 HID + 电池两个 UUID 加名字。
  adv->addServiceUUID(_hid->getHidService()->getUUID());
  adv->enableScanResponse(true);
}

void BleMouse::startAdvertising() {
  // 注意：这里只能 start()，不能再次 addServiceUUID()。
  // 每次断开重连都重新添加会让广播数据累积溢出
  // (E NimBLEAdvertisementData: Cannot add UUID, data length exceeded!)，
  // 广播包里丢掉 HID 服务 UUID 后，主机会连上又立刻断开 (0x213)。
  NimBLEDevice::getAdvertising()->start();
}

bool BleMouse::sendReport(int8_t dx, int8_t dy, int8_t wheel, int8_t pan) {
  if (!_connected || _input == nullptr) return false;

  const uint8_t report[5] = {
      _buttons,
      static_cast<uint8_t>(dx),
      static_cast<uint8_t>(dy),
      static_cast<uint8_t>(wheel),
      static_cast<uint8_t>(pan),
  };
  return _input->notify(report, sizeof(report));
}

bool BleMouse::move(int8_t dx, int8_t dy, int8_t wheel, int8_t pan) {
  return sendReport(dx, dy, wheel, pan);
}

void BleMouse::setButtons(uint8_t mask) {
  _buttons = mask;
  sendReport(0, 0, 0, 0);
}

void BleMouse::click(uint8_t mask, uint16_t holdMs) {
  _clickMask = mask;
  _clickRemaining = 1;
  _clickDown = false;
  _clickHoldMs = holdMs;
  _clickNextAt = millis();
}

void BleMouse::doubleClick(uint8_t mask, uint16_t holdMs, uint16_t gapMs) {
  _clickMask = mask;
  _clickRemaining = 2;
  _clickDown = false;
  _clickHoldMs = holdMs;
  _clickGapMs = gapMs;
  _clickNextAt = millis();
}

void BleMouse::tick() {
  if (_clickRemaining == 0) return;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - _clickNextAt) < 0) return;

  if (!_clickDown) {
    _buttons |= _clickMask;
    sendReport(0, 0, 0, 0);
    _clickDown = true;
    _clickNextAt = now + _clickHoldMs;
  } else {
    _buttons &= ~_clickMask;
    sendReport(0, 0, 0, 0);
    _clickDown = false;
    if (--_clickRemaining > 0) {
      _clickNextAt = now + _clickGapMs;
    }
  }
}

void BleMouse::setBatteryLevel(uint8_t percent) {
  if (percent == _battery) return;
  _battery = percent;
  if (_hid != nullptr) _hid->setBatteryLevel(percent, true);
}

void BleMouse::setTxPower(uint8_t level) {
  NimBLEDevice::setPower(level);
}

void BleMouse::onConnected() {
  _connected = true;
}

void BleMouse::onDisconnected() {
  _connected = false;
  _paired = false;
  _buttons = 0;
  _clickRemaining = 0;
  _clickDown = false;
  startAdvertising();
}

void BleMouse::onAuthenticated(bool encrypted) {
  _paired = encrypted;
}

}  // namespace sw
