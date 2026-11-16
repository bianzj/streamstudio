#include "material_table.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace animation {
MaterialTable::MaterialTable(const nlohmann::json& input) {
    if(!input.is_array() || input.empty() || input.size()>65535)
        throw std::runtime_error("materials must be a nonempty array of at most 65535 entries");
    for(const auto& item:input) {
        const auto name=item.at("id").get<std::string>();
        if(name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos
            || std::find(names.begin(),names.end(),name)!=names.end())
            throw std::runtime_error("material id must be unique and contain only letters, digits, _ or -");
        const auto color=item.at("color").get<std::vector<float>>();
        const auto e=item.at("emissivity").get<float>(),t=item.at("temperatureK").get<float>();
        if(color.size()!=3 || !std::isfinite(e) || e<0 || e>1 || !std::isfinite(t) || t<150 || t>2000)
            throw std::runtime_error("material color/emissivity/temperature outside supported range");
        for(float c:color)if(!std::isfinite(c)||c<0||c>1)throw std::runtime_error("material color outside [0,1]");
        float rough=item.value("roughness",0.f),metal=item.value("metallic",0.f),ior=item.value("ior",0.f);
        if(!std::isfinite(rough)||rough<0||rough>1||!std::isfinite(metal)||metal<0||metal>1||!std::isfinite(ior)||ior<0||ior>3||(item.contains("roughness")&&rough<.02f)||(item.contains("ior")&&ior<1.01f))throw std::runtime_error("material roughness/metallic/ior outside supported range");
        names.push_back(name);values.push_back({{color[0],color[1],color[2],e},{t,rough,metal,ior}});
    }
}
uint32_t MaterialTable::index(const std::string& name) const {
    const auto i=std::find(names.begin(),names.end(),name);
    if(i==names.end())throw std::runtime_error("Missing material mapping: "+name);
    return uint32_t(i-names.begin());
}
}
