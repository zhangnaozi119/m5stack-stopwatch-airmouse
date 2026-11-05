/**
 * @file touchpad.h
 * @brief 单指触控板：上下滑 = 垂直滚动，左右滑 = 水平滚动，
 *        快滑左右 = 后退/前进，轻点 = 左键，双击 = 双击。
 *
 * 只用大拇指单指操作，不依赖多点触控。
 */
#pragma once

#include <stdint.h>

namespace sw {

/// 一次 update() 产生的动作
struct TouchOutput {
  int8_t  wheel = 0;         ///< 垂直滚动（正 = 向上滚）
  int8_t  pan = 0;           ///< 水平滚动（正 = 向右滚）
  bool    leftClick = false;
  bool    doubleClick = false;
  bool    rightClick = false;
  bool    back = false;      ///< 鼠标 4 键
  bool    forward = false;   ///< 鼠标 5 键
};

class Touchpad {
 public:
  void begin();

  /// 在主循环里调用；把产生的动作写入 out
  void update(uint32_t nowMs, TouchOutput& out);

  bool    active() const { return _state != State::Idle; }
  int16_t x() const { return _x; }
  int16_t y() const { return _y; }
  const char* hint() const { return _hint; }

 private:
  enum class State : uint8_t { Idle, Tracking, VScroll, HScroll };

  void emitScroll(float dx, float dy, int& wheel, int& pan);
  void finishGesture(uint32_t nowMs, TouchOutput& out);

  State    _state = State::Idle;
  uint32_t _startMs = 0;
  uint32_t _lastMs = 0;

  int16_t _startX = 0, _startY = 0;
  int16_t _x = 0, _y = 0;
  int16_t _lastX = 0, _lastY = 0;

  float _totalX = 0, _totalY = 0;
  float _accX = 0, _accY = 0;      ///< 不足一个滚动单位的余量
  float _peakVx = 0;               ///< 水平方向峰值速度 px/ms

  bool     _scrolled = false;      ///< 本次手势实际产生过滚动（用于取消点击）
  bool     _axisLatched = false;   ///< 轴是否已锁定
  bool     _latchedH = false;      ///< 锁定的轴：true = 水平
  uint32_t _lastTapMs = 0;         ///< 上一次轻点时刻（判双击）
  const char* _hint = "";

  // ---- 阈值 ----
  static constexpr float kSwipeThreshold = 5.0f;   ///< 越过即开始滚动（越小越跟手）
  static constexpr float kTapMoveMax = 8.0f;       ///< 轻点的最大位移
  static constexpr uint32_t kTapMaxMs = 250;       ///< 轻点的最长时长
  static constexpr uint32_t kDoubleTapMs = 320;    ///< 双击间隔上限
  static constexpr float kAxisLockRatio = 1.6f;    ///< 另一轴超过当前轴多少倍才翻转
  static constexpr int   kMaxStepPerFrame = 2;     ///< 单帧最多发出的滚动格数
  static constexpr float kScrollStep = 6.0f;       ///< 垂直滚动: 每 6 px 一格
  static constexpr float kPanStep = 3.0f;          ///< 水平滚动: 每 3 px 一格（更灵敏）
  static constexpr float kFlickVx = 1.8f;          ///< 快滑的峰值速度 px/ms
  static constexpr float kFlickDist = 120.0f;      ///< 快滑的最小位移
  static constexpr uint32_t kFlickMaxMs = 350;     ///< 快滑的最长时长
};

}  // namespace sw
