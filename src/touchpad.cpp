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
    _state   = State::Tracking;
    _startMs = _lastMs = nowMs;
    _startX = _lastX = _x = t.x;
    _startY = _lastY = _y = t.y;
    _totalX = _totalY = _accX = _accY = 0;
    _peakVx = 0;
    _moved  = false;
    _hint   = "轻点=左键  连点两下=双击";
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

    if (std::fabs(_totalX) > kMoveThreshold || std::fabs(_totalY) > kMoveThreshold) {
      _moved = true;
    }

    if (_moved) {
      if (std::fabs(_totalX) > std::fabs(_totalY)) {
        _state = State::HScroll;
        _hint  = "左右滑=水平滚动  快滑=后退/前进";
      } else {
        _state = State::VScroll;
        _hint  = "上下滑=垂直滚动";
      }
      // 快滑期间不产生滚动，避免与"后退/前进"叠加
      if (_peakVx < kFlickVx) {
        emitScroll(dx, dy, out);
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

void Touchpad::emitScroll(float dx, float dy, TouchOutput& out) {
  if (_state == State::VScroll) {
    _accY += dy;
    while (_accY <= -kScrollStep) { out.wheel += 1; _accY += kScrollStep; }
    while (_accY >= kScrollStep)  { out.wheel -= 1; _accY -= kScrollStep; }
  } else if (_state == State::HScroll) {
    _accX += dx;
    while (_accX >= kScrollStep)  { out.pan += 1; _accX -= kScrollStep; }
    while (_accX <= -kScrollStep) { out.pan -= 1; _accX += kScrollStep; }
  }
}

void Touchpad::finishGesture(uint32_t nowMs, TouchOutput& out) {
  const uint32_t dur = nowMs - _startMs;

  // ---- 轻点 = 左键；连点两下由系统识别为双击
  if (!_moved && dur < kTapMaxMs &&
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
