// ==========================================================================
// 文件：effective_lai.h
// 功能：离散树冠/垄行冠层的有效 LAI 接口声明（实现见 effective_lai.cpp）
// 状态：当前引擎未调用，运行时所有冠层按均质冠层处理（见下方说明与 PORTING.md）
// 主要接口：
//   1. directionalGap / directionalLai：指定方向的间隙率与有效 LAI
//   2. hemisphericalLai / hemisphericalLaiAnalytical：半球有效 LAI（数值积分/Gauss 积分）
//   3. effectiveLai：一次返回半球、太阳方向与观测方向三个有效 LAI
// 约定：角度为度；均质冠层在所有方向返回自身 LAI
// ==========================================================================

#pragma once
#include "types.h"

namespace stream {

// 结构：三个有效 LAI——hemispherical（半球）、solar（太阳方向）、viewing（观测方向）。
struct EffectiveLai {
    float hemispherical = 0.0f;
    float solar = 0.0f;
    float viewing = 0.0f;
};

// Direction-specific effective LAI of homogeneous, crown or row canopies. A homogeneous
// canopy (no crown or row structure) returns its own LAI for every direction.
// FIX(2026-11-21 C9): NOT CALLED anywhere in this engine yet (kept for the later crown / row port); the runtime treats
// every canopy as homogeneous and warns when crown / row parameters are configured (scene/classes.cpp, app/image.cpp).
float directionalGap(const Canopy& canopy, const CropRows& rows, float zenithDeg, float azimuthDeg);
float directionalLai(const Canopy& canopy, const CropRows& rows, float zenithDeg, float azimuthDeg);
float hemisphericalLai(const Canopy& canopy, const CropRows& rows);
// Gauss-Legendre gap quadrature mapped to the equivalent homogeneous LAI.
float hemisphericalLaiAnalytical(const Canopy& canopy, const CropRows& rows);

EffectiveLai effectiveLai(const Canopy& canopy, const CropRows& rows, float szaDeg, float saaDeg,
                          float vzaDeg, float vaaDeg, bool analyticHemisphere = false);

}  // namespace stream
