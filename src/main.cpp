/**
 * @file main.cpp
 * @brief M5AirMouse —— 陀螺仪空中鼠标
 *
 * 两种模式:
 *   空中鼠 (陀螺仪角速度 -> 指针速度)
 *   水平鼠 (加速度计倾角 -> 指针速度, 可 8 向校准)
 *
 * 按键
 *   A 短按   左键单击        A 双击   左键双击 (打开)
 *   A 长按   按住左键拖拽
 *   B 短按   右键单击        B 双击   冻结/解冻
 *   B 长按   空中鼠: 切轴向   水平鼠: 切 8 种轴向组合
 *   A+B 长按 切换 空中鼠 / 水平鼠
 */
#include <M5Unified.h>
#include "ble_mouse.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "air_mouse.h"
#include "touchpad.h"
#include "version.h"

namespace {

constexpr const char* kDeviceName = "M5 AirMouse";
constexpr int16_t kScreen = 466;

/// 三种工作模式
enum class AppMode : uint8_t { Air = 0, Level, Touch };

sw::BleMouse g_mouse;
sw::AirMouse  g_air;
sw::Touchpad  g_touchpad;
AppMode       g_appMode = AppMode::Air;

bool     g_holdLeft = false;
bool     g_frozen = false;
uint32_t g_suppressUntil = 0;

uint32_t g_lastImuUs = 0;
uint32_t g_lastImuMs = 0;   ///< 上次采样
uint32_t g_lastSendMs = 0;  ///< 上次把累积位移交给库
uint32_t g_lastUiMs = 0;
uint32_t g_vibUntil = 0;
uint8_t  g_lastBattery = 255;
uint32_t g_sentCount = 0;

// 库的 move() 是覆盖语义, 高频调用会丢失中间位移。
// 这里自己累积, 按固定节奏一次性提交。
int32_t g_pendX = 0;
int32_t g_pendY = 0;

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
char     g_lastDiag[48] = {0};
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
    g_lastDiag[0] = '\0';
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
  } else if (g_appMode == AppMode::Touch) {
    snprintf(sub, sizeof(sub), "%s", g_touchpad.hint()[0] ? g_touchpad.hint() : "滑动=滚动  快滑左右=后退/前进");
  } else {
    snprintf(sub, sizeof(sub), "H %+5.1f  V %+5.1f  %s",
             g_air.lastHorizontalDps(), g_air.lastVerticalDps(),
             g_air.mode() == sw::AirMouse::Mode::Level ? "deg" : "dps");
  }
  if (strcmp(sub, g_lastSub) != 0) {
    snprintf(g_lastSub, sizeof(g_lastSub), "%s", sub);
    redrawText(d, sub, 244, 400, 22, kDim, &fonts::efontCN_14);
  }

  // ---- 方向指示 / 触控点
  if (conn && g_appMode == AppMode::Touch) {
    // 触控板: 底部画一个小的"触控镜"，圆点位置按比例映射手指位置。
    // 直接在屏幕上按手指坐标画点会和手指重叠，缩小到中央更直观。
    constexpr int16_t padCx = kScreen / 2, padCy = 300, padR = 44;
    static bool padDrawn = false;
    static int16_t lastDotX = INT16_MAX, lastDotY = INT16_MAX;

    if (!padDrawn) {
      d.drawCircle(padCx, padCy, padR, 0x2104);
      padDrawn = true;
      lastDotX = lastDotY = INT16_MAX;
    }
    if (g_touchpad.active()) {
      // 手指坐标 (0..466) → 镜内坐标，clamp 到圆内
      int16_t dx = (g_touchpad.x() - padCx) * padR / 233;
      int16_t dy = (g_touchpad.y() - padCy) * padR / 233;
      const int32_t dist2 = static_cast<int32_t>(dx) * dx + static_cast<int32_t>(dy) * dy;
      if (dist2 > (padR - 8) * (padR - 8)) {
        const float k = (padR - 8) / std::sqrt(static_cast<float>(dist2));
        dx = static_cast<int16_t>(dx * k);
        dy = static_cast<int16_t>(dy * k);
      }
      const int16_t dotX = padCx + dx, dotY = padCy + dy;
      if (dotX != lastDotX || dotY != lastDotY) {
        if (lastDotX != INT16_MAX) d.fillCircle(lastDotX, lastDotY, 8, 0x0000);
        d.fillCircle(dotX, dotY, 8, kAccent);
        lastDotX = dotX; lastDotY = dotY;
      }
    } else if (lastDotX != INT16_MAX) {
      d.fillCircle(lastDotX, lastDotY, 8, 0x0000);
      lastDotX = lastDotY = INT16_MAX;
    }
  } else if (conn) {
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

  // ---- 自诊断: 报告是否真的发出去了 (拔线也能判断)
  {
    char diag[48];
    const bool paired = g_mouse.isPaired();
    if (conn) {
      snprintf(diag, sizeof(diag), "%s   报告 %lu",
               paired ? "已配对" : "直连",
               static_cast<unsigned long>(g_sentCount));
    } else {
      diag[0] = '\0';
    }
    if (strcmp(diag, g_lastDiag) != 0) {
      snprintf(g_lastDiag, sizeof(g_lastDiag), "%s", diag);
      const uint16_t c = !conn ? kDim : (g_sentCount > 0 ? 0x07E0 : 0xFD20);
      redrawText(d, diag, 362, 300, 22, c, &fonts::efontCN_14);
    }
  }

  // ---- 陀螺仪峰值 (3 秒滚动窗口), 用于确认动作是否产生旋转
  if (conn && g_appMode != AppMode::Touch) {
    static float pkX = 0, pkY = 0, pkZ = 0;
    static uint32_t pkStart = 0;
    float gx = 0, gy = 0, gz = 0;
    g_air.lastGyro(gx, gy, gz);
    if (std::fabs(gx) > pkX) pkX = std::fabs(gx);
    if (std::fabs(gy) > pkY) pkY = std::fabs(gy);
    if (std::fabs(gz) > pkZ) pkZ = std::fabs(gz);
    const uint32_t now = millis();
    if (now - pkStart >= 3000) {
      pkStart = now;
      pkX = pkY = pkZ = 0;
    }
    char pk[40];
    snprintf(pk, sizeof(pk), "峰值 X%3.0f Y%3.0f Z%3.0f", pkX, pkY, pkZ);
    static char lastPk[40] = {0};
    if (strcmp(pk, lastPk) != 0) {
      snprintf(lastPk, sizeof(lastPk), "%s", pk);
      redrawText(d, pk, 376, 300, 20, kDim, &fonts::efontCN_12);
    }
  }

  // ---- 底部状态
  char info[64];
  char modeBuf[24];
  const char* modeText;
  switch (g_appMode) {
    case AppMode::Level:
      snprintf(modeBuf, sizeof(modeBuf), "水平鼠 %u", g_air.levelOrientation());
      modeText = modeBuf;
      break;
    case AppMode::Touch:
      modeText = "触控板";
      break;
    default:
      modeText = g_air.axisMode() == 0 ? "空中鼠 竖Y" : "空中鼠 竖X";
      break;
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
  switch (g_appMode) {
    case AppMode::Air:
      g_appMode = AppMode::Level;
      g_air.setMode(sw::AirMouse::Mode::Level);
      M5.Speaker.tone(1500, 80, 0, true);
      break;
    case AppMode::Level:
      g_appMode = AppMode::Touch;
      g_touchpad.begin();
      M5.Speaker.tone(2000, 80, 0, true);
      break;
    case AppMode::Touch:
      g_appMode = AppMode::Air;
      g_air.setMode(sw::AirMouse::Mode::Rate);
      M5.Speaker.tone(1000, 80, 0, true);
      break;
  }
  g_frozen = false;
  g_pendX = g_pendY = 0;
  M5.Power.setVibration(200);
  g_vibUntil = millis() + 120;
  g_lastFooter[0] = '\0';
  g_lastState[0] = '\0';
  g_lastSub[0] = '\0';
  g_lastDiag[0] = '\0';
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
        g_mouse.setButtons(0);
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
    g_mouse.setButtons(sw::kBtnLeft);
  } else if (M5.BtnA.wasReleasedAfterHold()) {
    g_holdLeft = false;
    g_mouse.setButtons(0);
  } else if (M5.BtnA.wasDoubleClicked()) {
    g_mouse.doubleClick(sw::kBtnLeft, 30, 45);
  } else if (M5.BtnA.wasSingleClicked()) {
    g_mouse.click(sw::kBtnLeft);
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
    g_mouse.click(sw::kBtnRight);
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

  g_mouse.setTxPower(3);
  if (!g_mouse.begin(kDeviceName)) {
    Serial.println("[airmouse] BLE 初始化失败");
  }
  Serial.println("[airmouse] HID 已启动");

  drawUi();
}

void loop() {
  M5.update();
  g_mouse.tick();  // 推进单击/双击时序

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

  if (g_appMode == AppMode::Touch) {
    // ---- 触控板：滚动 + 轻点 + 快滑
    g_lastImuUs = nowUs;
    g_pendX = g_pendY = 0;
    if (g_mouse.isConnected() && !g_frozen) {
      sw::TouchOutput to;
      g_touchpad.update(nowMs, to);
      if (to.wheel != 0 || to.pan != 0) {
        g_mouse.move(0, 0, to.wheel, to.pan);
        ++g_sentCount;
      }
      if (to.leftClick)    g_mouse.click(sw::kBtnLeft);
      if (to.rightClick)   g_mouse.click(sw::kBtnRight);
      if (to.back)         g_mouse.click(sw::kBtnBack);
      if (to.forward)      g_mouse.click(sw::kBtnForward);
    }
  } else if (g_air.calibrated() && g_mouse.isConnected() && !g_frozen) {
    // 采样: 高频读取传感器, 保证不丢动作
    if (nowMs - g_lastImuMs >= 8) {
      float dt = static_cast<float>(nowUs - g_lastImuUs) / 1000000.0f;
      if (dt > 0.05f) dt = 0.05f;
      g_lastImuUs = nowUs;

      int8_t dx = 0, dy = 0;
      if (g_air.update(dt, dx, dy)) {
        g_pendX += dx;
        g_pendY += dy;
      }
      g_lastImuMs = nowMs;
    }

    // 提交: 30ms 一次 (约 33Hz), 与 BLE 连接间隔匹配, 避免积压
    if (nowMs - g_lastSendMs >= 30 && (g_pendX != 0 || g_pendY != 0)) {
      int32_t sx = g_pendX;
      int32_t sy = g_pendY;
      if (sx > 127) sx = 127;
      if (sx < -127) sx = -127;
      if (sy > 127) sy = 127;
      if (sy < -127) sy = -127;
      g_mouse.move(static_cast<int8_t>(sx), static_cast<int8_t>(sy));
      g_pendX -= sx;
      g_pendY -= sy;
      ++g_sentCount;
      g_lastSendMs = nowMs;
    }
  } else {
    // 未连接 / 冻结 / 未校准: 保持时间基准, 避免恢复瞬间产生大位移
    g_lastImuUs = nowUs;
    g_pendX = 0;
    g_pendY = 0;
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

  // 诊断: 有动作时立即打印; 静止时每 5 秒一次心跳
  {
    static uint32_t lastDbg = 0;
    static float peakH = 0, peakV = 0, peakG = 0;
    float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
    const bool okA = M5.Imu.getAccel(&ax, &ay, &az);
    const bool okG = M5.Imu.getGyro(&gx, &gy, &gz);
    const float mag = std::fabs(gx) + std::fabs(gy) + std::fabs(gz);
    if (mag > peakG) peakG = mag;
    const float h = std::fabs(g_air.lastHorizontalDps());
    const float v = std::fabs(g_air.lastVerticalDps());
    if (h > peakH) peakH = h;
    if (v > peakV) peakV = v;

    const bool active = mag > 15.0f;
    if (active || nowMs - lastDbg >= 5000) {
      lastDbg = nowMs;
      Serial.printf("[dbg] cal=%d conn=%d mode=%d okA=%d okG=%d "
                    "A(%.2f,%.2f,%.2f) G(%.1f,%.1f,%.1f) HV(%.1f,%.1f) "
                    "peakG=%.1f peakHV=%.1f/%.1f sent=%lu\n",
                    g_air.calibrated() ? 1 : 0, g_mouse.isConnected() ? 1 : 0,
                    static_cast<int>(g_air.mode()), okA ? 1 : 0, okG ? 1 : 0,
                    ax, ay, az, gx, gy, gz,
                    g_air.lastHorizontalDps(), g_air.lastVerticalDps(),
                    peakG, peakH, peakV,
                    static_cast<unsigned long>(g_sentCount));
      if (!active) { peakG = 0; peakH = 0; peakV = 0; }
    }
  }

  // 界面 5fps
  if (nowMs - g_lastUiMs >= 200) {
    g_lastUiMs = nowMs;
    drawUi();
  }

  delay(1);
}
