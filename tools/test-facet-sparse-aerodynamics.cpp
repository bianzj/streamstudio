#include "../models/histream/src/facetrt/energyphysics.h"
#include <iostream>

int main() try {
    using facetphysics::aerodynamics;
    // A leaf protruding into a nearly empty column must not inherit a
    // ground-area canopy boundary resistance proportional to 1/LAI.
    const auto edge = aerodynamics(4, .4326, 1.50214875, .019359873, .08);
    const auto thinner = aerodynamics(4, .4326, 1.50214875, .001, .08);
    if (edge.leafResistance > 500 || thinner.leafResistance > 500)
        throw std::runtime_error("Sparse leaf resistance contains canopy-area dilution");
    const auto below = aerodynamics(4, .4326, 1.5, .0099999, .08);
    const auto above = aerodynamics(4, .4326, 1.5, .0100001, .08);
    if (std::abs(below.leafResistance-above.leafResistance) > .01)
        throw std::runtime_error("Leaf resistance jumps at the bare-soil LAI cutoff");
    // Under the same neutral wind profile, increasing width fourfold doubles
    // the leaf boundary resistance. Its increment is independent of LAI
    // dilution; reconstruct only wind, independently of the resistance path.
    for (double lai : {.000001, .001, .02, .5, 1.25, 3.5}) {
        const double h=1.5, wind=.4326, sq=std::sqrt(20.6*lai/2);
        const double d=h*(1+std::expm1(-sq)/sq);
        const double z0=(h-d)*std::exp(-.4*std::max(3.3,std::sqrt(.01+.3*lai/2))+.2);
        const double z=4, ustar=.4*wind/std::log((z-d)/z0);
        const double uh=ustar/.4*std::log((h-d)/z0);
        const double uz0=uh*std::exp(.3*lai/(2*.4*.4)*((z0+d)/h-1));
        const double boundary=70*std::sqrt(.08/uz0);
        const auto narrow=aerodynamics(4,wind,h,lai,.08);
        const auto wide=aerodynamics(4,wind,h,lai,.32);
        if (std::abs((wide.leafResistance-narrow.leafResistance)-boundary)>.001)
            throw std::runtime_error("Leaf boundary exchange has the wrong area basis");
    }
    for (double stability : {-.2,0.,.2}) for (double lai : {1e-12,1e-8,.001,.01,.02,1.,3.5}) {
        auto a=aerodynamics(4,.4326,1.5,lai,.08,stability);
        if (!std::isfinite(a.leafResistance+a.soilResistance)||a.leafResistance<1||a.soilResistance<1)
            throw std::runtime_error("Nonfinite sparse-canopy limit");
    }
    // Soil's established bare-column branch remains unchanged.
    const auto bare=aerodynamics(4,.4326,1.5,0,.08);
    if (below.soilResistance != bare.soilResistance)
        throw std::runtime_error("Sparse soil resistance changed");
    std::cout << "PASS: leaf-area scaling, continuous sparse limit, stability and unchanged sparse soil; edge="
              << edge.leafResistance << " s/m\n";
} catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
