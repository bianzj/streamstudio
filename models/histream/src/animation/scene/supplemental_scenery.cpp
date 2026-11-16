#include "supplemental_scenery.h"
#include "animation/scene/path_text.h"
#include <cmath>
#include <fstream>
#include <stdexcept>
namespace animation {
using Json=nlohmann::json;
SupplementalScenery::SupplementalScenery(const Json& ref,const std::filesystem::path& folder,const MeshScene& base){
    path=std::filesystem::absolute(folder/pathFrom(ref.at("path").get<std::string>())).lexically_normal();digest=ref.at("sha256");
    if(sha256File(path)!=digest)throw std::runtime_error("Supplemental scenery SHA256 mismatch");
    scene=std::make_unique<MeshScene>(path);
    if(scene->receipt["sourceSha256"]!=digest)throw std::runtime_error("Supplemental scenery input changed during load");
    if(scene->input.contains("backgroundField")||scene->input.contains("sequence")||scene->input.contains("thermalPerturbations"))throw std::runtime_error("Supplemental scenery requires independent prescribed temperatures");
    if(scene->input["coordinates"]!=base.input["coordinates"]||std::abs(scene->wavelength-base.wavelength)>1e-5)throw std::runtime_error("Supplemental scenery coordinates/wavelength mismatch");
    for(const auto& material:scene->input["materials"]){if(material.contains("thinLeaf")&&!material["thinLeaf"].is_boolean())throw std::runtime_error("Supplemental scenery thinLeaf must be boolean");thinLeaves.push_back(material.value("thinLeaf",false));}
    for(const auto& triangle:scene->triangles)if(thinLeaves.at(triangle.ids.x))thinLeafTriangles++;
}
void SupplementalScenery::appendTo(MeshScene& background)const{
    uint32_t materials=uint32_t(background.materials.values.size()),instances=uint32_t(background.input["instances"].size()),assets=uint32_t(background.assets.size());
    for(size_t i=0;i<scene->materials.names.size();i++){background.materials.names.push_back("scenery_"+scene->materials.names[i]);background.materials.values.push_back(scene->materials.values[i]);}
    for(auto t:scene->triangles){t.ids.w=thinLeaves.at(t.ids.x)?24u:8u;t.ids.x+=materials;t.ids.y+=instances;t.ids.z+=assets;background.triangles.push_back(t);}
}
Json SupplementalScenery::snapshot(const std::filesystem::path& provenance)const{
    if(sha256File(path)!=digest)throw std::runtime_error("Supplemental scenery changed before snapshot");
    auto folder=provenance/"scenery";std::filesystem::create_directory(folder);auto replay=scene->input;
    for(size_t i=0;i<scene->assets.size();i++){
        if(sha256File(scene->assets[i])!=scene->receipt["assets"][i]["sha256"])throw std::runtime_error("Supplemental scenery asset changed");
        auto name="asset_"+std::to_string(i)+".obj";std::filesystem::copy_file(scene->assets[i],folder/name);
        if(sha256File(folder/name)!=scene->receipt["assets"][i]["sha256"])throw std::runtime_error("Supplemental scenery snapshot mismatch");
        replay["assets"][i]["path"]=name;
    }
    std::ofstream f(folder/"scene.json");f<<replay.dump(2)<<'\n';f.flush();if(!f)throw std::runtime_error("Cannot write supplemental scenery snapshot");f.close();
    return {{"enabled",true},{"path","scenery/scene.json"},{"sha256",sha256File(folder/"scene.json")}};
}
Json SupplementalScenery::description()const{
    Json result={{"enabled",true},{"sourceSha256",digest},{"triangleCount",scene->triangles.size()},{"instanceCount",scene->input["instances"].size()},{"assets",scene->receipt["assets"]},{"temperatureSource","prescribed supplemental materials"},{"nativeBackgroundBinding","original FacetEB scene/triangle coverage unchanged; supplemental scenery is outside that field"},{"participation","static RGB/IR visibility and shadows; excluded from rigid collisions and coarse transport obstacles"},{"provenance",scene->input.value("provenance",Json::object())}};
    if(thinLeafTriangles)result["thinLeafTaggedTriangles"]=thinLeafTriangles;
    return result;
}
}
