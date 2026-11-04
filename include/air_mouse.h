/**
 * @file air_mouse.h
 * @brief 传感器 -> 鼠标指针
 *
 * 两种模式, 输出都是相对位移:
 *   Rate    空中飞鼠: 陀螺仪角速度 -> 指针速度 (手停即停)
 *   Level   水平仪:   加速度计倾角   -> 指针速度 (回正即停, 像摇杆)
 */
#pragma once

#include <stdint.h>

namespace sw {

class AirMouse {
 public:
  enum class Mode : uint8_t {
    Rate = 0,   ///< 空中飞鼠
    Level,      ///< 水平仪
  };

  void begin();

  Mode mode() const { return _mode; }
  void setMode(Mode m);

  /// 进入水平仪模式时, 把当前姿态记为"水平"基准
  void beginLevel();

  /// 开机静止校准 (陀螺零偏): 每次调用采一个样本, 采满后返回 true
  bool calibrateTick();
  bool calibrated() const { return _calibrated; }
  uint8_t calibrateProgress() const;

  /// 按当前模式算出相对位移; 产生整数像素时返回 true
  bool update(float dtSec, int8_t& dx, int8_t& dy);

  void  setGain(float gain) { _gain = gain; }
  float gain() const { return _gain; }

  /// 轴向 (仅飞鼠模式): 0 = 水平 Z / 垂直 Y; 1 = 水平 Z / 垂直 X
  void    setAxisMode(uint8_t mode) { _axisMode = mode & 1; }
  uint8_t axisMode() const { return _axisMode; }

  void setInvertX(bool inv) { _invertX = inv; }
  bool invertX() const { return _invertX; }

  /// 水平仪模式下的轴向 (0~7): 低 2 位是 90° 旋转, bit2 是水平镜像
  void    setLevelOrientation(uint8_t o) {
    _levelRot    = o & 3;
    _levelMirror = (o & 4) != 0;
  }
  uint8_t levelOrientation() const {
    return static_cast<uint8_t>(_levelRot | (_levelMirror ? 4 : 0));
  }

  void resetAccum() { _accumX = _accumY = 0; _lpX = _lpY = 0; }

  float lastHorizontalDps() const { return _lastH; }
  float lastVerticalDps() const { return _lastV; }

  /// 最近一次读取的原始角速度 (诊断用)
  void lastGyro(float& x, float& y, float& z) const { x = _gxRaw; y = _gyRaw; z = _gzRaw; }

 private:
  bool     _calibrated = false;
  uint16_t _calSamples = 0;
  double   _calSumX = 0, _calSumY = 0, _calSumZ = 0;

  float _biasX = 0, _biasY = 0, _biasZ = 0;

  float   _gain = 14.0f;       ///< 空中鼠: 像素 / (度/秒 · 秒)
  float   _levelGain = 60.0f;  ///< 水平鼠: 像素 / (度 · 秒)
  uint8_t _axisMode = 0;       ///< 0=世界坐标投影  1=设备坐标
  uint8_t _levelRot = 1;       ///< 水平鼠轴向旋转
  bool    _levelMirror = false;///< 水平鼠水平镜像
  bool    _invertX = true;   // 实测左右方向相反

  // 世界坐标投影用的重力方向 (设备坐标系, 低通后)
  float _upX = 0.0f, _upY = 0.0f, _upZ = 1.0f;
  bool  _upValid = false;

  Mode  _mode = Mode::Rate;
  float _refRoll = 0, _refPitch = 0;

  float _lpX = 0, _lpY = 0;
  float _accumX = 0, _accumY = 0;
  float _lastH = 0, _lastV = 0;
  float _gxRaw = 0, _gyRaw = 0, _gzRaw = 0;

  static constexpr float kDeadzoneDps = 0.8f;   ///< 飞鼠死区
  static constexpr float kDeadzoneDeg = 1.5f;   ///< 水平仪死区
  static constexpr float kLpfAlpha   = 0.35f;
  static constexpr float kMaxStep    = 40.0f;
  static constexpr float kMaxTiltDeg = 30.0f;   ///< 倾角上限
  static constexpr uint16_t kCalSamples = 30;
  static constexpr float kCalStableDps = 8.0f;
};

}  // namespace sw
