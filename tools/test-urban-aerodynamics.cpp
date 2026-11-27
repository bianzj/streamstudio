#include "../models/histream/src/base/urban_aerodynamics.h"

#include <cmath>
#include <iostream>

int main() {
    const auto exchange = urban::neutralExchange({15, 20}, 30, 3, 1200);
    const double air = 300;
    const double street = urban::canyonAirTemperature(exchange, air, 100, 250, 77250);
    const double surfaceHeat = exchange.surface * (100 * (315 - street) + 150 * (305 - street));
    const double outletHeat = 100 * exchange.outlet * (street - air);
    const auto narrow = urban::neutralExchange({15, 5}, 30, 3, 1200);
    const auto calm = urban::neutralExchange({15, 20}, 30, 0, 1200);
    if (std::abs(exchange.surface - 15.45238585) > 1e-6 ||
        std::abs(exchange.roof - 27.16008437) > 1e-6 ||
        std::abs(street - 302.79305766) > 1e-6 ||
        std::abs(surfaceHeat - outletHeat) > 1e-8 ||
        !(narrow.surface < exchange.surface) ||
        urban::canyonAirTemperature(calm, air, 100, 250, 77250) != 309 ||
        urban::canyonAirTemperature(calm, air, 0, 0, 0) != air) {
        std::cerr << "Urban canyon wind, roof exchange or sensible heat closure failed\n";
        return 1;
    }
    // Convective velocity (SURFEX eq. 3.23): zero without an upward flux, w* = (g/T Q0 h)^(1/3) otherwise,
    // and the surface coefficient reduces to the neutral one at w* = 0 and grows with w*.
    const double wStar = urban::convectiveVelocity(0.1, 300, 15);
    if (urban::convectiveVelocity(0, 300, 15) != 0 || urban::convectiveVelocity(-0.1, 300, 15) != 0 ||
        std::abs(wStar - std::cbrt(9.80665 / 300 * 0.1 * 15)) > 1e-12 ||
        std::abs(urban::surfaceCoefficient(exchange, 0) - exchange.surface) > 1e-12 ||
        !(urban::surfaceCoefficient(exchange, wStar) > exchange.surface) ||
        !(urban::surfaceCoefficient(calm, wStar) > 11.8)) {
        std::cerr << "Urban canyon convective velocity or surface coefficient failed\n";
        return 1;
    }
    std::cout << "Urban canyon aerodynamic checks passed\n";
}
