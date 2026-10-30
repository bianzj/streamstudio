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

// Gaussian elimination with partial pivoting; B is states x columns, overwritten by the solution.
void solveLinear(double a[kHomStates][kHomStates], double b[kHomStates][4], int columns)
{
    for (int column = 0; column < kHomStates; ++column) {
        int pivot = column;
        for (int row = column + 1; row < kHomStates; ++row)
            if (std::abs(a[row][column]) > std::abs(a[pivot][column])) pivot = row;
        if (pivot != column) {
            for (int k = 0; k < kHomStates; ++k) std::swap(a[column][k], a[pivot][k]);
            for (int k = 0; k < columns; ++k) std::swap(b[column][k], b[pivot][k]);
        }
        const double diagonal = a[column][column];
        if (std::abs(diagonal) < 1e-12) continue;
        for (int row = column + 1; row < kHomStates; ++row) {
            const double factor = a[row][column] / diagonal;
            if (factor == 0.0) continue;
            for (int k = column; k < kHomStates; ++k) a[row][k] -= factor * a[column][k];
            for (int k = 0; k < columns; ++k) b[row][k] -= factor * b[column][k];
        }
    }
    for (int row = kHomStates - 1; row >= 0; --row) {
        const double diagonal = a[row][row];
        if (std::abs(diagonal) < 1e-12) continue;
        for (int k = 0; k < columns; ++k) {
            double value = b[row][k];
            for (int column = row + 1; column < kHomStates; ++column) value -= a[row][column] * b[column][k];
            b[row][k] = value / diagonal;
        }
    }
}

void buildScatter(const HomCanopy& nodes, double soilAlbedo, double back, double forward,
                  double scatter[kHomStates][2 * kHomNodes])
{
    for (int state = 0; state < kHomStates; ++state)
        for (int arrival = 0; arrival < 2 * kHomNodes; ++arrival) scatter[state][arrival] = 0.0;
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

// Absorption per node of the external `arrivals` and of the scattered field `first`.
void absorbed(const HomCanopy& nodes, const double scatter[kHomStates][2 * kHomNodes],
              double soilAlbedo, double leafRho, double leafTau,
              const double arrivals[2 * kHomNodes], const double* sources,
              double first[kHomNodes], double total[kHomNodes])
{
    double rhs[kHomStates][4]{};
    for (int state = 0; state < kHomStates; ++state) {
        double value = sources ? sources[state] : 0.0;
        for (int arrival = 0; arrival < 2 * kHomNodes; ++arrival) value += scatter[state][arrival] * arrivals[arrival];
        rhs[state][0] = value;
    }
    double matrix[kHomStates][kHomStates];
    for (int row = 0; row < kHomStates; ++row)
        for (int column = 0; column < kHomStates; ++column) {
            double transfer = 0.0;
            for (int arrival = 0; arrival < 2 * kHomNodes; ++arrival) transfer += scatter[row][arrival] * nodes.arrive[column][arrival];
            matrix[row][column] = (row == column ? 1.0 : 0.0) - transfer;
        }
    solveLinear(matrix, rhs, 1);
    for (int node = 0; node < kHomNodes; ++node) {
        const double absorptivity = nodes.soil[node] ? 1.0 - soilAlbedo : 1.0 - leafRho - leafTau;
        first[node] = (arrivals[2 * node] + arrivals[2 * node + 1]) * absorptivity;
        double incident = 0.0;
        for (int state = 0; state < kHomStates; ++state)
            incident += rhs[state][0] * (nodes.arrive[state][2 * node] + nodes.arrive[state][2 * node + 1]);
        total[node] = incident * absorptivity;
    }
}

void landing(const HomCanopy& nodes, const double fractions[kHomNodes], double seen[kHomStates])
{
    for (int state = 0; state < kHomStates; ++state) seen[state] = 0.0;
    for (int node = 0; node < kHomNodes; ++node) seen[node] = fractions[node] * nodes.radiance[node];
}

}  // namespace

double exponentialIntegral3(double x)
{
    if (!(x > 0.0)) return 0.5;
    return 0.5 * (std::exp(-x) * (1.0 - x) + x * x * exponentialIntegral1(x));
}

HomCanopy::HomCanopy(double lai)
{
    const double area = std::max(lai, 0.0);
    const double tau = kG * area / 2.0;  // two equal layers
    capacity[0] = 1.0;
    capacity[1] = capacity[2] = 4.0 * tau;
    soil[0] = true;
    soil[1] = soil[2] = false;
    down[0] = -1;
    down[1] = 3;
    down[2] = 4;
    const auto intercept = [](double opticalDepth) { return 1.0 - 2.0 * exponentialIntegral3(opticalDepth); };
    double exchange[kHomNodes][kHomNodes]{};
    exchange[0][0] = 0.0;
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
        const double below = (i - 1) * tau;
        exchange[0][i] = exchange[i][0] = intercept(below + tau) - intercept(below);
    }
    for (int row = 0; row < kHomNodes; ++row)
        for (int column = 0; column < kHomNodes; ++column)
            factors[row][column] = capacity[row] > 0.0 ? exchange[row][column] / capacity[row] : 0.0;

    for (int node = 0; node < kHomNodes; ++node) {
        double sum = 0.0;
        for (int other = 0; other < kHomNodes; ++other) sum += factors[node][other];
        escape[node] = std::clamp(1.0 - sum, 0.0, 1.0);
    }
    for (int state = 0; state < kHomStates; ++state)
        for (int arrival = 0; arrival < 2 * kHomNodes; ++arrival) arrive[state][arrival] = 0.0;
    for (int node = 0; node < kHomNodes; ++node) {
        if (soil[node]) {
            for (int other = 0; other < kHomNodes; ++other) arrive[node][2 * other] = factors[node][other];
            continue;
        }
        double higher = 0.0, lower = 0.0;
        for (int other = 0; other < kHomNodes; ++other) {
            if (other > node) { arrive[node][2 * other] = 2.0 * factors[node][other]; higher += factors[node][other]; }
            else if (other < node) { arrive[down[node]][2 * other + 1] = 2.0 * factors[node][other]; lower += factors[node][other]; }
        }
        arrive[node][2 * node] = std::max(0.0, 1.0 - 2.0 * (higher + escape[node]));
        arrive[down[node]][2 * node + 1] = std::max(0.0, 1.0 - 2.0 * lower);
    }
    for (int state = 0; state < kHomStates; ++state) radiance[state] = 0.0;
    for (int node = 0; node < kHomNodes; ++node)
        radiance[node] = capacity[node] > 0.0 ? (soil[node] ? 1.0 : 2.0) / (kPi * capacity[node]) : 0.0;
}

void HomCanopy::split(double zenithDeg, double out[kHomNodes]) const
{
    const double cosine = std::max(cosZenith(zenithDeg), 1e-6);
    const double opticalDepth = kG * capacity[1] / cosine;  // capacity[1] = LAI, k = G / cos
    const double half = 0.5 * opticalDepth;
    const double top = 1.0 - std::exp(-half);
    out[0] = std::exp(-opticalDepth);
    out[1] = std::exp(-half) * top;
    out[2] = top;
}

void homAbsorbed(const HomCanopy& nodes, double soilAlbedo, double leafRho, double leafTau,
                 const double arrivals[2 * kHomNodes], const double* sources,
                 double first[kHomNodes], double total[kHomNodes])
{
    double scatter[kHomStates][2 * kHomNodes];
    buildScatter(nodes, soilAlbedo, (2.0 * leafRho + leafTau) / 3.0, (leafRho + 2.0 * leafTau) / 3.0, scatter);
    absorbed(nodes, scatter, soilAlbedo, leafRho, leafTau, arrivals, sources, first, total);
}

void homAbsorbedPair(const HomCanopy& nodes, double soilAlbedo, double leafRho, double leafTau,
                     const double arrivalsA[2 * kHomNodes], const double arrivalsB[2 * kHomNodes],
                     double firstA[kHomNodes], double totalA[kHomNodes],
                     double firstB[kHomNodes], double totalB[kHomNodes])
{
    double scatter[kHomStates][2 * kHomNodes];
    buildScatter(nodes, soilAlbedo, (2.0 * leafRho + leafTau) / 3.0, (leafRho + 2.0 * leafTau) / 3.0, scatter);
    double rhs[kHomStates][4]{};
    for (int state = 0; state < kHomStates; ++state)
        for (int arrival = 0; arrival < 2 * kHomNodes; ++arrival) {
            rhs[state][0] += scatter[state][arrival] * arrivalsA[arrival];
            rhs[state][1] += scatter[state][arrival] * arrivalsB[arrival];
        }
    double matrix[kHomStates][kHomStates];
    for (int row = 0; row < kHomStates; ++row)
        for (int column = 0; column < kHomStates; ++column) {
            double transfer = 0.0;
            for (int arrival = 0; arrival < 2 * kHomNodes; ++arrival) transfer += scatter[row][arrival] * nodes.arrive[column][arrival];
            matrix[row][column] = (row == column ? 1.0 : 0.0) - transfer;
        }
    solveLinear(matrix, rhs, 2);
    for (int node = 0; node < kHomNodes; ++node) {
        const double absorptivity = nodes.soil[node] ? 1.0 - soilAlbedo : 1.0 - leafRho - leafTau;
        firstA[node] = (arrivalsA[2 * node] + arrivalsA[2 * node + 1]) * absorptivity;
        firstB[node] = (arrivalsB[2 * node] + arrivalsB[2 * node + 1]) * absorptivity;
        double incidentA = 0.0, incidentB = 0.0;
        for (int state = 0; state < kHomStates; ++state) {
            const double weight = nodes.arrive[state][2 * node] + nodes.arrive[state][2 * node + 1];
            incidentA += rhs[state][0] * weight;
            incidentB += rhs[state][1] * weight;
        }
        totalA[node] = incidentA * absorptivity;
        totalB[node] = incidentB * absorptivity;
    }
}

ThermalScatter homThermalScatter(const HomCanopy& nodes, double leafEmissivity, double soilEmissivity,
                                 double viewZenithDeg, double sunZenithDeg)
{
    ThermalScatter result;
    double view[kHomNodes], sun[kHomNodes];
    nodes.split(viewZenithDeg, view);
    nodes.split(sunZenithDeg, sun);
    const double kSun = kG / std::max(cosZenith(sunZenithDeg), 1e-6);
    double sunlit[kHomNodes];
    for (int node = 0; node < kHomNodes; ++node) {
        if (nodes.soil[node]) sunlit[node] = sun[node];  // capacity 1
        else {
            const double leafArea = nodes.capacity[node] / (4.0 * kG);
            sunlit[node] = leafArea > 0.0 ? sun[node] / (kSun * leafArea) : 0.0;
        }
        sunlit[node] = std::clamp(sunlit[node], 0.0, 1.0);
    }
    double source[kHomNodes][4]{};
    for (int node = 0; node < kHomNodes; ++node) {
        if (nodes.soil[node]) {
            source[node][0] = soilEmissivity * sunlit[node];
            source[node][1] = soilEmissivity * (1.0 - sunlit[node]);
        } else {
            source[node][2] = leafEmissivity * sunlit[node];
            source[node][3] = leafEmissivity * (1.0 - sunlit[node]);
        }
    }
    double emitted[kHomStates][4]{};
    for (int node = 0; node < kHomNodes; ++node)
        for (int component = 0; component < 4; ++component) {
            const double power = kPi * nodes.capacity[node] * source[node][component];
            if (nodes.soil[node]) emitted[node][component] = power;
            else {
                emitted[node][component] = 0.5 * power;
                emitted[nodes.down[node]][component] = 0.5 * power;
            }
        }
    double scatter[kHomStates][2 * kHomNodes];
    buildScatter(nodes, 1.0 - soilEmissivity, 2.0 * (1.0 - leafEmissivity) / 3.0, (1.0 - leafEmissivity) / 3.0, scatter);
    double rhs[kHomStates][4]{};
    for (int state = 0; state < kHomStates; ++state)
        for (int component = 0; component < 4; ++component) rhs[state][component] = emitted[state][component];
    double matrix[kHomStates][kHomStates];
    for (int row = 0; row < kHomStates; ++row)
        for (int column = 0; column < kHomStates; ++column) {
            double transfer = 0.0;
            for (int arrival = 0; arrival < 2 * kHomNodes; ++arrival) transfer += scatter[row][arrival] * nodes.arrive[column][arrival];
            matrix[row][column] = (row == column ? 1.0 : 0.0) - transfer;
        }
    solveLinear(matrix, rhs, 4);
    double seen[kHomStates];
    landing(nodes, view, seen);
    for (int component = 0; component < 4; ++component) {
        double value = 0.0;
        for (int state = 0; state < kHomStates; ++state) value += seen[state] * (rhs[state][component] - emitted[state][component]);
        (component == 0 ? result.soilSun : component == 1 ? result.soilShade : component == 2 ? result.leafSun : result.leafShade) = value;
    }
    return result;
}

DirectionalMultiple homBeamMultiple(const HomCanopy& nodes, double soilAlbedo, double leafRho, double leafTau,
                                    double sunZenithDeg, double viewZenithDeg)
{
    DirectionalMultiple result;
    if (!(nodes.capacity[1] > 0.0)) return result;
    double view[kHomNodes], sun[kHomNodes];
    nodes.split(viewZenithDeg, view);
    nodes.split(sunZenithDeg, sun);
    double scatter[kHomStates][2 * kHomNodes];
    buildScatter(nodes, soilAlbedo, (2.0 * leafRho + leafTau) / 3.0, (leafRho + 2.0 * leafTau) / 3.0, scatter);
    double first[kHomStates]{}, rhs[kHomStates][4]{};
    for (int state = 0; state < kHomStates; ++state)
        for (int node = 0; node < kHomNodes; ++node) first[state] += scatter[state][2 * node + 1] * sun[node];
    for (int state = 0; state < kHomStates; ++state) rhs[state][0] = first[state];
    double matrix[kHomStates][kHomStates];
    for (int row = 0; row < kHomStates; ++row)
        for (int column = 0; column < kHomStates; ++column) {
            double transfer = 0.0;
            for (int arrival = 0; arrival < 2 * kHomNodes; ++arrival) transfer += scatter[row][arrival] * nodes.arrive[column][arrival];
            matrix[row][column] = (row == column ? 1.0 : 0.0) - transfer;
        }
    solveLinear(matrix, rhs, 1);
    double seen[kHomStates];
    landing(nodes, view, seen);
    for (int state = 0; state < kHomNodes; ++state) {
        const double difference = rhs[state][0] - first[state];
        if (nodes.soil[state]) result.soil += kPi * seen[state] * difference;
        else result.canopy += kPi * seen[state] * difference;
    }
    return result;
}

DirectionalMultiple homSkyMultiple(const HomCanopy& nodes, double soilAlbedo, double leafRho, double leafTau,
                                   double viewZenithDeg)
{
    DirectionalMultiple result;
    if (!(nodes.capacity[1] > 0.0)) return result;
    double view[kHomNodes];
    nodes.split(viewZenithDeg, view);
    double scatter[kHomStates][2 * kHomNodes];
    buildScatter(nodes, soilAlbedo, (2.0 * leafRho + leafTau) / 3.0, (leafRho + 2.0 * leafTau) / 3.0, scatter);
    double sky[kHomNodes], total = 0.0;
    for (int node = 0; node < kHomNodes; ++node) { sky[node] = nodes.escape[node] * nodes.capacity[node]; total += sky[node]; }
    if (!(total > 0.0)) return result;
    double rhs[kHomStates][4]{};
    for (int state = 0; state < kHomStates; ++state)
        for (int node = 0; node < kHomNodes; ++node) rhs[state][0] += scatter[state][2 * node + 1] * (sky[node] / total);
    double matrix[kHomStates][kHomStates];
    for (int row = 0; row < kHomStates; ++row)
        for (int column = 0; column < kHomStates; ++column) {
            double transfer = 0.0;
            for (int arrival = 0; arrival < 2 * kHomNodes; ++arrival) transfer += scatter[row][arrival] * nodes.arrive[column][arrival];
            matrix[row][column] = (row == column ? 1.0 : 0.0) - transfer;
        }
    solveLinear(matrix, rhs, 1);
    double seen[kHomStates];
    landing(nodes, view, seen);
    for (int state = 0; state < kHomNodes; ++state) {
        if (nodes.soil[state]) result.soil += kPi * seen[state] * rhs[state][0];
        else result.canopy += kPi * seen[state] * rhs[state][0];
    }
    return result;
}

LongwaveAbsorption homLongwave(const HomCanopy& nodes, double leafEmissivity, double soilEmissivity, double sky,
                               double soilSunlitTemperature, double soilShadedTemperature,
                               double leafSunlitTemperature, double leafShadedTemperature,
                               double sunZenithDeg, bool sunlit)
{
    LongwaveAbsorption result;
    const double sigma = kStefanBoltzmann;
    const bool lit = sunlit && sunZenithDeg < 90.0;
    double sun[kHomNodes]{};
    if (lit) nodes.split(sunZenithDeg, sun);
    const double extinction = kG / std::max(cosZenith(std::min(sunZenithDeg, 85.0)), 1e-6);
    double area[kHomNodes]{}, sunlitArea[kHomNodes]{};
    for (int node = 0; node < kHomNodes; ++node) {
        area[node] = nodes.soil[node] ? nodes.capacity[node] : nodes.capacity[node] / (4.0 * kG);
        sunlitArea[node] = lit ? std::min(nodes.soil[node] ? sun[node] : sun[node] / extinction, area[node]) : 0.0;
    }
    double exitance[kHomNodes];
    for (int node = 0; node < kHomNodes; ++node) {
        const double share = area[node] > 0.0 ? sunlitArea[node] / area[node] : 0.0;
        const double sunTemperature = nodes.soil[node] ? soilSunlitTemperature : leafSunlitTemperature;
        const double shadeTemperature = nodes.soil[node] ? soilShadedTemperature : leafShadedTemperature;
        const double emissivity = nodes.soil[node] ? soilEmissivity : leafEmissivity;
        const double meanFourthPower = share * std::pow(sunTemperature, 4.0) +
                                       (1.0 - share) * std::pow(shadeTemperature, 4.0);
        exitance[node] = emissivity * meanFourthPower * sigma / kPi;
    }
    double sources[kHomStates]{};
    for (int node = 0; node < kHomNodes; ++node) {
        const double power = kPi * nodes.capacity[node] * exitance[node];
        if (nodes.soil[node]) sources[node] = power;
        else {
            sources[node] = 0.5 * power;
            sources[nodes.down[node]] = 0.5 * power;
        }
    }
    double skyPower[kHomNodes], skySum = 0.0;
    for (int node = 0; node < kHomNodes; ++node) { skyPower[node] = nodes.escape[node] * nodes.capacity[node]; skySum += skyPower[node]; }
    double arrivals[2 * kHomNodes]{};
    for (int node = 0; node < kHomNodes; ++node)
        if (skySum > 0.0) arrivals[2 * node + 1] = sky * skyPower[node] / skySum;
    double scatter[kHomStates][2 * kHomNodes];
    buildScatter(nodes, 1.0 - soilEmissivity, 2.0 * (1.0 - leafEmissivity) / 3.0, (1.0 - leafEmissivity) / 3.0, scatter);
    double first[kHomNodes], total[kHomNodes];
    absorbed(nodes, scatter, 1.0 - soilEmissivity, 1.0 - leafEmissivity, 0.0, arrivals, sources, first, total);
    const auto mean = [&](bool soilMask, bool sunlitPart) {
        double weightSum = 0.0;
        for (int node = 0; node < kHomNodes; ++node) {
            if (nodes.soil[node] != soilMask || !(area[node] > 0.0)) continue;
            weightSum += sunlitPart ? sunlitArea[node] : (area[node] - sunlitArea[node]);
        }
        const bool fallback = !(weightSum > 1e-12);
        double sum = 0.0, normalizer = 0.0;
        for (int node = 0; node < kHomNodes; ++node) {
            if (nodes.soil[node] != soilMask || !(area[node] > 0.0)) continue;
            const double weight = fallback ? area[node] : (sunlitPart ? sunlitArea[node] : (area[node] - sunlitArea[node]));
            sum += (first[node] + total[node]) / area[node] * weight;
            normalizer += weight;
        }
        return normalizer > 0.0 ? sum / normalizer : 0.0;
    };
    result.soilSun = mean(true, true);
    result.soilShade = mean(true, false);
    result.leafSun = mean(false, true);
    result.leafShade = mean(false, false);
    return result;
}

}  // namespace stream
