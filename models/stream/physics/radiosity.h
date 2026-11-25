// ==========================================================================
// 文件：radiosity.h
// 功能：hom3 均匀冠层节点辐射度的接口声明（实现与算法说明见 radiosity.cpp）。
// 主要模块：
//   1. 常量 kHomNodes / kHomStates 与 HomCanopyT（由 LAI 生成的节点几何：容量、交换因子、到达矩阵）
//   2. homAbsorbed / homAbsorbedPair：短波（也可含发射源）全阶吸收
//   3. homThermalScatter：热红外多次散射对有效发射率的贡献
//   4. homBeamMultiple / homSkyMultiple：可见/近红外方向多次散射反射
//   5. homLongwave：长波全阶吸收（天空下行 + 各节点发射）
// 数据流：调用方给出 LAI、反照率/反射/透射或发射率、太阳与观测天顶角 → 返回各节点吸收或方向散射项。
// 约定：所有函数是以标量类型 T 为参数的模板；T = double 为正向，T = Ad（dual.h）带导数；
//       SameAs<T> 只是阻止模板参数从该实参推导；天顶角单位为度；通量单位 W m-2。
// ==========================================================================

#pragma once
#include "constants.h"
#include "dual.h"

#include <type_traits>

namespace stream {

// 模块：hom3 节点辐射度。节点数 kHomNodes = 3（土壤、下层冠层、上层冠层），
// 状态数 kHomStates = 5（3 个节点出射 + 2 个冠层向下半部分）。
// Homogeneous canopy node radiosity (TiRT "hom3": soil + two equal canopy layers).
// Capacities: soil = 1, each layer = 4 tau with tau = G LAI / 2. Exchange factors follow from
// reciprocity with the diffuse interception X(tau) = 1 - 2 E3(tau); every order of shortwave and
// longwave exchange is solved in the 5-state system (3 up states + 2 down-going canopy halves).
//
// Every kernel is a template on the scalar type: T = double is the forward model, T = Ad (physics/dual.h)
// carries exact derivatives with respect to LAI, soil albedo / emissivity and component temperatures.
constexpr int kHomNodes = 3;
constexpr int kHomStates = 5;

// SameAs<T>：非推导上下文，让 T 只由 HomCanopyT<T> 等主参数推导，标量实参可隐式转换。
template <class T> using SameAs = std::type_identity_t<T>;

template <class T>
// 结构体：由 LAI 构造的节点几何，含容量 capacity、交换因子 factors、逃逸概率 escape、
// 到达矩阵 arrive[状态][到达通道]（通道 2n 从下方、2n+1 从上方到达节点 n）、辐亮度系数 radiance。
struct HomCanopyT {
    explicit HomCanopyT(const T& lai);

    T capacity[kHomNodes]{};
    T factors[kHomNodes][kHomNodes]{};
    T escape[kHomNodes]{};
    bool soil[kHomNodes]{};
    int down[kHomNodes]{};  // down-going state of a canopy node; -1 for soil
    T arrive[kHomStates][2 * kHomNodes]{};
    T radiance[kHomStates]{};

    // Transmission of a unit horizontal beam from above down to each node (soil gap, then layer hits).
    void split(double zenithDeg, T out[kHomNodes]) const;
};
using HomCanopy = HomCanopyT<double>;

// Exponential integral E3(x), x >= 0.
double exponentialIntegral3(double x);

// Absorption per node (W m-2 of ground) from external arrivals and from the scattered field.
// soilAlbedo / leafRho / leafTau are per band; a null sources pointer means pure reflection.
template <class T>
void homAbsorbed(const HomCanopyT<T>& nodes, const SameAs<T>& soilAlbedo, const SameAs<T>& leafRho,
                 const SameAs<T>& leafTau, const T arrivals[2 * kHomNodes], const T* sources,
                 T first[kHomNodes], T total[kHomNodes]);

// Two arrival sets sharing one band's system matrix (beam and sky), solved together.
template <class T>
void homAbsorbedPair(const HomCanopyT<T>& nodes, const SameAs<T>& soilAlbedo, double leafRho, double leafTau,
                     const T arrivalsA[2 * kHomNodes], const T arrivalsB[2 * kHomNodes],
                     T firstA[kHomNodes], T totalA[kHomNodes], T firstB[kHomNodes], T totalB[kHomNodes]);

// 热红外多次散射贡献（对应四个组分的有效发射率增量，TiRT 比例之外的部分）。
// Multiple-scattering effective emissivity contributions [soil sun, soil shade, leaf sun, leaf shade].
template <class T> struct ThermalScatterT { T soilSun{}, soilShade{}, leafSun{}, leafShade{}; };
using ThermalScatter = ThermalScatterT<double>;
template <class T>
ThermalScatterT<T> homThermalScatter(const HomCanopyT<T>& nodes, const SameAs<T>& leafEmissivity,
                                     const SameAs<T>& soilEmissivity, double viewZenithDeg, double sunZenithDeg);

// 方向多次散射反射中来自冠层与来自土壤的两部分。
// Orders >= 2 of a unit beam and all orders of a unit isotropic sky, seen along the view.
template <class T> struct DirectionalMultipleT { T canopy{}, soil{}; };
using DirectionalMultiple = DirectionalMultipleT<double>;
template <class T>
DirectionalMultipleT<T> homBeamMultiple(const HomCanopyT<T>& nodes, const SameAs<T>& soilAlbedo, double leafRho,
                                        double leafTau, double sunZenithDeg, double viewZenithDeg);
template <class T>
DirectionalMultipleT<T> homSkyMultiple(const HomCanopyT<T>& nodes, const SameAs<T>& soilAlbedo, double leafRho,
                                       double leafTau, double viewZenithDeg);

// 长波吸收结果：四个组分单位面积吸收通量，W m-2。
// Absorbed longwave per unit area of each component [soil sun, soil shade, leaf sun, leaf shade] from an
// isotropic downward irradiance and the emission of all nodes, every order (TiRT tirteb_radiation.longwave).
template <class T> struct LongwaveAbsorptionT { T soilSun{}, soilShade{}, leafSun{}, leafShade{}; };
using LongwaveAbsorption = LongwaveAbsorptionT<double>;
template <class T>
LongwaveAbsorptionT<T> homLongwave(const HomCanopyT<T>& nodes, const SameAs<T>& leafEmissivity,
                                   const SameAs<T>& soilEmissivity, double sky,
                                   const SameAs<T>& soilSunlitTemperature, const SameAs<T>& soilShadedTemperature,
                                   const SameAs<T>& leafSunlitTemperature, const SameAs<T>& leafShadedTemperature,
                                   double sunZenithDeg, bool sunlit);

}  // namespace stream
