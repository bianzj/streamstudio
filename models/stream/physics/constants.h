#pragma once

namespace stream {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRad = 0.01745329;          // degrees to radians, same rounding as the original kernels
constexpr float kDefaultLeafProjection = 0.5f;

constexpr double kAirDensity = 1.2047;       // kg m-3
constexpr double kAirHeatCapacity = 1004.0;  // J kg-1 K-1
constexpr double kMolarMassAir = 28.96;      // g mol-1
constexpr double kMolarMassWater = 18.0;     // g mol-1
constexpr double kStefanBoltzmann = 5.67e-8; // W m-2 K-4
constexpr double kVonKarman = 0.4;
constexpr double kGravity = 9.80665;
constexpr double kDryAirGasConstant = 287.05;
constexpr double kKelvin = 273.15;

constexpr int kSpectralCount = 2001;  // 400-2400 nm at 1 nm
constexpr int kFirstWavelength = 400;

}  // namespace stream
