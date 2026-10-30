#pragma once
#include "constants.h"

namespace stream {

// Homogeneous canopy node radiosity (TiRT "hom3": soil + two equal canopy layers).
// Capacities: soil = 1, each layer = 4 tau with tau = G LAI / 2. Exchange factors follow from
// reciprocity with the diffuse interception X(tau) = 1 - 2 E3(tau); every order of shortwave and
// longwave exchange is solved in the 5-state system (3 up states + 2 down-going canopy halves).
constexpr int kHomNodes = 3;
constexpr int kHomStates = 5;

struct HomCanopy {
    explicit HomCanopy(double lai);

    double capacity[kHomNodes]{};
    double factors[kHomNodes][kHomNodes]{};
    double escape[kHomNodes]{};
    bool soil[kHomNodes]{};
    int down[kHomNodes]{};  // down-going state of a canopy node; -1 for soil
    double arrive[kHomStates][2 * kHomNodes]{};
    double radiance[kHomStates]{};

    // Transmission of a unit horizontal beam from above down to each node (soil gap, then layer hits).
    void split(double zenithDeg, double out[kHomNodes]) const;
};

// Exponential integral E3(x), x >= 0.
double exponentialIntegral3(double x);

// Absorption per node (W m-2 of ground) from external arrivals and from the scattered field.
// soilAlbedo / leafRho / leafTau are per band; a null sources pointer means pure reflection.
void homAbsorbed(const HomCanopy& nodes, double soilAlbedo, double leafRho, double leafTau,
                 const double arrivals[2 * kHomNodes], const double* sources,
                 double first[kHomNodes], double total[kHomNodes]);

// Two arrival sets sharing one band's system matrix (beam and sky), solved together.
void homAbsorbedPair(const HomCanopy& nodes, double soilAlbedo, double leafRho, double leafTau,
                     const double arrivalsA[2 * kHomNodes], const double arrivalsB[2 * kHomNodes],
                     double firstA[kHomNodes], double totalA[kHomNodes],
                     double firstB[kHomNodes], double totalB[kHomNodes]);

// Multiple-scattering effective emissivity contributions [soil sun, soil shade, leaf sun, leaf shade].
struct ThermalScatter { double soilSun = 0, soilShade = 0, leafSun = 0, leafShade = 0; };
ThermalScatter homThermalScatter(const HomCanopy& nodes, double leafEmissivity, double soilEmissivity,
                                 double viewZenithDeg, double sunZenithDeg);

// Orders >= 2 of a unit beam and all orders of a unit isotropic sky, seen along the view.
struct DirectionalMultiple { double canopy = 0, soil = 0; };
DirectionalMultiple homBeamMultiple(const HomCanopy& nodes, double soilAlbedo, double leafRho, double leafTau,
                                    double sunZenithDeg, double viewZenithDeg);
DirectionalMultiple homSkyMultiple(const HomCanopy& nodes, double soilAlbedo, double leafRho, double leafTau,
                                   double viewZenithDeg);

// Absorbed longwave per unit area of each component [soil sun, soil shade, leaf sun, leaf shade] from an
// isotropic downward irradiance and the emission of all nodes, every order (TiRT tirteb_radiation.longwave).
struct LongwaveAbsorption { double soilSun = 0, soilShade = 0, leafSun = 0, leafShade = 0; };
LongwaveAbsorption homLongwave(const HomCanopy& nodes, double leafEmissivity, double soilEmissivity, double sky,
                               double soilSunlitTemperature, double soilShadedTemperature,
                               double leafSunlitTemperature, double leafShadedTemperature,
                               double sunZenithDeg, bool sunlit);

}  // namespace stream
