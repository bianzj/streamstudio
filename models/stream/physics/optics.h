// ==========================================================================
// 文件：optics.h
// 功能：叶片 PROSPECT、土壤 BSM 光谱模型及其基础函数的声明（实现见 optics.cpp）
// 主要接口：
//   1. readOptCoeff：读取系数表 optipar_new.csv
//   2. prospect：叶片反射率/透射率（1 nm，400-2400 nm）
//   3. bsm：随体积含水量变化的土壤反射率
//   4. calctav / expint：PROSPECT 的界面平均透射率与指数积分
// 约定：光谱向量长度恒为 kSpectralCount = 2001；全部 float 计算
// ==========================================================================

#pragma once
#include "types.h"

#include <filesystem>
#include <vector>

namespace stream {

OptCoeff readOptCoeff(const std::filesystem::path& file);

// PROSPECT leaf reflectance and transmittance on the 1 nm grid.
void prospect(const OptCoeff& coeff, const LeafOptics& leaf, std::vector<float>& reflectance, std::vector<float>& transmittance);

// BSM soil reflectance for a volumetric soil moisture.
void bsm(const OptCoeff& coeff, const SoilOptics& soil, float moisture, std::vector<float>& reflectance);

float calctav(float alfa, float nr);
float expint(float x);

}  // namespace stream
