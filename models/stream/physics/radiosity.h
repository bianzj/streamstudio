#pragma once
#include "constants.h"
#include "dual.h"

#include <type_traits>

namespace stream {

// Homogeneous canopy node radiosity (TiRT "hom3": soil + two equal canopy layers).
// Capacities: soil = 1, each layer = 4 tau with tau = G LAI / 2. Exchange factors follow from
// reciprocity with the diffuse interception X(tau) = 1 - 2 E3(tau); every order of shortwave and
// longwave exchange is solved in the 5-state system (3 up states + 2 down-going canopy halves).
//
// Every kernel is a template on the scalar type: T = double is the forward model, T = Ad (physics/dual.h)
// carries exact derivatives with respect to LAI, soil albedo / emissivity and component temperatures.
constexpr int kHomNodes = 3;
constexpr int kHomStates = 5;

template <class T> using SameAs = std::type_identity_t<T>;

template <class T>
struct HomCanopyT {
    explicit HomCanopyT(const T& lai);

    T capacity[kHomNodes]{};
    T factors[kHomNodes][kHomNodes]{};
    T escape[kHomNodes]{};
    bool soil[kHomNodes]{};
    int down[kHomNodes]{};  // down-going state of a canopy node; -1 for soil
    T arrive[kHomStates][2 * kHomNodes]{};
    T radiance[kHomStates]{};

    // Transmission of a unit horizontal beam from above down to each node (soil gap, then layer hits).
    void split(double zenithDeg, T out[kHomNodes]) const;
};
using HomCanopy = HomCanopyT<double>;

// Exponential integral E3(x), x >= 0.
double exponentialIntegral3(double x);

// Absorption per node (W m-2 of ground) from external arrivals and from the scattered field.
// soilAlbedo / leafRho / leafTau are per band; a null sources pointer means pure reflection.
template <class T>
void homAbsorbed(const HomCanopyT<T>& nodes, const SameAs<T>& soilAlbedo, const SameAs<T>& leafRho,
                 const SameAs<T>& leafTau, const T arrivals[2 * kHomNodes], const T* sources,
                 T first[kHomNodes], T total[kHomNodes]);

// Two arrival sets sharing one band's system matrix (beam and sky), solved together.
template <class T>
void homAbsorbedPair(const HomCanopyT<T>& nodes, const SameAs<T>& soilAlbedo, double leafRho, double leafTau,
                     const T arrivalsA[2 * kHomNodes], const T arrivalsB[2 * kHomNodes],
                     T firstA[kHomNodes], T totalA[kHomNodes], T firstB[kHomNodes], T totalB[kHomNodes]);

// Multiple-scattering effective emissivity contributions [soil sun, soil shade, leaf sun, leaf shade].
template <class T> struct ThermalScatterT { T soilSun{}, soilShade{}, leafSun{}, leafShade{}; };
using ThermalScatter = ThermalScatterT<double>;
template <class T>
ThermalScatterT<T> homThermalScatter(const HomCanopyT<T>& nodes, const SameAs<T>& leafEmissivity,
                                     const SameAs<T>& soilEmissivity, double viewZenithDeg, double sunZenithDeg);

// Orders >= 2 of a unit beam and all orders of a unit isotropic sky, seen along the view.
template <class T> struct DirectionalMultipleT { T canopy{}, soil{}; };
using DirectionalMultiple = DirectionalMultipleT<double>;
template <class T>
DirectionalMultipleT<T> homBeamMultiple(const HomCanopyT<T>& nodes, const SameAs<T>& soilAlbedo, double leafRho,
                                        double leafTau, double sunZenithDeg, double viewZenithDeg);
template <class T>
DirectionalMultipleT<T> homSkyMultiple(const HomCanopyT<T>& nodes, const SameAs<T>& soilAlbedo, double leafRho,
                                       double leafTau, double viewZenithDeg);

// Absorbed longwave per unit area of each component [soil sun, soil shade, leaf sun, leaf shade] from an
// isotropic downward irradiance and the emission of all nodes, every order (TiRT tirteb_radiation.longwave).
template <class T> struct LongwaveAbsorptionT { T soilSun{}, soilShade{}, leafSun{}, leafShade{}; };
using LongwaveAbsorption = LongwaveAbsorptionT<double>;
template <class T>
LongwaveAbsorptionT<T> homLongwave(const HomCanopyT<T>& nodes, const SameAs<T>& leafEmissivity,
                                   const SameAs<T>& soilEmissivity, double sky,
                                   const SameAs<T>& soilSunlitTemperature, const SameAs<T>& soilShadedTemperature,
                                   const SameAs<T>& leafSunlitTemperature, const SameAs<T>& leafShadedTemperature,
                                   double sunZenithDeg, bool sunlit);

}  // namespace stream
