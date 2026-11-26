// ==========================================================================
// 文件：test-diffuse-directions.cpp
// 功能：VoxelRT / VoxelEB 漫射 64 方向格点的射线效应（ray effect）离线检验，不需要 GPU。
//   场景：水平地面（接收体，朝上，权重 cos/Σcos，与着色器 receiverQuadratureWeight 一致）上方 h 处
//   的一个球形各向同性发射体（半径 a，代表一个发亮的体元）。接收体 (x, z) 处的辐照比
//       f = Σ_{命中发射体的方向} w_k ,  w_k = cos_k / Σ cos
//   的解析值为 f* = (a/R)^2 cosθ_c（朗伯球发射体，R 为到球心距离，θ_c 为球心天顶角）。
//   比较的方向集：
//     fixed64 ：64 方向格点，所有接收体相同（HISTREAM_VOXEL_DIRROT=legacy 的 TIR 与 VoxelEB 散射）
//     az5     ：旧版 VoxelRT：体元哈希方位偏移 + 5 组黄金角方位旋转（共 320 条，|y| 层只有 64 个）
//     so3x1   ：每体元一个哈希三维随机旋转（64 条；HISTREAM_VOXEL_DIRROT=rot 的 TIR）
//     so3x5   ：每体元 5 个独立哈希三维旋转（320 条；rot 的 VoxelRT）
//     ref320  ：固定 320 方向 Fibonacci 反对称格点（"更多方向"参考）
//   输出：对 f 地图（81x81 接收体）统计——偏差 mean(f)/mean(f*)、逐点相对 RMS、5x5 三角滤波后的相对 RMS
//   （VoxelRT 影像对土壤做同样的 5x5 滤波）、环内方位 12 扇区的变异系数（辐条指标，解析解为 0）、
//   未命中比例。另做守恒检验：各向同性天空下 Σ w_k·[y_k>0] 对水平地面与叶片权重均应等于 1。
//   哈希与四元数公式与 shader/voxel_dirrot.glsl 完全一致（uint32 pcg3d，Shoemake 四元数）。
// 构建与运行（不属默认构建目标）：
//   cmake --build build\histream --config Release --target histream_diffuse_directions_tests
//   或  cl /nologo /std:c++17 /O2 /EHsc tools\test-diffuse-directions.cpp /Fe:%TEMP%\tdd.exe
//   用法：tdd.exe [发射体半径 a=0.5] [高度 h=3] [旋转共享块边长 1]
// 退出码 0：守恒与无偏检查通过（辐条/噪声指标仅供比较，不做判定）。
// ==========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kGolden = 2.39996323;  // 与着色器黄金角常数相同

struct Vec3 { double x, y, z; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Mat3 { double m[3][3]; };  // m[row][col]
inline Vec3 mul(const Mat3& r, Vec3 v)
{
    return {r.m[0][0] * v.x + r.m[0][1] * v.y + r.m[0][2] * v.z,
            r.m[1][0] * v.x + r.m[1][1] * v.y + r.m[1][2] * v.z,
            r.m[2][0] * v.x + r.m[2][1] * v.y + r.m[2][2] * v.z};
}

// 与 global.glsl 的 DIFFUSE_DIRECTIONS 相同的构造：方向 2k：y = (2k+1)/N，方位 k·黄金角；2k+1 为其反向。
std::vector<Vec3> latticeDirections(int n)
{
    std::vector<Vec3> d(n);
    for (int k = 0; k < n / 2; ++k) {
        const double y = (2.0 * k + 1.0) / n;
        const double r = std::sqrt(1.0 - y * y);
        const double phi = k * kGolden;
        d[2 * k] = {r * std::cos(phi), y, r * std::sin(phi)};
        d[2 * k + 1] = d[2 * k] * -1.0;
    }
    return d;
}

// ---- shader/voxel_dirrot.glsl 的孪生实现 ------------------------------------------------
struct U3 { uint32_t x, y, z; };
U3 pcg3d(U3 v)
{
    v.x = v.x * 1664525u + 1013904223u; v.y = v.y * 1664525u + 1013904223u; v.z = v.z * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v.x ^= v.x >> 16u; v.y ^= v.y >> 16u; v.z ^= v.z >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

Mat3 hashedRotation(int ix, int iy, int iz, uint32_t salt)
{
    U3 h = pcg3d({uint32_t(ix) ^ (salt * 0x9E3779B9u), uint32_t(iy) ^ (salt * 0x85EBCA6Bu),
                  uint32_t(iz) ^ (salt * 0xC2B2AE35u)});
    h = pcg3d(h);
    const double u1 = double(h.x >> 8u) / 16777216.0;
    const double u2 = double(h.y >> 8u) / 16777216.0;
    const double u3 = double(h.z >> 8u) / 16777216.0;
    const double a = std::sqrt(1.0 - u1), b = std::sqrt(u1);
    const double x = a * std::sin(2 * kPi * u2), y = a * std::cos(2 * kPi * u2);
    const double z = b * std::sin(2 * kPi * u3), w = b * std::cos(2 * kPi * u3);
    const double xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z;
    const double wx = w * x, wy = w * y, wz = w * z;
    Mat3 r;
    r.m[0][0] = 1 - 2 * (yy + zz); r.m[0][1] = 2 * (xy - wz);     r.m[0][2] = 2 * (xz + wy);
    r.m[1][0] = 2 * (xy + wz);     r.m[1][1] = 1 - 2 * (xx + zz); r.m[1][2] = 2 * (yz - wx);
    r.m[2][0] = 2 * (xz - wy);     r.m[2][1] = 2 * (yz + wx);     r.m[2][2] = 1 - 2 * (xx + yy);
    return r;
}

Mat3 yRotation(double angle)  // rotateDiffuseAzimuth：x' = c x + s z, z' = -s x + c z
{
    const double c = std::cos(angle), s = std::sin(angle);
    Mat3 r;
    r.m[0][0] = c; r.m[0][1] = 0; r.m[0][2] = s;
    r.m[1][0] = 0; r.m[1][1] = 1; r.m[1][2] = 0;
    r.m[2][0] = -s; r.m[2][1] = 0; r.m[2][2] = c;
    return r;
}

double legacyAzimuthOffset(int ix, int iy, int iz)  // diffuseAzimuthOffset()
{
    const double hv = ix * 12.9898 + iy * 78.233 + iz * 37.719;
    const double s = std::sin(hv) * 43758.5453;
    return (s - std::floor(s)) * 2 * kPi;
}

enum class Method { Fixed64, Az5, So3x1, So3x5, Ref320 };

int gBlock = 1;  // 旋转按 gBlock x gBlock x gBlock 体元块共享（HISTREAM_VOXEL_DIRROT_BLOCK）
inline int blockIndex(int i) { return int(std::floor(double(i) / gBlock)); }

// 一个接收体（水平朝上，id = ix,iz）在方法 method 下对球形发射体的辐照比 f（所有组平均）。
double receiverFraction(Method method, const std::vector<Vec3>& lattice64, const std::vector<Vec3>& lattice320,
                        int ix, int iz, Vec3 receiver, Vec3 center, double radius)
{
    int sets = 1;
    const std::vector<Vec3>* base = &lattice64;
    if (method == Method::Az5) sets = 5;
    if (method == Method::So3x5) sets = 5;
    if (method == Method::Ref320) base = &lattice320;
    double total = 0.0;
    for (int set = 0; set < sets; ++set) {
        Mat3 rot{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
        if (method == Method::Az5) rot = yRotation(legacyAzimuthOffset(ix, 0, iz) + set * kGolden);
        if (method == Method::So3x1) rot = hashedRotation(blockIndex(ix), 0, blockIndex(iz), 4096u);
        if (method == Method::So3x5) rot = hashedRotation(blockIndex(ix), 0, blockIndex(iz), 4096u + 8u * set);
        double cosSum = 0.0, hit = 0.0;
        for (const Vec3& d0 : *base) {
            const Vec3 d = mul(rot, d0);
            if (d.y <= 0.0) continue;
            cosSum += d.y;
            const Vec3 toCenter = center - receiver;
            const double t = dot(toCenter, d);
            if (t <= 0.0) continue;
            const Vec3 miss = toCenter - d * t;
            if (dot(miss, miss) <= radius * radius) hit += d.y;
        }
        total += hit / std::max(cosSum, 1e-6);
    }
    return total / sets;
}

const char* name(Method m)
{
    switch (m) {
        case Method::Fixed64: return "fixed64 (64)";
        case Method::Az5: return "az5 legacy RT (320)";
        case Method::So3x1: return "so3x1 (64)";
        case Method::So3x5: return "so3x5 (320)";
        default: return "ref320 fixed (320)";
    }
}

int failures = 0;
void check(bool ok, const std::string& text)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", text.c_str());
    if (!ok) ++failures;
}

}  // namespace

int main(int argc, char** argv)
{
    const double radius = argc > 1 ? std::atof(argv[1]) : 0.5;
    const double height = argc > 2 ? std::atof(argv[2]) : 3.0;
    gBlock = argc > 3 ? std::max(1, std::atoi(argv[3])) : 1;
    const std::vector<Vec3> l64 = latticeDirections(64);
    const std::vector<Vec3> l320 = latticeDirections(320);

    // ---- 1. 格点性质：与 global.glsl 表的前几项一致、32 对精确反向 ------------------------------
    check(std::fabs(l64[2].x + 0.736558335) < 1e-5 && std::fabs(l64[2].z - 0.674747770) < 1e-5 &&
          std::fabs(l64[4].x - 0.087158514) < 1e-5 && std::fabs(l64[62].y - 0.984375) < 1e-9,
          "64-direction lattice reproduces the DIFFUSE_DIRECTIONS table (entries 2, 4, 62)");
    bool pairs = true;
    for (int k = 0; k < 64; k += 2)
        pairs &= std::fabs(l64[k].x + l64[k + 1].x) + std::fabs(l64[k].y + l64[k + 1].y) + std::fabs(l64[k].z + l64[k + 1].z) < 1e-12;
    check(pairs, "directions 2k and 2k+1 are exact antipodes");

    // ---- 2. 守恒：各向同性天空，水平地面 / 叶片权重（旋转后）Σ w[y>0] = 1 ----------------------------
    double worstSoil = 0.0, worstLeaf = 0.0;
    for (int trial = 0; trial < 2000; ++trial) {
        const Mat3 rot = hashedRotation(trial, 3, 7 * trial + 1, 4096u + 8u * (trial % 5));
        double cosUp = 0.0, cosAll = 0.0, gUp = 0.0, gAll = 0.0;
        for (const Vec3& d0 : l64) {
            const Vec3 d = mul(rot, d0);
            if (d.y > 0.0) { cosUp += d.y; }
            cosAll += std::max(d.y, 0.0);
            const double g = 0.5 + 0.35 * (std::fabs(d.y) - 0.5);  // 任意关于 |y| 的光滑 G
            gAll += g;
            if (d.y > 0.0) gUp += g;
        }
        worstSoil = std::max(worstSoil, std::fabs(cosUp / cosAll - 1.0));
        worstLeaf = std::max(worstLeaf, std::fabs(2.0 * gUp / gAll - 1.0));
    }
    check(worstSoil < 1e-12, "isotropic sky, flat soil: sum w[y>0] == 1 for every rotation (max err " + std::to_string(worstSoil) + ")");
    check(worstLeaf < 1e-12, "isotropic sky, leaf receiver 2G/sum(G): sum w[y>0] == 1 for every rotation (max err " + std::to_string(worstLeaf) + ")");

    // ---- 3. 球形发射体 f 地图 ------------------------------------------------------------------
    const int half = 40, size = 2 * half + 1;
    const Vec3 center{0.0, height, 0.0};
    std::vector<double> truth(size * size);
    for (int iz = -half; iz <= half; ++iz)
        for (int ix = -half; ix <= half; ++ix) {
            const Vec3 receiver{ix + 0.5, 0.0, iz + 0.5};
            const Vec3 to = center - receiver;
            const double R = std::sqrt(dot(to, to));
            truth[(iz + half) * size + ix + half] = (radius / R) * (radius / R) * (to.y / R);
        }
    auto filtered = [&](const std::vector<double>& f) {
        std::vector<double> out(f.size(), 0.0);
        for (int iz = 0; iz < size; ++iz)
            for (int ix = 0; ix < size; ++ix) {
                double sum = 0.0, wsum = 0.0;
                for (int dz = -2; dz <= 2; ++dz)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int x = ix + dx, z = iz + dz;
                        if (x < 0 || z < 0 || x >= size || z >= size) continue;
                        const double w = (3 - std::abs(dx)) * (3 - std::abs(dz));
                        sum += w * f[z * size + x];
                        wsum += w;
                    }
                out[iz * size + ix] = sum / wsum;
            }
        return out;
    };
    const std::vector<double> truthFiltered = filtered(truth);

    std::printf("\nemitter sphere a=%.2f at height %.2f over a %dx%d receiver plane (x,z in [-%d,%d])\n", radius, height, size, size, half, half);
    std::printf("%-22s %9s %10s %12s %10s %10s\n", "method", "bias", "rms(raw)", "rms(5x5)", "spokeCoV", "missed");
    double so3Bias = 1.0, so3Filtered = 0.0, fixedFiltered = 0.0;
    for (Method method : {Method::Fixed64, Method::Az5, Method::So3x1, Method::So3x5, Method::Ref320}) {
        std::vector<double> f(size * size);
        for (int iz = -half; iz <= half; ++iz)
            for (int ix = -half; ix <= half; ++ix)
                f[(iz + half) * size + ix + half] = receiverFraction(method, l64, l320, ix, iz,
                                                                      {ix + 0.5, 0.0, iz + 0.5}, center, radius);
        const std::vector<double> ff = filtered(f);
        double sumF = 0, sumT = 0, e2 = 0, e2f = 0, n = 0, missed = 0, nMiss = 0;
        std::vector<double> sector(12 * 36, 0.0), sectorN(12 * 36, 0.0);
        for (int iz = -half; iz <= half; ++iz)
            for (int ix = -half; ix <= half; ++ix) {
                const double rho = std::hypot(ix + 0.5, iz + 0.5);
                if (rho < 4.0 || rho > 32.0) continue;
                const int i = (iz + half) * size + ix + half;
                sumF += f[i]; sumT += truth[i];
                e2 += std::pow((f[i] - truth[i]) / truth[i], 2);
                e2f += std::pow((ff[i] - truthFiltered[i]) / truthFiltered[i], 2);
                ++n;
                nMiss += 1; missed += f[i] == 0.0 ? 1 : 0;
                const int sec = std::min(11, int((std::atan2(iz + 0.5, ix + 0.5) + kPi) / (2 * kPi) * 12));
                const int ring = std::min(35, int(rho));
                // 环内扇区均值按 f/f* 归一，去掉径向衰减
                sector[ring * 12 + sec] += f[i] / truth[i];
                sectorN[ring * 12 + sec] += 1;
            }
        double cov = 0, rings = 0;
        for (int ring = 8; ring < 30; ++ring) {
            double mean = 0, m2 = 0; int cnt = 0;
            for (int s = 0; s < 12; ++s)
                if (sectorN[ring * 12 + s] > 0) { const double v = sector[ring * 12 + s] / sectorN[ring * 12 + s]; mean += v; m2 += v * v; ++cnt; }
            if (cnt < 2 || mean <= 0) continue;
            mean /= cnt; m2 /= cnt;
            cov += std::sqrt(std::max(m2 - mean * mean, 0.0)) / mean; rings += 1;
        }
        const double bias = sumF / sumT, rms = std::sqrt(e2 / n), rmsF = std::sqrt(e2f / n);
        std::printf("%-22s %9.3f %10.3f %12.3f %10.3f %9.1f%%\n", name(method), bias, rms, rmsF, cov / std::max(rings, 1.0), 100.0 * missed / nMiss);
        if (method == Method::So3x5) { so3Bias = bias; so3Filtered = rmsF; }
        if (method == Method::Fixed64) fixedFiltered = rmsF;
    }
    std::printf("\n");
    check(std::fabs(so3Bias - 1.0) < 0.10, "so3x5 is unbiased on the f map (bias within 10%)");
    check(so3Filtered < fixedFiltered, "so3x5 (320 rays) has a lower 5x5-filtered error than the fixed 64-direction set");
    return failures == 0 ? 0 : 1;
}
