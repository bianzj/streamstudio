#pragma once
// impact-v1 event model: projectile -> penetration/fuze -> burst point -> per-block blast load ->
// failure -> fragment launch, plus the energy budget that drives fireball, heat and dust.
// Engineering estimate from public empirical formulas (see blast.h, penetration.h, empirical.h);
// not a structural, detonation or combustion simulation and not independently validated.
#include "blast.h"
#include "penetration.h"
#include "empirical.h"
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace animation::impact {
struct EventOptions {
    // Projectile (generic example values, not a specific type).
    double massKg=250,diameterM=.34,lengthM=3,speedMps=250,noseN=.85,nozzleK=900;bool powered=false;
    // Charge and fuze.
    double chargeKg=100,tntEquivalence=1;std::string fuzeMode="delay";double fuzeDelayS=.005;
    // Wall (the target instance) and its blocks.
    double wallThicknessM=.25,youngS=.9,criticalImpulsePaS=2000,wallDensity=2300;
    double minimumBlockMassKg=2,minimumBlockThicknessM=.05;
    // Energy split of E = W * 4.184 MJ/kg.
    double blastFraction=.6,heatFraction=.3;
    // Dust appearance proxy: pulverised/failed mass fraction and mass extinction coefficient.
    double dustMassFraction=.01,dustExtinctionM2PerKg=200;
    double fireballPeakK=2000,ambientK=288;
    BurstType burst=BurstType::FreeAirSpherical;
    // Presentation: physical seconds per presentation second (0.04 = 25x slow motion).
    double timeScale=1;
    static EventOptions parse(const nlohmann::json&);
    double chargeTntKg()const{return chargeKg*tntEquivalence;}
    nlohmann::json describe()const;
};
struct BlockInput {glm::vec3 center{},size{};};
struct BlockResult {
    double rangeM=0,incidenceRad=0,scaledDistance=0,incidentPressurePa=0,reflectedImpulsePaS=0,arrivalS=0,areaM2=0,massKg=0;
    bool clamped=false,failed=false,pulverised=false;std::string criterion;glm::vec3 velocity{};
};
struct Event {
    EventOptions options;
    double contactTime=0,burstTime=0,penetrationPathM=0,burstTravelM=0,wallTimeS=0,stopTimeS=0;
    glm::vec3 surfaceContact{},burstPoint{},direction{},normal{};
    PenetrationResult penetration{};std::string burstLocation;
    double energyJ=0,heatJ=0,blastJ=0;
    std::vector<BlockResult> blocks;size_t failed=0,released=0,pulverised=0;
    double fragmentKineticJ=0,fragmentMomentumNs=0,deliveredImpulseNs=0,absorbedImpulseNs=0,maxFragmentSpeed=0;glm::vec3 momentumVector{};
    double failedVolumeM3=0,dustKg=0;
    Fireball fireball{};double heatSigmaM=0,heatAmplitudeK=0,tracerAmplitude=0;bool heatSigmaWidened=false;
    double recoveryK=0,noseK=0,bodyK=0,finK=0,nozzleK=0;
    // Distance travelled along the flight line (from the nose position at contact) at physical time t >= contactTime.
    double travelAfterContact(double t)const;
    BlastParameters at(double rangeM)const;
    nlohmann::json receipt()const;
};
// contactOrigin: nose position at first contact; normal: outward normal of the hit facade.
Event solve(const EventOptions&,double contactTime,glm::vec3 surfaceContact,glm::vec3 contactOrigin,glm::vec3 direction,glm::vec3 normal,const std::vector<BlockInput>&);
}
