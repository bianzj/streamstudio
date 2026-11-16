#include "background_field.h"
#include "animation/scene/mesh_scene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <map>
#include <stdexcept>

namespace animation {
namespace {
using Json=nlohmann::json;
std::string read(const std::filesystem::path& path){
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f)throw std::runtime_error("Cannot open background field/source: "+pathText(path));
    auto n=f.tellg();if(n<0||n>256*1024*1024)throw std::runtime_error("Background source exceeds 256 MiB limit");
    std::string bytes(size_t(n),'\0');f.seekg(0);f.read(bytes.data(),n);
    if(!f)throw std::runtime_error("Cannot read background field/source");return bytes;
}
std::string checked(const std::filesystem::path& path,const std::string& hash){
    auto bytes=read(path);if(sha256Bytes(bytes)!=hash)throw std::runtime_error("Background field/source SHA256 mismatch: "+pathText(path));return bytes;
}
bool validTime(const std::string& s){
    if(!std::regex_match(s,std::regex(R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(Z|[+-][0-9]{2}:[0-9]{2})$)")))return false;
    int y=std::stoi(s.substr(0,4)),m=std::stoi(s.substr(5,2)),d=std::stoi(s.substr(8,2));
    const int days[12]={31,28+(y%4==0&&(y%100!=0||y%400==0)),31,30,31,30,31,31,30,31,30,31};
    return m>=1&&m<=12&&d>=1&&d<=days[m-1]&&std::stoi(s.substr(11,2))<24&&std::stoi(s.substr(14,2))<60&&std::stoi(s.substr(17,2))<60
        &&(s.size()==20||(std::stoi(s.substr(20,2))<=14&&std::stoi(s.substr(23,2))<60));
}
}

BackgroundField::BackgroundField(MeshScene& scene,const std::filesystem::path& directory){
    const auto& ref=scene.input.at("backgroundField");
    const auto path=std::filesystem::absolute(directory/pathFrom(ref.at("path").get<std::string>())).lexically_normal();
    metadata=Json::parse(checked(path,ref.at("sha256").get<std::string>()));
    if(metadata.at("kind")!="histream-background-field"||metadata.at("schemaVersion")!=1)throw std::runtime_error("Unsupported background field schema");
    if(metadata.at("temperatureUnits")!="K")throw std::runtime_error("Background temperature units must be K");
    if(metadata.at("coordinates")!=scene.input.at("coordinates"))throw std::runtime_error("Background coordinates disagree with mesh scene");
    timestamp=metadata.at("timestamp").get<std::string>();
    if(!validTime(timestamp)||timestamp!=ref.at("expectedTimestamp").get<std::string>()||metadata.at("timeConvention")!="native-node-instantaneous-state")
        throw std::runtime_error("Background timestamp/time convention does not match requested scene time");
    const auto& solver=metadata.at("solver");
    if(solver.at("engine")!="HiStream FacetEB"||solver.at("revision").get<std::string>().empty())throw std::runtime_error("Missing background solver source version");
    if(solver.at("converged")!=true)throw std::runtime_error("Background native node is not converged");
    Json assets=Json::array(),ids=Json::array(),emissivities=Json::array();
    for(const auto& a:scene.receipt.at("assets"))assets.push_back({{"id",a.at("id")},{"sha256",a.at("sha256")}});
    for(size_t i=0;i<scene.materials.names.size();i++){ids.push_back(scene.materials.names[i]);emissivities.push_back(scene.input["materials"][i].at("emissivity"));}
    const Json binding={{"assets",assets},{"instances",scene.input.at("instances")},{"materialIds",ids},{"emissivities",emissivities}};
    if(metadata.at("sceneBinding")!=binding)throw std::runtime_error("Background scene/material/instance binding mismatch");
    const auto& field=metadata.at("field");
    if(field.at("recordFloats")!=12||field.at("layout")!="triangle-interleaved-a-b-c-material-frontK-backK")throw std::runtime_error("Unsupported background field layout");
    if(field.at("triangleCount")!=scene.triangles.size())throw std::runtime_error("Background field coverage does not match triangle count");
    records={path.parent_path()/pathFrom(field.at("path").get<std::string>()),field.at("sha256").get<std::string>(),"records"};
    auto bytes=checked(records.path,records.sha256);
    if(bytes.size()!=scene.triangles.size()*12*sizeof(float))throw std::runtime_error("Incomplete background coverage/record bytes");
    constexpr std::array<const char*,10> required{{"project","forcing","history","geometry","state","runIndex","engine","solverSourceReceipt","mapping","runReceipt"}};
    std::set<std::string> seen;
    std::map<std::string,File> sourceFiles;
    for(const auto& source:metadata.at("sources")){
        auto role=source.at("role").get<std::string>();
        if(std::find(required.begin(),required.end(),role)==required.end()||!seen.insert(role).second)throw std::runtime_error("Unknown/duplicate background source role");
        File file{path.parent_path()/pathFrom(source.at("path").get<std::string>()),source.at("sha256").get<std::string>(),role};checked(file.path,file.sha256);sources.push_back(file);sourceFiles.emplace(role,file);
    }
    if(seen.size()!=required.size())throw std::runtime_error("Missing background forcing/history/source records");
    auto jsonSource=[&](const std::string& role){const auto& file=sourceFiles.at(role);return Json::parse(checked(file.path,file.sha256));};
    const auto nativeReceipt=jsonSource("runReceipt"),nativeIndex=jsonSource("runIndex"),project=jsonSource("project");
    if(nativeReceipt.at("status")!="completed"||nativeReceipt.at("returncode")!=0||nativeReceipt.at("inputsUnchanged")!=true||nativeReceipt.at("engineSha256")!=sourceFiles.at("engine").sha256)
        throw std::runtime_error("Background pre-solve source receipt is not completed/unchanged");
    for(const char* role:{"project","forcing","history","mapping"})if(nativeReceipt.at("inputs").at(role).at("sha256")!=sourceFiles.at(role).sha256)
        throw std::runtime_error(std::string("Background source changed since native solve: ")+role);
    if(nativeIndex.at("status")!="completed")throw std::runtime_error("Background native run is not completed/converged");
    const auto nodeId=solver.at("node");Json node;
    for(const auto& candidate:nativeIndex.at("completed"))if(candidate.at("node")==nodeId){node=candidate;break;}
    if(node.is_null()||node.at("converged")!=true||node.at("energyFailingFacetFraction").get<double>()>=.001)
        throw std::runtime_error("Background source node is absent or not converged");
    const auto& meteo=project.at("configuration").at("meteo");int year=std::stoi(timestamp.substr(0,4)),month=std::stoi(timestamp.substr(5,2));
    const int days[12]={31,28+(year%4==0&&(year%100!=0||year%400==0)),31,30,31,30,31,31,30,31,30,31};
    double julian=std::stoi(timestamp.substr(8,2));for(int i=0;i<month-1;i++)julian+=days[i];
    julian+=(std::stoi(timestamp.substr(11,2))*3600+std::stoi(timestamp.substr(14,2))*60+std::stoi(timestamp.substr(17,2)))/86400.;
    double timezone=timestamp.size()==20?0:(std::stoi(timestamp.substr(20,2))+std::stoi(timestamp.substr(23,2))/60.)*(timestamp[19]=='-'?-1:1);
    if(meteo.at("year")!=year||std::abs(meteo.at("timezone").get<double>()-timezone)>1e-6||std::abs(node.at("julianTime").get<double>()-julian)*86400>.51)
        throw std::runtime_error("Background timestamp disagrees with native node/project");
    double maxError=0;float minK=2000,maxK=150;
    for(size_t i=0;i<scene.triangles.size();i++){
        float record[12];std::memcpy(record,bytes.data()+i*sizeof(record),sizeof(record));
        auto& tri=scene.triangles[i];const glm::vec4 vertices[3]={tri.a,tri.b,tri.c};
        for(int v=0;v<3;v++)for(int d=0;d<3;d++){
            const float error=std::abs(record[v*3+d]-vertices[v][d]);
            if(!std::isfinite(error)||error>1e-4f)throw std::runtime_error("Background triangle geometry mismatch at index "+std::to_string(i));
            maxError=std::max(maxError,double(error));
        }
        if(record[9]!=float(tri.ids.x))throw std::runtime_error("Background triangle material binding mismatch");
        for(int k:{10,11})if(!std::isfinite(record[k])||record[k]<150||record[k]>2000)throw std::runtime_error("Invalid background temperature");
        tri.thermal.x=record[10];tri.thermal.y=record[11];
        minK=std::min({minK,record[10],record[11]});maxK=std::max({maxK,record[10],record[11]});
    }
    const auto& sky=metadata.at("sky");double lw=sky.at("longwaveWm2").get<double>(),skyK=sky.at("equivalentTemperatureK").get<double>();
    if(!std::isfinite(lw)||lw<=0||!std::isfinite(skyK)||skyK<150||skyK>500||std::abs(std::pow(lw/5.670374419e-8,.25)-skyK)>.01)
        throw std::runtime_error("Background sky longwave/temperature mismatch");
    const auto sun=metadata.at("sunDirectionWorld").get<std::vector<float>>();
    if(sun.size()!=3)throw std::runtime_error("Invalid background sun direction");
    scene.sunDirection={sun[0],sun[1],sun[2]};
    if(!std::isfinite(glm::length(scene.sunDirection))||glm::length(scene.sunDirection)<1e-6)throw std::runtime_error("Invalid background sun direction");
    scene.skyK=float(skyK);
    scene.receipt["temperatureSource"]="immutable native FacetEB per-triangle front/back background plus supplied perturbation once";
    scene.receipt["backgroundField"]={{"timestamp",timestamp},{"temperatureUnits","K"},{"coverageFraction",1.0},{"triangleCount",scene.triangles.size()},
        {"geometryMappingMaxErrorM",maxError},{"temperatureMinK",minK},{"temperatureMaxK",maxK},{"sourceMetadataSha256",ref.at("sha256")},
        {"solver",solver},{"sources",metadata.at("sources")},{"interpolation","none; exact triangle correspondence"}};
}

nlohmann::json BackgroundField::snapshot(const std::filesystem::path& provenance) const{
    const auto folder=provenance/"background";std::filesystem::create_directory(folder);auto replay=metadata;
    auto copy=[&](const File& file,const std::string& name){
        const auto bytes=checked(file.path,file.sha256);std::ofstream f(folder/name,std::ios::binary);f.write(bytes.data(),bytes.size());f.flush();
        if(!f)throw std::runtime_error("Cannot snapshot background source");
    };
    copy(records,"triangles.f32");replay["field"]["path"]="triangles.f32";
    for(size_t i=0;i<sources.size();i++){auto name=sources[i].role+".source";copy(sources[i],name);replay["sources"][i]["path"]=name;}
    std::ofstream f(folder/"field.json");f<<replay.dump(2)<<'\n';f.flush();if(!f)throw std::runtime_error("Cannot snapshot background metadata");f.close();
    return {{"path","background/field.json"},{"sha256",sha256File(folder/"field.json")},{"expectedTimestamp",timestamp}};
}
}
