#pragma once

#include <cmath>

namespace stream {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRad = 0.01745329;          // degrees to radians, same rounding as the original kernels
constexpr float kDefaultLeafProjection = 0.5f;

constexpr double kAirDensity = 1.2047;       // kg m-3
constexpr double kAirHeatCapacity = 1004.0;  // J kg-1 K-1
constexpr double kMolarMassAir = 28.96;      // g mol-1
constexpr double kMolarMassWater = 18.0;     // g mol-1
constexpr double kStefanBoltzmann = 5.670374419e-8; // W m-2 K-4 (CODATA 2018; FIX 2026-11-24 T1: one value for EB, radiosity, TIR view and forcing)
constexpr double kVonKarman = 0.4;
constexpr double kGravity = 9.80665;
constexpr double kDryAirGasConstant = 287.05;
constexpr double kKelvin = 273.15;

// FIX(2026-11-24 M1): largest LAI accepted as valid input (m2 m-2). The former 100 let packed-integer fill codes
// (e.g. MODIS LAI 249-254 x 0.1 = 24.9-25.4 when the file does not declare nodata) through as real canopies.
// nodata, negative and larger values are invalid; LAI = 0 (bare ground) stays valid.
constexpr double kMaxLai = 20.0;
inline bool validLai(double lai) { return std::isfinite(lai) && lai >= 0.0 && lai <= kMaxLai; }

constexpr int kSpectralCount = 2001;  // 400-2400 nm at 1 nm
constexpr int kFirstWavelength = 400;

}  // namespace stream
