#include "radiosity.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace stream {
namespace {

constexpr double kG = 0.5;  // leaf projection used by the TiRT standard closure

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

template <class T>
void landing(const HomCanopyT<T>& nodes, const T fractions[kHomNodes], T seen[kHomStates])
{
    for (int state = 0; state < kHomStates; ++state) seen[state] = T(0.0);
    for (int node = 0; node < kHomNodes; ++node) seen[node] = fractions[node] * nodes.radiance[node];
}

}  // namespace

double exponentialIntegral3(double x)
{
    if (!(x > 0.0)) return 0.5;
    return 0.5 * (std::exp(-x) * (1.0 - x) + x * x * exponentialIntegral1(x));
}

template <class T>
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

template <class T>
void homAbsorbed(const HomCanopyT<T>& nodes, const SameAs<T>& soilAlbedo, const SameAs<T>& leafRho,
                 const SameAs<T>& leafTau, const T arrivals[2 * kHomNodes], const T* sources,
                 T first[kHomNodes], T total[kHomNodes])
{
    T scatter[kHomStates][2 * kHomNodes];
    buildScatter<T>(nodes, soilAlbedo, (2.0 * leafRho + leafTau) / 3.0, (leafRho + 2.0 * leafTau) / 3.0, scatter);
    absorbed<T>(nodes, scatter, soilAlbedo, leafRho, leafTau, arrivals, sources, first, total);
}

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
