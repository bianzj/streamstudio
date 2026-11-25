// ==========================================================================
// 文件：mwir_solar.glsl
// 功能：为 2.5–6 µm 中波红外（MWIR）热波段补充“被反射的太阳光”源项，使该范围热波段
//       在绝对辐亮度单位下同时包含地表自发射和太阳直射/天空散射反射。
// 主要模块：
//   1. MWIR_SOLAR_SHAPE：2500–6000 nm、100 nm 间隔的归一化太阳光谱形状（36 点）
//   2. MWIR_SKY_SOLAR_RATIO：天空散射相对直射的比值（常数上限估计）
//   3. mwirSolarShape：按波长对形状表线性插值
//   4. clearSkyGlobalShortwave / thermalSolarGlobal：水平面全球短波辐照度（W m-2）
//   5. voxelRtThermalSolar：VoxelRT 热波段的直射/散射太阳源（已除以 π）
// 数据流：光照设置（direct、diffuse、sunDirection、lightSet.Rin）→ 本文件 → 返回 vec2
//         （x 直射项，y 散射天空项），由 VoxelRT 着色器加到热波段光源上。
// 与其他文件关系：形状表来自 defined/Esun_.dat（VoxelEB/FacetEB 共用的短波光谱形状）；
//         referenceIrradianceRT 在别处定义（原注释提及）。
// 约定：热波段为绝对 Planck 辐亮度（W m-2 sr-1 µm-1），短波波段为归一化光照；
//       形状表单位 1/µm（除以 400–2400 nm 积分，µm 为单位）；2.5 µm 以下与 6 µm 以上返回 0。
// ==========================================================================

#ifndef MWIR_SOLAR_GLSL
#define MWIR_SOLAR_GLSL

// Reflected sunlight in thermal (> 2.5 um) bands.
//
// Shortwave bands are normalized-illumination bands; thermal bands are
// absolute Planck radiance (W m-2 sr-1 um-1). Between 2.5 um and the 6.3 um
// water-vapour band the surface solar irradiance is comparable to the
// emitted radiance (about 10 W m-2 um-1 at the top of the atmosphere near
// 3.8 um), so thermal bands there also receive the solar beam and the
// scattered sky, in absolute units.
//
// MWIR_SOLAR_SHAPE is the bundled defined/Esun_.dat (the same file VoxelEB
// and FacetEB use for the shortwave spectral shape) on its 2500-6000 nm,
// 100 nm grid, divided by its 400-2400 nm integral in um. Multiplied by a
// shortwave total (W m-2, distributed over 400-2400 nm as in the energy
// balance) it is the spectral irradiance in W m-2 um-1. The file already
// carries the atmospheric absorption bands (2.7, 4.3 um) and falls to zero
// in the 6.3 um water band; beyond 6 um (8-14 um window) the solar term is
// below 0.1 % of the emission and is not modelled, so long-wave bands are
// unchanged.
// 模块：MWIR 太阳光谱形状表。索引 i 对应波长 2.5 + 0.1·i µm（i=0..35，即 2.5–6.0 µm）。
// 数值来自 Esun_.dat（已含 2.7、4.3 µm 大气吸收带，6.3 µm 水汽带为 0）。
const int MWIR_SOLAR_COUNT = 36;
const float MWIR_SOLAR_SHAPE[MWIR_SOLAR_COUNT] = float[MWIR_SOLAR_COUNT](
    1.7800e-02, 1.3376e-03, 6.2215e-09, 3.8114e-04, 2.7788e-03, 4.6614e-03,
    6.4018e-03, 6.2056e-03, 7.0701e-03, 1.1103e-02, 1.3684e-02, 1.3498e-02,
    1.2362e-02, 1.1301e-02, 1.0473e-02, 9.0935e-03, 6.4206e-03, 3.2183e-03,
    1.2786e-04, 1.4780e-04, 1.0580e-03, 2.4357e-03, 3.0036e-03, 2.3447e-03,
    1.7338e-03, 1.3681e-03, 7.3824e-04, 2.0866e-04, 6.5639e-05, 1.2040e-05,
    1.1073e-06, 3.5406e-08, 9.0807e-14, 0.0, 0.0, 0.0);

// 模块：天空散射太阳光相对于直射的比值缺省常数（用 2300–2400 nm 的 Esky_/Esun_ 比值，
//       实际天空散射随波长继续下降，所以这是上限；见下方原注释）。
// Scattered (sky) sunlight relative to the beam beyond 2.5 um, per unit of
// the respective shortwave share: the ratio of the normalized Esky_/Esun_
// shapes over 2300-2400 nm. Sky scattering keeps falling with wavelength,
// so holding it constant is an upper bound; the Esky_ values beyond 2.5 um
// are atmospheric emission, already modelled by the sky temperature.
const float MWIR_SKY_SOLAR_RATIO = 0.0847;

// 作用：返回波段中心 wavelength 处的太阳光谱形状（1/µm）。
// 说明：wavelength > 50 视为 nm，否则视为 µm（兼容两种单位的调用）；范围外返回 0；
//       在表内线性插值，结果下限 0。
// Spectral shape (1/um) at a band centre given in um or nm; 0 for
// wavelength <= 2.5 um (optical bands) and >= 6 um.
float mwirSolarShape(float wavelength)
{
    float um = wavelength > 50.0 ? wavelength * 0.001 : wavelength;
    if(!(um > 2.5) || !(um < 6.0)) return 0.0;
    // 步骤：换算成表下标（100 nm 间隔）并限制到 [0, COUNT-2]，保证 index+1 不越界。
    float x = (um - 2.5) * 10.0;
    int index = clamp(int(floor(x)), 0, MWIR_SOLAR_COUNT - 2);
    float t = clamp(x - float(index), 0.0, 1.0);
    return max(mix(MWIR_SOLAR_SHAPE[index], MWIR_SOLAR_SHAPE[index + 1], t), 0.0);
}

// 作用：Haurwitz 晴空模型 G = 1098·cosZ·exp(-0.057/cosZ)，输入太阳天顶角余弦，
//       太阳在地平线上或以下时返回 0。
// Clear-sky global horizontal shortwave irradiance (Haurwitz 1945), W m-2:
// the default absolute scale for RayTracing/VoxelRT, which have no
// meteorology (about 890 W m-2 at 30 deg zenith).
float clearSkyGlobalShortwave(float cosSolarZenith)
{
    return cosSolarZenith > 0.0
        ? 1098.0 * cosSolarZenith * exp(-0.057 / cosSolarZenith) : 0.0;
}

// 作用：选择水平面全球短波。用户配置值 configuredShortwave > 0 时用配置值，否则用晴空公式；
//       太阳在地平线以下返回 0。
// Horizontal global shortwave (W m-2) for RayTracing/VoxelRT: the project's
// light.shortwaveIrradiance (carried in lightSet.Rin) when > 0, otherwise
// the clear-sky value; 0 with the sun at or below the horizon. The
// direct/diffuse split stays with the light settings (direct fraction), so
// an overcast setting (low total, low direct fraction) lowers both.
float thermalSolarGlobal(float cosSolarZenith, float configuredShortwave)
{
    if(!(cosSolarZenith > 0.0)) return 0.0;
    return configuredShortwave > 0.0 ? configuredShortwave
        : clearSkyGlobalShortwave(cosSolarZenith);
}

// 作用：VoxelRT 热波段（2.5–6 µm）的太阳源项。
// 输入：wavelength、direct（光束法向归一化直射）、diffuse（水平天空辐照度归一化）、
//       sunDirection（y 为向上）、configuredShortwave。
// 输出：vec2(x 直射项, y 散射天空项)，已除以 π，因为热场用辐亮度 L，光学场用 π·L。
// VoxelRT thermal-band solar sources, divided by pi because its thermal
// fields are radiance L while the optical fields are pi*L. The normalized
// illumination (direct beam-normal, diffuse = horizontal sky irradiance,
// reference direct*cos + diffuse with the beam only above y = 0.01, see
// referenceIrradianceRT) is scaled to the global shortwave above.
// x: beam (used like lightSet.direct), y: scattered sky (added to the
// Planck sky like lightSet.diffuse). Both are exactly 0 outside 2.5-6 um.
vec2 voxelRtThermalSolar(float wavelength, float direct, float diffuse,
                         vec3 sunDirection, float configuredShortwave)
{
    float shape = mwirSolarShape(wavelength);
    if(!(shape > 0.0)) return vec2(0.0);
    // 步骤 1：太阳天顶角余弦 = 太阳方向的 y 分量；形状为 0 的波段直接返回 0。
    float cosSolarZenith = normalize(sunDirection).y;
    // 步骤 2：太阳高度过低（cos<0.01）时忽略直射束，与 referenceIrradianceRT 的约定一致。
    //       以 reference = 直射·cos + 散射 作为归一化光照的水平辐照度基准。
    float beam = cosSolarZenith >= 0.01 ? direct : 0.0;
    float reference = beam * cosSolarZenith + diffuse;
    if(!(reference > 1.0e-6)) return vec2(0.0);
    // 步骤 3：缩放因子 = 全球短波 / 归一化基准 × 光谱形状 × (1/π)；
    //       0.318309886 = 1/π。天空项再乘 MWIR_SKY_SOLAR_RATIO。
    float scale = thermalSolarGlobal(cosSolarZenith, configuredShortwave) / reference
        * shape * 0.318309886;
    return vec2(scale * beam, scale * diffuse * MWIR_SKY_SOLAR_RATIO);
}

#endif
