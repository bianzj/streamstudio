#include "heat_conduction.h"

#include <algorithm>
#include <cmath>

namespace stream {
namespace {

// TiRT/TIRTEB integrate 48 h of surface temperature; STREAM only needs the last 7 h (project decision).
constexpr double kHistorySeconds = 7.0 * 3600.0;
constexpr double kColumnDepth = 2.0;      // m
constexpr double kColumnTopLayer = 0.01;  // m
constexpr double kColumnGrowth = 1.3;

std::vector<double> columnLayers()
{
    std::vector<double> layers;
    double total = 0.0, thickness = kColumnTopLayer;
    while (total + thickness < kColumnDepth - 1.0e-9) {
        layers.push_back(thickness);
        total += thickness;
        thickness *= kColumnGrowth;
    }
    const double remainder = kColumnDepth - total;
    if (!layers.empty() && remainder < layers.back()) layers.back() += remainder;
    else layers.push_back(remainder);
    return layers;
}

// The layer geometry is fixed; build it once (thread-safe static initialisation).
const std::vector<double>& sharedColumnLayers()
{
    static const std::vector<double> layers = columnLayers();
    return layers;
}

std::vector<double> columnStart(const std::vector<double>& profile, double temperature, size_t nodes)
{
    std::vector<double> old = profile;
    if (old.empty()) old.push_back(temperature);
    while (old.size() < nodes) old.push_back(old.back());
    old.resize(nodes);
    return old;
}

// Backward-Euler finite volumes: Dirichlet surface, zero-flux bottom.
std::vector<double> columnProfile(const std::vector<double>& old, double surface, double conductivity,
                                  double capacity, const std::vector<double>& layers, double dt)
{
    const size_t count = layers.size();
    if (count == 0) return {surface};
    std::vector<double> conductance(count), storage(count);
    for (size_t index = 0; index < count; ++index) conductance[index] = conductivity / layers[index];
    for (size_t index = 0; index < count; ++index) {
        const double above = layers[index];
        const double below = index + 1 < count ? layers[index + 1] : 0.0;
        storage[index] = capacity * 0.5 * (above + below) / dt;
    }
    std::vector<double> lower(count), upper(count), diagonal(count), rhs(count);
    // Unknown i is the node at the bottom of layer i: linked upward through layer i (to the surface when i == 0)
    // and downward through layer i + 1 (none at the zero-flux bottom). The surface link belongs on the diagonal
    // too, otherwise a uniform column gains heat in proportion to its absolute temperature.
    for (size_t index = 0; index < count; ++index) {
        const double above = conductance[index];
        const double below = index + 1 < count ? conductance[index + 1] : 0.0;
        lower[index] = index > 0 ? -above : 0.0;
        upper[index] = -below;
        diagonal[index] = storage[index] + above + below;
        rhs[index] = storage[index] * old[index + 1];
    }
    rhs[0] += conductance[0] * surface;
    for (size_t index = 1; index < count; ++index) {
        const double multiplier = lower[index] / diagonal[index - 1];
        diagonal[index] -= multiplier * upper[index - 1];
        rhs[index] -= multiplier * rhs[index - 1];
    }
    std::vector<double> result(count);
    result.back() = rhs.back() / diagonal.back();
    for (size_t index = count - 1; index-- > 0;)
        result[index] = (rhs[index] - upper[index] * result[index + 1]) / diagonal[index];
    result.insert(result.begin(), surface);
    return result;
}

}  // namespace

void initializeColumn(std::vector<double>& profile, double surface, double level1, double level2)
{
    const std::vector<double>& layers = sharedColumnLayers();
    constexpr double kLevel1Depth = 0.035;   // ERA5 soil temperature level 1: 0-7 cm
    constexpr double kLevel2Depth = 0.175;   // ERA5 soil temperature level 2: 7-28 cm
    const auto at = [&](double depth) {
        if (!(depth > kLevel1Depth)) return surface + (level1 - surface) * (depth / kLevel1Depth);
        if (!(depth > kLevel2Depth)) return level1 + (level2 - level1) * ((depth - kLevel1Depth) / (kLevel2Depth - kLevel1Depth));
        return level2;
    };
    profile.assign(layers.size() + 1, surface);
    double depth = 0.0;
    for (size_t index = 0; index < layers.size(); ++index) {
        depth += layers[index];
        profile[index + 1] = at(depth);
    }
}

template <class T>
T surfaceHeatFluxT(const T& temperature, const SoilThermalState& state, double dtSeconds, const T& inertia)
{
    const double dt = std::max(dtSeconds, 1.0);
    double elapsed = 0.0;
    T weighted(0.0);
    for (size_t index = 0; index < state.history.size(); ++index) {
        const double duration = index == 0 ? dt : (index - 1 < state.intervals.size() ? state.intervals[index - 1] : dt);
        const double interval = std::max(duration, 1.0);
        const double end = std::min(elapsed + interval, kHistorySeconds);
        const T newer = index == 0 ? temperature : T(state.history[index - 1]);
        weighted += (newer - state.history[index]) / interval * (std::sqrt(end) - std::sqrt(elapsed));
        elapsed += interval;
        if (elapsed >= kHistorySeconds) break;
    }
    return 2.0 * inertia / std::sqrt(3.14159265358979323846) * weighted;
}

double surfaceHeatFlux(double temperature, const SoilThermalState& state, double dtSeconds, double inertia)
{
    return surfaceHeatFluxT<double>(temperature, state, dtSeconds, inertia);
}

template double surfaceHeatFluxT<double>(const double&, const SoilThermalState&, double, const double&);
template Ad surfaceHeatFluxT<Ad>(const Ad&, const SoilThermalState&, double, const Ad&);

double depthHeatFlux(double temperature, const SoilThermalState& state, double dtSeconds, const ThermalMaterial& material)
{
    // Cold start: the column is taken at the surface temperature itself, so no heat flows (unchanged behaviour).
    if (state.profile.empty()) return 0.0;
    const double dt = std::max(dtSeconds, 1.0);
    if (!state.depthCached || state.depthDt != dt || state.depthConductivity != material.conductivity ||
        state.depthCapacity != material.volumetricCapacity) {
        // The energy-balance iteration calls this thousands of times per step with only the surface temperature
        // changing. The implicit column is linear in that boundary value, so two solves give G(T) exactly.
        const std::vector<double>& layers = sharedColumnLayers();
        const std::vector<double> old = columnStart(state.profile, state.profile.front(), layers.size() + 1);
        const std::vector<double> zero = columnProfile(old, 0.0, material.conductivity, material.volumetricCapacity, layers, dt);
        const std::vector<double> unit = columnProfile(old, 1.0, material.conductivity, material.volumetricCapacity, layers, dt);
        const double response = unit[1] - zero[1];   // d(node 1)/d(surface)
        const double conduction = material.conductivity / layers[0];
        const double storage = material.volumetricCapacity * 0.5 * layers[0] / dt;
        state.depthSlope = conduction * (1.0 - response) + storage;
        state.depthIntercept = -conduction * zero[1] - storage * old[0];
        state.depthDt = dt;
        state.depthConductivity = material.conductivity;
        state.depthCapacity = material.volumetricCapacity;
        state.depthCached = true;
    }
    return state.depthIntercept + state.depthSlope * temperature;
}

void advanceThermalState(double temperature, SoilThermalState& state, double dtSeconds, const ThermalMaterial& material,
                         bool depthMethod)
{
    const double dt = std::max(dtSeconds, 1.0);
    std::vector<double> intervals;
    intervals.push_back(dt);
    for (const double value : state.intervals) intervals.push_back(value);
    double elapsed = 0.0;
    size_t retained = 0;
    for (const double interval : intervals) {
        elapsed += interval;
        ++retained;
        if (elapsed >= kHistorySeconds) break;
    }
    std::vector<double> history;
    history.push_back(temperature);
    for (size_t index = 0; index < state.history.size() && history.size() < retained + 1; ++index)
        history.push_back(state.history[index]);
    state.history = std::move(history);
    state.intervals.assign(intervals.begin(), intervals.begin() + static_cast<long>(std::min(retained, intervals.size())));
    if (depthMethod) {
        const std::vector<double>& layers = sharedColumnLayers();
        const size_t nodes = layers.size() + 1;
        const std::vector<double> old = columnStart(state.profile, temperature, nodes);
        state.profile = columnProfile(old, temperature, material.conductivity, material.volumetricCapacity, layers, dt);
        state.depthCached = false;
    }
}

}  // namespace stream
