// ==========================================================================
// 文件：pixel_forcing.cpp
// 功能：把强迫源（ForcingSource）的网格/站点数据插值为单个像元的瞬时气象强迫，并给出地形下的接收面短波
// 主要模块：
//   1. approximateCosZenith / approximateSunAzimuth：NOAA 近似太阳天顶角余弦与方位角
//   2. PixelForcing 构造：按像元经纬度取插值权重，记录高程差、坡度坡向与地形（锥/脊）参数
//   3. instant / temperature / state：整点值之间的线性时间插值，气温按高程差做递减率订正
//   4. longwave / shortwave：小时通量放在小时中心后插值；短波按晴空指数 F/cos(z) 插值以保持日变化
//   5. surfaceShortwave：水平短波拆分直射/漫射，再按坡面（平面/锥/脊）入射余弦与天空视角因子
//      得到接收面短波、漫射占比与有效入射余弦
//   6. at：组装 EB 所需的 Forcing（气温、水汽压、气压、风速、短波、长波）
// 数据流：ForcingSource.at / hourlyFlux（标准量，整点）→ 本文件 → Forcing → physics/energy
// 约定：时间为 Unix 秒（UTC）；温度 K，水汽压/气压 hPa，辐射 W m-2；角度参数入口为度、内部为弧度；
//   方位角自北顺时针；整点 k 的小时通量代表 (k−1, k]，放在 k−0.5 处
// 注意：水平像元（非 tilted）保持旧算法逐位不变；高程订正只作用于气温与气压，水汽压由露点
//   换算后再被饱和水汽压封顶（不超过订正后气温的饱和值）；标注 FIX(2026-11-21 C10) 的改动
//   使同一太阳位置（EB 步进使用的 SPA）同时用于通量、符号判断与倾斜
// 与其他文件：pixel_forcing.h 声明；forcing_source.h 数据接口；physics/shortwave.h 的
//   requestedDiffuseFraction / admissibleDiffuseFraction；physics/energy.h 的 Forcing
// ==========================================================================

#include "pixel_forcing.h"
#include "utc.h"
#include "../physics/constants.h"
#include "../physics/shortwave.h"

#include <algorithm>
#include <cmath>

namespace stream {

// 作用：NOAA 通用太阳位置公式的天顶角余弦（经纬度为度，时间为 Unix 秒 UTC）。
// 步骤 1：由日序与分钟求分数年角 gamma；步骤 2：用傅里叶级数求时差 equation（分钟）与赤纬；
// 步骤 3：真太阳时 = UTC 分钟 + 时差 + 4×经度，换算时角；步骤 4：cos z = sinφ sinδ + cosφ cosδ cos H。
// 仅用于两个整点中心之间的短波插值与旧的水平/倾斜路径，精度低于 SPA（EB 步进使用 SPA）。
double approximateCosZenith(double latitudeDeg, double longitudeDeg, double unixSeconds)
{
    const double wholeDays = std::floor(unixSeconds / 86400.0);
    int year;
    unsigned month, day;
    civilFromDays(static_cast<int64_t>(wholeDays), year, month, day);
    const double dayIndex = wholeDays - static_cast<double>(daysFromCivil(year, 1, 1));
    const double minutes = (unixSeconds - wholeDays * 86400.0) / 60.0;
    const double gamma = 2.0 * kPi / (isLeapYear(year) ? 366.0 : 365.0) * (dayIndex + (minutes / 60.0 - 12.0) / 24.0);
    const double equation = 229.18 * (0.000075 + 0.001868 * std::cos(gamma) - 0.032077 * std::sin(gamma) - 0.014615 * std::cos(2 * gamma) -
                                      0.040849 * std::sin(2 * gamma));
    const double declination = 0.006918 - 0.399912 * std::cos(gamma) + 0.070257 * std::sin(gamma) - 0.006758 * std::cos(2 * gamma) +
                               0.000907 * std::sin(2 * gamma) - 0.002697 * std::cos(3 * gamma) + 0.00148 * std::sin(3 * gamma);
    const double solarMinutes = minutes + equation + 4.0 * longitudeDeg;
    const double hourAngle = (solarMinutes / 4.0 - 180.0) * kPi / 180.0;
    const double latitude = latitudeDeg * kPi / 180.0;
    return std::sin(latitude) * std::sin(declination) + std::cos(latitude) * std::cos(declination) * std::cos(hourAngle);
}

// 作用：与上同一近似下的太阳方位角（弧度，自北顺时针）。天顶角正弦接近 0（太阳在天顶）时返回 0。
// 前面的太阳位置步骤与 approximateCosZenith 完全相同，这里再复制了一份。
double approximateSunAzimuth(double latitudeDeg, double longitudeDeg, double unixSeconds)
{
    const double wholeDays = std::floor(unixSeconds / 86400.0);
    int year;
    unsigned month, day;
    civilFromDays(static_cast<int64_t>(wholeDays), year, month, day);
    const double dayIndex = wholeDays - static_cast<double>(daysFromCivil(year, 1, 1));
    const double minutes = (unixSeconds - wholeDays * 86400.0) / 60.0;
    const double gamma = 2.0 * kPi / (isLeapYear(year) ? 366.0 : 365.0) * (dayIndex + (minutes / 60.0 - 12.0) / 24.0);
    const double equation = 229.18 * (0.000075 + 0.001868 * std::cos(gamma) - 0.032077 * std::sin(gamma) - 0.014615 * std::cos(2 * gamma) -
                                      0.040849 * std::sin(2 * gamma));
    const double declination = 0.006918 - 0.399912 * std::cos(gamma) + 0.070257 * std::sin(gamma) - 0.006758 * std::cos(2 * gamma) +
                               0.000907 * std::sin(2 * gamma) - 0.002697 * std::cos(3 * gamma) + 0.00148 * std::sin(3 * gamma);
    const double solarMinutes = minutes + equation + 4.0 * longitudeDeg;
    const double hourAngle = (solarMinutes / 4.0 - 180.0) * kPi / 180.0;
    const double latitude = latitudeDeg * kPi / 180.0;
    const double cosZenith = std::sin(latitude) * std::sin(declination) + std::cos(latitude) * std::cos(declination) * std::cos(hourAngle);
    const double sinZenith = std::sqrt(std::max(0.0, 1.0 - cosZenith * cosZenith));
    if (sinZenith < 1e-6) return 0.0;
    const double sinAzimuth = -std::cos(declination) * std::sin(hourAngle) / sinZenith;
    const double cosAzimuth = (std::sin(declination) - std::sin(latitude) * cosZenith) / (std::cos(latitude) * sinZenith);
    return std::atan2(sinAzimuth, cosAzimuth);
}

// 作用：为一个像元建立强迫视图。构造时即取好插值权重；elevationShift = 像元高程 − 数据源海拔（m）。
// slope/aspect（度）描述接收面（0 为水平）；cone*（统计锥）与 ridge*（山脊）为复合地形参数，
// 密度为 0–1 的覆盖比例；diffuseFraction 为水平短波的缺省漫射份额。
PixelForcing::PixelForcing(const ForcingSource& pack, double latitude, double longitude, double elevationShift,
                           double slopeDeg, double aspectDeg, double diffuseFraction,
                           double coneSlopeDeg, double coneDensity,
                           double ridgeSlopeDeg, double ridgeStrikeDeg, double ridgeDensity, double ridgeEndFraction)
    : pack_(pack), weights_(pack.weights(latitude, longitude)), latitude_(latitude), longitude_(longitude), shift_(elevationShift),
      slope_(slopeDeg * kPi / 180.0), aspect_(aspectDeg * kPi / 180.0), diffuse_(diffuseFraction),
      coneSlope_(coneSlopeDeg * kPi / 180.0), coneDensity_(coneDensity),
      ridgeSlope_(ridgeSlopeDeg * kPi / 180.0), ridgeStrike_(ridgeStrikeDeg * kPi / 180.0),
      ridgeDensity_(ridgeDensity), ridgeEndFraction_(ridgeEndFraction)
{
}

// 作用：标准量在任意时刻的值：取前后两个整点值做线性插值（恰在整点时直接取值）。
double PixelForcing::instant(const std::string& variable, double unixSeconds) const
{
    const double hours = unixSeconds / 3600.0;
    const int64_t hour = static_cast<int64_t>(std::floor(hours));
    const double fraction = hours - hour;
    const double a = pack_.at(variable, hour, weights_);
    if (fraction < 1e-9) return a;
    const double b = pack_.at(variable, hour + 1, weights_);
    return (1 - fraction) * a + fraction * b;
}

// 作用：温度类量的高程订正：T = T_源 − Γ × 高程差，Γ 为数据源的递减率（缺省 0.0065 K m-1）。
double PixelForcing::temperature(const std::string& variable, double unixSeconds) const
{
    return instant(variable, unixSeconds) - pack_.lapseRate() * shift_;
}

double PixelForcing::state(const std::string& variable, double unixSeconds) const { return instant(variable, unixSeconds); }

// 模块：长波。逐小时平均长波放在小时中心，对时间做线性插值（不做高程订正）。
// Hourly flux of hour k covers (k-1, k] and is placed at its centre k - 0.5.
double PixelForcing::longwave(double unixSeconds) const
{
    const double centre = unixSeconds / 3600.0 + 0.5;
    const int64_t k = static_cast<int64_t>(std::floor(centre));
    const double f = centre - k;
    return (1 - f) * pack_.hourlyFlux("strd", k, weights_) + f * pack_.hourlyFlux("strd", k + 1, weights_);
}

// 模块：水平面短波。以小时中心的“晴空指数” F/cos(z) 线性插值，再乘瞬时 cos(z)，
// 使日变化跟随太阳高度而不是小时平均的阶梯。
// Interpolates the clearness F / cos(z) between hour centres so the diurnal cycle follows the sun.
double PixelForcing::shortwave(double unixSeconds) const
{
    return shortwave(unixSeconds, approximateCosZenith(latitude_, longitude_, unixSeconds));
}

// FIX(2026-11-21 C10): the instantaneous cos(z) is a parameter so the EB march can use the SPA sun of its nodes.
// 作用：给定瞬时 cos(z)=mu 的水平面短波（W m-2）。
// 步骤 1：mu ≤ 0 返回 0；步骤 2：取包住该时刻的两个小时中心及其通量 fluxA、fluxB 与 cos(z)；
// 步骤 3：小时中心 cos(z) > 0.05 且通量有效才用晴空指数插值（避免日出日落附近 F/cos(z) 发散）；
// 只有一侧有效就用该侧；两侧都无效则对通量本身线性插值；
// 步骤 4：结果限幅在 [0, 1400·mu]（1400 W m-2 的取值依据待核对，应是物理上界/抑制异常值）。
double PixelForcing::shortwave(double unixSeconds, double mu) const
{
    if (mu <= 0.0) return 0.0;
    const double centre = unixSeconds / 3600.0 + 0.5;
    const int64_t k = static_cast<int64_t>(std::floor(centre));
    const double f = centre - k;
    const double fluxA = pack_.hourlyFlux("ssrd", k, weights_), fluxB = pack_.hourlyFlux("ssrd", k + 1, weights_);
    const double muA = approximateCosZenith(latitude_, longitude_, (k - 0.5) * 3600.0);
    const double muB = approximateCosZenith(latitude_, longitude_, (k + 0.5) * 3600.0);
    const bool validA = muA > 0.05 && std::isfinite(fluxA), validB = muB > 0.05 && std::isfinite(fluxB);
    double value;
    if (validA && validB) value = ((1 - f) * fluxA / muA + f * fluxB / muB) * mu;
    else if (validA) value = fluxA / muA * mu;
    else if (validB) value = fluxB / muB * mu;
    else value = (1 - f) * fluxA + f * fluxB;
    // Horizontal global shortwave. Terrain tilting is done in surfaceShortwave() after the direct/diffuse split.
    return std::clamp(value, 0.0, 1400.0 * mu);
}

// 作用：接收面短波的 NOAA 版本入口：太阳位置取 NOAA 近似的余弦与方位角，转交共用实现。
SurfaceShortwave PixelForcing::surfaceShortwave(double unixSeconds, double szaDeg, int dayOfYear, bool enforceLimit) const
{
    // FIX(2026-11-21 C10): unchanged NOAA behaviour, now through the shared implementation.
    return surfaceShortwave(unixSeconds, szaDeg, dayOfYear, enforceLimit,
                            approximateCosZenith(latitude_, longitude_, unixSeconds),
                            approximateSunAzimuth(latitude_, longitude_, unixSeconds));
}

// 作用：接收面短波的 SPA 版本入口：调用方给出天顶角 szaDeg 与方位角 saaDeg（度），
// 用 cos(sza) 与 saa 作为太阳位置；任一为非有限数时退回 NOAA 版本。
SurfaceShortwave PixelForcing::surfaceShortwave(double unixSeconds, double szaDeg, int dayOfYear, bool enforceLimit,
                                                double saaDeg) const
{
    // FIX(2026-11-21 C10): one solar position (the caller's SPA) for the flux, its sign and the tilt.
    if (!std::isfinite(saaDeg) || !std::isfinite(szaDeg)) return surfaceShortwave(unixSeconds, szaDeg, dayOfYear, enforceLimit);
    return surfaceShortwave(unixSeconds, szaDeg, dayOfYear, enforceLimit, std::cos(szaDeg * kPi / 180.0), saaDeg * kPi / 180.0);
}

// 作用：接收面短波的共用实现。输入：水平短波经 mu（瞬时 cos z）得到，szaDeg 用于漫射份额与
// 晴空 DNI 上限，dayOfYear 用于日地距离修正，sunAzimuth（弧度，自北顺时针）。
// 步骤 1：水平短波 horizontal；水平像元（非 tilted）直接返回；
// 步骤 2：按配置/Erbs 与晴空 DNI 上限得到可接受的水平漫射份额 fd（被上限抬高则 limited）；
// 步骤 3：太阳在地平线下或水平短波为 0 时，全部视为漫射、无直射入射；
// 步骤 4：按地形类型求面积平均入射余弦 meanCos 与天空视角项 skyTerm；
// 步骤 5：direct = 水平 × (1−fd) × meanCos/mu（法向直射 × 平均入射余弦），diffuse = 水平 × fd × skyTerm；
// 合计超过 1400 W m-2 时等比缩小；输出 global、漫射占比与冠层直射用的有效入射余弦。
SurfaceShortwave PixelForcing::surfaceShortwave(double unixSeconds, double szaDeg, int dayOfYear, bool enforceLimit,
                                                double mu, double sunAzimuth) const
{
    SurfaceShortwave out;
    out.horizontal = shortwave(unixSeconds, mu);  // FIX(2026-11-21 C10): cos(z) of the selected solar position
    out.global = out.horizontal;
    out.cosIncidence = std::max(std::cos(szaDeg * kPi / 180.0), 0.0);
    if (!tilted()) return out;  // horizontal surface: absorbShortwave keeps doing the split (unchanged path)
    out.tilted = true;
    const double requested = requestedDiffuseFraction(out.horizontal, szaDeg, diffuse_, dayOfYear);  // FIX(2026-11-21 C7)
    out.horizontalDiffuseFraction = admissibleDiffuseFraction(out.horizontal, szaDeg, dayOfYear, diffuse_, enforceLimit);
    out.limited = out.horizontalDiffuseFraction > requested + 1e-12;
    // FIX(2026-11-21 C10): mu is the parameter (NOAA in the legacy overload, SPA from the EB march).
    if (!(out.horizontal > 0.0) || !(mu > 0.0)) {
        out.global = 0.0;
        out.diffuseShare = 1.0;
        out.cosIncidence = 0.0;
        return out;
    }
    // Area-mean max(0, cos i) of the receiving surface (NOAA sun, as the old tilt) and its sky-view factor.
    double meanCos = 0.0, skyTerm = 1.0;
    const double sinZenith = std::sqrt(std::max(0.0, 1.0 - mu * mu));
    // 地形分支 A：统计锥。坡度 β 的面元在方位上均匀分布：cos i = a + b·cosφ，a = cosβ·mu，
    // b = sinβ·sin z；对 φ 平均 max(0, cos i) 的解析式为 (a·φ0 + b·sinφ0)/π，φ0 = acos(−a/b)；
    // ratio ≤ −1 时处处受光，ratio ≥ 1 时处处背光。锥占比 d，其余为水平面。
    if (coneDensity_ > 0.0) {
        // Statistical cone: facets of slope beta uniformly distributed in azimuth; density d of the pixel
        // is covered by cones and the rest is flat. The direct term averages max(0, cosI) over the facets.
        const double a = std::cos(coneSlope_) * mu;
        const double b = std::sin(coneSlope_) * sinZenith;
        double average = std::max(0.0, a);
        if (b > 1e-12) {
            const double ratio = -a / b;
            if (ratio <= -1.0) average = a;
            else if (ratio < 1.0) {
                const double phi0 = std::acos(ratio);
                average = (a * phi0 + b * std::sin(phi0)) / kPi;
            } else average = 0.0;
        }
        meanCos = coneDensity_ * average + (1.0 - coneDensity_) * mu;
        skyTerm = coneDensity_ * 0.5 * (1.0 + std::cos(coneSlope_)) + (1.0 - coneDensity_);
    // 地形分支 B：山脊（占比 ridgeDensity_）：两个坡面（坡度 ridgeSlope_，朝向 走向 ± 90°）加两个竖直端面，
    // 端面占脊面积的 endFraction（限幅到 0–0.5）；天空视角项为坡面 (1+cosβ)/2 与端面 1/2 的面积加权。
    } else if (ridgeDensity_ > 0.0) {
        // Ridge: two flanks (slope beta, facing strike ± 90°) plus two vertical end faces (facing strike
        // and strike + 180°). endFraction e is the end-face share of the ridge surface; density d is
        // the ridge footprint coverage, the rest is flat.
        const double azimuth = sunAzimuth;  // FIX(2026-11-21 C10): solar azimuth of the selected solar position
        const double endFraction = std::clamp(ridgeEndFraction_, 0.0, 0.5);
        double sum = 0.0;
        for (int side = 0; side < 2; ++side) {
            const double phi = ridgeStrike_ + (side == 0 ? kPi / 2.0 : -kPi / 2.0);
            sum += 0.5 * (1.0 - endFraction) * std::max(0.0, std::cos(ridgeSlope_) * mu + std::sin(ridgeSlope_) * sinZenith * std::cos(phi - azimuth));
        }
        for (int end = 0; end < 2; ++end) {
            const double phi = ridgeStrike_ + (end == 0 ? 0.0 : kPi);
            sum += 0.5 * endFraction * std::max(0.0, sinZenith * std::cos(phi - azimuth));
        }
        meanCos = ridgeDensity_ * sum + (1.0 - ridgeDensity_) * mu;
        const double skyView = (1.0 - endFraction) * 0.5 * (1.0 + std::cos(ridgeSlope_)) + endFraction * 0.5;
        skyTerm = ridgeDensity_ * skyView + (1.0 - ridgeDensity_);
    } else {
        // 地形分支 C：单一平面（single / facet / hill 等）：cos i = cosβ·mu + sinβ·sin z·cos(az − aspect)，
        // 天空视角因子 (1 + cosβ)/2。
        // One plane: beam by the surface normal, diffuse by the sky-view factor.
        const double azimuth = sunAzimuth;  // FIX(2026-11-21 C10): solar azimuth of the selected solar position
        meanCos = std::max(0.0, std::cos(slope_) * mu + std::sin(slope_) * sinZenith * std::cos(azimuth - aspect_));
        skyTerm = 0.5 * (1.0 + std::cos(slope_));
    }
    const double fd = out.horizontalDiffuseFraction;
    double direct = out.horizontal * (1.0 - fd) * meanCos / mu;  // beam normal flux × mean cos i
    double diffuse = out.horizontal * fd * skyTerm;
    const double total = direct + diffuse;
    if (total > 1400.0) { direct *= 1400.0 / total; diffuse *= 1400.0 / total; }
    out.global = direct + diffuse;
    out.diffuseShare = out.global > 0.0 ? diffuse / out.global : 1.0;
    out.cosIncidence = direct > 0.0 ? std::clamp(meanCos, 0.0, 1.0) : 0.0;
    return out;
}

// 作用：给出像元在 unixSeconds 的 EB 强迫 Forcing。
// ta：高程订正后气温；ea：露点（不订正）算出的水汽压，并封顶到订正后气温的饱和值；
// p：地面气压（Pa→hPa），按 exp(−g·Δh/(R_d·Ta)) 折到像元高程；wind：10 m 风分量合成的风速；
// shortwave 为水平面值（地形倾斜在 surfaceShortwave 中完成）；longwave 为向下长波。
Forcing PixelForcing::at(double unixSeconds) const
{
    Forcing forcing;
    forcing.ta = temperature("t2m", unixSeconds);
    const double dewpoint = instant("d2m", unixSeconds);
    forcing.ea = std::min(saturationVapourPressure(dewpoint - kKelvin), saturationVapourPressure(forcing.ta - kKelvin));
    forcing.p = instant("sp", unixSeconds) / 100.0 * std::exp(-kGravity * shift_ / (kDryAirGasConstant * forcing.ta));
    const double u = instant("u10", unixSeconds), v = instant("v10", unixSeconds);
    forcing.wind = std::sqrt(u * u + v * v);
    forcing.shortwave = shortwave(unixSeconds);
    forcing.longwave = longwave(unixSeconds);
    return forcing;
}

}  // namespace stream
