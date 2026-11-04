#include "../models/histream/src/facetrt/energyphysics.h"
#include <iostream>

int main() try {
    // Verify work limits by executing the production solver, not inspecting text.
    int thermalCalls = 0, physiologyCalls = 0;
    const auto leaf = facetphysics::coupledLeaf(
        [&](double) { ++thermalCalls; return 300.0; },
        [&](double, double) { ++physiologyCalls; return 1000.0; });
    if (thermalCalls > 25 || physiologyCalls > 24)
        throw std::runtime_error("Exceeded the twenty-four resistance trial budget");
    if (std::abs(std::log(leaf.resistance / 1000.0)) > .01)
        throw std::runtime_error("Resistance did not meet the inner tolerance");
    int exactThermal = 0, exactPhysiology = 0;
    const auto exact = facetphysics::coupledLeaf(
        [&](double) { ++exactThermal; return 300.0; },
        [&](double, double) { ++exactPhysiology; return 790.5694150420948; });
    if (exactThermal != 1 || exactPhysiology != 1 ||
        std::abs(exact.resistance - 790.5694150420948) > 1e-8)
        throw std::runtime_error("An already converged root did not exit immediately");
    int budgetCalls = 0;
    (void)facetphysics::coupledLeaf([](double) { return 300.0; },
        [&](double, double) { ++budgetCalls; return 1e9; });
    if (budgetCalls != 24) throw std::runtime_error("Unconverged search did not stop at its budget");
    int energyCalls = 0;
    (void)facetphysics::temperatureRoot(300, [&](double) {
        ++energyCalls;
        return std::array<double, 2>{1, 1}; // Deliberately never reaches zero.
    });
    if (energyCalls != 32) throw std::runtime_error("Thermal work exceeded the thirty-two-step budget");
    int convergedCalls = 0;
    const auto t = facetphysics::temperatureRoot(300, [&](double temperature) {
        ++convergedCalls;
        return std::array<double, 2>{310 - temperature, 1};
    });
    if (std::abs(t - 310) > 1e-9 || convergedCalls != 2)
        throw std::runtime_error("Early convergence no longer works");
    int subToleranceCalls = 0;
    const auto close = facetphysics::temperatureRoot(300, [&](double temperature) {
        ++subToleranceCalls;
        return std::array<double, 2>{100 * (300.005 - temperature), 100};
    });
    if (subToleranceCalls != 1 || std::abs(close - 300.005) > .01)
        throw std::runtime_error("Temperature correction below 0.01 K did not terminate");
    // Float roundoff on either side of the 0.01 K exit threshold must not
    // create a 0.01 K discontinuity in the returned temperature.
    const auto nearExit = [](double root) {
        return facetphysics::temperatureRoot(300, [&](double t) {
            return std::array<double, 2>{100 * (root - t), 100};
        });
    };
    if (std::abs(nearExit(300.010001) - nearExit(300.009999)) > .00001)
        throw std::runtime_error("Thermal early exit jumps across its tolerance boundary");
    std::cout << "PASS: at most 24 resistance trials, 32 thermal cap, both early exits; log mismatch="
              << std::abs(std::log(leaf.resistance / 1000.0)) << '\n';
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
