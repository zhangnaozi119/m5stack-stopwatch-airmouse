/**
 * @file main.cpp
 * @brief M5AirMouse —— 陀螺仪空中鼠标
 *
 * 两种模式:
 *   空中飞鼠 (陀螺仪角速度 -> 指针速度)
 *   水平仪   (加速度计倾角 -> 指针速度, 可 8 向校准)
 *
 * 按键
 *   A 短按   左键单击        A 双击   左键双击 (打开)
 *   A 长按   按住左键拖拽
 *   B 短按   右键单击        B 双击   冻结/解冻
 *   B 长按   飞鼠: 切轴向    水平仪: 切 8 种轴向组合
 *   A+B 长按 切换 飞鼠 / 水平仪
 */
#include <M5Unified.h>
#include <HijelHID_BLEMouse.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "air_mouse.h"
#include "version.h"

namespace {

constexpr const char* kDeviceName = "M5 AirMouse 3";
constexpr int16_t kScreen = 466;

HijelBLEMouse g_mouse(kDeviceName, "M5Stack", 100, 3, false);
sw::AirMouse  g_air;

bool     g_holdLeft = false;
bool     g_frozen = false;
uint32_t g_suppressUntil = 0;

uint32_t g_lastImuUs = 0;
uint32_t g_lastSendMs = 0;
uint32_t g_lastUiMs = 0;
uint32_t g_vibUntil = 0;
uint8_t  g_lastBattery = 255;

// ---------------------------------------------------------------- 绘制

constexpr uint16_t kAccent = 0x07FF;
constexpr uint16_t kDim    = 0x8410;

void centerText(LovyanGFX& d, const char* s, int16_t y, uint16_t color,
                const lgfx::IFont* font = &fonts::efontCN_16) {
  d.setFont(font);
  d.setTextColor(color);
  d.drawString(s, kScreen / 2, y);
}

void redrawText(LovyanGFX& d, const char* s, int16_t y, int16_t w, int16_t h,
                uint16_t color, const lgfx::IFont* font) {
  d.fillRect((kScreen - w) / 2, y - h / 2, w, h, 0x0000);
  centerText(d, s, y, color, font);
}

char     g_lastState[32] = {0};
uint16_t g_lastStateColor = 0xFFFF;
char     g_lastSub[48] = {0};
char     g_lastFooter[64] = {0};
int16_t  g_lastDotX = -1, g_lastDotY = -1;
bool     g_lastCal = true;

void drawStatic() {
  M5.Display.fillScreen(0x0000);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setFont(&fonts::efontCN_24);
  M5.Display.setTextColor(kAccent);
  M5.Display.drawString("M5 AirMouse", kScreen / 2, 90);
  M5.Display.setFont(&fonts::efontCN_14);
  M5.Display.setTextColor(kDim);
  M5.Display.drawString("v" AIRMOUSE_VERSION, kScreen / 2, 124);
}

void drawUi() {
  auto& d = M5.Display;
  d.setTextDatum(textdatum_t::middle_center);

  // ---- 校准阶段
  if (!g_air.calibrated()) {
    if (g_lastCal) {
      d.fillRect(40, 168, kScreen - 80, 130, 0x0000);
      centerText(d, "正在校准，请保持静止", 200, 0xFD20);
      g_lastCal = false;
    }
    const int16_t fill = 260 * g_air.calibrateProgress() / 100;
    d.fillRect(103, 240, 260, 14, 0x0000);
    d.drawRoundRect(103, 240, 260, 14, 7, kDim);
    if (fill > 0) d.fillRoundRect(103, 240, fill, 14, 7, kAccent);
    char pct[16];
    snprintf(pct, sizeof(pct), "%u%%", g_air.calibrateProgress());
    redrawText(d, pct, 278, 120, 24, kDim, &fonts::efontCN_14);
    return;
  }
  if (!g_lastCal) {
    d.fillRect(40, 168, kScreen - 80, 130, 0x0000);
    g_lastCal = true;
    g_lastState[0] = '\0';
    g_lastSub[0] = '\0';
  }

  // ---- 状态行
  const bool conn = g_mouse.isConnected();
  const char* state = conn ? "已连接" : "等待配对…";
  const uint16_t stateColor = conn ? 0x07E0 : 0xFD20;
  if (strcmp(state, g_lastState) != 0 || stateColor != g_lastStateColor) {
    snprintf(g_lastState, sizeof(g_lastState), "%s", state);
    g_lastStateColor = stateColor;
    redrawText(d, state, 200, 320, 44, stateColor, &fonts::efontCN_24);
  }

  // ---- 副标题 / 实时读数
  char sub[48];
  if (!conn) {
    snprintf(sub, sizeof(sub), "在电脑蓝牙里选择 %s", kDeviceName);
  } else {
    snprintf(sub, sizeof(sub), "H %+5.1f  V %+5.1f  %s",
             g_air.lastHorizontalDps(), g_air.lastVerticalDps(),
             g_air.mode() == sw::AirMouse::Mode::Level ? "deg" : "dps");
  }
  if (strcmp(sub, g_lastSub) != 0) {
    snprintf(g_lastSub, sizeof(g_lastSub), "%s", sub);
    redrawText(d, sub, 244, 400, 22, kDim, &fonts::efontCN_14);
  }

  // ---- 方向指示
  if (conn) {
    const int16_t cx = kScreen / 2, cy = 316;
    const float h = g_air.lastHorizontalDps();
    const float v = g_air.lastVerticalDps();
    const float mag = std::fabs(h) + std::fabs(v);
    int16_t px = cx, py = cy;
    if (mag > 2.0f) {
      const float scale = mag > 60.0f ? (30.0f / mag) : 0.5f;
      px = cx + static_cast<int16_t>(h * scale);
      py = cy - static_cast<int16_t>(v * scale);
    }
    if (px != g_lastDotX || py != g_lastDotY) {
      if (g_lastDotX >= 0) d.fillRect(g_lastDotX - 8, g_lastDotY - 8, 16, 16, 0x0000);
      if (g_lastDotX < 0) d.drawCircle(cx, cy, 34, 0x2104);
      d.fillCircle(px, py, 6, kAccent);
      g_lastDotX = px;
      g_lastDotY = py;
    }
  }

  // ---- 底部状态
  char info[64];
  char modeBuf[24];
  const char* modeText;
  if (g_air.mode() == sw::AirMouse::Mode::Level) {
    snprintf(modeBuf, sizeof(modeBuf), "水平仪 %u", g_air.levelOrientation());
    modeText = modeBuf;
  } else {
    modeText = g_air.axisMode() == 0 ? "飞鼠 Z/Y" : "飞鼠 Z/X";
  }
  snprintf(info, sizeof(info), "%s   %s   电量 %u%%",
           modeText, g_frozen ? "已冻结" : "跟随",
           g_lastBattery == 255 ? 0 : g_lastBattery);
  if (strcmp(info, g_lastFooter) != 0) {
    snprintf(g_lastFooter, sizeof(g_lastFooter), "%s", info);
    redrawText(d, info, 400, 430, 24, kDim, &fonts::efontCN_14);
  }
}

// ---------------------------------------------------------------- 模式

void toggleMode() {
  const bool toLevel = g_air.mode() == sw::AirMouse::Mode::Rate;
  g_air.setMode(toLevel ? sw::AirMouse::Mode::Level : sw::AirMouse::Mode::Rate);
  g_frozen = false;
  M5.Power.setVibration(200);
  g_vibUntil = millis() + 120;
  M5.Speaker.tone(toLevel ? 1500 : 1000, 80, 0, true);
  g_lastFooter[0] = '\0';
}

// ---------------------------------------------------------------- 按键

void handleButtons() {
  static uint32_t bothSince = 0;
  static bool chordFired = false;

  const bool both = M5.BtnA.isPressed() && M5.BtnB.isPressed();
  if (both) {
    if (bothSince == 0) bothSince = millis();
    if (!chordFired && millis() - bothSince >= 600) {
      chordFired = true;
      if (g_holdLeft) {
        g_holdLeft = false;
        g_mouse.release(MouseButton::Left);
      }
      toggleMode();
      g_suppressUntil = millis() + 300;
    }
    return;
  }
  bothSince = 0;
  chordFired = false;
  if (millis() < g_suppressUntil) return;

  // A
  if (M5.BtnA.wasHold()) {
    g_holdLeft = true;
    g_mouse.press(MouseButton::Left);
  } else if (M5.BtnA.wasReleasedAfterHold()) {
    g_holdLeft = false;
    g_mouse.release(MouseButton::Left);
  } else if (M5.BtnA.wasDoubleClicked()) {
    g_mouse.doubleClick(MouseButton::Left, 30, 45);
  } else if (M5.BtnA.wasSingleClicked()) {
    g_mouse.click(MouseButton::Left);
  }

  // B
  if (M5.BtnB.wasHold()) {
    if (g_air.mode() == sw::AirMouse::Mode::Level) {
      g_air.setLevelOrientation((g_air.levelOrientation() + 1) & 7);
    } else {
      g_air.setAxisMode(g_air.axisMode() ^ 1);
    }
    g_air.resetAccum();
    M5.Power.setVibration(160);
    g_vibUntil = millis() + 150;
    M5.Speaker.tone(1200, 50, 0, true);
    g_lastFooter[0] = '\0';
  } else if (M5.BtnB.wasDoubleClicked()) {
    g_frozen = !g_frozen;
    g_air.resetAccum();
    M5.Speaker.tone(g_frozen ? 600 : 900, 60, 0, true);
  } else if (M5.BtnB.wasSingleClicked()) {
    g_mouse.click(MouseButton::Right);
  }
}

}  // namespace

// ================================================================ setup / loop

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);

  Serial.printf("[airmouse] M5AirMouse v%s reset_reason=%d\n",
                AIRMOUSE_VERSION, static_cast<int>(esp_reset_reason()));

  M5.Display.setRotation(0);
  M5.Display.setBrightness(120);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  drawStatic();

  g_air.begin();

  g_mouse.setUpdateRate(HIDRate::Hz125);
  g_mouse.setTxPower(3);
  g_mouse.setLogLevel(HIDLogLevel::Verbose);
  g_mouse.begin();
  Serial.println("[airmouse] HID 已启动");

  drawUi();
}

void loop() {
  M5.update();

  if (g_vibUntil && static_cast<int32_t>(millis() - g_vibUntil) >= 0) {
    M5.Power.setVibration(0);
    g_vibUntil = 0;
  }

  if (!g_air.calibrated()) {
    static uint32_t lastCal = 0;
    if (millis() - lastCal >= 20) {
      lastCal = millis();
      g_air.calibrateTick();
    }
  }

  handleButtons();

  const uint32_t nowUs = micros();
  const uint32_t nowMs = millis();
  if (g_lastImuUs == 0) g_lastImuUs = nowUs;

  if (g_air.calibrated() && g_mouse.isConnected() && !g_frozen) {
    if (nowMs - g_lastSendMs >= 8) {
      float dt = static_cast<float>(nowUs - g_lastImuUs) / 1000000.0f;
      if (dt > 0.05f) dt = 0.05f;
      g_lastImuUs = nowUs;

      int8_t dx = 0, dy = 0;
      if (g_air.update(dt, dx, dy)) {
        g_mouse.move(dx, dy);
      }
      g_lastSendMs = nowMs;
    }
  } else {
    g_lastImuUs = nowUs;
  }

  // 电量
  static uint32_t lastBatt = 0;
  if (nowMs - lastBatt >= 30000) {
    lastBatt = nowMs;
    const int lvl = static_cast<int>(M5.Power.getBatteryLevel());
    if (lvl >= 0 && lvl <= 100) {
      g_lastBattery = static_cast<uint8_t>(lvl);
      g_mouse.setBatteryLevel(g_lastBattery);
    }
  }

  // 界面 5fps
  if (nowMs - g_lastUiMs >= 200) {
    g_lastUiMs = nowMs;
    drawUi();
  }

  delay(1);
}
