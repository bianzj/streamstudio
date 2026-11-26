// ==========================================================================
// 文件：image_environment.glsl
// 功能：VoxelEB 光学成像“多次散射环境”系数在 voxelRads 缓冲区中的存取与光谱换算工具。
//       仅被 include，不含 main。
// 主要模块：
//   1. leafSpectralInvariant：叶片介质碰撞场的光谱不变因子 w/(1 - p·w)
//   2. imageEnvironmentBand：传感器波长 → 短波光谱组（0..2000 对应 400..2400 nm）
//   3. imageEnvironmentEntry / loadImageEnvironment / storeImageEnvironment：系数的读写
// 数据流：体元光谱散射第 3 阶段写入（storeImageEnvironment），成像着色器读取并按波段重标度
//         E_b = value·I_b [·w_b/(1 - p·w_b)]（详见下方原注释）。
// 与其他文件：缓冲区布局由 C++ 端 buffer.cpp 决定；与 voxelrad_scattering_VNIR.comp 的能量平衡尾项
//         使用相同的 0.9 上限；mwir_solar.glsl 为 2.5–6 µm 太阳项。
// 约定：每个 VoxelRad 条目存两个系数（偶数索引存 cumulated，奇数存 induced）；
//         setting.imageEnvironment 为 0 表示未分配，成像只保留单次散射。
// ==========================================================================

#include "../voxel_radindex.glsl"
#ifndef HISTREAM_IMAGE_ENVIRONMENT_GLSL
#define HISTREAM_IMAGE_ENVIRONMENT_GLSL

// 模块：多次散射环境系数的说明（以下为原作者英文说明，保持不变）。
// Multiple-scattering environment of the VoxelEB optical image.
//
// The shortwave scattering pass reuses the N x 64 directional field for every
// order and spectral group, and the TIR diffuse pass overwrites it afterwards,
// so the image cannot read it. Stage 3 of every order therefore also stores,
// per voxel and sensor band, the collided (scattered at least once) incident
// shortwave of that band's spectral group - two-face total for foliage and
// media, cosine-weighted irradiance for solid facets, i.e. the skyDiffuse
// convention - summed over the explicit orders plus the spectral-invariant
// tail of the energy balance. It is normalized by the group illumination and,
// for foliage, by w/(1 - p w) of the group-mean leaf albedo, so the image
// rescales it to each band: E_b = value * I_b [* w_b/(1 - p w_b)].
//
// Radiance buffer layout (buffer.cpp): N*64 VoxelRad directional field, N*64
// sky gap transmittances (two per VoxelRad entry), then N*n_wave of these
// coefficients (two per entry). setting.imageEnvironment is 0 when they are
// not allocated (index range); the image then keeps single scattering only.

// 模块：叶片光谱不变量。albedo=w 为叶片单次散射反照率（限幅 [0,1]），recollision=p 为再碰撞概率，
// 分母中的 p·w 限制不超过 0.9，防止 p·w→1 时发散。
// Spectral invariants: the collided field of a leaf medium scales with
// w/(1 - p w) (first collision w, each further one p w). Same cap as the
// energy-balance tail (voxelrad_scattering_VNIR.comp).
float leafSpectralInvariant(float albedo, float recollision)
{
    float w = clamp(albedo, 0.0, 1.0);
    return w / (1.0 - min(recollision * w, 0.9));
}

// 模块：波长换算。入参 wavelength 同时兼容 µm（<=2.5 则乘 1000 视为 nm）与 nm；
// <=2500 nm 的光学波段取 round(nm-400) 并限制在 [0,2000]；2.5–6 µm 用最后一组；其它返回 -1。
// Shortwave band (0..2000 = 400..2400 nm) whose spectral group carries the
// environment of a sensor band, or -1. Bands between 2.5 and 6 um that get a
// solar term (mwir_solar.glsl) use the last group (up to 2400 nm). The optical
// test and band index follow isOpticalBand/atmosphereBand of the image.
int imageEnvironmentBand(float wavelength)
{
    float nm = wavelength <= 2.5 ? wavelength * 1000.0 : wavelength;
    if(nm <= 2500.0) return clamp(int(round(nm - 400.0)), 0, 2000);
    float um = wavelength > 50.0 ? wavelength * 0.001 : wavelength;
    return (um > 2.5 && um < 6.0) ? 2000 : -1;
}

// 系数在 voxelRads 中的位置：位于前面 N×64 方向场与 N×64 天空缝隙透过率（每条目两个，
// 合 N×32）之后，即偏移 N×(DIFFUSE_ANGLE_NUM×3/2)（假定 DIFFUSE_ANGLE_NUM=64，待核对），
// 再按 (体元×n_wave + 波段)/2 索引。
// 注意（2026-11-26）：下标按 64 位计算（见 voxel_radindex.glsl）：N×96 超过 2^32 条目时不再回绕。
uint64_t imageEnvironmentEntry(uint voxel, uint band)
{
    uint64_t k = uint64_t(voxel) * uint64_t(setting.n_wave) + uint64_t(band);
    return uint64_t(setting.voxelCount) * uint64_t(DIFFUSE_ANGLE_NUM * 3 / 2) + (k >> 1);
}

float loadImageEnvironment(uint voxel, uint band)
{
    uint64_t entry = imageEnvironmentEntry(voxel, band);
    return (((uint64_t(voxel) * uint64_t(setting.n_wave) + uint64_t(band)) & uint64_t(1)) == uint64_t(0))
        ? VOXEL_RAD(entry).cumulated : VOXEL_RAD(entry).induced;
}

void storeImageEnvironment(uint voxel, uint band, float value)
{
    uint64_t entry = imageEnvironmentEntry(voxel, band);
    if(((uint64_t(voxel) * uint64_t(setting.n_wave) + uint64_t(band)) & uint64_t(1)) == uint64_t(0)) VOXEL_RAD(entry).cumulated = value;
    else VOXEL_RAD(entry).induced = value;
}
#endif
