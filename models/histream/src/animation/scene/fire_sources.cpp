#include "fire_sources.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
namespace animation {
namespace {
using Json=nlohmann::json;
float number(const Json& j,const char* key,double fallback,double lo,double hi){double v=j.value(key,fallback);if(!std::isfinite(v)||v<lo||v>hi)throw std::runtime_error(std::string("sustained fire ")+key+" outside supported range");return float(v);}
}
FireSources::FireSources(const Json& j,glm::vec3 contact,glm::vec3 origin){
    if(!j.is_object()||(j.contains("enabled")&&!j["enabled"].is_boolean()))throw std::runtime_error("sustained fire options/enabled invalid");
    if(!j.value("enabled",false))return;
    const auto& list=j.at("sources");if(!list.is_array()||list.empty()||list.size()>8)throw std::runtime_error("sustained fire requires1..8sources");
    for(const auto& s:list){
        auto offset=s.at("offsetMeters").get<std::vector<float>>();if(offset.size()!=3)throw std::runtime_error("sustained fire offset requires3values");
        for(float v:offset)if(!std::isfinite(v))throw std::runtime_error("Nonfinite sustained fire offset");
        glm::vec3 p=contact+glm::vec3(offset[0],offset[1],offset[2]);
        float radius=number(s,"radiusMeters",1,.25,4),height=number(s,"heightMeters",3,.5,8);
        if(p.x-3*radius<origin.x||p.x+3*radius>origin.x+64||p.y-3*radius<origin.y||p.y+3*radius>origin.y+56||p.z<0||p.z+height>origin.z+56)throw std::runtime_error("sustained fire source outside transport domain");
        float onset=number(s,"onsetSeconds",.4,0,30),duration=number(s,"durationSeconds",30,.5,120),fade=number(s,"fadeSeconds",std::min(2.f,duration),.1,duration);
        const auto style=s.value("flameStyle",std::string("legacy"));int profile=style=="legacy"?0:style=="window-licking"?1:style=="window-billowing"?2:style=="ground-pool"?3:-1;
        if(profile<0)throw std::runtime_error("Unknown sustained fire flameStyle");int seed=0;
        if(profile){if(s.contains("seed")&&!s["seed"].is_number_integer())throw std::runtime_error("sustained fire seed must be integer");seed=int(number(s,"seed",12345+block.count.x*7919,1,999999));}
        else if(s.contains("seed"))throw std::runtime_error("sustained fire seed requires named flameStyle");
        float growth=number(s,"growthSeconds",0,0,25.5);if(growth>0&&growth<.1f)throw std::runtime_error("sustained fire growthSeconds must be0or>=0.1");
        // Header.y/z each hold four unsigned byte codes. Keep the48/400byte ABI
        // and the full source seed. Use memcpy for the signed high-bit slots.
        uint32_t code=uint32_t(std::lround(double(growth)*10.));int index=block.count.x,slot=1+index/4;uint32_t bits=0;
        std::memcpy(&bits,&block.count[slot],sizeof(bits));bits|=code<<(8*(index%4));std::memcpy(&block.count[slot],&bits,sizeof(bits));
        block.sources[size_t(block.count.x++)]={{p,radius},{onset,duration,fade,number(s,"tracerRate",1.5,0,10)},
            {height,number(s,"temperatureK",1100,500,1400),number(s,"heatKelvinPerSecond",120,0,300),float(profile?profile*1000000+seed:0)}};
    }
}
Json FireSources::description()const{
    Json sources=Json::array();for(int i=0;i<block.count.x;i++){const auto& f=block.sources[size_t(i)];sources.push_back({{"positionMeters",{f.positionRadius.x,f.positionRadius.y,f.positionRadius.z}},{"radiusMeters",f.positionRadius.w},{"heightMeters",f.flame.x},{"temperatureK",f.flame.y},{"heatKelvinPerSecond",f.flame.z},{"onsetAfterContactSeconds",f.timing.x},{"durationSeconds",f.timing.y},{"fadeSeconds",f.timing.z},{"tracerRate",f.timing.w}});}
    for(int i=0;i<block.count.x;i++)if(block.sources[size_t(i)].flame.w>0){int packed=int(block.sources[size_t(i)].flame.w),profile=packed/1000000;sources[size_t(i)]["flameStyle"]=profile==1?"window-licking":profile==2?"window-billowing":"ground-pool";sources[size_t(i)]["seed"]=packed%1000000;sources[size_t(i)]["smokeRelease"]="broadened upper-flame supplied tracer region";}
    for(int i=0;i<block.count.x;i++){uint32_t bits=0;std::memcpy(&bits,&block.count[1+i/4],sizeof(bits));uint32_t code=(bits>>(8*(i%4)))&255u;if(code){sources[size_t(i)]["growthSeconds"]=double(code)/10.;sources[size_t(i)]["growthModel"]={{"curve","smoothstep from individual ignition"},{"initialRadiusFraction",.20},{"initialHeightFraction",.15},{"initialHeatTracerRateFraction",.03},{"resolutionSeconds",.1},{"prediction",false}};}}
    return {{"enabled",block.count.x>0},{"sources",sources},{"timing","prescribed ignition delay relative to existing contact; smooth ramp/finite duration"},{"model","volumetric flame/ember appearance, radiance-space IR mixing, supplied continuous heat/tracer into existing low-speed buoyant transport"},{"limitations","no ignition prediction, fuel/oxygen/chemistry, flame propagation or vegetation burning; supplied temperatures and rates are illustrative"}};
}
}
