#include "event_model.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace animation::impact {
namespace {
using Json=nlohmann::json;
constexpr double kAirRhoCp=1206; // J m^-3 K^-1, the transport solver's air heat capacity
void keys(const Json& j,const char* where,std::initializer_list<const char*> allowed){
    if(!j.is_object())throw std::runtime_error(std::string("impactPhysics.")+where+" must be an object");
    std::set<std::string> ok(allowed.begin(),allowed.end());
    for(auto& item:j.items())if(!ok.count(item.key()))throw std::runtime_error(std::string("Unknown impactPhysics.")+where+" field: "+item.key());
}
double number(const Json& j,const char* key,double fallback,double lo,double hi,const char* where){
    if(!j.contains(key))return fallback;
    if(!j[key].is_number())throw std::runtime_error(std::string("impactPhysics.")+where+"."+key+" must be a number");
    const double v=j[key].get<double>();
    if(!std::isfinite(v)||v<lo||v>hi)throw std::runtime_error(std::string("impactPhysics.")+where+"."+key+" outside "+std::to_string(lo)+".."+std::to_string(hi));
    return v;
}
bool flag(const Json& j,const char* key,bool fallback,const char* where){
    if(!j.contains(key))return fallback;if(!j[key].is_boolean())throw std::runtime_error(std::string("impactPhysics.")+where+"."+key+" must be boolean");return j[key].get<bool>();
}
const Json& section(const Json& j,const char* key){static const Json empty=Json::object();return j.contains(key)?j[key]:empty;}
glm::vec3 closest(glm::vec3 p,glm::vec3 lo,glm::vec3 hi){return glm::clamp(p,lo,hi);}
bool segmentHitsBox(glm::vec3 a,glm::vec3 b,glm::vec3 lo,glm::vec3 hi){
    // Slab test of the closed segment a->b against an axis-aligned box.
    double t0=0,t1=1;const glm::vec3 d=b-a;
    for(int k=0;k<3;k++){
        if(std::abs(d[k])<1e-9){if(a[k]<lo[k]||a[k]>hi[k])return false;continue;}
        double u=(lo[k]-a[k])/d[k],v=(hi[k]-a[k])/d[k];if(u>v)std::swap(u,v);t0=std::max(t0,u);t1=std::min(t1,v);if(t0>t1)return false;
    }
    return true;
}
Json vec(glm::vec3 v){return Json::array({v.x,v.y,v.z});}
}

EventOptions EventOptions::parse(const Json& j){
    keys(j,"(root)",{"projectile","charge","fuze","targetMaterial","fragments","energySplit","dust","fireball","ambientTemperatureK","burst","presentationTimeScale"});
    EventOptions o;
    const auto& p=section(j,"projectile");keys(p,"projectile",{"massKg","diameterM","lengthM","speedMetersPerSecond","trajectory","noseCoefficientN","powered","nozzleTemperatureK"});
    o.massKg=number(p,"massKg",o.massKg,1,5000,"projectile");o.diameterM=number(p,"diameterM",o.diameterM,.05,2,"projectile");o.lengthM=number(p,"lengthM",o.lengthM,.2,15,"projectile");
    o.speedMps=number(p,"speedMetersPerSecond",o.speedMps,50,1500,"projectile");o.noseN=number(p,"noseCoefficientN",o.noseN,.5,1.5,"projectile");
    o.powered=flag(p,"powered",o.powered,"projectile");o.nozzleK=number(p,"nozzleTemperatureK",o.nozzleK,300,2500,"projectile");
    if(p.contains("trajectory")&&(!p["trajectory"].is_string()||p["trajectory"]!="straight"))throw std::runtime_error("impactPhysics.projectile.trajectory supports only \"straight\" (constant speed along the authored line)");
    const auto& c=section(j,"charge");keys(c,"charge",{"massKg","tntEquivalence"});
    o.chargeKg=number(c,"massKg",o.chargeKg,.1,2000,"charge");o.tntEquivalence=number(c,"tntEquivalence",o.tntEquivalence,.3,2,"charge");
    const auto& f=section(j,"fuze");keys(f,"fuze",{"mode","delaySeconds"});
    if(f.contains("mode")){if(!f["mode"].is_string())throw std::runtime_error("impactPhysics.fuze.mode must be \"instant\" or \"delay\"");o.fuzeMode=f["mode"].get<std::string>();}
    if(o.fuzeMode!="instant"&&o.fuzeMode!="delay")throw std::runtime_error("impactPhysics.fuze.mode must be \"instant\" or \"delay\"");
    o.fuzeDelayS=number(f,"delaySeconds",o.fuzeDelayS,0,.1,"fuze");if(o.fuzeMode=="instant"){if(f.contains("delaySeconds"))throw std::runtime_error("impactPhysics.fuze.delaySeconds applies only to mode \"delay\"");o.fuzeDelayS=0;}
    const auto& t=section(j,"targetMaterial");keys(t,"targetMaterial",{"wall"});const auto& w=section(t,"wall");
    keys(w,"targetMaterial.wall",{"type","thicknessM","youngS","criticalImpulsePaS","densityKgPerM3"});
    if(w.contains("type")&&(!w["type"].is_string()||w["type"]!="concrete"))throw std::runtime_error("impactPhysics.targetMaterial.wall.type supports only \"concrete\" (Young concrete penetration equation)");
    o.wallThicknessM=number(w,"thicknessM",o.wallThicknessM,.02,3,"targetMaterial.wall");o.youngS=number(w,"youngS",o.youngS,.2,20,"targetMaterial.wall");
    o.criticalImpulsePaS=number(w,"criticalImpulsePaS",o.criticalImpulsePaS,10,1e6,"targetMaterial.wall");o.wallDensity=number(w,"densityKgPerM3",o.wallDensity,500,8000,"targetMaterial.wall");
    const auto& g=section(j,"fragments");keys(g,"fragments",{"minimumMassKg","minimumThicknessM"});
    o.minimumBlockMassKg=number(g,"minimumMassKg",o.minimumBlockMassKg,0,1000,"fragments");o.minimumBlockThicknessM=number(g,"minimumThicknessM",o.minimumBlockThicknessM,0,1,"fragments");
    const auto& e=section(j,"energySplit");keys(e,"energySplit",{"blast","heat"});
    o.blastFraction=number(e,"blast",o.blastFraction,0,1,"energySplit");o.heatFraction=number(e,"heat",o.heatFraction,0,1,"energySplit");
    if(o.blastFraction+o.heatFraction>1+1e-12)throw std::runtime_error("impactPhysics.energySplit blast + heat must not exceed 1");
    const auto& d=section(j,"dust");keys(d,"dust",{"massFraction","massExtinctionM2PerKg"});
    o.dustMassFraction=number(d,"massFraction",o.dustMassFraction,0,.5,"dust");o.dustExtinctionM2PerKg=number(d,"massExtinctionM2PerKg",o.dustExtinctionM2PerKg,1,5000,"dust");
    const auto& b=section(j,"fireball");keys(b,"fireball",{"peakTemperatureK"});o.fireballPeakK=number(b,"peakTemperatureK",o.fireballPeakK,800,4000,"fireball");
    o.ambientK=number(j,"ambientTemperatureK",o.ambientK,220,330,"(root)");
    if(j.contains("burst")){if(!j["burst"].is_string()||(j["burst"]!="free-air"&&j["burst"]!="surface"))throw std::runtime_error("impactPhysics.burst must be \"free-air\" or \"surface\"");o.burst=j["burst"]=="surface"?BurstType::SurfaceHemispherical:BurstType::FreeAirSpherical;}
    o.timeScale=number(j,"presentationTimeScale",o.timeScale,.001,1,"(root)");
    return o;
}
Json EventOptions::describe()const{
    return {{"projectile",{{"massKg",massKg},{"diameterM",diameterM},{"lengthM",lengthM},{"speedMetersPerSecond",speedMps},{"trajectory","straight"},{"noseCoefficientN",noseN},{"powered",powered},{"nozzleTemperatureK",powered?Json(nozzleK):Json(nullptr)}}},
        {"charge",{{"massKg",chargeKg},{"tntEquivalence",tntEquivalence},{"tntMassKg",chargeTntKg()}}},
        {"fuze",{{"mode",fuzeMode},{"delaySeconds",fuzeDelayS}}},
        {"targetMaterial",{{"wall",{{"type","concrete"},{"thicknessM",wallThicknessM},{"youngS",youngS},{"criticalImpulsePaS",criticalImpulsePaS},{"densityKgPerM3",wallDensity}}}}},
        {"fragments",{{"minimumMassKg",minimumBlockMassKg},{"minimumThicknessM",minimumBlockThicknessM}}},
        {"energySplit",{{"blast",blastFraction},{"heat",heatFraction}}},
        {"dust",{{"massFraction",dustMassFraction},{"massExtinctionM2PerKg",dustExtinctionM2PerKg}}},
        {"fireball",{{"peakTemperatureK",fireballPeakK}}},{"ambientTemperatureK",ambientK},
        {"burst",burst==BurstType::SurfaceHemispherical?"surface":"free-air"},{"presentationTimeScale",timeScale}};
}
double Event::travelAfterContact(double tau)const{
    if(tau<=0)return 0;
    const double v=options.speedMps;
    if(penetration.perforated){
        const double vr=penetration.residualSpeedMps,a=(v*v-vr*vr)/(2*penetrationPathM);
        return tau<=wallTimeS?v*tau-.5*a*tau*tau:penetrationPathM+vr*(tau-wallTimeS);
    }
    const double depth=penetration.depthM,a=depth>0?v*v/(2*depth):0;
    return tau<=stopTimeS?v*tau-.5*a*tau*tau:depth;
}
BlastParameters Event::at(double range)const{return blast(std::max(range,1e-3),options.chargeTntKg(),options.burst);}
Event solve(const EventOptions& o,double contactTime,glm::vec3 surfaceContact,glm::vec3 contactOrigin,glm::vec3 direction,glm::vec3 normal,const std::vector<BlockInput>& blocks){
    if(blocks.empty())throw std::runtime_error("impact-v1 requires the preset block table");
    Event e;e.options=o;e.contactTime=contactTime;e.surfaceContact=surfaceContact;e.direction=glm::normalize(direction);e.normal=glm::normalize(normal);
    // Penetration along the flight line; oblique incidence lengthens the path through the wall.
    const double cosine=std::max(.2,double(std::abs(glm::dot(e.direction,e.normal))));
    e.penetrationPathM=o.wallThicknessM/cosine;
    e.penetration=youngConcrete({o.massKg,o.diameterM,o.speedMps,o.youngS,o.noseN,e.penetrationPathM});
    if(e.penetration.perforated)e.wallTimeS=2*e.penetrationPathM/(o.speedMps+e.penetration.residualSpeedMps);
    else e.stopTimeS=e.penetration.depthM>0?2*e.penetration.depthM/o.speedMps:0;
    e.burstTravelM=e.travelAfterContact(o.fuzeDelayS);
    e.burstPoint=contactOrigin+e.direction*float(e.burstTravelM);e.burstTime=contactTime+o.fuzeDelayS;
    e.burstLocation=e.burstTravelM<1e-6?"at first contact":(e.penetration.perforated&&e.burstTravelM>e.penetrationPathM)?"behind the wall (perforated)":"inside the wall";
    // Energy budget.
    const double W=o.chargeTntKg();e.energyJ=W*kTntSpecificEnergyJPerKg;e.blastJ=o.blastFraction*e.energyJ;e.heatJ=o.heatFraction*e.energyJ;
    // Per-block blast load and failure.
    const double zMin=validScaledDistanceMin(o.burst),cube=std::cbrt(W);
    for(const auto& b:blocks){
        BlockResult r;const glm::vec3 lo=b.center-b.size*.5f,hi=b.center+b.size*.5f;
        const glm::vec3 point=closest(e.burstPoint,lo,hi);const glm::vec3 delta=point-e.burstPoint;r.rangeM=std::max(.05,double(glm::length(delta)));
        // Load direction burst -> nearest block point (block centre when the burst is inside the block). The loaded
        // area is the block's projected area across that direction; the angle of incidence is measured against the
        // normal of the box face that the direction enters (the axis with the largest outside offset).
        glm::vec3 load=glm::length(delta)>1e-6f?delta/glm::length(delta):(glm::length(b.center-e.burstPoint)>1e-6f?glm::normalize(b.center-e.burstPoint):e.direction);
        {int axis=-1;float best=0;for(int k=0;k<3;k++)if(std::abs(delta[k])>best){best=std::abs(delta[k]);axis=k;}
         r.incidenceRad=axis<0?0.:std::acos(std::clamp(double(best/glm::length(delta)),0.,1.));}
        const auto p=e.at(r.rangeM);r.scaledDistance=r.rangeM/cube;r.clamped=p.clamped;r.incidentPressurePa=p.incidentPressurePa;r.arrivalS=p.arrivalTimeS;
        r.reflectedImpulsePaS=reflectedImpulseAtIncidence(p,r.incidenceRad);
        r.areaM2=std::abs(load.x)*b.size.y*b.size.z+std::abs(load.y)*b.size.x*b.size.z+std::abs(load.z)*b.size.x*b.size.y;
        r.massKg=o.wallDensity*double(b.size.x)*b.size.y*b.size.z;
        const float radius=float(o.diameterM*.5);
        if(segmentHitsBox(surfaceContact,e.burstPoint,lo-radius,hi+radius)&&e.burstTravelM>0){r.failed=true;r.criterion="penetration channel";}
        else if(r.rangeM/cube<=zMin){r.failed=true;r.criterion="near field (scaled distance below the fit range)";}
        else if(r.reflectedImpulsePaS>o.criticalImpulsePaS){r.failed=true;r.criterion="reflected impulse above critical impulse";}
        else r.criterion="intact (reflected impulse below critical impulse)";
        if(r.failed){
            e.failed++;e.failedVolumeM3+=double(b.size.x)*b.size.y*b.size.z;
            const double excess=std::max(0.,r.reflectedImpulsePaS-o.criticalImpulsePaS);
            glm::vec3 away=b.center-e.burstPoint;away=glm::length(away)>1e-6f?glm::normalize(away):e.direction;
            r.velocity=away*float(excess*r.areaM2/r.massKg);
            e.deliveredImpulseNs+=r.reflectedImpulsePaS*r.areaM2;e.absorbedImpulseNs+=std::min(r.reflectedImpulsePaS,o.criticalImpulsePaS)*r.areaM2;
            const double thinnest=std::min({b.size.x,b.size.y,b.size.z});
            if(r.massKg<o.minimumBlockMassKg||thinnest<o.minimumBlockThicknessM){r.pulverised=true;e.pulverised++;r.criterion+="; below minimum fragment mass/thickness: pulverised (not simulated as a rigid body)";}
            else{
                e.released++;const double speed=glm::length(r.velocity);e.maxFragmentSpeed=std::max(e.maxFragmentSpeed,speed);
                e.fragmentKineticJ+=.5*r.massKg*speed*speed;e.fragmentMomentumNs+=r.massKg*speed;e.momentumVector+=r.velocity*float(r.massKg);
            }
        }
        e.blocks.push_back(r);
    }
    e.dustKg=e.failedVolumeM3*o.wallDensity*o.dustMassFraction;
    // Fireball and transport sources (Gaussian, mean air temperature rise below the 1500 K solver clamp).
    e.fireball=fireball(W);
    e.heatSigmaM=e.fireball.diameterM/4;
    auto amplitude=[&](double sigma){return e.heatJ/(kAirRhoCp*std::pow(2*3.14159265358979*sigma*sigma,1.5));};
    if(amplitude(e.heatSigmaM)>1000){e.heatSigmaM=std::sqrt(std::pow(e.heatJ/(kAirRhoCp*1000),2./3)/(2*3.14159265358979));e.heatSigmaWidened=true;}
    e.heatAmplitudeK=amplitude(e.heatSigmaM);
    e.tracerAmplitude=e.dustKg*o.dustExtinctionM2PerKg/.7/std::pow(2*3.14159265358979*e.heatSigmaM*e.heatSigmaM,1.5);
    // Projectile surface temperatures from aerodynamic recovery (r=1 at the nose stagnation point).
    e.recoveryK=recoveryTemperatureK(o.ambientK,o.speedMps,.89);e.noseK=recoveryTemperatureK(o.ambientK,o.speedMps,1.);
    e.bodyK=e.recoveryK;e.finK=e.recoveryK;e.nozzleK=o.powered?std::max(o.nozzleK,e.recoveryK):e.recoveryK;
    return e;
}
Json Event::receipt()const{
    Json table=Json::array();
    for(size_t i=0;i<blocks.size();i++){const auto& b=blocks[i];
        table.push_back({{"block",i},{"rangeM",b.rangeM},{"scaledDistance",b.scaledDistance},{"incidenceDeg",b.incidenceRad*57.29577951308232},{"incidentPressurePa",b.incidentPressurePa},
            {"reflectedImpulsePaS",b.reflectedImpulsePaS},{"arrivalSeconds",b.arrivalS},{"areaM2",b.areaM2},{"massKg",b.massKg},{"clamped",b.clamped},{"failed",b.failed},{"pulverised",b.pulverised},
            {"criterion",b.criterion},{"launchVelocityMps",vec(b.velocity)}});}
    const double momentumMagnitude=glm::length(momentumVector);
    return {{"model","impact-v1"},{"classification","empirical engineering estimate from public formulas; not independently validated; no structural FEM, compressible detonation CFD or combustion chemistry"},
        {"inputs",options.describe()},
        {"timeline",{{"physicalContactSeconds",contactTime},{"physicalBurstSeconds",burstTime},{"presentationTimeScale",options.timeScale},{"equivalentSlowMotionFactor",1/options.timeScale}}},
        {"geometry",{{"surfaceContactMeters",vec(surfaceContact)},{"burstPointMeters",vec(burstPoint)},{"flightDirection",vec(direction)},{"facadeNormal",vec(normal)},{"burstLocation",burstLocation},{"burstTravelAfterContactM",burstTravelM}}},
        {"penetration",{{"method","Young (SAND97-2426) concrete equation, path thickness = wall thickness / |cos obliquity|"},{"pathThicknessM",penetrationPathM},{"depthM",penetration.depthM},{"perforated",penetration.perforated},
            {"residualSpeedMps",penetration.residualSpeedMps},{"massScaleFactorKs",penetration.massScaleFactorKs},{"outsideYoungValidity",penetration.outsideValidity},{"wallTransitSeconds",wallTimeS},{"stopSeconds",stopTimeS},
            {"kinematics","constant deceleration through the wall (estimate), projectile visible until the burst"}}},
        {"blast",{{"method","Kingery-Bulmash polynomial fits (UFC 3-340-02); Hopkinson-Cranz scaling; oblique reflection interpolated"},{"tntMassKg",options.chargeTntKg()},{"burst",options.burst==BurstType::SurfaceHemispherical?"surface hemispherical":"free-air spherical"},
            {"fitRangeScaledDistance",{validScaledDistanceMin(options.burst),validScaledDistanceMax(options.burst)}}}},
        {"failure",{{"criterion","reflected impulse at the block point nearest the burst (oblique reflection at the entered face) above criticalImpulsePaS; blocks in the penetration channel or below the fit range fail directly"},
            {"loadedArea","block area projected across the burst -> nearest-point direction"},
            {"blocks",blocks.size()},{"failed",failed},{"releasedRigid",released},{"pulverised",pulverised},{"intact",blocks.size()-failed},{"failedVolumeM3",failedVolumeM3}}},
        {"fragments",{{"launch","v = (i_r - i_c) A / m along burst->block centre; release at the blast arrival time; no spin (uniform face load)"},
            {"maxSpeedMps",maxFragmentSpeed},{"kineticEnergyJ",fragmentKineticJ},{"kineticFractionOfCharge",energyJ>0?fragmentKineticJ/energyJ:0},
            {"kineticWithinBlastBudget",fragmentKineticJ<=blastJ},{"momentumMagnitudeSumNs",fragmentMomentumNs},{"momentumVectorNs",vec(momentumVector)},{"momentumVectorMagnitudeNs",momentumMagnitude},
            {"deliveredImpulseNs",deliveredImpulseNs},{"absorbedCriticalImpulseNs",absorbedImpulseNs},
            {"impulseBalanceNs",deliveredImpulseNs-absorbedImpulseNs},{"impulseBalanceNote","sum m|v| of all failed blocks (released + pulverised) equals delivered minus absorbed impulse by construction; the released subset is reported in momentumMagnitudeSumNs"}}},
        {"energy",{{"chargeEnergyJ",energyJ},{"tntSpecificEnergyJPerKg",kTntSpecificEnergyJPerKg},{"blastJ",blastJ},{"heatToAirJ",heatJ},{"fragmentKineticJ",fragmentKineticJ},
            {"unmodelledJ",energyJ-blastJ-heatJ},{"note","blast share is used only through the empirical load; heat share is injected into the transport air; the remainder is not modelled"}}},
        {"fireball",{{"diameterM",fireball.diameterM},{"durationS",fireball.durationS},{"peakTemperatureK",options.fireballPeakK},
            {"cooling","T(t) = Ta + (T0 - Ta) exp(-2 t / duration); radius grows as Rmax (1 - exp(-t / (0.1 duration))); fades after the duration"},{"radiance","Planck emission of an optically thick volume (extinction 3 /m at full fill)"}}},
        {"transportSources",{{"heatSigmaM",heatSigmaM},{"heatAmplitudeK",heatAmplitudeK},{"heatSigmaWidenedToStayBelowSolverClamp",heatSigmaWidened},{"airVolumetricHeatCapacityJPerM3K",kAirRhoCp},
            {"dustKg",dustKg},{"tracerAmplitude",tracerAmplitude},{"tracerUnits","IR extinction / 0.7 per metre (renderer convention)"},
            {"heatRelease","instantaneous at the burst step"},{"dustRelease","rate exp(-a/tau)/tau over 0 <= a < 4 tau after the burst (tau = fireball duration) into the current fireball volume, weight (1 - r/R)^2 with the renderer's fireball radius and centre; total = dustKg x extinction; the transport then carries it (wind, buoyancy)"}}},
        {"projectileTemperaturesK",{{"recovery",recoveryK},{"nose",noseK},{"body",bodyK},{"fins",finK},{"nozzle",nozzleK},{"method","Tr = Ta (1 + r (gamma-1)/2 M^2), r=0.89 (turbulent) and r=1 at the nose"}}},
        {"blocks",table}};
}
}
