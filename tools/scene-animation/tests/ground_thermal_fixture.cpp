#define TINYOBJLOADER_IMPLEMENTATION
#include "thirdparty/tiny_obj_loader.h"
#include "animation/thermal/ground_thermal.h"
#include "animation/bridge/histream_mesh_adapter.h"
#include <iostream>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using namespace animation;
struct Runtime{AppSetting app;bool started=false;~Runtime(){if(started)app.destroy();}};
int main(int argc,char** argv){try{
    if(argc!=6)throw std::runtime_error("Usage:histream_ground_tests BASE_SCENE BACKGROUND_SCENE PARAMS SHADER NEW_OUTPUT");
    if(std::filesystem::exists(argv[5]))throw std::runtime_error("Fixture output exists");
    MeshScene base(argv[1]),background(argv[2]);auto opts=GroundThermalOptions::load({{"path",std::filesystem::absolute(argv[3]).string()},{"sha256",sha256File(argv[3])}},std::filesystem::current_path());
    Runtime r;r.started=true;r.app.init(0,ComputeBackend::Gpu);HistreamMeshAdapter a(r.app,base),b(r.app,background);
    GroundThermal field(r.app,argv[4],opts,base,background,a.tlas(),a.triangles(),b.tlas(),b.triangles(),argv[5]);std::cout<<field.description().dump(2)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
