// ==========================================================================
// 文件：constants.h
// 功能：STREAM 物理核心共用的常数与输入有效性判据
// 主要内容：
//   1. 数学常数（kPi、角度转弧度 kRad）
//   2. 空气与水的物性（密度、比热、摩尔质量、气体常数）、重力、von Kármán 常数
//   3. Stefan–Boltzmann 常数（EB、辐射度、热红外成像与气象强迫的长波共用同一值）
//   4. LAI 有效范围（kMaxLai、validLai）与光谱网格尺寸
// 约定：单位见各常数行尾注释；温度 K，通量 W m-2，角度除非注明为弧度（kRad 换算）
//       数据流中均以度传递
// ==========================================================================

#pragma once

#include <cmath>

namespace stream {

// 模块：数学常数与几何缺省。kRad 取 0.01745329（比 π/180 略短），为与原有内核
//       逐位一致而保留，不要改成精确值（与 π/180 = 0.0174532925 的相对差约 1.4e-8，对角度换算无实际影响）。
constexpr double kPi = 3.14159265358979323846;
constexpr double kRad = 0.01745329;          // degrees to radians, same rounding as the original kernels
constexpr float kDefaultLeafProjection = 0.5f;

// 模块：空气/水物性与基本物理常数（固定值，单位见行尾）。
constexpr double kAirDensity = 1.2047;       // kg m-3
constexpr double kAirHeatCapacity = 1004.0;  // J kg-1 K-1
constexpr double kMolarMassAir = 28.96;      // g mol-1
constexpr double kMolarMassWater = 18.0;     // g mol-1
constexpr double kStefanBoltzmann = 5.670374419e-8; // W m-2 K-4 (CODATA 2018; FIX 2026-11-24 T1: one value for EB, radiosity, TIR view and forcing)
constexpr double kVonKarman = 0.4;
constexpr double kGravity = 9.80665;
constexpr double kDryAirGasConstant = 287.05;
constexpr double kKelvin = 273.15;

// 模块：LAI 有效范围。有效 = 有限、>= 0 且 <= kMaxLai；超出的（含 nodata、负值）按无效输入处理。
// FIX(2026-11-24 M1): largest LAI accepted as valid input (m2 m-2). The former 100 let packed-integer fill codes
// (e.g. MODIS LAI 249-254 x 0.1 = 24.9-25.4 when the file does not declare nodata) through as real canopies.
// nodata, negative and larger values are invalid; LAI = 0 (bare ground) stays valid.
constexpr double kMaxLai = 20.0;
inline bool validLai(double lai) { return std::isfinite(lai) && lai >= 0.0 && lai <= kMaxLai; }

// 模块：光学光谱网格（PROSPECT/BSM/太阳谱共用）：400-2400 nm，1 nm 间隔，共 2001 点。
constexpr int kSpectralCount = 2001;  // 400-2400 nm at 1 nm
constexpr int kFirstWavelength = 400;

}  // namespace stream
