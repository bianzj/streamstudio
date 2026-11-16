#include "mesh_timeline.h"
#include "animation/scene/mesh_scene.h"
#include "animation/collision/rigid_scene.h"
#include "animation/impact/event_model.h"
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>

namespace animation {
namespace {
using Json=nlohmann::json;
int integer(const Json& j,const char* key,int fallback,int lo,int hi){if(!j.contains(key))return fallback;if(!j[key].is_number_integer())throw std::runtime_error(std::string(key)+" must be integer");double v=j[key].get<double>();if(v<lo||v>hi)throw std::runtime_error(std::string(key)+" outside supported range");return int(v);}
glm::vec3 vector(const Json& j){auto a=j.get<std::vector<float>>();if(a.size()!=3)throw std::runtime_error("Sequence vector requires3values");for(float x:a)if(!std::isfinite(x)||std::abs(x)>10000)throw std::runtime_error("Invalid sequence vector");return {a[0],a[1],a[2]};}
float smooth(float x){x=std::clamp(x,0.f,1.f);return x*x*(3-2*x);}
void transform(Triangle& t,const glm::mat3& r,const glm::vec3& origin,const glm::vec3& center){
    for(auto* p:{&t.a,&t.b,&t.c})*p=glm::vec4(r*(glm::vec3(*p)-origin)+center,1);
    for(auto* n:{&t.na,&t.nb,&t.nc})*n=glm::vec4(glm::normalize(r*glm::vec3(*n)),0);
}
bool overlap(const std::vector<glm::vec2>& a,const std::vector<glm::vec2>& b,float margin=0){
    for(const auto* polygon:{&a,&b})for(size_t i=0;i<polygon->size();i++){
        auto edge=(*polygon)[(i+1)%polygon->size()]-(*polygon)[i];auto axis=glm::normalize(glm::vec2(-edge.y,edge.x));
        float al=1e9f,ah=-1e9f,bl=1e9f,bh=-1e9f;
        for(auto p:a){float v=glm::dot(p,axis);al=std::min(al,v);ah=std::max(ah,v);}for(auto p:b){float v=glm::dot(p,axis);bl=std::min(bl,v);bh=std::max(bh,v);}
        if(ah+margin<=bl||bh+margin<=al)return false;
    }
    return true;
}
}
MeshTimeline::MeshTimeline(const std::filesystem::path& input):base(input),shell(base){
    const auto& seq=base.input.at("sequence");fps=integer(seq,"fps",24,1,60);count=integer(seq,"frameCount",288,1,2000);first=integer(seq,"firstFrame",1,1,count);last=integer(seq,"lastFrame",count,first,count);
    contact=vector(seq.at("sourcePoint"));projectileStart=vector(seq.at("projectileStart"));flowOrigin=vector(seq.at("flowOrigin"));
    // The flight is a constant-speed translation. Speed is either supplied directly
    // (preferred: it is a physical input) or derived from the authored contact time.
    const float nominalPath=glm::length(contact-projectileStart);
    const auto motion=seq.value("motionBackend",std::string("authored"));
    // impact-v1: the flight speed and the presentation time scale come from impactPhysics.
    std::unique_ptr<impact::EventOptions> impactOptions;
    if(motion=="impact-v1"){
        if(!seq.contains("impactPhysics"))throw std::runtime_error("motionBackend impact-v1 requires sequence.impactPhysics");
        for(const char* key:{"impactTime","flightSpeedMetersPerSecond","fragmentRelease","fragmentExit"})if(seq.contains(key))throw std::runtime_error(std::string("impactPhysics and ")+key+" are mutually exclusive (impact-v1 computes them)");
        impactOptions=std::make_unique<impact::EventOptions>(impact::EventOptions::parse(seq.at("impactPhysics")));timeScale=impactOptions->timeScale;
        impact=float(nominalPath/impactOptions->speedMps);flightSpeedSupplied=true;
    }else if(seq.contains("impactPhysics"))throw std::runtime_error("impactPhysics requires motionBackend \"impact-v1\"");
    else if(seq.contains("flightSpeedMetersPerSecond")){
        if(seq.contains("impactTime"))throw std::runtime_error("flightSpeedMetersPerSecond and impactTime are mutually exclusive");
        const auto& v=seq.at("flightSpeedMetersPerSecond");if(!v.is_number())throw std::runtime_error("flightSpeedMetersPerSecond must be a number");
        const double speed=v.get<double>();if(!std::isfinite(speed)||speed<1||speed>2000)throw std::runtime_error("flightSpeedMetersPerSecond outside 1..2000");
        impact=float(nominalPath/speed);flightSpeedSupplied=true;
    }else impact=seq.value("impactTime",3.f);
    if(!std::isfinite(impact)||impact<=0||impact>double(count-1)/fps*timeScale)throw std::runtime_error("Invalid sequence impactTime");
    flightSpeed=nominalPath/impact;
    projectileEnd=contact;
    if(motion!="authored"&&motion!="rigid-v1"&&motion!="impact-v1")throw std::runtime_error("Unknown motionBackend");
    if(glm::length(contact-projectileStart)<1e-3f||glm::length(glm::cross(contact-projectileStart,glm::vec3(0,0,1)))<1e-3f||flowOrigin.z!=0)throw std::runtime_error("Unsupported projectile path/flow origin");
    auto w=seq.at("wind").get<std::vector<float>>();if(w.size()!=2||!std::isfinite(w[0])||!std::isfinite(w[1])||std::abs(w[0])+std::abs(w[1])>8)throw std::runtime_error("Invalid sequence wind");wind={w[0],w[1]};
    sensor=seq.value("sensor",true);media=seq.value("media",true);
    const auto appearance=seq.value("projectileAppearance",std::string("legacy"));
    if(appearance!="legacy"&&appearance!="separated-v2")throw std::runtime_error("Unknown projectileAppearance");
    projectileV2=appearance=="separated-v2";
    if(seq.contains("trackingCameraOffset")){tracking=true;trackingOffset=vector(seq.at("trackingCameraOffset"));if(glm::length(trackingOffset)<4)throw std::runtime_error("Tracking camera offset too small");}
    if(seq.contains("cameraMount")){
        const auto& mount=seq.at("cameraMount");
        if(!mount.is_object()||mount.value("body",std::string())!="projectile")throw std::runtime_error("Unsupported camera mount body");
        if(tracking)throw std::runtime_error("Body mount and tracking camera are mutually exclusive");
        mountOffset=vector(mount.at("offsetMeters"));
        if(glm::length(mountOffset)>3)throw std::runtime_error("Body camera offset exceeds3m");
        mounted=true;
    }
    bool found=false;for(const auto& c:base.cameras)if(c.id==seq.at("camera")){camera=c;found=true;}if(!found)throw std::runtime_error("Unknown sequence camera");
    found=false;for(size_t i=0;i<base.input["instances"].size();i++)if(base.input["instances"][i]["id"]==seq.at("targetInstance")){target=uint32_t(i);found=true;}if(!found)throw std::runtime_error("Unknown sequence target instance");
    exports=seq.value("exportFrames",std::vector<int>{});std::set<int> unique;for(int n:exports)if(n<first||n>last||!unique.insert(n).second)throw std::runtime_error("Invalid sequence exportFrames");
    auto mat=seq.at("fractureMaterial");MaterialTable extra(Json::array({mat}));fractureMaterial=uint32_t(base.materials.values.size());base.materials.names.push_back(extra.names.front());base.materials.values.push_back(extra.values.front());
    projectileMaterial=uint32_t(base.materials.values.size());base.materials.names.push_back("projectile_visual");base.materials.values.push_back({{.3f,.3f,.3f,.9f},{650,0,0,0}});
    if(projectileV2){
        auto material=[&](const char* name,float temperature,float emissivity){auto id=uint32_t(base.materials.values.size());base.materials.names.push_back(name);base.materials.values.push_back({{.35f,.35f,.35f,emissivity},{temperature,0,0,0}});return id;};
        bodyMaterial=material("visual_body",bodyK,.84f);noseMaterial=material("visual_nose",noseK,.88f);
        nozzleMaterial=material("visual_nozzle",nozzleK,.93f);finMaterial=material("visual_fin",finK,.82f);
    }
    shell.materials=base.materials;
    auto read=[&](const char* key){const auto& ref=seq.at(key);auto p=std::filesystem::absolute(input.parent_path()/pathFrom(ref.at("path").get<std::string>())).lexically_normal();auto hash=ref.at("sha256").get<std::string>();if(sha256File(p)!=hash)throw std::runtime_error(std::string("Sequence asset SHA256 mismatch: ")+key);paths.push_back(p);hashes.push_back(hash);keys.push_back(key);return p;};
    auto rem=read("remainder"),remLinks=read("remainderLinks"),frag=read("fragments"),fragLinks=read("fragmentLinks"),table=read("chunks");
    auto load=[&](const std::filesystem::path& asset,const std::filesystem::path& links){
        auto config=base.input;config.erase("backgroundField");config.erase("thermalPerturbations");config.erase("sequence");config["materials"].push_back(mat);config["assets"]=Json::array({{{"id","preset"},{"path",pathText(asset)},{"sha256",sha256File(asset)},{"upAxis","Z"},{"units","m"}}});config["instances"]=Json::array({{{"id","preset"},{"asset","preset"},{"transform",{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}}}});
        // Load through the same parser without writing a temporary project.
        MeshScene loaded(input,config);std::ifstream f(links);Json data;f>>data;if(data.size()!=loaded.triangles.size())throw std::runtime_error("Preset link coverage mismatch");
        for(size_t i=0;i<data.size();i++){
            auto link=data[i].get<std::vector<int64_t>>();if(link.size()!=3||link[0]<0||size_t(link[0])>=base.triangles.size())throw std::runtime_error("Invalid preset source link");
            const auto& source=base.triangles[size_t(link[0])];if(source.ids.y!=target)throw std::runtime_error("Preset link points outside target");
            auto& t=loaded.triangles[i];t.ids={t.ids.x,target,uint32_t(link[1]+1),uint32_t(link[2])};t.thermal=source.thermal;
        }
        return loaded.triangles;
    };
    remainder=load(rem,remLinks);auto fragments=load(frag,fragLinks);std::ifstream tf(table);Json data;tf>>data;
    if(!data.is_array()||data.empty()||data.size()>3000)throw std::runtime_error("Invalid fragment table");
    for(size_t index=0;index<data.size();index++){
        const auto& row=data[index];Chunk c;c.center=vector(row.at("center"));c.size=vector(row.at("size"));c.velocity=vector(row.at("velocity"));c.delay=row.at("launchDelay");c.exit=row.at("exitDuration");
        int start=integer(row,"firstTriangle",0,0,int(fragments.size())),n=integer(row,"triangleCount",0,1,int(fragments.size()));
        if(size_t(start)+n>fragments.size()||c.delay<0||c.delay>1||c.exit<=0||c.exit>2||!std::isfinite(c.delay)||!std::isfinite(c.exit)||glm::any(glm::lessThanEqual(c.size,glm::vec3(0))))throw std::runtime_error("Invalid fragment range/timing");
        c.triangles.assign(fragments.begin()+start,fragments.begin()+start+n);for(const auto& t:c.triangles)if(t.ids.z!=index+1)throw std::runtime_error("Fragment link identity mismatch");
        int axis=c.size.x<c.size.y?(c.size.x<c.size.z?0:2):(c.size.y<c.size.z?1:2);
        glm::quat align=axis==0?glm::angleAxis(glm::half_pi<float>(),glm::vec3(0,1,0)):axis==1?glm::angleAxis(glm::half_pi<float>(),glm::vec3(1,0,0)):glm::quat(1,0,0,0);
        c.rest=glm::angleAxis(float(row.at("seed").get<double>()*glm::two_pi<double>()),glm::vec3(0,0,1))*align;chunks.push_back(std::move(c));
    }
    for(const auto& t:base.triangles)if(base.input["instances"][t.ids.y]["id"]=="ground"&&glm::cross(glm::vec3(t.b-t.a),glm::vec3(t.c-t.a)).z>1e-6)ground.push_back(t);
    if(ground.empty())throw std::runtime_error("Sequence requires explicit ground support mesh");
    {
        // Preset breach extent, thin-fragment census and a free-fall speed bound: with no
        // energy source after release, speed cannot exceed sqrt(v0^2+2 g H) except by a
        // (1+restitution) bump from an equally fast heavier body.
        glm::vec3 lo(1e9f),hi(-1e9f);minChunkDimension=1e9;float v0=0;
        for(const auto& c:chunks){lo=glm::min(lo,c.center-c.size*.5f);hi=glm::max(hi,c.center+c.size*.5f);float d=std::min({c.size.x,c.size.y,c.size.z});minChunkDimension=std::min(minChunkDimension,double(d));if(d<.05f)sliverChunks++;v0=std::max(v0,glm::length(c.velocity));}
        breachLower=lo;breachUpper=hi;float groundLow=1e9f;for(const auto& t:ground)groundLow=std::min({groundLow,t.a.z,t.b.z,t.c.z});
        speedBound=std::sqrt(double(v0)*v0+2*9.81*std::max(0.f,hi.z-groundLow));
        if(seq.contains("requireContactInBreach")&&!seq["requireContactInBreach"].is_boolean())throw std::runtime_error("requireContactInBreach must be boolean");
        if(seq.contains("rigidSpeedGuardFactor")){const auto& f=seq["rigidSpeedGuardFactor"];if(!f.is_number())throw std::runtime_error("rigidSpeedGuardFactor must be a number");double factor=f.get<double>();if(!std::isfinite(factor)||(factor!=0&&(factor<1||factor>5)))throw std::runtime_error("rigidSpeedGuardFactor must be 0 (off) or within 1..5");speedGuard=factor*speedBound;}
        else speedGuard=1.25*speedBound;
    }
    auto reportBreach=[&](glm::vec3 point){
        const float margin=.25f;
        contactInBreach=glm::all(glm::greaterThanEqual(point,breachLower-margin))&&glm::all(glm::lessThanEqual(point,breachUpper+margin));
        breachReceipt={{"contactInsideAuthoredBreach",contactInBreach},{"marginMeters",margin},{"contactMeters",{point.x,point.y,point.z}},{"breachLowerMeters",{breachLower.x,breachLower.y,breachLower.z}},{"breachUpperMeters",{breachUpper.x,breachUpper.y,breachUpper.z}},
            {"note","opening, fireball, dust, heat pulse and fragment exit directions are authored in world axes (facade facing -Y); they do not follow the computed contact point or normal"}};
        if(!contactInBreach){
            if(seq.value("requireContactInBreach",false))throw std::runtime_error("Computed contact lies outside the authored breach; regenerate the fracture preset for this flight path");
            std::cerr<<"WARNING: contact lies outside the authored breach bounds; opening and effects will not match the contact point\n";
        }
    };
    // Optional: release fragments as a disturbance front from the contact point instead of
    // the preset's pseudo-random delays. Delay = onset + distance / propagation speed.
    auto applyRelease=[&]{
        if(!seq.contains("fragmentRelease"))return;
        const auto& r=seq["fragmentRelease"];if(!r.is_object()||!r.contains("mode")||r["mode"]!="distance")throw std::runtime_error("fragmentRelease requires mode \"distance\"");
        if(!r.contains("propagationMetersPerSecond")||!r["propagationMetersPerSecond"].is_number()||(r.contains("onsetSeconds")&&!r["onsetSeconds"].is_number()))throw std::runtime_error("fragmentRelease requires numeric propagationMetersPerSecond/onsetSeconds");
        const double speed=r["propagationMetersPerSecond"].get<double>(),onset=r.value("onsetSeconds",.015);
        if(!std::isfinite(speed)||speed<10||speed>2000||!std::isfinite(onset)||onset<0||onset>.5)throw std::runtime_error("fragmentRelease propagationMetersPerSecond must be 10..2000 and onsetSeconds 0..0.5");
        double lo=1e9,hi=-1e9;
        for(auto& c:chunks){const double delay=onset+glm::length(c.center-contact)/speed;if(delay>1)throw std::runtime_error("fragmentRelease delay exceeds 1 s; raise propagationMetersPerSecond");c.delay=float(delay);lo=std::min(lo,delay);hi=std::max(hi,delay);}
        releaseReceipt={{"mode","distance"},{"propagationMetersPerSecond",speed},{"onsetSeconds",onset},{"minimumDelaySeconds",lo},{"maximumDelaySeconds",hi},{"note","authored disturbance front from the contact point; not a computed stress wave"}};
    };
    if(motion=="impact-v1"){
        if(!projectileV2)throw std::runtime_error("impact-v1 requires separated-v2 projectileAppearance");
        // First contact: the same convex sweep as rigid-v1, at the physical flight speed.
        auto initial=frame(0);std::vector<glm::vec3> hull;
        for(const auto& t:initial.triangles)if(t.ids.w==4)for(auto p:{t.a,t.b,t.c})hull.push_back(glm::vec3(p)-projectileStart);
        auto direction=glm::normalize(contact-projectileStart);auto extended=contact+direction*2.f;
        auto hit=sweepContact(base.triangles,hull,projectileStart,extended);
        if(!hit.hit||hit.triangle<0||size_t(hit.triangle)>=base.triangles.size()||base.triangles[size_t(hit.triangle)].ids.y!=target)throw std::runtime_error("impact-v1 flight path has no first contact with target");
        const double contactTime=double(glm::length(hit.origin-projectileStart))/impactOptions->speedMps;
        glm::vec3 normal=hit.normal;if(glm::dot(normal,direction)>0)normal=-normal;
        std::vector<impact::BlockInput> blocks;for(const auto& c:chunks)blocks.push_back({c.center,c.size});
        event=std::make_unique<impact::Event>(impact::solve(*impactOptions,contactTime,hit.point,hit.origin,direction,normal,blocks));
        impact=float(event->burstTime);projectileEnd=projectile(event->burstTime);contact=event->burstPoint;flightSpeed=float(impactOptions->speedMps);
        if(!std::isfinite(impact)||impact<=0||impact>double(count-1)/fps*timeScale)throw std::runtime_error("impact-v1 burst lies outside the sequence; lengthen the clip or raise presentationTimeScale");
        firstContact={{"computed",true},{"method","Bullet convex sweep against actual intact scene triangles"},{"timeSeconds",contactTime},{"positionMeters",{hit.point.x,hit.point.y,hit.point.z}},{"projectileOriginMeters",{hit.origin.x,hit.origin.y,hit.origin.z}},{"flightDirection",{direction.x,direction.y,direction.z}},{"normal",{normal.x,normal.y,normal.z}},{"triangle",hit.triangle},{"burstSeconds",event->burstTime},{"burstPointMeters",{contact.x,contact.y,contact.z}}};
        {const glm::vec2 h(normal.x,normal.y);if(glm::length(h)>=.5f)outwardAzimuth=std::atan2(h.x/glm::length(h),-h.y/glm::length(h));}
        // Aerodynamic recovery temperatures replace the authored projectile temperatures.
        noseK=float(event->noseK);bodyK=float(event->bodyK);nozzleK=float(event->nozzleK);finK=float(event->finK);
        const std::pair<uint32_t,float> parts[]{{bodyMaterial,bodyK},{noseMaterial,noseK},{nozzleMaterial,nozzleK},{finMaterial,finK}};
        for(const auto& [id,k]:parts)base.materials.values[id].thermal.x=k;
        shell.materials=base.materials;
        // Static scene: everything but the target, the preset remainder and every intact block.
        shell.triangles.clear();for(const auto& t:base.triangles)if(t.ids.y!=target)shell.triangles.push_back(t);shell.triangles.insert(shell.triangles.end(),remainder.begin(),remainder.end());
        std::vector<RigidBodySpec> bodies;glm::vec3 lo(1e9f),hi(-1e9f);double v0=0;float top=-1e9f;
        for(size_t i=0;i<chunks.size();i++){
            auto& c=chunks[i];const auto& r=event->blocks[i];
            if(!r.failed){shell.triangles.insert(shell.triangles.end(),c.triangles.begin(),c.triangles.end());continue;}
            lo=glm::min(lo,c.center-c.size*.5f);hi=glm::max(hi,c.center+c.size*.5f);
            if(r.pulverised)continue;
            glm::vec3 blo(1e9f),bhi(-1e9f);for(const auto& t:c.triangles)for(auto v:{t.a,t.b,t.c}){blo=glm::min(blo,glm::vec3(v));bhi=glm::max(bhi,glm::vec3(v));}
            if(glm::length((blo+bhi)*.5f-c.center)>1e-4f||glm::length(bhi-blo-c.size)>1e-4f)throw std::runtime_error("Rigid box proxy does not match fragment geometry");
            c.delay=float(r.arrivalS);c.velocity=r.velocity;v0=std::max(v0,double(glm::length(r.velocity)));top=std::max(top,bhi.z);
            bodies.push_back({c.center,c.size,r.velocity,glm::vec3(0),float(r.arrivalS),float(impactOptions->wallDensity)});bodyChunk.push_back(i);
        }
        contactInBreach=true;
        breachReceipt={{"computed",true},{"failedBlocks",event->failed},{"releasedRigidBlocks",event->released},{"pulverisedBlocks",event->pulverised},{"intactBlocks",chunks.size()-event->failed},
            {"failedLowerMeters",event->failed?Json{lo.x,lo.y,lo.z}:Json(nullptr)},{"failedUpperMeters",event->failed?Json{hi.x,hi.y,hi.z}:Json(nullptr)},
            {"note","opening = preset blocks whose empirical reflected impulse exceeds the critical impulse (see event.json); unfailed blocks stay in the static wall"}};
        float groundLow=1e9f;for(const auto& t:ground)groundLow=std::min({groundLow,t.a.z,t.b.z,t.c.z});
        speedBound=std::sqrt(v0*v0+2*9.81*std::max(0.f,(bodies.empty()?groundLow:top)-groundLow));
        const double factor=seq.contains("rigidSpeedGuardFactor")?seq["rigidSpeedGuardFactor"].get<double>():1.25;speedGuard=factor*speedBound;
        if(!bodies.empty()){
            RigidOptions rigidOptions;rigidOptions.maxLinearSpeed=speedGuard;
            if(seq.contains("rigidRequireSolidStatic")){const auto& r=seq["rigidRequireSolidStatic"];if(!r.is_boolean())throw std::runtime_error("rigidRequireSolidStatic must be boolean");rigidOptions.requireSolidStatic=r.get<bool>();}
            if(seq.contains("rigidCache")){
                const auto& c=seq["rigidCache"];if(!c.is_object()||(c.contains("enabled")&&!c["enabled"].is_boolean())||!c.contains("directory")||!c["directory"].is_string())throw std::runtime_error("rigidCache requires an object with string \"directory\" and optional boolean \"enabled\"");
                if(c.value("enabled",true)){rigidOptions.trackCacheDirectory=pathText(std::filesystem::absolute(input.parent_path()/pathFrom(c["directory"].get<std::string>())).lexically_normal());rigidOptions.trackHorizonSeconds=std::max(0.,double(last-1)/fps*timeScale-double(impact));}
            }
            dynamics=std::make_unique<RigidScene>(shell.triangles,bodies,rigidOptions);
        }
        return;
    }
    if(motion=="rigid-v1"){
        if(seq.contains("fragmentExit"))throw std::runtime_error("fragmentExit applies only to the authored motionBackend (rigid-v1 uses the fragment table velocities)");
        if(!projectileV2)throw std::runtime_error("rigid-v1 requires separated-v2 projectileAppearance");
        // Query the same projectile vertices rendered at t=0; a convex proxy
        // encloses its fins. No prescribed timer is used as the contact event.
        auto initial=frame(0);std::vector<glm::vec3> hull;
        for(const auto& t:initial.triangles)if(t.ids.w==4)for(auto p:{t.a,t.b,t.c})hull.push_back(glm::vec3(p)-projectileStart);
        auto direction=glm::normalize(contact-projectileStart);auto extended=contact+direction*2.f;
        auto hit=sweepContact(base.triangles,hull,projectileStart,extended);
        if(!hit.hit||hit.triangle<0||size_t(hit.triangle)>=base.triangles.size()||base.triangles[size_t(hit.triangle)].ids.y!=target)throw std::runtime_error("Authored flight path has no first contact with target");
        double actual=hit.fraction*glm::length(extended-projectileStart)/glm::length(contact-projectileStart)*impact;
        if(actual<=0||actual>double(count-1)/fps)throw std::runtime_error("First contact lies outside sequence");
        firstContact={{"computed",true},{"method","Bullet convex sweep against actual intact scene triangles"},{"timeSeconds",actual},{"requestedTimeSeconds",impact},{"positionMeters",{hit.point.x,hit.point.y,hit.point.z}},{"projectileOriginMeters",{hit.origin.x,hit.origin.y,hit.origin.z}},{"flightDirection",{direction.x,direction.y,direction.z}},{"normal",{hit.normal.x,hit.normal.y,hit.normal.z}},{"triangle",hit.triangle},{"projectileProxy","convex hull of rendered body/fins; fixed orientation translation"}};
        impact=float(actual);contact=hit.point;projectileEnd=hit.origin;
        {const glm::vec2 h(hit.normal.x,hit.normal.y);if(glm::length(h)>=.5f)outwardAzimuth=std::atan2(h.x/glm::length(h),-h.y/glm::length(h));} // roof/ground hits keep the default
        reportBreach(contact);applyRelease();
        shell.triangles.clear();for(const auto& t:base.triangles)if(t.ids.y!=target)shell.triangles.push_back(t);shell.triangles.insert(shell.triangles.end(),remainder.begin(),remainder.end());
        std::vector<RigidBodySpec> bodies;
        for(size_t i=0;i<chunks.size();i++){
            const auto& c=chunks[i];glm::vec3 lo(1e9f),hi(-1e9f);
            for(const auto& t:c.triangles)for(auto v:{t.a,t.b,t.c}){lo=glm::min(lo,glm::vec3(v));hi=glm::max(hi,glm::vec3(v));}
            if(glm::length((lo+hi)*.5f-c.center)>1e-4f||glm::length(hi-lo-c.size)>1e-4f)throw std::runtime_error("Rigid box proxy does not match fragment geometry");
            float seed=float(i+1);bodies.push_back({c.center,c.size,c.velocity,{.7f*std::sin(seed*1.7f),.55f*std::cos(seed*2.1f),.4f*std::sin(seed*.9f)},c.delay,2300});
        }
        RigidOptions rigidOptions;rigidOptions.maxLinearSpeed=speedGuard;
        if(seq.contains("rigidRequireSolidStatic")){const auto& r=seq["rigidRequireSolidStatic"];if(!r.is_boolean())throw std::runtime_error("rigidRequireSolidStatic must be boolean");rigidOptions.requireSolidStatic=r.get<bool>();}
        if(seq.contains("rigidCache")){
            // Opt-in: record the solved poses once, replay them for every later run with identical physics input.
            const auto& c=seq["rigidCache"];if(!c.is_object()||(c.contains("enabled")&&!c["enabled"].is_boolean())||!c.contains("directory")||!c["directory"].is_string())throw std::runtime_error("rigidCache requires an object with string \"directory\" and optional boolean \"enabled\"");
            if(c.value("enabled",true)){rigidOptions.trackCacheDirectory=pathText(std::filesystem::absolute(input.parent_path()/pathFrom(c["directory"].get<std::string>())).lexically_normal());rigidOptions.trackHorizonSeconds=std::max(0.,double(last-1)/fps-double(impact));}
        }
        dynamics=std::make_unique<RigidScene>(shell.triangles,bodies,rigidOptions);return;
    }
    // Author non-overlapping landing footprints first, then choose horizontal
    // velocities that reach them under gravity. This avoids false AABB support
    // and ordering-dependent stacks without claiming a rigid-body simulation.
    auto area=seq.at("landingArea");auto low=area.at("lower").get<std::vector<float>>(),high=area.at("upper").get<std::vector<float>>();
    if(low.size()!=2||high.size()!=2||!std::isfinite(low[0]+low[1]+high[0]+high[1])||low[0]>=high[0]||low[1]>=high[1])throw std::runtime_error("Invalid preset landingArea");
    // Optional authored exit plane for a facade that is not the canonical -Y wall.
    if(seq.contains("fragmentExit")){
        const auto& e=seq["fragmentExit"];
        if(!e.is_object()||!e.contains("normal")||!e.contains("planeOffset")||!e["planeOffset"].is_number())throw std::runtime_error("fragmentExit requires {\"normal\":[nx,ny],\"planeOffset\":s}");
        auto n=e["normal"].get<std::vector<float>>();const float s=e["planeOffset"].get<float>();
        if(n.size()!=2||!std::isfinite(n[0])||!std::isfinite(n[1])||std::hypot(n[0],n[1])<1e-3f||!std::isfinite(s)||std::abs(s)>10000)throw std::runtime_error("fragmentExit normal must be a non-zero horizontal 2-vector and planeOffset finite");
        exitNormal=glm::normalize(glm::vec2(n[0],n[1]));exitOffset=s;customExit=true;
        for(auto& c:chunks){const glm::vec2 xy(c.center);c.exitPoint=xy+exitNormal*(exitOffset-glm::dot(exitNormal,xy));}
    }
    std::vector<size_t> order(chunks.size());std::iota(order.begin(),order.end(),0);
    for(auto& c:chunks){
        auto r=glm::mat3_cast(c.rest);float top=-1e9f;c.bottom=1e9f;
        std::vector<glm::vec3> points;
        for(const auto& t:c.triangles)for(auto v:{t.a,t.b,t.c}){auto p=r*(glm::vec3(v)-c.center);c.bottom=std::min(c.bottom,p.z);top=std::max(top,p.z);points.push_back(p);}
        c.restHeight=top-c.bottom;std::vector<glm::vec2> corners;
        for(auto p:points)if(std::abs(p.z-c.bottom)<1e-4f){glm::vec2 xy(p);if(std::none_of(corners.begin(),corners.end(),[&](auto q){return glm::length(q-xy)<1e-5f;}))corners.push_back(xy);}
        if(corners.size()!=4)throw std::runtime_error("Preset fragment is not a closed rectangular chunk");
        std::sort(corners.begin(),corners.end(),[](auto a,auto b){return std::atan2(a.y,a.x)<std::atan2(b.y,b.x);});std::copy(corners.begin(),corners.end(),c.footprint.begin());
    }
    std::sort(order.begin(),order.end(),[&](size_t a,size_t b){return chunks[a].size.x*chunks[a].size.y*chunks[a].size.z/chunks[a].restHeight>chunks[b].size.x*chunks[b].size.y*chunks[b].size.z/chunks[b].restHeight;});
    std::vector<std::vector<glm::vec2>> occupied;
    for(auto id:order){auto& c=chunks[id];float initial=std::max(.1f,c.center.z+c.bottom);float duration=(c.velocity.z+std::sqrt(c.velocity.z*c.velocity.z+19.62f*initial))/9.81f;
        glm::vec2 wanted=customExit?c.exitPoint+glm::vec2(c.velocity)*duration:glm::vec2(c.center.x+c.velocity.x*duration,-2.8f+c.velocity.y*duration);std::array<glm::vec2,4> polygon;glm::vec2 chosen;bool placed=false;
        for(int trial=0;trial<30000&&!placed;trial++){
            float radius=.10f*std::sqrt(float(trial)),angle=trial*2.39996323f;chosen=wanted+radius*glm::vec2(std::cos(angle),std::sin(angle));bool inside=true;
            for(int k=0;k<4;k++){polygon[k]=c.footprint[k]+chosen;inside=inside&&polygon[k].x>=low[0]&&polygon[k].x<=high[0]&&polygon[k].y>=low[1]&&polygon[k].y<=high[1];}
            if(!inside)continue;std::vector<glm::vec2> candidate(polygon.begin(),polygon.end());placed=std::none_of(occupied.begin(),occupied.end(),[&](const auto& other){return overlap(candidate,other,.02f);});
        }
        if(!placed)throw std::runtime_error("Preset landing area cannot fit all fragments without overlap");
        float floor=footprintSupport(polygon);float height=c.center.z+c.bottom-floor;if(height<=0)throw std::runtime_error("Fragment starts below landing plane");
        c.contactTime=(c.velocity.z+std::sqrt(c.velocity.z*c.velocity.z+19.62f*height))/9.81f;
        if(c.contactTime<1.05f)throw std::runtime_error("Preset fragment contacts before settling orientation completes");
        c.landing={chosen.x,chosen.y,floor-c.bottom};
        if(customExit){c.velocity.x=(chosen.x-c.exitPoint.x)/c.contactTime;c.velocity.y=(chosen.y-c.exitPoint.y)/c.contactTime;}
        else{c.velocity.x=(chosen.x-c.center.x)/c.contactTime;c.velocity.y=(chosen.y+2.8f)/c.contactTime;}
        occupied.emplace_back(polygon.begin(),polygon.end());
    }
    for(size_t a=0;a<occupied.size();a++)for(size_t b=0;b<a;b++)if(overlap(occupied[a],occupied[b]))overlapPairs++;
    shell.triangles.clear();for(const auto& t:base.triangles)if(t.ids.y!=target)shell.triangles.push_back(t);shell.triangles.insert(shell.triangles.end(),remainder.begin(),remainder.end());reportBreach(contact);applyRelease();
}
float MeshTimeline::support(glm::vec3 p)const{
    float height=-1000;
    for(const auto& t:ground){auto a=glm::vec3(t.a),b=glm::vec3(t.b),c=glm::vec3(t.c);glm::vec2 u=glm::vec2(b-a),v=glm::vec2(c-a),w=glm::vec2(p-a);float d=u.x*v.y-u.y*v.x;if(std::abs(d)<1e-8)continue;float s=(w.x*v.y-w.y*v.x)/d,q=(u.x*w.y-u.y*w.x)/d;if(s>=-1e-5&&q>=-1e-5&&s+q<=1.00001f)height=std::max(height,a.z+s*(b.z-a.z)+q*(c.z-a.z));}
    // Diagnostic only: a fragment that left the ground mesh must not abort a long render.
    if(height<-100)return std::numeric_limits<float>::quiet_NaN();return height;
}
float MeshTimeline::footprintSupport(const std::array<glm::vec2,4>& footprint,bool strict)const{
    float height=-1000;std::vector<glm::vec2> polygon(footprint.begin(),footprint.end());
    for(const auto& t:ground){std::vector<glm::vec2> triangle{glm::vec2(t.a),glm::vec2(t.b),glm::vec2(t.c)};if(overlap(polygon,triangle))height=std::max(height,t.a.z);}
    if(height<-100){if(strict)throw std::runtime_error("Fragment footprint left ground support coverage");return std::numeric_limits<float>::quiet_NaN();}return height;
}
glm::mat3 MeshTimeline::rotation(const Chunk& c,double freeTime)const{return glm::mat3_cast(glm::slerp(glm::quat(1,0,0,0),c.rest,smooth(float(freeTime))));}
MeshTimeline::~MeshTimeline()=default;
glm::vec3 MeshTimeline::projectile(double time)const{
    if(event){
        // impact-v1: physical speed to first contact, then the estimated constant deceleration in the wall until the burst.
        const double t=std::max(0.,time),before=std::min(t,event->contactTime)*event->options.speedMps;
        const double after=event->travelAfterContact(std::min(t,event->burstTime)-event->contactTime);
        return projectileStart+event->direction*float(before+after);
    }
    return glm::mix(projectileStart,projectileEnd,std::clamp(float(time/impact),0.f,1.f));
}
Camera MeshTimeline::view(const CameraRig& rig,double time)const{
    if(rig.mounted){
        if(!std::isfinite(time)||time<0||time>=impact)throw std::runtime_error("Body camera only valid before first contact");
        // Same right/up/forward basis used by the rendered body's geometry.
        const auto forward=flightDirection(),right=glm::normalize(glm::cross(forward,glm::vec3(0,0,1))),up=glm::cross(right,forward);
        auto c=rig.camera;c.position=projectile(time)+right*rig.mountOffset.x+up*rig.mountOffset.y+forward*rig.mountOffset.z;
        c.target=c.position+forward*4.f;c.up=up;return c;
    }
    auto c=rig.camera;if(rig.tracking){auto p=projectile(time);c.position=p+rig.trackingOffset;c.target=p-flightDirection()*1.5f;c.fovY=35;
        auto d=glm::normalize(c.target-c.position);if(glm::length(glm::cross(d,c.up))<1e-4f)throw std::runtime_error("Degenerate tracking camera");}return c;
}
MeshTimeline::CameraRig MeshTimeline::rigFrom(const nlohmann::json& spec)const{
    CameraRig rig;bool found=false;
    if(!spec.contains("camera")||!spec["camera"].is_string())throw std::runtime_error("additional camera requires a string \"camera\" id");
    for(const auto& c:base.cameras)if(c.id==spec["camera"].get<std::string>()){rig.camera=c;found=true;}
    if(!found)throw std::runtime_error("Unknown additional camera id: "+spec["camera"].get<std::string>());
    if(spec.contains("trackingCameraOffset")){rig.tracking=true;rig.trackingOffset=vector(spec.at("trackingCameraOffset"));if(glm::length(rig.trackingOffset)<4)throw std::runtime_error("Tracking camera offset too small");}
    if(spec.contains("cameraMount")){
        const auto& mount=spec.at("cameraMount");
        if(!mount.is_object()||mount.value("body",std::string())!="projectile")throw std::runtime_error("Unsupported camera mount body");
        if(rig.tracking)throw std::runtime_error("Body mount and tracking camera are mutually exclusive");
        rig.mountOffset=vector(mount.at("offsetMeters"));if(glm::length(rig.mountOffset)>3)throw std::runtime_error("Body camera offset exceeds3m");rig.mounted=true;
    }
    return rig;
}
nlohmann::json MeshTimeline::cameraMountDescription()const{
    return {{"body","projectile"},{"offsetMeters",{mountOffset.x,mountOffset.y,mountOffset.z}},
        {"axes","body right, up, forward; same fixed basis as rendered body"},
        {"poseSampling","each native causal shutter sample; camera.csv"},{"validBeforeSeconds",impact},
        {"postContact","no body-camera observations; display packaging may mark signal loss"},
        {"scope","camera rendering of existing authored fixed-orientation motion; no attitude or guidance dynamics"}};
}
MeshScene MeshTimeline::frame(double time,bool recordDiagnostics,std::vector<MeshPart>* parts){
    float age=float(time)-impact;MeshScene result=age<0?base:shell;const size_t appendStart=result.triangles.size();
    if(parts)parts->clear();
    // Records one rigid group of the triangles appended since `from` (instanced geometry only).
    auto part=[&](uint64_t key,size_t from,const glm::mat4& transform){if(parts)parts->push_back({key,uint32_t(from-appendStart),uint32_t(result.triangles.size()-from),transform});};
    auto pose=[](const glm::mat3& r,glm::vec3 center,glm::vec3 position){glm::mat4 m(r);m[3]=glm::vec4(position-r*center,1);return m;};
    if(age>=0){
        for(auto& t:result.triangles)if(t.ids.w&1)t.thermal.z=100*std::exp(-age/3.f);
        if(dynamics){
            const auto sampleStart=std::chrono::steady_clock::now();
            auto poses=dynamics->sample(std::max(0.,time-double(impact)));
            rigidSampleSeconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-sampleStart).count();
            for(size_t b=0;b<poses.size();b++){const size_t i=bodyChunk.empty()?b:bodyChunk[b];const size_t from=result.triangles.size();for(auto t:chunks[i].triangles){
                transform(t,glm::mat3_cast(poses[b].orientation),chunks[i].center,poses[b].position);float elapsed=age-chunks[i].delay;t.thermal.z=elapsed>0?65*std::exp(-elapsed/4.f):0;
                if(recordDiagnostics)for(auto p:{t.a,t.b,t.c}){float surface=support(glm::vec3(p));if(!std::isfinite(surface)){outsideSupportSamples++;continue;}double depth=std::max(0.f,surface-p.z);if(depth>maxPenetration){maxPenetration=depth;groundIssue={{"fragment",i},{"timeSeconds",time},{"pointMeters",{p.x,p.y,p.z}},{"centerMeters",{poses[b].position.x,poses[b].position.y,poses[b].position.z}}};}}result.triangles.push_back(t);
            }part(1000+i,from,pose(glm::mat3_cast(poses[b].orientation),chunks[i].center,poses[b].position));}
            return result;
        }
        if(event)return result; // impact-v1 without released blocks: intact blocks are already static in the shell
        for(size_t ci=0;ci<chunks.size();ci++){const auto& c=chunks[ci];const size_t from=result.triangles.size();float elapsed=age-c.delay,free=elapsed-c.exit;glm::vec3 position=c.center;glm::mat3 r(1);
            if(elapsed>0&&free<0){if(customExit){const glm::vec2 xy=glm::mix(glm::vec2(c.center),c.exitPoint,smooth(elapsed/c.exit));position.x=xy.x;position.y=xy.y;}else position.y=glm::mix(c.center.y,-2.8f,smooth(elapsed/c.exit));}
            if(free>=0){if(free>=c.contactTime){position=c.landing;r=glm::mat3_cast(c.rest);}
                else if(customExit){position=glm::vec3(c.exitPoint.x+c.velocity.x*free,c.exitPoint.y+c.velocity.y*free,c.center.z+c.velocity.z*free-4.905f*free*free);r=rotation(c,free);}
                else{position=c.center+glm::vec3(c.velocity.x*free,-2.8f-c.center.y+c.velocity.y*free,c.velocity.z*free-4.905f*free*free);r=rotation(c,free);}}
            for(auto t:c.triangles){transform(t,r,c.center,position);t.thermal.z=elapsed>0?65*std::exp(-elapsed/4.f):0;
                if(recordDiagnostics)for(auto v:{t.a,t.b,t.c}){float surface=support(glm::vec3(v));if(!std::isfinite(surface)){outsideSupportSamples++;continue;}maxPenetration=std::max(maxPenetration,double(std::max(0.f,surface-v.z)));}
                result.triangles.push_back(t);
            }
            part(1000+ci,from,pose(r,c.center,position));
            if(recordDiagnostics&&free>=c.contactTime){auto polygon=c.footprint;for(auto& p:polygon)p+=glm::vec2(position);float support=footprintSupport(polygon,false);if(std::isfinite(support))maxRestGap=std::max(maxRestGap,double(std::max(0.f,position.z+c.bottom-support)));else outsideSupportSamples++;}
        }
    }else{
        const auto pos=projectile(time),direction=flightDirection();const auto side=glm::normalize(glm::cross(direction,glm::vec3(0,0,1))),up=glm::cross(side,direction);
        if(projectileV2){
            auto add=[&](glm::vec3 a,glm::vec3 b,glm::vec3 c,uint32_t mat,float temperature){Triangle t{};t.a={a,1};t.b={b,1};t.c={c,1};auto n=glm::normalize(glm::cross(b-a,c-a));t.na=t.nb=t.nc={n,0};t.ids={mat,target,0,4};t.thermal={temperature,temperature,0,0};result.triangles.push_back(t);};
            for(int i=0;i<16;i++){
                float a=i*glm::two_pi<float>()/16,b=(i+1)*glm::two_pi<float>()/16;
                auto u=side*std::cos(a)+up*std::sin(a),v=side*std::cos(b)+up*std::sin(b);
                auto shoulder=pos-direction*.55f,rear=pos-direction*2.3f,nozzle=pos-direction*2.7f;
                add(pos,shoulder+.18f*u,shoulder+.18f*v,noseMaterial,noseK);
                add(shoulder+.18f*u,rear+.18f*u,rear+.18f*v,bodyMaterial,bodyK);add(shoulder+.18f*u,rear+.18f*v,shoulder+.18f*v,bodyMaterial,bodyK);
                add(rear+.18f*u,nozzle+.13f*u,nozzle+.13f*v,bodyMaterial,bodyK);add(rear+.18f*u,nozzle+.13f*v,rear+.18f*v,bodyMaterial,bodyK);
                add(nozzle,nozzle+.13f*v,nozzle+.13f*u,nozzleMaterial,nozzleK);
            }
            for(int i=0;i<4;i++){
                float angle=i*glm::half_pi<float>();auto radial=side*std::cos(angle)+up*std::sin(angle);
                auto a=pos-direction*1.85f+radial*.17f,b=pos-direction*2.65f+radial*.17f,c=pos-direction*2.60f+radial*.50f;
                add(a,b,c,finMaterial,finK); // Thin two-sided fins; no culling in the mesh renderer.
            }
            part(2,appendStart,pose(glm::mat3(1),glm::vec3(0),pos));
            return result;
        }
        auto add=[&](glm::vec3 a,glm::vec3 b,glm::vec3 c){Triangle t{};t.a={a,1};t.b={b,1};t.c={c,1};auto n=glm::normalize(glm::cross(b-a,c-a));t.na=t.nb=t.nc={n,0};t.ids={projectileMaterial,target,0,4};t.thermal={650,650,0,0};result.triangles.push_back(t);};
        for(int i=0;i<12;i++){float a=i*glm::two_pi<float>()/12,b=(i+1)*glm::two_pi<float>()/12;auto u=.13f*(side*std::cos(a)+up*std::sin(a)),v=.13f*(side*std::cos(b)+up*std::sin(b));auto tail=pos-direction*1.8f;add(pos+u,tail+u,tail+v);add(pos+u,tail+v,pos+v);add(pos+u,pos+v,pos+direction*.55f);add(tail+v,tail+u,tail);}
        part(2,appendStart,pose(glm::mat3(1),glm::vec3(0),pos));
    }
    return result;
}
nlohmann::json MeshTimeline::rigidStatistics()const{return dynamics?dynamics->statistics():Json::object();}
void MeshTimeline::flushRigidTrack(){if(dynamics)dynamics->flushTrack();}
nlohmann::json MeshTimeline::receipt()const{
    Json r;
    if(event){
        r={{"motionBackend","impact-v1"},{"fragmentCount",bodyChunk.size()},{"presetBlocks",chunks.size()},{"failedBlocks",event->failed},{"pulverisedBlocks",event->pulverised},{"intactBlocks",chunks.size()-event->failed},{"remainderTriangles",remainder.size()},
            {"structuralResponseComputed",false},{"damage","empirical failure of preset blocks (Kingery-Bulmash reflected impulse vs critical impulse); no structural analysis"},
            {"contactModel",dynamics?"native rigid box dynamics of released blocks against the static scene (intact blocks included); launch velocity and release time from the event model":"no block released"},
            {"maxGroundPenetrationM",maxPenetration},{"maximumGroundIssue",groundIssue},{"firstContact",firstContact},{"event","see event.json"},{"presentationTimeScale",timeScale}};
        if(dynamics)r["solver"]=dynamics->statistics();
    }
    else if(dynamics)r={{"motionBackend","rigid-v1"},{"fragmentCount",chunks.size()},{"remainderTriangles",remainder.size()},{"structuralResponseComputed",false},{"damage","authored closed-mesh partition; no predicted failure"},{"contactModel","native rigid box dynamics against actual static triangles and other fragments; friction, restitution and supplied release/launch data"},{"maxGroundPenetrationM",maxPenetration},{"maximumGroundIssue",groundIssue},{"firstContact",firstContact},{"solver",dynamics->statistics()}};
    else r={{"fragmentCount",chunks.size()},{"remainderTriangles",remainder.size()},{"structuralResponseComputed",false},{"damage","authored closed-mesh partition; no predicted failure"},{"contactModel","authored non-overlapping actual landing footprints, gravity after clearance, inelastic ground rest; no inter-fragment flight collisions"},{"maxGroundPenetrationM",maxPenetration},{"maxUnsupportedRestGapM",maxRestGap},{"settledOverlapPairs",overlapPairs}};
    r["flight"]={{"speedMetersPerSecond",flightSpeed},{"speedSource",event?"impactPhysics.projectile.speedMetersPerSecond (physical)":flightSpeedSupplied?"supplied flightSpeedMetersPerSecond":"derived from impactTime and path length"},{"model",event?"constant physical speed to first contact, estimated constant deceleration in the wall; fixed orientation; no guidance, gravity drop or drag":"constant-speed fixed-orientation translation; no guidance, gravity drop, drag or attitude dynamics; not a weapon performance value"}};
    r["breach"]=breachReceipt;
    r["effectOrientation"]={{"outwardAzimuthRadians",outwardAzimuth},{"source",dynamics?"horizontal part of the computed contact normal (default -Y when the hit surface is not a wall)":"default -Y facade"},{"scope","fireball, dust, heat pulse and their volumes; the breach and fragment table are not rotated"}};
    r["fragmentRelease"]=event?Json{{"mode","blast arrival"},{"note","each failed block is released at its Kingery-Bulmash arrival time"}}:releaseReceipt.is_null()?Json{{"mode","table"},{"note","preset pseudo-random release delays; independent of distance from the contact point"}}:releaseReceipt;
    if(!dynamics&&!event)r["fragmentExit"]=customExit?Json{{"source","supplied fragmentExit"},{"normal",{exitNormal.x,exitNormal.y}},{"planeOffsetMeters",exitOffset},{"note","authored exit plane dot(normal,xy)=planeOffset; launch directions in the fragment table are not rotated"}}
        :Json{{"source","default -Y facade"},{"normal",{0,-1}},{"planeOffsetMeters",2.8},{"note","canonical authored exit plane y=-2.8; supply sequence.fragmentExit for another facade"}};
    r["fragmentCensus"]={{"minimumDimensionMeters",minChunkDimension},{"sliversUnder5cm",sliverChunks},{"note","very thin low-mass slivers between tonne-scale blocks are the typical source of contact-solver speed artefacts"}};
    r["outsideGroundCoverageSamples"]=outsideSupportSamples;
    if(dynamics){r["freeFallSpeedBoundMetersPerSecond"]=speedBound;r["speedGuardLimitMetersPerSecond"]=speedGuard;}
    return r;
}
nlohmann::json MeshTimeline::snapshot(const std::filesystem::path& provenance)const{
    auto seq=base.input.at("sequence");for(size_t i=0;i<paths.size();i++){if(sha256File(paths[i])!=hashes[i])throw std::runtime_error("Sequence asset changed before snapshot");auto name="sequence_"+keys[i]+paths[i].extension().string();std::filesystem::copy_file(paths[i],provenance/name);seq[keys[i]]["path"]=name;}return seq;
}
}
