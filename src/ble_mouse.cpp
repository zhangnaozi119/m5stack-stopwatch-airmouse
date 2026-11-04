/**
 * @file ble_mouse.cpp
 * @brief 自研 BLE HID 鼠标层实现
 */
#include "ble_mouse.h"

#include <Arduino.h>

namespace sw {
namespace {

// 标准 3 键鼠标 + 相对位移 + 滚轮，不使用 Report ID
const uint8_t kReportMap[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x03,        //     Usage Maximum (3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
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
    0xC0,              //   End Collection
    0xC0,              // End Collection
};

class ServerCB : public NimBLEServerCallbacks {
 public:
  explicit ServerCB(BleMouse* owner) : _owner(owner) {}

  void onConnect(NimBLEServer* /*server*/, NimBLEConnInfo& connInfo) override {
    _owner->onConnected();
    // 主机不会主动为 HID 设备发起配对，这里由设备请求
    int rc = 0;
    NimBLEDevice::startSecurity(connInfo.getConnHandle(), &rc);
    Serial.printf("[ble] 已连接 handle=%u paired_req_rc=%d\n",
                  connInfo.getConnHandle(), rc);
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

 private:
  BleMouse* _owner;
};

}  // namespace

bool BleMouse::begin(const char* deviceName) {
  _name = deviceName;

  NimBLEDevice::init(deviceName);
  if (NimBLEDevice::getNumBonds() > 0) {
    NimBLEDevice::deleteAllBonds();
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

  // 自建 Input Report 特征：只要 READ | NOTIFY，不带 READ_ENC，
  // 这样主机未完成配对也能订阅。
  _input = _hid->getHidService()->createCharacteristic(
      NimBLEUUID(static_cast<uint16_t>(0x2A4D)),
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  if (_input == nullptr) {
    Serial.println("[ble] Input Report 特征创建失败");
    return false;
  }

  NimBLEDescriptor* ref = _input->createDescriptor(
      NimBLEUUID(static_cast<uint16_t>(0x2908)), NIMBLE_PROPERTY::READ);
  const uint8_t refVal[2] = {0x00, 0x01};  // report id 0, type input
  if (ref != nullptr) ref->setValue(refVal, sizeof(refVal));

  const uint8_t idle[4] = {0, 0, 0, 0};
  _input->setValue(idle, sizeof(idle));

  _hid->setBatteryLevel(_battery);

  // 必须显式启动 GATT server
  if (!_server->start()) {
    Serial.println("[ble] GATT server 启动失败");
    return false;
  }

  startAdvertising();
  Serial.printf("[ble] 广播中: %s\n", deviceName);
  return true;
}

void BleMouse::startAdvertising() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(HID_MOUSE);
  adv->setName(_name ? _name : "M5 AirMouse");
  adv->addServiceUUID(_hid->getHidService()->getUUID());
  adv->addServiceUUID(_hid->getBatteryService()->getUUID());
  adv->enableScanResponse(true);
  adv->start();
}

bool BleMouse::sendReport(int8_t dx, int8_t dy, int8_t wheel) {
  if (!_connected || _input == nullptr) return false;

  const uint8_t report[4] = {
      _buttons,
      static_cast<uint8_t>(dx),
      static_cast<uint8_t>(dy),
      static_cast<uint8_t>(wheel),
  };
  return _input->notify(report, sizeof(report));
}

bool BleMouse::move(int8_t dx, int8_t dy, int8_t wheel) {
  return sendReport(dx, dy, wheel);
}

void BleMouse::setButtons(uint8_t mask) {
  _buttons = mask;
  sendReport(0, 0, 0);
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
    sendReport(0, 0, 0);
    _clickDown = true;
    _clickNextAt = now + _clickHoldMs;
  } else {
    _buttons &= ~_clickMask;
    sendReport(0, 0, 0);
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
