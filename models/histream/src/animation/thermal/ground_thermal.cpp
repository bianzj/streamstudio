#include "ground_thermal.h"
#include "animation/scene/path_text.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
namespace animation {namespace {
using Json=nlohmann::json;
double number(const Json& j,const char* key,double low,double high){double x=j.at(key);if(!std::isfinite(x)||x<low||x>high)throw std::runtime_error(std::string("groundThermal invalid ")+key);return x;}
int integer(const Json& j,const char* key,int low,int high){if(!j.at(key).is_number_integer())throw std::runtime_error(std::string("groundThermal integer required: ")+key);return int(number(j,key,low,high));}
glm::vec3 vector(const Json& j){auto x=j.get<std::vector<float>>();if(x.size()!=3||!std::isfinite(x[0])||!std::isfinite(x[1])||!std::isfinite(x[2]))throw std::runtime_error("groundThermal invalid sun");glm::vec3 v(x[0],x[1],x[2]);float len=glm::length(v);if(!std::isfinite(len)||len<.1)throw std::runtime_error("groundThermal invalid sun length");return v/len;}
struct Header{glm::ivec4 size;glm::vec4 domain,settings,control;};
struct GMaterial{glm::vec4 energy{0},flags{0};};
struct Cell{glm::vec4 point,sky,emission,solar,state;};
struct Step{glm::vec4 sun,weather,time;};
static_assert(sizeof(Header)==64&&sizeof(GMaterial)==32&&sizeof(Cell)==80&&sizeof(Step)==48);
void writeJson(const std::filesystem::path& p,const Json& j){std::ofstream f(p);f<<j.dump(2)<<'\n';if(!f)throw std::runtime_error("Cannot write groundThermal receipt");}
}
GroundThermalOptions GroundThermalOptions::load(const Json& ref,const std::filesystem::path& folder){
    GroundThermalOptions o;o.path=std::filesystem::absolute(folder/pathFrom(ref.at("path").get<std::string>())).lexically_normal();o.digest=ref.at("sha256");
    if(sha256File(o.path)!=o.digest)throw std::runtime_error("groundThermal input SHA256 mismatch");std::ifstream f(o.path);f>>o.input;const auto& j=o.input;
    if(j.at("kind")!="histream-ground-shadow-delta"||j.at("schemaVersion")!=1)throw std::runtime_error("Unsupported groundThermal input");
    if(j.value("temperatureMapping",std::string("absolute-local-ground"))!="absolute-local-ground")throw std::runtime_error("Unsupported groundThermal temperature mapping");
    int nx=integer(j,"width",2,2048),ny=integer(j,"height",2,2048);if(size_t(nx)*ny>1048576)throw std::runtime_error("groundThermal grid budget exceeded");
    auto origin=j.at("originMeters").get<std::vector<double>>();if(origin.size()!=2||!std::isfinite(origin[0])||!std::isfinite(origin[1])||std::abs(origin[0])+std::abs(origin[1])>100000)throw std::runtime_error("Invalid groundThermal origin");
    number(j,"cellMeters",.1,10);number(j,"maxReceiverZMeters",-100,1000);number(j,"initialK",220,360);number(j,"deepK",220,360);double dt=number(j,"stepSeconds",1,120),shadow=number(j,"shadowIntervalSeconds",dt,1800);integer(j,"viewSamples",8,64);number(j,"directFraction",0,1);
    const auto& history=j.at("history");if(!history.is_array()||history.size()<2||history.size()>10000)throw std::runtime_error("Invalid groundThermal history");double last=-1;
    glm::vec3 lastSun(0);for(const auto& s:history){double t=number(s,"seconds",0,604800);if(t<=last)throw std::runtime_error("groundThermal history not strictly ordered");auto sun=vector(s.at("sun"));if(last>=0&&glm::dot(lastSun,sun)<-.9f)throw std::runtime_error("groundThermal solar history too sparse for vector interpolation");lastSun=sun;last=t;number(s,"airK",200,350);number(s,"wind",0,40);number(s,"shortwave",0,1500);number(s,"longwave",100,800);}
    if(history[0]["seconds"].get<double>()!=0||last<=0||size_t(nx)*ny*std::ceil(last/dt)>3e9||size_t(nx)*ny*(8*std::ceil(last/shadow)+8*j["viewSamples"].get<int>())>5e9)throw std::runtime_error("groundThermal history/work budget invalid");
    if(!j.at("receiverMaterials").is_object()||j["receiverMaterials"].empty()||!j.at("canopyMaterials").is_array())throw std::runtime_error("Invalid groundThermal materials");
    for(auto it=j["receiverMaterials"].begin();it!=j["receiverMaterials"].end();++it){number(it.value(),"albedo",0,1);number(it.value(),"heatCapacity",100,1e7);number(it.value(),"groundConductance",0,100);}
    for(const auto& s:j.at("sourceFiles")){auto p=std::filesystem::absolute(o.path.parent_path()/pathFrom(s.at("path").get<std::string>())).lexically_normal();if(sha256File(p)!=s.at("sha256").get<std::string>())throw std::runtime_error("groundThermal forcing/source SHA256 mismatch");}
    return o;
}
struct GroundThermal::Impl{
    HostBuffer cells,params;Json receipt;
    Impl(AppSetting& app,size_t n,size_t bytes):cells(app,n*sizeof(Cell)),params(app,bytes){}
};
GroundThermal::GroundThermal(AppSetting& app,const std::filesystem::path& shader,const GroundThermalOptions& o,const MeshScene& base,const MeshScene& background,VkAccelerationStructureKHR baseAS,VkDescriptorBufferInfo baseTriangles,VkAccelerationStructureKHR bgAS,VkDescriptorBufferInfo bgTriangles,const std::filesystem::path& out){
    const auto& j=o.input;int nx=j["width"],ny=j["height"];size_t n=size_t(nx)*ny;auto origin=j["originMeters"].get<std::vector<float>>();float top=1;
    for(const auto* scene:{&base,&background})for(const auto& t:scene->triangles)top=std::max({top,t.a.z+2,t.b.z+2,t.c.z+2});
    Header h{{nx,ny,int(background.materials.names.size()),0},{origin[0],origin[1],j["cellMeters"].get<float>(),top},{j["maxReceiverZMeters"].get<float>(),j["initialK"].get<float>(),j["deepK"].get<float>(),0},{j["directFraction"].get<float>(),j["viewSamples"].get<float>(),0,0}};
    std::vector<GMaterial> mats(background.materials.names.size());std::set<std::string> found;
    auto canopies=j["canopyMaterials"].get<std::vector<std::string>>();
    for(size_t i=0;i<mats.size();i++){
        const auto& name=background.materials.names[i];auto& g=mats[i];g.energy.y=background.materials.values[i].colorEmissivity.w;
        if(std::find(canopies.begin(),canopies.end(),name)!=canopies.end()){g.flags.x=1;found.insert(name);}
        if(j["receiverMaterials"].contains(name)){const auto& p=j["receiverMaterials"][name];g.energy.x=p["albedo"].get<float>();g.energy.z=p["heatCapacity"].get<float>();g.energy.w=p["groundConductance"].get<float>();g.flags.y=1;found.insert(name);}
    }
    for(const auto& name:canopies)if(!found.count(name))throw std::runtime_error("groundThermal unknown canopy material: "+name);
    for(auto it=j["receiverMaterials"].begin();it!=j["receiverMaterials"].end();++it)if(!found.count(it.key()))throw std::runtime_error("groundThermal unknown receiver material: "+it.key());
    for(const auto& g:mats)if(g.flags.x>.5f&&g.flags.y>.5f)throw std::runtime_error("groundThermal material cannot be both canopy and receiver");
    for(const auto& t:base.triangles)if(mats.at(t.ids.x).flags.x>.5f)throw std::runtime_error("groundThermal canopy occluders must be supplemental geometry");
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(app.m_context.m_physicalDevice,&properties);if(n*sizeof(Cell)>properties.limits.maxStorageBufferRange)throw std::runtime_error("groundThermal grid exceeds hardware storage-buffer range");
    m=std::make_unique<Impl>(app,n,sizeof(Header)+mats.size()*sizeof(GMaterial));std::memcpy(m->params.data(),&h,sizeof(h));std::memcpy(static_cast<char*>(m->params.data())+sizeof(h),mats.data(),mats.size()*sizeof(GMaterial));
    ComputePass pass(app,shader,{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},sizeof(Step));
    pass.bind(0,baseAS);pass.bind(1,baseTriangles);pass.bind(2,bgAS);pass.bind(3,bgTriangles);pass.bind(4,m->params.info());pass.bind(5,m->cells.info());
    auto evaluate=[&](double t){const auto& values=j["history"];size_t k=0;while(k+1<values.size()&&values[k+1]["seconds"].get<double>()<t)k++;size_t b=std::min(k+1,values.size()-1);const auto& a=values[k];const auto& z=values[b];double w=b==k?0:std::clamp((t-a["seconds"].get<double>())/(z["seconds"].get<double>()-a["seconds"].get<double>()),0.,1.);
        auto mix=[&](const char* key){return float((1-w)*a[key].get<double>()+w*z[key].get<double>());};glm::vec3 sun=glm::normalize(vector(a["sun"])*float(1-w)+vector(z["sun"])*float(w));return Step{{sun,0},{mix("airK"),mix("wind"),mix("shortwave"),mix("longwave")},{0,0,0,0}};};
    auto dispatch=[&](Step s,int phase,double dt){s.time={float(dt),float(phase),0,0};pass.dispatch(&s,(nx+7)/8,(ny+7)/8);};
    std::cout<<"GROUND_INIT\t"<<nx<<'x'<<ny<<std::endl;dispatch(evaluate(0),0,1);
    double end=j["history"].back()["seconds"],dt=j["stepSeconds"],interval=j["shadowIntervalSeconds"],nextShadow=0;size_t steps=0;
    for(double t=0;t<end-1e-7;){double d=std::min(dt,end-t);auto p=evaluate(t+d*.5);if(t+1e-7>=nextShadow){dispatch(p,1,d);nextShadow=t+interval;}dispatch(p,2,d);t+=d;steps++;if(steps%360==0)std::cout<<"GROUND_HISTORY\t"<<t/3600<<"h"<<std::endl;}
    dispatch(evaluate(end),1,1);std::filesystem::create_directories(out);pass.snapshot(out/"ground_thermal.comp.spv");
    if(sha256File(o.path)!=o.digest)throw std::runtime_error("groundThermal inputs changed during solve");std::filesystem::copy_file(o.path,out/"inputs.json");
    // Preserve the original input receipt, then make separate relocatable replay
    // parameters. Reuse equal-hash background snapshots, copy custom sources.
    auto replayInput=j;size_t sourceIndex=0;
    for(auto& s:replayInput["sourceFiles"]){
        auto src=std::filesystem::absolute(o.path.parent_path()/pathFrom(s["path"].get<std::string>())).lexically_normal();auto expected=s["sha256"].get<std::string>();if(sha256File(src)!=expected)throw std::runtime_error("groundThermal source changed during solve");
        const auto role=s["role"].get<std::string>();auto dest=out.parent_path()/"provenance/background"/(role=="background"?"field.json":role+".source");
        if(!std::filesystem::is_regular_file(dest)||sha256File(dest)!=expected){std::filesystem::create_directory(out/"sources");dest=out/"sources"/("source_"+std::to_string(sourceIndex)+".bin");std::filesystem::copy_file(src,dest);}
        s["path"]=pathText(std::filesystem::relative(dest,out));s["sha256"]=sha256File(dest);sourceIndex++;
    }writeJson(out/"replay-inputs.json",replayInput);
    auto* cells=static_cast<Cell*>(m->cells.data());size_t valid=0,shaded=0;double minDelta=1e9,maxDelta=-1e9,sum=0,maxResidual=0;
    std::ofstream csv(out/"cells.csv");csv<<"x_m,y_m,z_m,material,baseline_K,local_without_K,local_with_K,delta_K,rendered_K,sun_without,sun_with,sky_without,sky_with,residual_W_m2\n"<<std::setprecision(10);
    for(size_t i=0;i<n;i++){const auto& c=cells[i];if(c.point.w<0)continue;for(int k=0;k<4;k++)if(!std::isfinite(c.state[k]))throw std::runtime_error("groundThermal nonfinite cell");if(c.state.x<150||c.state.x>500||c.state.y<150||c.state.y>500)throw std::runtime_error("groundThermal invalid temperature");
        valid++;if(c.solar.y<c.solar.x-.1f)shaded++;minDelta=std::min(minDelta,double(c.state.z));maxDelta=std::max(maxDelta,double(c.state.z));sum+=c.state.z;maxResidual=std::max(maxResidual,std::abs(double(c.state.w)));
        csv<<c.point.x<<','<<c.point.y<<','<<c.point.z<<','<<int(c.point.w)<<','<<c.emission.z<<','<<c.state.x<<','<<c.state.y<<','<<c.state.z<<','<<c.state.y<<','<<c.solar.x<<','<<c.solar.y<<','<<c.sky.x<<','<<c.sky.y<<','<<c.state.w<<'\n';
    }csv.close();if(!valid||maxResidual>=2)throw std::runtime_error("groundThermal no receivers or local EB residual>=2W/m2");
    std::ofstream raw(out/"cells.f32",std::ios::binary);raw.write(static_cast<const char*>(m->cells.data()),n*sizeof(Cell));raw.close();std::ofstream par(out/"parameters.bin",std::ios::binary);par.write(static_cast<const char*>(m->params.data()),sizeof(Header)+mats.size()*sizeof(GMaterial));par.close();
    m->receipt={{"enabled",true},{"model","paired-local-ground-energy-balance"},{"temperatureMapping","absolute local with-canopy state before existing impact perturbation"},{"residualScope","last implicit timestep, all valid receivers"},{"backend","HiStream Vulkan GPU ray queries and implicit surface energy balance"},{"gridPixels",{nx,ny}},{"cellMeters",h.domain.z},{"validReceivers",valid},{"currentCanopyShadedReceivers",shaded},{"historySeconds",end},{"stepSeconds",dt},{"steps",steps},{"shadowIntervalSeconds",interval},{"viewSamples",j["viewSamples"]},{"minDeltaK",minDelta},{"maxDeltaK",maxDelta},{"meanDeltaK",sum/valid},{"maxResidualWm2",maxResidual},{"inputSha256",o.digest},{"baseSceneSha256",base.receipt["sourceSha256"]},{"materialNames",background.materials.names},{"fieldRecordFloats",20},{"fieldSha256",sha256File(out/"cells.f32")},{"cameraDisplayOnly",false},{"nativeBaselineModified",false},{"fullyCoupledFacetEB",false},{"fieldValidation",false},{"scope","actual mesh occlusion; local differential dry surface EB; fixed current occluder temperatures, no leaf EB/ET, multi-bounce or atmosphere feedback"}};
    writeJson(out/"verification.json",m->receipt);std::cout<<"GROUND_COMPLETE\tminDeltaK="<<minDelta<<"\tmaxResidual="<<maxResidual<<std::endl;
}
GroundThermal::~GroundThermal()=default;
VkDescriptorBufferInfo GroundThermal::field()const{return m->cells.info();}
VkDescriptorBufferInfo GroundThermal::parameters()const{return m->params.info();}
nlohmann::json GroundThermal::description()const{return m->receipt;}
}
