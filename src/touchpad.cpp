/**
 * @file touchpad.cpp
 * @brief 单指触控板实现
 */
#include "touchpad.h"

#include <M5Unified.h>

#include <cmath>

namespace sw {

void Touchpad::begin() {
  _state = State::Idle;
  _hint = "";
  _lastTapMs = 0;
}

void Touchpad::update(uint32_t nowMs, TouchOutput& out) {
  auto& t = M5.Touch.getDetail();

  // ---- 按下：进入跟踪
  if (t.wasPressed()) {
    _state       = State::Tracking;
    _startMs     = _lastMs = nowMs;
    _startX      = _lastX = _x = t.x;
    _startY      = _lastY = _y = t.y;
    _totalX = _totalY = _accX = _accY = 0;
    _peakVx      = 0;
    _scrolled    = false;
    _axisLatched = false;
    _latchedH    = false;
    _hint        = "轻点=左键  连点两下=双击";
    return;
  }

  // ---- 按住移动
  if (t.isPressed() && _state != State::Idle) {
    const int16_t nx = t.x;
    const int16_t ny = t.y;
    const float   dx = static_cast<float>(nx - _lastX);
    const float   dy = static_cast<float>(ny - _lastY);
    const uint32_t dt = (nowMs > _lastMs) ? (nowMs - _lastMs) : 1;

    _x = nx;
    _y = ny;
    _totalX += dx;
    _totalY += dy;

    const float vx = std::fabs(dx) / static_cast<float>(dt);
    if (vx > _peakVx) _peakVx = vx;

    _lastX  = nx;
    _lastY  = ny;
    _lastMs = nowMs;

    // ---- 轴锁定：首次判定后保持，另一轴要明显超过才允许改判。
    // 每帧重新比较两轴会让斜滑在手势中途来回翻转，表现为滚动抖动。
    if (!_axisLatched) {
      if (std::fabs(_totalX) > kSwipeThreshold ||
          std::fabs(_totalY) > kSwipeThreshold) {
        _axisLatched = true;
        _latchedH    = std::fabs(_totalX) > std::fabs(_totalY);
      }
    } else {
      const float cur   = _latchedH ? std::fabs(_totalX) : std::fabs(_totalY);
      const float other = _latchedH ? std::fabs(_totalY) : std::fabs(_totalX);
      if (other > cur * kAxisLockRatio) _latchedH = !_latchedH;
    }

    if (_axisLatched) {
      _state = _latchedH ? State::HScroll : State::VScroll;
      _hint  = _latchedH ? "左右滑=水平滚动  快滑=后退/前进"
                         : "上下滑=垂直滚动";

      // 快滑期间不产生滚动，避免与"后退/前进"叠加。
      // 必须用"当前帧"速度：用整段手势的峰值会因起手抖动而误判。
      const float speed =
          std::fabs(_latchedH ? dx : dy) / static_cast<float>(dt);
      if (speed < kFlickVx) {
        int w = 0, p = 0;
        emitScroll(dx, dy, w, p);
        // 单帧钳制：掉帧攒下的大量位移不要一次性甩出去，否则屏幕上是一次猛跳
        if (w >  kMaxStepPerFrame) w =  kMaxStepPerFrame;
        if (w < -kMaxStepPerFrame) w = -kMaxStepPerFrame;
        if (p >  kMaxStepPerFrame) p =  kMaxStepPerFrame;
        if (p < -kMaxStepPerFrame) p = -kMaxStepPerFrame;
        out.wheel += static_cast<int8_t>(w);
        out.pan   += static_cast<int8_t>(p);
        if (w != 0 || p != 0) _scrolled = true;
      }
    }
    return;
  }

  // ---- 抬起：判定轻点 / 快滑
  if (t.wasReleased() && _state != State::Idle) {
    finishGesture(nowMs, out);
    _state = State::Idle;
    _hint  = "";
    return;
  }
}

void Touchpad::emitScroll(float dx, float dy, int& wheel, int& pan) {
  if (_state == State::VScroll) {
    _accY += dy;
    while (_accY <= -kScrollStep) { wheel += 1; _accY += kScrollStep; }
    while (_accY >= kScrollStep)  { wheel -= 1; _accY -= kScrollStep; }
  } else if (_state == State::HScroll) {
    _accX += dx;
    while (_accX >= kPanStep)  { pan += 1; _accX -= kPanStep; }
    while (_accX <= -kPanStep) { pan -= 1; _accX += kPanStep; }
  }
}

void Touchpad::finishGesture(uint32_t nowMs, TouchOutput& out) {
  const uint32_t dur = nowMs - _startMs;

  // ---- 轻点 = 左键；本次手势一旦产生过滚动就不再算轻点
  if (!_scrolled && dur < kTapMaxMs &&
      std::fabs(_totalX) < kTapMoveMax && std::fabs(_totalY) < kTapMoveMax) {
    out.leftClick = true;
    if (_lastTapMs != 0 && nowMs - _lastTapMs < kDoubleTapMs) {
      out.doubleClick = true;  // 仅供界面提示，实际仍是一次点击
    }
    _lastTapMs = nowMs;
    return;
  }

  // ---- 快滑：向左后退，向右前进
  if (_state == State::HScroll && dur < kFlickMaxMs &&
      std::fabs(_totalX) > kFlickDist && _peakVx > kFlickVx) {
    if (_totalX < 0) {
      out.back = true;
    } else {
      out.forward = true;
    }
  }
}

}  // namespace sw
