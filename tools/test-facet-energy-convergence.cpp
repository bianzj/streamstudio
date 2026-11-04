#include "../models/histream/src/facetrt/energyphysics.h"
#include <iostream>
#include <limits>

int main() try {
    using namespace facetphysics;
    if (facetEnergyViolation(1.999) || facetEnergyViolation(-1.999) ||
        !facetEnergyViolation(2) || !facetEnergyViolation(-2))
        throw std::runtime_error("Energy residual must be strictly below 2 W/m2");
    if (!facetEnergyViolation(std::numeric_limits<double>::quiet_NaN()))
        throw std::runtime_error("Nonfinite residual must not pass");
    if (!facetEnergyConverged(0, 1000) || facetEnergyConverged(1, 1000) ||
        !facetEnergyConverged(1, 1001) || facetEnergyConverged(0, 0))
        throw std::runtime_error("Failure fraction must be strictly below 0.1 percent");
    if (!facetEnergyConverged(141, 141152) || facetEnergyConverged(142, 141152))
        throw std::runtime_error("Wrong threshold for the original maize scene");
    if (!facetEnergyConverged(4000000, 4294967295U) ||
        facetEnergyConverged(5000000, 4294967295U))
        throw std::runtime_error("Facet-count multiplication overflow");
    std::cout << "PASS: signed residuals, strict boundaries, zero scene, large counts\n";
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
