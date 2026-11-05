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

/// 两种工作模式（触控板手势已并入两者）
enum class AppMode : uint8_t { Air = 0, Level };

sw::BleMouse g_mouse;
sw::AirMouse  g_air;
sw::Touchpad  g_touchpad;

// 滚动惯性: 快滑/快滚后按衰减继续滚, Mac 触控板手感
float    g_scrollVel = 0;      ///< 最近滚动速度
uint32_t g_lastScrollMs = 0;
bool          g_touchActive = false;  ///< 拇指正按在屏幕上
AppMode       g_appMode = AppMode::Air;

bool     g_holdLeft = false;
bool     g_dragLock = false;   ///< 双击拖拽锁定 (drag lock)
bool     g_voiceOn = false;    ///< 语音输入进行中 (Ctrl+Win 已按下)
bool     g_voiceCancelled = false;  ///< 本次 A 按住期间语音已被取消
uint32_t g_voiceTouchSince = 0;     ///< A 按住后拇指首次触屏的时刻
bool     g_voiceAWasHeld = false;   ///< 上一帧 A 是否按住 (边沿检测)

// 省电状态机: 无输入 5 分钟降亮 + IMU 降频; 30 分钟深度睡眠
uint32_t g_lastActivityMs = 0;
uint8_t  g_brightNormal = 120;
bool     g_powerSave = false;
uint32_t g_lastImuSampleMs = 0;
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
  } else if (g_touchActive && g_touchpad.hint()[0]) {
    snprintf(sub, sizeof(sub), "%s", g_touchpad.hint());
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
  if (conn && g_touchActive) {
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
  if (conn && !g_touchActive) {
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
  if (g_appMode == AppMode::Air) {
    g_appMode = AppMode::Level;
    g_air.setMode(sw::AirMouse::Mode::Level);
    M5.Speaker.tone(1500, 80, 0, true);
  } else {
    g_appMode = AppMode::Air;
    g_air.setMode(sw::AirMouse::Mode::Rate);
    M5.Speaker.tone(1000, 80, 0, true);
  }
  g_frozen = false;
  g_pendX = g_pendY = 0;
  if (g_dragLock || g_holdLeft) {
    g_dragLock = false;
    g_holdLeft = false;
    g_mouse.setButtons(0);
  }
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
    const uint32_t heldMs = millis() - bothSince;
    // A+B 按住 10 秒 = 清除配对（换主机用）
    if (heldMs >= 10000) {
      g_mouse.clearPairing();
      M5.Display.setTextDatum(textdatum_t::middle_center);
      M5.Display.setTextDatum(textdatum_t::middle_center);
      M5.Display.setFont(&fonts::efontCN_24);
      M5.Display.setTextColor(0xFD20);
      M5.Display.drawString("已清除配对", kScreen / 2, 233);
      Serial.println("[ble] 用户请求清除配对");
      delay(1500);
      ESP.restart();
    }
    if (!chordFired && heldMs >= 600) {
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
  if (bothSince != 0 && !chordFired) {
    // A+B 同时按住后松开且未触发模式切换 = 中键
    const uint32_t held = millis() - bothSince;
    bothSince = 0;
    chordFired = false;
    if (held >= 50 && held < 600 && millis() >= g_suppressUntil) {
      g_mouse.click(sw::kBtnMiddle);
      M5.Power.setVibration(120);
      g_vibUntil = millis() + 80;
    }
    if (millis() < g_suppressUntil) return;
  } else {
    bothSince = 0;
    chordFired = false;
  }
  if (millis() < g_suppressUntil) return;

  // A
  // mediaHold: A 按住 + 拇指在屏上 = 媒体模式, A 的左键语义让位
  const bool mediaHold = M5.BtnA.isPressed() && g_touchActive;
  if (mediaHold) {
    // 媒体模式由 loop() 的手势分支处理
  } else if (M5.BtnA.wasHold()) {
    g_holdLeft = true;
    g_mouse.setButtons(sw::kBtnLeft);
  } else if (M5.BtnA.wasReleasedAfterHold()) {
    g_holdLeft = false;
    g_mouse.setButtons(0);
  } else if (M5.BtnA.wasDoubleClicked()) {
    if (g_dragLock) {
      // 已处于拖拽锁定: 双击 = 释放
      g_dragLock = false;
      g_mouse.setButtons(0);
      M5.Power.setVibration(200);
      g_vibUntil = millis() + 150;
    } else {
      g_mouse.doubleClick(sw::kBtnLeft, 30, 45);
      // Drag lock: 双击后进入拖拽锁定, 单击或移动结束? Mac 风格: 双击保持按下
      g_dragLock = true;
      g_mouse.setButtons(sw::kBtnLeft);  // 保持按下
      M5.Power.setVibration(150);
      g_vibUntil = millis() + 120;
    }
  } else if (M5.BtnA.wasSingleClicked()) {
    if (g_dragLock) {
      // 拖拽锁定中: 单击 = 释放
      g_dragLock = false;
      g_mouse.setButtons(0);
    } else {
      g_mouse.click(sw::kBtnLeft);
    }
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
    if (!g_voiceOn) g_mouse.click(sw::kBtnRight);
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
  g_brightNormal = 120;
  M5.Display.setTextDatum(textdatum_t::middle_center);
  drawStatic();

  g_air.begin();
  g_lastActivityMs = millis();


  if (!g_mouse.begin(kDeviceName)) {
    Serial.println("[airmouse] BLE 初始化失败");
  }
  Serial.println("[airmouse] HID 已启动");

  drawUi();
}

void loop() {
  M5.update();
  g_mouse.tick();  // 推进单击/双击时序

  // ---- 输入活动检测（按键 / 触摸 / 指针移动都算活动）
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.Touch.getDetail().isPressed() ||
      g_touchActive || std::fabs(g_air.lastHorizontalDps()) > 10.0f ||
      std::fabs(g_air.lastVerticalDps()) > 10.0f) {
    g_lastActivityMs = millis();
    if (g_powerSave) {
      g_powerSave = false;
      M5.Display.setBrightness(g_brightNormal);
    }
  }

  // ---- 省电: 5 分钟无输入 -> 降亮 + IMU 降到 1/4 采样率
  if (!g_powerSave && g_lastActivityMs != 0 &&
      millis() - g_lastActivityMs > 5UL * 60UL * 1000UL) {
    g_powerSave = true;
    M5.Display.setBrightness(30);
    Serial.println("[pwr] 进入省电模式");
  }
  // ---- 深度睡眠: 30 分钟无输入（B 键或 USB 唤醒）
  if (g_lastActivityMs != 0 &&
      millis() - g_lastActivityMs > 30UL * 60UL * 1000UL) {
    Serial.println("[pwr] 深度睡眠, 按任意键唤醒");
    M5.Display.setBrightness(0);
    // GPIO0 (BtnA) 低电平唤醒; 电源键也可唤醒
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_1, 0);   // BtnB = GPIO1
    esp_sleep_enable_ext1_wakeup(0x2ULL << 0, ESP_EXT1_WAKEUP_ANY_LOW);
    delay(100);
    esp_deep_sleep_start();
  }

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

  // ---- 触控板手势: 两种模式下都可用（滚动 + 快滑后退/前进）
  // 拇指按住屏幕期间暂停 IMU 指针，避免滚动的同时指针乱飞。
  // A 键按住 + 拇指：时间窗区分语音与媒体
  //   - 拇指触屏 0.5s 内不动  -> 语音开始 (Ctrl+Win 按住)
  //   - 0.5s 内就滑动          -> 本次转媒体 (音量/播放)
  //   - 语音进行中              -> 其它手势忽略
  const bool aHeld = M5.BtnA.isPressed();
  const bool mediaMode = aHeld && !g_voiceOn;
  sw::TouchOutput to;
  g_touchpad.update(nowMs, to);
  g_touchActive = g_touchpad.active();

  // ---- 语音输入: A 按住 + 拇指按屏 0.5s 不动 = Ctrl+Win 按住
  if (aHeld && !g_voiceAWasHeld) {
    // A 刚按下: 复位时间窗
    g_voiceCancelled = false;
    g_voiceTouchSince = 0;
  }
  g_voiceAWasHeld = aHeld;
  if (aHeld && g_touchActive && g_voiceTouchSince == 0) {
    g_voiceTouchSince = nowMs;
  }
  if (!aHeld) {
    g_voiceTouchSince = 0;
    g_voiceCancelled = false;
  }
  const bool touchMoved = to.wheel != 0 || to.pan != 0 || to.leftClick;
  if (aHeld && g_touchActive && !g_voiceOn && !g_voiceCancelled &&
      g_voiceTouchSince != 0 &&
      nowMs - g_voiceTouchSince >= 500 && !touchMoved) {
    g_voiceOn = g_mouse.voiceStart();
    if (g_voiceOn) {
      M5.Power.setVibration(255);
      g_vibUntil = nowMs + 200;   // 开始: 长震
      Serial.println("[voice] Ctrl+Win down");
    }
  }
  // 0.5s 内就动了 -> 本次取消语音 (标记到 A 松开为止)
  if (aHeld && g_touchActive && !g_voiceOn && !g_voiceCancelled &&
      g_voiceTouchSince != 0 && touchMoved) {
    g_voiceCancelled = true;
  }
  // 语音结束: A 松开 / 拇指离开 / 被取消
  if (g_voiceOn && (!aHeld || !g_touchActive)) {
    g_mouse.voiceStop();
    g_voiceOn = false;
    M5.Power.setVibration(90);
    g_vibUntil = nowMs + 60;        // 结束: 短震
    Serial.println("[voice] modifiers up");
  }

  if (mediaMode && !g_voiceOn) {
    // 媒体模式: 滚动转音量, 轻点转播放/暂停
    if (to.wheel != 0) {
      // 连续调节: 每 2 格一档, 音量键是增量式的
      const int steps = (to.wheel > 0) ? to.wheel : -to.wheel;
      for (int i = 0; i < steps; i += 2) {
        g_mouse.sendConsumer(to.wheel > 0 ? 0x00E9 : 0x00EA);  // Vol+ / Vol-
      }
      ++g_sentCount;
    }
    if (to.leftClick) {
      g_mouse.sendConsumer(0x00CD);  // Play/Pause
      M5.Power.setVibration(150);
      g_vibUntil = nowMs + 100;
    }
  } else {
    // 滚动 + 惯性
    if (to.wheel != 0) {
      g_mouse.move(0, 0, to.wheel, to.pan);
      ++g_sentCount;
      // 记录滚动速度 (格/秒), 供惯性使用
      const uint32_t dt = nowMs - g_lastScrollMs;
      if (dt > 0 && dt < 200) {
        g_scrollVel = g_scrollVel * 0.5f + (to.wheel * 1000.0f / dt) * 0.5f;
      }
      g_lastScrollMs = nowMs;
    } else if (to.pan != 0) {
      g_mouse.move(0, 0, to.wheel, to.pan);
      ++g_sentCount;
    } else {
      // 无手指输入时推进惯性
      if (std::fabs(g_scrollVel) > 2.0f && nowMs - g_lastScrollMs >= 30) {
        const int8_t coast = static_cast<int8_t>(
            g_scrollVel * 0.03f > 0 ? std::ceil(g_scrollVel * 0.03f)
                                    : std::floor(g_scrollVel * 0.03f));
        if (coast != 0) {
          g_mouse.move(0, 0, coast, 0);
          ++g_sentCount;
          g_scrollVel *= 0.92f;   // 每帧衰减 8%
          g_lastScrollMs = nowMs;
        } else {
          g_scrollVel = 0;
        }
      } else if (nowMs - g_lastScrollMs > 300) {
        g_scrollVel = 0;  // 停顿 300ms 后取消惯性
      }
    }
    if (to.leftClick)  { g_mouse.click(sw::kBtnLeft); g_scrollVel = 0; }
    if (to.back)       { g_mouse.click(sw::kBtnBack); g_scrollVel = 0; }
    if (to.forward)    { g_mouse.click(sw::kBtnForward); g_scrollVel = 0; }
  }

  if (g_air.calibrated() && g_mouse.isConnected() && !g_frozen && !g_touchActive) {
    // 采样: 高频读取传感器, 保证不丢动作
    const uint32_t imuInterval = g_powerSave ? 32 : 8;
    if (nowMs - g_lastImuMs >= imuInterval) {
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
    // 未连接 / 冻结 / 未校准 / 拇指按住屏幕: 保持时间基准,
    // 避免恢复瞬间产生大位移
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
