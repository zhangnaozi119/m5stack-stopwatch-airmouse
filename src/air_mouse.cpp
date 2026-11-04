/**
 * @file air_mouse.cpp
 * @brief 传感器 -> 鼠标指针实现
 */
#include "air_mouse.h"

#include <M5Unified.h>

#include <cmath>

namespace sw {
namespace {

float deadzone(float v, float dz) {
  if (std::fabs(v) < dz) return 0.0f;
  return v > 0 ? v - dz : v + dz;
}

float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

int8_t clampStep(float v) {
  if (v > 127.0f) v = 127.0f;
  if (v < -127.0f) v = -127.0f;
  return static_cast<int8_t>(std::lround(v));
}

}  // namespace

void AirMouse::begin() {
  _calibrated = false;
  _calSamples = 0;
  _calSumX = _calSumY = _calSumZ = 0;
  _biasX = _biasY = _biasZ = 0;
  resetAccum();
}

uint8_t AirMouse::calibrateProgress() const {
  return static_cast<uint8_t>(_calSamples * 100 / kCalSamples);
}

void AirMouse::setMode(Mode m) {
  _mode = m;
  resetAccum();
  if (m == Mode::Level) beginLevel();
}

void AirMouse::beginLevel() {
  float ax = 0, ay = 0, az = 0;
  if (M5.Imu.getAccel(&ax, &ay, &az)) {
    _refRoll  = std::atan2(ax, az) * 57.29578f;
    _refPitch = std::atan2(ay, az) * 57.29578f;
  }
  resetAccum();
}

bool AirMouse::calibrateTick() {
  if (_calibrated) return true;

  float gx = 0, gy = 0, gz = 0;
  if (!M5.Imu.getGyro(&gx, &gy, &gz)) return false;

  if (std::fabs(gx) + std::fabs(gy) + std::fabs(gz) > kCalStableDps) {
    _calSamples = 0;
    _calSumX = _calSumY = _calSumZ = 0;
    return false;
  }

  _calSumX += gx;
  _calSumY += gy;
  _calSumZ += gz;
  if (++_calSamples < kCalSamples) return false;

  _biasX = static_cast<float>(_calSumX / _calSamples);
  _biasY = static_cast<float>(_calSumY / _calSamples);
  _biasZ = static_cast<float>(_calSumZ / _calSamples);
  _calibrated = true;
  resetAccum();
  return true;
}

bool AirMouse::update(float dtSec, int8_t& dx, int8_t& dy) {
  dx = 0;
  dy = 0;
  if (!_calibrated || dtSec <= 0.0f) return false;

  float h = 0, v = 0;

  if (_mode == Mode::Rate) {
    // ---- 空中鼠: 陀螺仪角速度
    float gx = 0, gy = 0, gz = 0;
    if (!M5.Imu.getGyro(&gx, &gy, &gz)) return false;

    if (_axisMode == 0) {
      // 世界坐标投影: 用加速度计估计重力方向, 把角速度分解为
      //   yaw   (绕世界竖直轴) -> 水平
      //   pitch (绕世界水平右轴) -> 垂直
      // 这样无论平放还是竖握, 响应都一致。
      float ax = 0, ay = 0, az = 0;
      if (M5.Imu.getAccel(&ax, &ay, &az)) {
        float n = std::sqrt(ax * ax + ay * ay + az * az);
        if (n > 0.3f) {
          // 低通, 抑制加速度计噪声
          _upX = _upX * 0.9f + (ax / n) * 0.1f;
          _upY = _upY * 0.9f + (ay / n) * 0.1f;
          _upZ = _upZ * 0.9f + (az / n) * 0.1f;
          _upValid = true;
        }
      }

      if (_upValid) {
        float ux = _upX, uy = _upY, uz = _upZ;
        const float un = std::sqrt(ux * ux + uy * uy + uz * uz);
        if (un > 0.1f) { ux /= un; uy /= un; uz /= un; }

        // 设备 X 轴投影到水平面 -> 世界"右"方向
        const float d = ux;  // dot((1,0,0), up)
        float rx = 1.0f - d * ux;
        float ry = -d * uy;
        float rz = -d * uz;
        const float rn = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (rn > 0.2f) {
          rx /= rn; ry /= rn; rz /= rn;
          h = gx * ux + gy * uy + gz * uz;      // yaw
          v = gx * rx + gy * ry + gz * rz;      // pitch
        } else {
          h = gx; v = gy;  // 设备 X 轴几乎竖直, 退化
        }
      } else {
        h = gx; v = gy;
      }
    } else {
      // 设备坐标 (旧行为), 适合固定握姿
      h = gx - _biasX;
      v = gy - _biasY;
    }
    _lastH = h;
    _lastV = v;
    h = deadzone(h, kDeadzoneDps);
    v = deadzone(v, kDeadzoneDps);
    _lpX = _lpX * (1.0f - kLpfAlpha) + h * kLpfAlpha;
    _lpY = _lpY * (1.0f - kLpfAlpha) + v * kLpfAlpha;
    _accumX += (_invertX ? -1.0f : 1.0f) * _lpX * _gain * dtSec;
    _accumY += -_lpY * _gain * dtSec;
  } else {
    // ---- 水平仪: 加速度计倾角 (相对进入时的姿态)
    float ax = 0, ay = 0, az = 0;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return false;
    float roll  = std::atan2(ax, az) * 57.29578f - _refRoll;
    float pitch = std::atan2(ay, az) * 57.29578f - _refPitch;
    roll  = clampf(roll, -kMaxTiltDeg, kMaxTiltDeg);
    pitch = clampf(pitch, -kMaxTiltDeg, kMaxTiltDeg);

    // 圆屏握持方向不固定, 按 90° 步进旋转轴向
    float rx = roll, ry = pitch;
    switch (_levelRot) {
      case 1:  rx = -pitch; ry = roll;  break;
      case 2:  rx = -roll;  ry = -pitch; break;
      case 3:  rx = pitch;  ry = -roll; break;
      default: break;
    }
    _lastH = rx;
    _lastV = ry;

    rx = deadzone(rx, kDeadzoneDeg);
    ry = deadzone(ry, kDeadzoneDeg);
    _lpX = _lpX * (1.0f - kLpfAlpha) + rx * kLpfAlpha;
    _lpY = _lpY * (1.0f - kLpfAlpha) + ry * kLpfAlpha;
    _accumX += (_levelMirror ? -1.0f : 1.0f) * _lpX * _levelGain * dtSec;
    _accumY += -_lpY * _levelGain * dtSec;
  }

  _accumX = clampf(_accumX, -kMaxStep, kMaxStep);
  _accumY = clampf(_accumY, -kMaxStep, kMaxStep);

  const int8_t stepX = clampStep(_accumX);
  const int8_t stepY = clampStep(_accumY);
  if (stepX == 0 && stepY == 0) return false;

  _accumX -= stepX;
  _accumY -= stepY;
  dx = stepX;
  dy = stepY;
  return true;
}

}  // namespace sw
