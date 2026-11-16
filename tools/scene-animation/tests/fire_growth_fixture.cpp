#include "animation/scene/fire_sources.h"
#include "animation/render/compute_pass.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using namespace animation;
struct Runtime{AppSetting app;bool started=false;~Runtime(){if(started)app.destroy();}};
struct Result{FireSource source;glm::vec4 schedule;};static_assert(sizeof(Result)==64);
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char** argv){try{
    check(argc==3,"Usage:histream_fire_growth_tests PROBE_SPV NEW_OUTPUT");std::filesystem::path out=argv[2];check(!std::filesystem::exists(out),"Output exists");std::filesystem::create_directories(out);
    double durations[]={.1,2.5,5,12.8,6.5,9,20,25.5};nlohmann::json sources=nlohmann::json::array();
    for(int i=0;i<8;i++)sources.push_back({{"offsetMeters",{0,0,0}},{"radiusMeters",1},{"heightMeters",3},{"onsetSeconds",i*.3},{"heatKelvinPerSecond",120},{"tracerRate",2},{"growthSeconds",durations[i]}});
    FireSources fires({{"enabled",true},{"sources",sources}},{10,0,5},{-22,-28,0});
    Runtime r;r.started=true;r.app.init(0,ComputeBackend::Gpu);HostBuffer input(r.app,sizeof(FireBlock)),output(r.app,8*sizeof(Result));
    ComputePass probe(r.app,argv[1],std::vector<VkDescriptorType>(2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),16);probe.bind(0,input.info());probe.bind(1,output.info());probe.snapshot(out/"probe.comp.spv");
    double maxError=0;int checks=0;
    for(int mode=0;mode<2;mode++){
        auto block=fires.block;if(mode)block.count.y=block.count.z=0;std::memcpy(input.data(),&block,sizeof(block));
        for(float age:{-1.f,0.f,.15f,.5f,2.f,10.f,40.f}){
            glm::vec4 push(age,0,0,0);probe.dispatch(&push,1,1);auto* result=static_cast<Result*>(output.data());
            for(int i=0;i<8;i++){
                const auto& f=block.sources[size_t(i)];double x=std::clamp((double(age)-f.timing.x)/durations[i],0.,1.),g=mode?1:x*x*(3-2*x),rate=mode?1:.03+.97*g;
                double expected[]={g,rate,mode?0:durations[i],f.positionRadius.w*(mode?1:.2+.8*g),f.flame.x*(mode?1:.15+.85*g),f.flame.z*rate,f.timing.w*rate};
                double got[]={result[i].schedule.x,result[i].schedule.y,result[i].schedule.w,result[i].source.positionRadius.w,result[i].source.flame.x,result[i].source.flame.z,result[i].source.timing.w};
                for(int k=0;k<7;k++){maxError=std::max(maxError,std::abs(got[k]-expected[k]));check(std::abs(got[k]-expected[k])<5e-5,"GPU growth differs from CPU reference");checks++;}
                if(age<=f.timing.x||age>=f.timing.x+f.timing.y)check(result[i].schedule.z==0,"Fire active outside ignition/lifetime gate");
                if(mode)check(std::memcmp(&result[i].source,&f,sizeof(f))==0,"Zero duration changed original packet");
            }
        }
    }
    nlohmann::json receipt={{"status","passed"},{"scalarChecks",checks},{"maxGpuError",maxError},{"sourceCount",8},{"signedHeaderSources",{3,7}},{"independentOnsets",true},{"zeroDurationPacketExact",true},{"sourceBytes",48},{"blockBytes",400}};
    std::ofstream(out/"verification.json")<<receipt.dump(2)<<'\n';std::cout<<receipt.dump(2)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
