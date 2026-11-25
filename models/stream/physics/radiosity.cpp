// ==========================================================================
// 文件：radiosity.cpp
// 功能：hom3 均匀冠层“节点辐射度”的实现（土壤 + 两层等厚冠层的 5 状态线性系统），
//       给出短波/长波的全阶多次散射吸收、热红外有效发射率的多次散射项、方向多次散射反射。
// 主要模块：
//   1. 辅助函数：E1/E3 指数积分（含对 x 的导数）、5x5 带选主元的高斯消元
//   2. 系统构造：buildScatter（散射矩阵）、systemMatrix（I - 散射×到达）、absorbed（求解并算吸收）
//   3. HomCanopyT 构造：由 LAI 求各节点容量、交换因子、逃逸概率、到达矩阵、辐亮度系数
//   4. HomCanopyT::split：垂直方向上光束到达各节点的透过率
//   5. 对外接口：homAbsorbed / homAbsorbedPair（短波吸收）、homThermalScatter（热红外多次散射）、
//      homBeamMultiple / homSkyMultiple（方向多次散射）、homLongwave（长波吸收）
//   6. 文件末尾对 T = double 与 T = Ad（前向自动微分）做显式实例化
// 数据流：canopy.cpp / shortwave.cpp / energy.cpp / app 成像代码构造 HomCanopyT，再调用本文件接口；
//       T = double 为正向模型，T = Ad（physics/dual.h）同时携带对 LAI、反照率、发射率、温度的导数（同化用）。
// 约定：节点 0 = 土壤（容量 1）；节点 1 = 下层冠层、节点 2 = 上层冠层（容量各 4 tau，tau = G·LAI/2）；
//       状态 0..2 为各节点的“向上/出射”辐亮度状态，状态 3、4 为节点 1、2 的“向下”半部分（down[]）；
//       arrive 的两个通道：2*n 为从下方到达节点 n，2*n+1 为从上方到达节点 n（由 buildScatter 的用法推得）。
//       叶片投影系数 kG = 0.5；天顶角单位为度；辐亮度与通量按 π 换算（Lambertian）。
// ==========================================================================

#include "radiosity.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace stream {
namespace {

// 常数：叶片投影系数 G（球面叶片分布取 0.5），光束消光系数 k = kG / cos(天顶角)。
constexpr double kG = 0.5;  // leaf projection used by the TiRT standard closure

// 天顶角（度）的余弦；角度限幅到 89.999° 以避免 cos → 0 造成除零。
double cosZenith(double deg) { return std::cos(std::min(std::abs(deg), 89.999) * kPi / 180.0); }

// E1(x) for x > 0 (series below one, Lentz continued fraction above).
double exponentialIntegral1(double x)
{
    if (!(x > 0.0)) return std::numeric_limits<double>::infinity();
    if (x <= 1.0) {
        constexpr double euler = 0.57721566490153286;
        double sum = -euler - std::log(x);
        double power = 1.0;
        for (int k = 1; k <= 80; ++k) {
            power *= x / k;  // x^k / k!
            const double term = power / k;
            sum += (k % 2 == 1) ? term : -term;
            if (std::abs(term) < 1e-17 * std::abs(sum)) break;
        }
        return sum;
    }
    double b = x + 1.0, c = 1e300, d = 1.0 / b, h = d;
    for (int i = 1; i <= 200; ++i) {
        const double a = -static_cast<double>(i) * i;
        b += 2.0;
        d = 1.0 / (a * d + b);
        c = b + a / c;
        const double delta = c * d;
        h *= delta;
        if (std::abs(delta - 1.0) < 1e-16) break;
    }
    return h * std::exp(-x);
}

// E3 with its derivative dE3/dx = -E2(x) = -(exp(-x) - x E1(x)).
// 作用：模板化的 E3(x)；T = Ad 时用链式法则补上导数 dE3/dx = -E2(x)。
// 说明：x <= 0 时取 E2 = 1（E3 在 0 处的斜率），仅为避开 E1(0) 发散。
template <class T>
T exponentialIntegral3T(const T& x)
{
    if constexpr (std::is_same_v<T, double>) {
        return exponentialIntegral3(x);
    } else {
        const double v = x.v;
        const double e2 = v > 0.0 ? std::exp(-v) - v * exponentialIntegral1(v) : 1.0;
        return ad::chain(x, exponentialIntegral3(v), -e2);
    }
}

// Gaussian elimination with partial pivoting; B is states x columns, overwritten by the solution.
template <class T>
void solveLinear(T a[kHomStates][kHomStates], T b[kHomStates][4], int columns)
{
    for (int column = 0; column < kHomStates; ++column) {
        int pivot = column;
        for (int row = column + 1; row < kHomStates; ++row)
            if (std::abs(ad::value(a[row][column])) > std::abs(ad::value(a[pivot][column]))) pivot = row;
        if (pivot != column) {
            for (int k = 0; k < kHomStates; ++k) std::swap(a[column][k], a[pivot][k]);
            for (int k = 0; k < columns; ++k) std::swap(b[column][k], b[pivot][k]);
        }
        const T diagonal = a[column][column];
        if (std::abs(ad::value(diagonal)) < 1e-12) continue;
        for (int row = column + 1; row < kHomStates; ++row) {
            const T factor = a[row][column] / diagonal;
            // The double path skips exact zeros as before; a dual zero may still carry a derivative.
            if constexpr (std::is_same_v<T, double>) {
                if (factor == 0.0) continue;
            }
            for (int k = column; k < kHomStates; ++k) a[row][k] -= factor * a[column][k];
            for (int k = 0; k < columns; ++k) b[row][k] -= factor * b[column][k];
        }
    }
    for (int row = kHomStates - 1; row >= 0; --row) {
        const T diagonal = a[row][row];
        if (std::abs(ad::value(diagonal)) < 1e-12) continue;
        for (int k = 0; k < columns; ++k) {
            T value = b[row][k];
            for (int column = row + 1; column < kHomStates; ++column) value -= a[row][column] * b[column][k];
            b[row][k] = value / diagonal;
        }
    }
}

// 作用：由叶片散射系数构造“到达 → 状态”的散射矩阵 scatter[状态][到达通道]。
// 输入：soilAlbedo 土壤（或长波的 1-ε）反射；back = 叶片回到来向半球的份额，forward = 继续前进份额。
// 说明：土壤各向同性反射，两个通道都进入土壤状态；冠层节点：从下方到达→上行状态取 forward、
//       下行状态取 back；从上方到达则相反。back + forward = ρ + τ。
template <class T>
void buildScatter(const HomCanopyT<T>& nodes, const T& soilAlbedo, const T& back, const T& forward,
                  T scatter[kHomStates][2 * kHomNodes])
{
    for (int state = 0; state < kHomStates; ++state)
        for (int arrival = 0; arrival < 2 * kHomNodes; ++arrival) scatter[state][arrival] = T(0.0);
    for (int node = 0; node < kHomNodes; ++node) {
        if (nodes.soil[node]) {
            scatter[node][2 * node] = soilAlbedo;
            scatter[node][2 * node + 1] = soilAlbedo;
        } else {
            scatter[node][2 * node] = forward;
            scatter[node][2 * node + 1] = back;
            const int down = nodes.down[node];
            scatter[down][2 * node] = back;
            scatter[down][2 * node + 1] = forward;
        }
    }
}

// 作用：组装系统矩阵 (I - scatter × arrive)，即状态辐亮度的闭合方程 x = source + M x 中的 I - M。
// 作用：5x5 线性方程组 A X = B 的高斯消元（列选主元），B 被解覆盖，columns 为右端项列数。
// 说明：对角元绝对值 < 1e-12 的列/行直接跳过（奇异保护，该状态保持原值，不报错）；
//       double 路径跳过 factor 恰为 0 的行以保持旧结果逐位不变，Ad 路径不跳过（零值仍可能带导数）。
template <class T>
void systemMatrix(const HomCanopyT<T>& nodes, const T scatter[kHomStates][2 * kHomNodes], T matrix[kHomStates][kHomStates])
{
    for (int row = 0; row < kHomStates; ++row)
        for (int column = 0; column < kHomStates; ++column) {
            T transfer(0.0);
            for (int arrival = 0; arrival < 2 * kHomNodes; ++arrival) transfer += scatter[row][arrival] * nodes.arrive[column][arrival];
            matrix[row][column] = T(row == column ? 1.0 : 0.0) - transfer;
        }
}

// 作用：求解 5 状态系统并给出各节点吸收（W m-2 地面）：first = 外部到达的未散射（一次）吸收，
//       total = 经过 >=1 次散射的辐射场（含 sources 发射）的吸收，不含 first；全阶 = first + total。
//       sources 为各状态的自身发射（可为空，表示纯反射）。
// 步骤：1) rhs = 外部到达经 scatter 产生的一次散射 + 发射；2) 解 (I-M)x = rhs；
//       3) 用 arrive 把各状态辐亮度折算成到达各节点的入射，再乘吸收率 (1-ρ-τ) 或 (1-土壤反照率)。
// Absorption per node of the external `arrivals` and of the scattered field `first`.
template <class T>
void absorbed(const HomCanopyT<T>& nodes, const T scatter[kHomStates][2 * kHomNodes],
              const T& soilAlbedo, const T& leafRho, const T& leafTau,
              const T arrivals[2 * kHomNodes], const T* sources,
              T first[kHomNodes], T total[kHomNodes])
{
    T rhs[kHomStates][4]{};
    for (int state = 0; state < kHomStates; ++state) {
        T value = sources ? sources[state] : T(0.0);
        for (int arrival = 0; arrival < 2 * kHomNodes; ++arrival) value += scatter[state][arrival] * arrivals[arrival];
        rhs[state][0] = value;
    }
    T matrix[kHomStates][kHomStates];
    systemMatrix(nodes, scatter, matrix);
    solveLinear(matrix, rhs, 1);
    for (int node = 0; node < kHomNodes; ++node) {
        const T absorptivity = nodes.soil[node] ? T(1.0) - soilAlbedo : T(1.0) - leafRho - leafTau;
        first[node] = (arrivals[2 * node] + arrivals[2 * node + 1]) * absorptivity;
        T incident(0.0);
        for (int state = 0; state < kHomStates; ++state)
            incident += rhs[state][0] * (nodes.arrive[state][2 * node] + nodes.arrive[state][2 * node + 1]);
        total[node] = incident * absorptivity;
    }
}

// 作用：由观测方向的节点透过率 fractions 与 radiance 系数，得到各状态在观测方向上的可见权重。
// 说明：只有节点 0..2 的状态直接被传感器看到，下行半部分 (3、4) 权重为 0。
template <class T>
void landing(const HomCanopyT<T>& nodes, const T fractions[kHomNodes], T seen[kHomStates])
{
    for (int state = 0; state < kHomStates; ++state) seen[state] = T(0.0);
    for (int node = 0; node < kHomNodes; ++node) seen[node] = fractions[node] * nodes.radiance[node];
}

}  // namespace

// 作用：E3(x) = 0.5·[e^-x (1 - x) + x² E1(x)]；x <= 0 时返回 E3(0) = 0.5。
double exponentialIntegral3(double x)
{
    if (!(x > 0.0)) return 0.5;
    return 0.5 * (std::exp(-x) * (1.0 - x) + x * x * exponentialIntegral1(x));
}

template <class T>
// 作用：由 LAI 构造三节点（土壤 + 两层冠层）的几何量。
// 步骤 1：tau = G·LAI/2（LAI 取非负），节点容量 capacity = (1, 4tau, 4tau)。
// 步骤 2：用漫射截获函数 X(tau) = 1 - 2·E3(tau) 求各节点对之间的交换量 exchange（互易），
//         再除以容量得交换因子 factors；逃逸概率 escape = 1 - Σ factors，限幅到 [0,1]。
// 步骤 3：把交换因子拆成“到达矩阵” arrive：土壤状态直接取 factors；冠层节点向上/向下的出射
//         各取 2×factor（半球辐亮度折算），到达自身的份额取 1 - 2(…)，且限幅不小于 0。
// 步骤 4：radiance = 辐射度 → 辐亮度的折算系数：土壤 1/(π·容量)，冠层 2/(π·容量)。
HomCanopyT<T>::HomCanopyT(const T& lai)
{
    const T area = ad::max(lai, 0.0);
    const T tau = kG * area / 2.0;  // two equal layers
    capacity[0] = T(1.0);
    capacity[1] = capacity[2] = 4.0 * tau;
    soil[0] = true;
    soil[1] = soil[2] = false;
    down[0] = -1;
    down[1] = 3;
    down[2] = 4;
    const auto intercept = [](const T& opticalDepth) { return T(1.0) - 2.0 * exponentialIntegral3T(opticalDepth); };
    T exchange[kHomNodes][kHomNodes]{};
    exchange[0][0] = T(0.0);
    for (int i = 1; i < kHomNodes; ++i) {
        for (int j = 1; j < kHomNodes; ++j) {
            if (i == j) exchange[i][j] = 4.0 * tau - 2.0 * intercept(tau);
            else if (std::abs(i - j) == 1) exchange[i][j] = 2.0 * intercept(tau) - intercept(2.0 * tau);
            else {
                const int gap = std::abs(i - j) - 1;
                exchange[i][j] = (intercept((gap + 1) * tau) - intercept(gap * tau)) -
                                 (intercept((gap + 2) * tau) - intercept((gap + 1) * tau));
            }
        }
        const T below = (i - 1) * tau;
        exchange[0][i] = exchange[i][0] = intercept(below + tau) - intercept(below);
    }
    for (int row = 0; row < kHomNodes; ++row)
        for (int column = 0; column < kHomNodes; ++column)
            factors[row][column] = capacity[row] > 0.0 ? exchange[row][column] / capacity[row] : T(0.0);

    for (int node = 0; node < kHomNodes; ++node) {
        T sum(0.0);
        for (int other = 0; other < kHomNodes; ++other) sum += factors[node][other];
        escape[node] = ad::clamp(T(1.0) - sum, 0.0, 1.0);
    }
    for (int state = 0; state < kHomStates; ++state)
        for (int arrival = 0; arrival < 2 * kHomNodes; ++arrival) arrive[state][arrival] = T(0.0);
    for (int node = 0; node < kHomNodes; ++node) {
        if (soil[node]) {
            for (int other = 0; other < kHomNodes; ++other) arrive[node][2 * other] = factors[node][other];
            continue;
        }
        T higher(0.0), lower(0.0);
        for (int other = 0; other < kHomNodes; ++other) {
            if (other > node) { arrive[node][2 * other] = 2.0 * factors[node][other]; higher += factors[node][other]; }
            else if (other < node) { arrive[down[node]][2 * other + 1] = 2.0 * factors[node][other]; lower += factors[node][other]; }
        }
        arrive[node][2 * node] = ad::max(0.0, T(1.0) - 2.0 * (higher + escape[node]));
        arrive[down[node]][2 * node + 1] = ad::max(0.0, T(1.0) - 2.0 * lower);
    }
    for (int state = 0; state < kHomStates; ++state) radiance[state] = T(0.0);
    for (int node = 0; node < kHomNodes; ++node)
        radiance[node] = capacity[node] > 0.0 ? T(soil[node] ? 1.0 : 2.0) / (kPi * capacity[node]) : T(0.0);
}

template <class T>
// 作用：垂直光束（或沿视线方向）自上而下到达各节点的份额：out[0] 为穿过全冠层到达土壤的缝隙率，
//       out[1]、out[2] 为被下层/上层冠层截获的份额（上层先截获 top，下层截获 exp(-half)·top）。
// 输入：zenithDeg 天顶角（度，取绝对值并限幅 89.999°，cos 下限 1e-6）；opticalDepth = G·LAI/cos。
// 说明：capacity[1] = 4·tau = 2·kG·LAI = LAI（kG = 0.5），故 opticalDepth = kG·LAI/cos。
void HomCanopyT<T>::split(double zenithDeg, T out[kHomNodes]) const
{
    const double cosine = std::max(cosZenith(zenithDeg), 1e-6);
    const T opticalDepth = kG * capacity[1] / cosine;  // capacity[1] = LAI, k = G / cos
    const T half = 0.5 * opticalDepth;
    const T top = T(1.0) - ad::exp(-half);
    out[0] = ad::exp(-opticalDepth);
    out[1] = ad::exp(-half) * top;
    out[2] = top;
}

// 作用：单组到达（arrivals）的全阶短波吸收，叶片反射/透射分别为 leafRho/leafTau。
// 输入：arrivals[2*n(+1)] 为从下方/上方到达节点 n 的外部辐照；soilAlbedo 为该波段土壤反照率。
// 输出：first 未散射吸收、total 散射场吸收（全阶 = first + total，W m-2 地面）。
//       leafRho/leafTau 拆成 back/forward 见 buildScatter。
template <class T>
void homAbsorbed(const HomCanopyT<T>& nodes, const SameAs<T>& soilAlbedo, const SameAs<T>& leafRho,
                 const SameAs<T>& leafTau, const T arrivals[2 * kHomNodes], const T* sources,
                 T first[kHomNodes], T total[kHomNodes])
{
    T scatter[kHomStates][2 * kHomNodes];
    buildScatter<T>(nodes, soilAlbedo, (2.0 * leafRho + leafTau) / 3.0, (leafRho + 2.0 * leafTau) / 3.0, scatter);
    absorbed<T>(nodes, scatter, soilAlbedo, leafRho, leafTau, arrivals, sources, first, total);
}

// 作用：同一波段系统矩阵下同时求解两组到达（A：光束，B：天空），只做一次消元（右端项 2 列）。
// 输出含义同 homAbsorbed（first 未散射、total 散射场），分别给出 A、B 两组。
template <class T>
void homAbsorbedPair(const HomCanopyT<T>& nodes, const SameAs<T>& soilAlbedo, double leafRho, double leafTau,
                     const T arrivalsA[2 * kHomNodes], const T arrivalsB[2 * kHomNodes],
                     T firstA[kHomNodes], T totalA[kHomNodes], T firstB[kHomNodes], T totalB[kHomNodes])
{
    T scatter[kHomStates][2 * kHomNodes];
    buildScatter<T>(nodes, soilAlbedo, T((2.0 * leafRho + leafTau) / 3.0), T((leafRho + 2.0 * leafTau) / 3.0), scatter);
    T rhs[kHomStates][4]{};
    for (int state = 0; state < kHomStates; ++state)
        for (int arrival = 0; arrival < 2 * kHomNodes; ++arrival) {
            rhs[state][0] += scatter[state][arrival] * arrivalsA[arrival];
            rhs[state][1] += scatter[state][arrival] * arrivalsB[arrival];
        }
    T matrix[kHomStates][kHomStates];
    systemMatrix(nodes, scatter, matrix);
    solveLinear(matrix, rhs, 2);
    for (int node = 0; node < kHomNodes; ++node) {
        const T absorptivity = nodes.soil[node] ? T(1.0) - soilAlbedo : T(1.0 - leafRho - leafTau);
        firstA[node] = (arrivalsA[2 * node] + arrivalsA[2 * node + 1]) * absorptivity;
        firstB[node] = (arrivalsB[2 * node] + arrivalsB[2 * node + 1]) * absorptivity;
        T incidentA(0.0), incidentB(0.0);
        for (int state = 0; state < kHomStates; ++state) {
            const T weight = nodes.arrive[state][2 * node] + nodes.arrive[state][2 * node + 1];
            incidentA += rhs[state][0] * weight;
            incidentB += rhs[state][1] * weight;
        }
        totalA[node] = incidentA * absorptivity;
        totalB[node] = incidentB * absorptivity;
    }
}

// 作用：热红外各组分（土壤光照/阴影、叶片光照/阴影）在观测方向上的“多次散射有效发射率”增量。
// 输入：节点发射率、观测天顶角与太阳天顶角（后者决定光照面积份额）；输出 ThermalScatterT。
// 步骤 1：用 split 分别求视线与太阳方向的透过率；由太阳方向透过率换算各节点的光照面积份额 sunlit（限幅 [0,1]）。
// 步骤 2：每个组分以 π·容量·ε·份额 为发射源（不含温度，调用方再乘 Planck 项），冠层发射对半分到上行/下行状态。
// 步骤 3：热红外反射率取 1-ε（叶片透射 0，back = 2(1-ε)/3、forward = (1-ε)/3），解全阶系统。
// 步骤 4：解减去发射本身（只留经散射后的部分），再按视线可见权重 seen 汇总到四个组分。
template <class T>
ThermalScatterT<T> homThermalScatter(const HomCanopyT<T>& nodes, const SameAs<T>& leafEmissivity,
                                     const SameAs<T>& soilEmissivity, double viewZenithDeg, double sunZenithDeg)
{
    ThermalScatterT<T> result;
    T view[kHomNodes], sun[kHomNodes];
    nodes.split(viewZenithDeg, view);
    nodes.split(sunZenithDeg, sun);
    const double kSun = kG / std::max(cosZenith(sunZenithDeg), 1e-6);
    T sunlit[kHomNodes];
    for (int node = 0; node < kHomNodes; ++node) {
        if (nodes.soil[node]) sunlit[node] = sun[node];  // capacity 1
        else {
            const T leafArea = nodes.capacity[node] / (4.0 * kG);
            sunlit[node] = leafArea > 0.0 ? sun[node] / (kSun * leafArea) : T(0.0);
        }
        sunlit[node] = ad::clamp(sunlit[node], 0.0, 1.0);
    }
    T source[kHomNodes][4]{};
    for (int node = 0; node < kHomNodes; ++node) {
        if (nodes.soil[node]) {
            source[node][0] = soilEmissivity * sunlit[node];
            source[node][1] = soilEmissivity * (T(1.0) - sunlit[node]);
        } else {
            source[node][2] = leafEmissivity * sunlit[node];
            source[node][3] = leafEmissivity * (T(1.0) - sunlit[node]);
        }
    }
    T emitted[kHomStates][4]{};
    for (int node = 0; node < kHomNodes; ++node)
        for (int component = 0; component < 4; ++component) {
            const T power = kPi * nodes.capacity[node] * source[node][component];
            if (nodes.soil[node]) emitted[node][component] = power;
            else {
                emitted[node][component] = 0.5 * power;
                emitted[nodes.down[node]][component] = 0.5 * power;
            }
        }
    T scatter[kHomStates][2 * kHomNodes];
    buildScatter<T>(nodes, T(1.0) - soilEmissivity, 2.0 * (T(1.0) - leafEmissivity) / 3.0, (T(1.0) - leafEmissivity) / 3.0, scatter);
    T rhs[kHomStates][4]{};
    for (int state = 0; state < kHomStates; ++state)
        for (int component = 0; component < 4; ++component) rhs[state][component] = emitted[state][component];
    T matrix[kHomStates][kHomStates];
    systemMatrix(nodes, scatter, matrix);
    solveLinear(matrix, rhs, 4);
    T seen[kHomStates];
    landing(nodes, view, seen);
    for (int component = 0; component < 4; ++component) {
        T value(0.0);
        for (int state = 0; state < kHomStates; ++state) value += seen[state] * (rhs[state][component] - emitted[state][component]);
        (component == 0 ? result.soilSun : component == 1 ? result.soilShade : component == 2 ? result.leafSun : result.leafShade) = value;
    }
    return result;
}

// 作用：太阳光束的二阶及以上多次散射，沿视线方向的冠层/土壤贡献（单位光束，无量纲反射率）。
// 步骤：1) 一阶源 = 光束从上方到达各节点后的散射；2) 解全阶系统；3) 解 - 一阶 = 二阶及以上；
//       4) 按视线可见权重与 π 折算为方向反射，分别累计到 canopy 与 soil。无冠层（LAI = 0）返回 0。
template <class T>
DirectionalMultipleT<T> homBeamMultiple(const HomCanopyT<T>& nodes, const SameAs<T>& soilAlbedo, double leafRho,
                                        double leafTau, double sunZenithDeg, double viewZenithDeg)
{
    DirectionalMultipleT<T> result;
    if (!(nodes.capacity[1] > 0.0)) return result;
    T view[kHomNodes], sun[kHomNodes];
    nodes.split(viewZenithDeg, view);
    nodes.split(sunZenithDeg, sun);
    T scatter[kHomStates][2 * kHomNodes];
    buildScatter<T>(nodes, soilAlbedo, T((2.0 * leafRho + leafTau) / 3.0), T((leafRho + 2.0 * leafTau) / 3.0), scatter);
    T first[kHomStates]{}, rhs[kHomStates][4]{};
    for (int state = 0; state < kHomStates; ++state)
        for (int node = 0; node < kHomNodes; ++node) first[state] += scatter[state][2 * node + 1] * sun[node];
    for (int state = 0; state < kHomStates; ++state) rhs[state][0] = first[state];
    T matrix[kHomStates][kHomStates];
    systemMatrix(nodes, scatter, matrix);
    solveLinear(matrix, rhs, 1);
    T seen[kHomStates];
    landing(nodes, view, seen);
    for (int state = 0; state < kHomNodes; ++state) {
        const T difference = rhs[state][0] - first[state];
        if (nodes.soil[state]) result.soil += kPi * seen[state] * difference;
        else result.canopy += kPi * seen[state] * difference;
    }
    return result;
}

// 作用：单位各向同性天空辐照的全阶多次散射，沿视线方向的冠层/土壤贡献。
// 说明：天空辐照按各节点 escape × capacity 的权重分配到“从上方到达”通道（权重归一化）；
//       与 homBeamMultiple 不同，此处不扣除一阶（返回全阶）。
template <class T>
DirectionalMultipleT<T> homSkyMultiple(const HomCanopyT<T>& nodes, const SameAs<T>& soilAlbedo, double leafRho,
                                       double leafTau, double viewZenithDeg)
{
    DirectionalMultipleT<T> result;
    if (!(nodes.capacity[1] > 0.0)) return result;
    T view[kHomNodes];
    nodes.split(viewZenithDeg, view);
    T scatter[kHomStates][2 * kHomNodes];
    buildScatter<T>(nodes, soilAlbedo, T((2.0 * leafRho + leafTau) / 3.0), T((leafRho + 2.0 * leafTau) / 3.0), scatter);
    T sky[kHomNodes], total(0.0);
    for (int node = 0; node < kHomNodes; ++node) { sky[node] = nodes.escape[node] * nodes.capacity[node]; total += sky[node]; }
    if (!(total > 0.0)) return result;
    T rhs[kHomStates][4]{};
    for (int state = 0; state < kHomStates; ++state)
        for (int node = 0; node < kHomNodes; ++node) rhs[state][0] += scatter[state][2 * node + 1] * (sky[node] / total);
    T matrix[kHomStates][kHomStates];
    systemMatrix(nodes, scatter, matrix);
    solveLinear(matrix, rhs, 1);
    T seen[kHomStates];
    landing(nodes, view, seen);
    for (int state = 0; state < kHomNodes; ++state) {
        if (nodes.soil[state]) result.soil += kPi * seen[state] * rhs[state][0];
        else result.canopy += kPi * seen[state] * rhs[state][0];
    }
    return result;
}

// 作用：长波全阶吸收：天空下行 sky（W m-2，各向同性）加各节点自身热发射，求各组分平均吸收。
// 输入：发射率、四个组分温度（K）、太阳天顶角与 sunlit（决定光照/阴影面积份额）。
// 输出：四个组分每单位面积的吸收长波（W m-2）。对应 TiRT tirteb_radiation.longwave。
// 步骤 1：太阳在地平线以上且 sunlit 时，用 split 求各节点光照面积；否则光照面积为 0。
// 步骤 2：节点发射 = ε·σ·(share·T_sun⁴ + (1-share)·T_shade⁴)/π，冠层发射对半分到上行/下行状态。
// 步骤 3：天空辐照按 escape×capacity 权重分到各节点“从上方到达”通道；
//         反射率取 1-ε（叶片透射 0），解全阶系统。
// 步骤 4：mean() 对同类节点（土壤或叶片）按光照/阴影面积加权平均 (first+total)/area；
//         光照/阴影面积全为 0 时退化为按总面积加权。
template <class T>
LongwaveAbsorptionT<T> homLongwave(const HomCanopyT<T>& nodes, const SameAs<T>& leafEmissivity,
                                   const SameAs<T>& soilEmissivity, double sky,
                                   const SameAs<T>& soilSunlitTemperature, const SameAs<T>& soilShadedTemperature,
                                   const SameAs<T>& leafSunlitTemperature, const SameAs<T>& leafShadedTemperature,
                                   double sunZenithDeg, bool sunlit)
{
    LongwaveAbsorptionT<T> result;
    const double sigma = kStefanBoltzmann;
    const bool lit = sunlit && sunZenithDeg < 90.0;
    T sun[kHomNodes]{};
    if (lit) nodes.split(sunZenithDeg, sun);
    const double extinction = kG / std::max(cosZenith(std::min(sunZenithDeg, 85.0)), 1e-6);
    T area[kHomNodes]{}, sunlitArea[kHomNodes]{};
    for (int node = 0; node < kHomNodes; ++node) {
        area[node] = nodes.soil[node] ? nodes.capacity[node] : nodes.capacity[node] / (4.0 * kG);
        sunlitArea[node] = lit ? ad::min(nodes.soil[node] ? sun[node] : sun[node] / extinction, area[node]) : T(0.0);
    }
    T exitance[kHomNodes];
    for (int node = 0; node < kHomNodes; ++node) {
        const T share = area[node] > 0.0 ? sunlitArea[node] / area[node] : T(0.0);
        const T& sunTemperature = nodes.soil[node] ? soilSunlitTemperature : leafSunlitTemperature;
        const T& shadeTemperature = nodes.soil[node] ? soilShadedTemperature : leafShadedTemperature;
        const T& emissivity = nodes.soil[node] ? soilEmissivity : leafEmissivity;
        const T meanFourthPower = share * ad::pow(sunTemperature, 4.0) +
                                  (T(1.0) - share) * ad::pow(shadeTemperature, 4.0);
        exitance[node] = emissivity * meanFourthPower * sigma / kPi;
    }
    T sources[kHomStates]{};
    for (int node = 0; node < kHomNodes; ++node) {
        const T power = kPi * nodes.capacity[node] * exitance[node];
        if (nodes.soil[node]) sources[node] = power;
        else {
            sources[node] = 0.5 * power;
            sources[nodes.down[node]] = 0.5 * power;
        }
    }
    T skyPower[kHomNodes], skySum(0.0);
    for (int node = 0; node < kHomNodes; ++node) { skyPower[node] = nodes.escape[node] * nodes.capacity[node]; skySum += skyPower[node]; }
    T arrivals[2 * kHomNodes]{};
    for (int node = 0; node < kHomNodes; ++node)
        if (skySum > 0.0) arrivals[2 * node + 1] = sky * skyPower[node] / skySum;
    T scatter[kHomStates][2 * kHomNodes];
    buildScatter<T>(nodes, T(1.0) - soilEmissivity, 2.0 * (T(1.0) - leafEmissivity) / 3.0, (T(1.0) - leafEmissivity) / 3.0, scatter);
    T first[kHomNodes], total[kHomNodes];
    absorbed<T>(nodes, scatter, T(1.0) - soilEmissivity, T(1.0) - leafEmissivity, T(0.0), arrivals, sources, first, total);
    const auto mean = [&](bool soilMask, bool sunlitPart) {
        T weightSum(0.0);
        for (int node = 0; node < kHomNodes; ++node) {
            if (nodes.soil[node] != soilMask || !(area[node] > 0.0)) continue;
            weightSum += sunlitPart ? sunlitArea[node] : (area[node] - sunlitArea[node]);
        }
        const bool fallback = !(weightSum > 1e-12);
        T sum(0.0), normalizer(0.0);
        for (int node = 0; node < kHomNodes; ++node) {
            if (nodes.soil[node] != soilMask || !(area[node] > 0.0)) continue;
            const T weight = fallback ? area[node] : (sunlitPart ? sunlitArea[node] : (area[node] - sunlitArea[node]));
            sum += (first[node] + total[node]) / area[node] * weight;
            normalizer += weight;
        }
        return normalizer > 0.0 ? sum / normalizer : T(0.0);
    };
    result.soilSun = mean(true, true);
    result.soilShade = mean(true, false);
    result.leafSun = mean(false, true);
    result.leafShade = mean(false, false);
    return result;
}

// 显式实例化：对 double（正向）与 Ad（带导数）两种标量类型生成上述模板，使实现保留在本 .cpp 中。
// Array parameters are spelled exactly as declared: MSVC mangles a decayed array differently from a plain pointer.
#define STREAM_RADIOSITY_INSTANTIATE(T)                                                                              \
    template struct HomCanopyT<T>;                                                                                 \
    template void homAbsorbed<T>(const HomCanopyT<T>&, const T&, const T&, const T&, const T[2 * kHomNodes],        \
                                 const T*, T[kHomNodes], T[kHomNodes]);                                            \
    template void homAbsorbedPair<T>(const HomCanopyT<T>&, const T&, double, double, const T[2 * kHomNodes],       \
                                     const T[2 * kHomNodes], T[kHomNodes], T[kHomNodes], T[kHomNodes],             \
                                     T[kHomNodes]);                                                                \
    template ThermalScatterT<T> homThermalScatter<T>(const HomCanopyT<T>&, const T&, const T&, double, double);     \
    template DirectionalMultipleT<T> homBeamMultiple<T>(const HomCanopyT<T>&, const T&, double, double, double,    \
                                                        double);                                                   \
    template DirectionalMultipleT<T> homSkyMultiple<T>(const HomCanopyT<T>&, const T&, double, double, double);    \
    template LongwaveAbsorptionT<T> homLongwave<T>(const HomCanopyT<T>&, const T&, const T&, double, const T&,     \
                                                   const T&, const T&, const T&, double, bool);

STREAM_RADIOSITY_INSTANTIATE(double)
STREAM_RADIOSITY_INSTANTIATE(Ad)

}  // namespace stream
