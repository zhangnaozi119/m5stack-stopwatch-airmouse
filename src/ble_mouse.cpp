/**
 * @file ble_mouse.cpp
 * @brief 自研 BLE HID 鼠标层
 *
 * 当前配置**严格复刻 v0.0.15**（Windows 已验证可用）：
 *   - 3 键鼠标 + X/Y/Wheel
 *   - **不使用 Report ID**，报告 4 字节 [buttons, dx, dy, wheel]
 *   - 公开地址（芯片 MAC），与 v0.0.15 一致
 *   - 每次开机清空绑定 + 连接时主动配对（与 v0.0.15 相同）
 *
 * 媒体/键盘报告暂时为空实现，待本配置验证通过后再逐项加入。
 */
#include "ble_mouse.h"

#include <Arduino.h>
#include <Preferences.h>

namespace sw {
namespace {

// 3 键鼠标 + 相对位移 + 滚轮，无 Report ID（与 v0.0.15 完全一致）
const uint8_t kReportMap[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x05,        //     Usage Maximum (5)  左/右/中/后退/前进
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
    // AC Pan (水平滚动): 必须与 X/Y 在同一 Physical Collection 内,
    // 并切到 Consumer page, Windows 才识别为水平滚轮
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
    server->updateConnParams(connInfo.getConnHandle(), 6, 12, 0, 400);
    if (!connInfo.isEncrypted()) {
      int rc = 0;
      NimBLEDevice::startSecurity(connInfo.getConnHandle(), &rc);
      Serial.printf("[ble] 已连接 handle=%u interval=%u 发起配对rc=%d\n",
                    connInfo.getConnHandle(), connInfo.getConnInterval(), rc);
    } else {
      Serial.printf("[ble] 已连接(已有绑定) handle=%u interval=%u\n",
                    connInfo.getConnHandle(), connInfo.getConnInterval());
    }
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

  uint32_t onPassKeyDisplay() override { return 0; }

  void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pin) override {
    Serial.printf("[ble] 配对码 %06lu, 自动确认\n", static_cast<unsigned long>(pin));
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }

 private:
  BleMouse* _owner;
};

}  // namespace

bool BleMouse::begin(const char* deviceName, bool /*forceRepairing*/) {
  _name = deviceName;

  NimBLEDevice::init(deviceName);

  // 保留绑定：开机不再清空，主机重连时用已有密钥直接加密恢复。
  // 只有确实没有绑定时才在 onConnect 里主动发起配对。
  _hasBond = (NimBLEDevice::getNumBonds() > 0);
  Serial.printf("[ble] 绑定记录: %s\n", _hasBond ? "有(直接重连)" : "无(等待配对)");

  NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_SC);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  _server = NimBLEDevice::createServer();
  _server->setCallbacks(new ServerCB(this));
  _server->advertiseOnDisconnect(false);

  _hid = new NimBLEHIDDevice(_server);
  _hid->setManufacturer("M5Stack");
  _hid->setPnp(0x02, 0x303A, 0x0000, 0x0001);
  _hid->setHidInfo(0x00, 0x01);
  _hid->setReportMap(const_cast<uint8_t*>(kReportMap), sizeof(kReportMap));

  // 无 Report ID 的输入报告特征（与 v0.0.15 相同：READ | NOTIFY）
  _input = _hid->getHidService()->createCharacteristic(
      NimBLEUUID(static_cast<uint16_t>(0x2A4D)),
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  if (_input == nullptr) {
    Serial.println("[ble] Input Report 特征创建失败");
    return false;
  }

  NimBLEDescriptor* ref = _input->createDescriptor(
      NimBLEUUID(static_cast<uint16_t>(0x2908)), NIMBLE_PROPERTY::READ);
  const uint8_t refVal[2] = {0x00, 0x01};  // report id 0 (无 ID), type input
  if (ref != nullptr) ref->setValue(refVal, sizeof(refVal));

  const uint8_t idle[5] = {0, 0, 0, 0, 0};
  _input->setValue(idle, sizeof(idle));

  // 厂商自定义服务: 供 PC 端助手订阅 (语音输入等 HID 做不到的事)
  NimBLEService* ctrlSvc = _server->createService(
      NimBLEUUID("6d3578a0-1e2b-4c3d-9f4a-8b7c6d5e4f01"));
  _ctrl = ctrlSvc->createCharacteristic(
      NimBLEUUID("6d3578a1-1e2b-4c3d-9f4a-8b7c6d5e4f01"),
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  if (_ctrl == nullptr) {
    Serial.println("[ble] 控制特征创建失败");
    return false;
  }
  const uint8_t ctrlIdle = kCtrlVoiceUp;
  _ctrl->setValue(&ctrlIdle, 1);
  Serial.printf("[ble] 控制服务已注册 ctrl=%p\n", static_cast<void*>(_ctrl));

  _hid->setBatteryLevel(_battery);

  if (!_server->start()) {
    Serial.println("[ble] GATT server 启动失败");
    return false;
  }
  Serial.printf("[ble] 控制服务可查=%d\n",
                _server->getServiceByUUID(
                    NimBLEUUID("6d3578a0-1e2b-4c3d-9f4a-8b7c6d5e4f01")) != nullptr
                    ? 1 : 0);

  setupAdvertising();
  startAdvertising();
  Serial.printf("[ble] 广播中: %s (v0.0.15 兼容描述符, 4 字节无 Report ID)\n", deviceName);
  return true;
}

void BleMouse::setupAdvertising() {
  // 换全新广播地址: 强制主机重新做服务发现 (否则会沿用缓存的旧描述符)
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
  {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    uint8_t addr[6] = {mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]};
    addr[0] = (addr[0] ^ 0x99) | 0xC0;
    NimBLEDevice::setOwnAddr(addr);
    Serial.printf("[ble] 广播地址: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
  }
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(HID_MOUSE);
  adv->setName(_name ? _name : "M5 AirMouse");
  adv->addServiceUUID(_hid->getHidService()->getUUID());
  adv->enableScanResponse(true);
  adv->setScanFilter(false, false);
  adv->setAdvertisingCompleteCallback([](NimBLEAdvertising* a) {
    Serial.println("[ble] 广播停止, 自动重启");
    a->start();
  });
}

void BleMouse::startAdvertising() {
  // 只 start(), 不重复 addServiceUUID (会累积溢出)
  NimBLEDevice::getAdvertising()->start();
}

bool BleMouse::sendReport(int8_t dx, int8_t dy, int8_t wheel, int8_t pan) {
  if (!_connected || _input == nullptr) return false;

  // 5 字节, 无 Report ID: buttons, X, Y, Wheel, AC Pan
  const uint8_t report[5] = {
      _buttons,
      static_cast<uint8_t>(dx),
      static_cast<uint8_t>(dy),
      static_cast<uint8_t>(wheel),
      static_cast<uint8_t>(pan),
  };
  const bool ok = _input->notify(report, sizeof(report));
  // 诊断: 打印非零报告 (定位手势是否发出)
  if (dx || dy || wheel || pan || _buttons) {
    static uint32_t lastLog = 0;
    if (millis() - lastLog >= 60) {
      lastLog = millis();
      Serial.printf("[rep] %02X %02X %02X %02X %02X\n",
                    report[0], report[1], report[2], report[3], report[4]);
    }
  }
  return ok;
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

bool BleMouse::sendControl(uint8_t cmd) {
  if (!_connected || _ctrl == nullptr) return false;
  return _ctrl->notify(&cmd, 1);
}

void BleMouse::setBatteryLevel(uint8_t percent) {
  if (percent == _battery) return;
  _battery = percent;
  if (_hid != nullptr) _hid->setBatteryLevel(percent, true);
}

void BleMouse::setTxPower(uint8_t /*level*/) {
  // ESP32-S3 上 setPower 部分档位返回 rc=-1，不使用
}

void BleMouse::onConnected() {
  _connected = true;
  // 连接期间继续广播: PC 端助手需要另开一条 GATT 连接来收语音命令。
  // ESP32 NimBLE 默认支持 3 条并发连接, HID 与助手互不影响。
  startAdvertising();
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
  if (encrypted) _hasBond = true;
}

void BleMouse::clearPairing() {
  Preferences prefs;
  prefs.begin("pairing", false);
  prefs.putBool("paired", false);
  prefs.end();
  if (NimBLEDevice::getNumBonds() > 0) {
    NimBLEDevice::deleteAllBonds();
  }
  _paired = false;
}

}  // namespace sw
