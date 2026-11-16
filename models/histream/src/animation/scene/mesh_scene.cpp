#include "mesh_scene.h"
#include "animation/thermal/background_field.h"
#include "thirdparty/tiny_obj_loader.h"
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace animation {
namespace {
using Json=nlohmann::json;
std::string read(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f)throw std::runtime_error("Cannot open mesh asset/input: "+pathText(path));
    const auto length=f.tellg();
    if(length<0 || length>256*1024*1024)throw std::runtime_error("mesh asset/input exceeds 256 MiB limit");
    std::string s(size_t(length),'\0');f.seekg(0);f.read(s.data(),length);
    if(!f)throw std::runtime_error("Cannot read mesh asset/input: "+pathText(path));
    return s;
}
uint32_t rotate(uint32_t x,int n){return (x>>n)|(x<<(32-n));}
std::string hash(const std::string& input) {
    constexpr uint32_t k[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t state[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::vector<uint8_t> bytes(input.begin(),input.end());const uint64_t bits=uint64_t(bytes.size())*8;
    bytes.push_back(128);while(bytes.size()%64!=56)bytes.push_back(0);
    for(int i=7;i>=0;i--)bytes.push_back(uint8_t(bits>>(8*i)));
    for(size_t off=0;off<bytes.size();off+=64){
        uint32_t w[64]{};
        for(int i=0;i<16;i++)for(int j=0;j<4;j++)w[i]=(w[i]<<8)|bytes[off+i*4+j];
        for(int i=16;i<64;i++)w[i]=w[i-16]+(rotate(w[i-15],7)^rotate(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(rotate(w[i-2],17)^rotate(w[i-2],19)^(w[i-2]>>10));
        uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
        for(int i=0;i<64;i++){
            const auto t1=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            const auto t2=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
            h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
    }
    std::ostringstream out;out<<std::hex<<std::setfill('0');for(auto v:state)out<<std::setw(8)<<v;return out.str();
}
float value(const Json& j,const char* key,float fallback,float lo,float hi){
    float f=j.value(key,fallback);if(!std::isfinite(f)||f<lo||f>hi)throw std::runtime_error(std::string(key)+" outside supported range");return f;
}
int integer(const Json& j,const char* key,int fallback,int lo,int hi){
    if(!j.contains(key))return fallback;
    if(!j.at(key).is_number_integer())throw std::runtime_error(std::string(key)+" must be an integer");
    const double v=j.at(key).get<double>();if(v<lo||v>hi)throw std::runtime_error(std::string(key)+" outside supported range");return int(v);
}
glm::vec3 vector(const Json& j){
    const auto a=j.get<std::vector<float>>();if(a.size()!=3)throw std::runtime_error("camera/vector requires three components");
    for(float v:a)if(!std::isfinite(v)||std::abs(v)>1e5f)throw std::runtime_error("camera/vector outside supported finite range");return {a[0],a[1],a[2]};
}
std::string identifier(const Json& j){
    const auto name=j.at("id").get<std::string>();
    if(name.empty()||name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)
        throw std::runtime_error("scene id must contain only letters, digits, _ or -");return name;
}
}
std::string sha256File(const std::filesystem::path& path){return hash(read(path));}
std::string sha256Bytes(const std::string& bytes){return hash(bytes);}

MeshScene::MeshScene(const std::filesystem::path& source)
    : MeshScene(source,Json()) {}
MeshScene::MeshScene(const std::filesystem::path& source,const Json& supplied)
    : sourceBytes(supplied.is_null()?read(source):supplied.dump()),input(Json::parse(sourceBytes)),materials(input.at("materials")) {
    if(input.at("schemaVersion")!=1)throw std::runtime_error("Unsupported mesh scene schemaVersion");
    const auto& coordinates=input.at("coordinates");
    if(coordinates.at("units")!="m"||coordinates.at("upAxis")!="Z"||coordinates.at("handedness")!="right")
        throw std::runtime_error("Mesh world coordinates must be right-handed Z-up metres");
    width=integer(input,"width",1280,32,4096);height=integer(input,"height",800,32,4096);
    samples=integer(input,"samples",1,1,3);gpuIndex=integer(input,"gpuIndex",0,0,128);
    const auto thermal=input.value("thermal",Json::object());
    wavelength=value(thermal,"wavelengthMicrometers",10,3,14);minK=value(thermal,"displayMinK",280,150,2000);
    maxK=value(thermal,"displayMaxK",320,150,2000);skyK=value(thermal,"skyK",275,150,500);
    if(maxK<=minK)throw std::runtime_error("thermal displayMaxK must exceed displayMinK");
    receipt={{"backend","mesh"},{"schemaVersion",1},{"sourceSha256",hash(sourceBytes)},
        {"coordinates",coordinates},{"transformConvention","row-major asset-to-world; Y-up asset first converted (x,y,z)->(x,-z,y), then instance transform"},
        {"geometryService","existing HiStream nvvk Vulkan BLAS/TLAS; explicit position stride 12 bytes"},
        {"materialSource","explicit scene table matched by OBJ usemtl name; external MTL optical values/textures are not consumed"},
        {"temperatureSource","prescribed material temperature; no energy-balance or transient field adapter in this phase"},
        {"radiometricallyValidated",false},{"structuralResponseComputed",false},{"assets",Json::array()},{"instances",Json::array()}};
    struct Asset {std::vector<Triangle> triangles;};std::map<std::string,Asset> loaded;
    std::string mtl;for(const auto& name:materials.names)mtl+="newmtl "+name+"\nKd 0.5 0.5 0.5\n";
    const auto& assetList=input.at("assets");
    if(!assetList.is_array()||assetList.empty()||assetList.size()>256)throw std::runtime_error("mesh assets must contain 1..256 entries");
    for(const auto& entry:assetList){
        const auto id=identifier(entry);if(loaded.count(id))throw std::runtime_error("Duplicate mesh asset id");
        const auto path=std::filesystem::absolute(source.parent_path()/pathFrom(entry.at("path").get<std::string>())).lexically_normal();
        const auto bytes=read(path),digest=hash(bytes);
        if(entry.at("sha256").get<std::string>()!=digest)throw std::runtime_error("mesh asset SHA256 mismatch: "+id);
        const auto up=entry.at("upAxis").get<std::string>();
        if((up!="Z"&&up!="Y")||entry.at("units")!="m")throw std::runtime_error("mesh asset requires units=m and upAxis=Y or Z");
        tinyobj::ObjReader reader;tinyobj::ObjReaderConfig options;options.triangulate=true;
        if(!reader.ParseFromString(bytes,mtl,options)||!reader.Valid())throw std::runtime_error("Invalid OBJ asset: "+reader.Error());
        const auto& attr=reader.GetAttrib();const auto& objMaterials=reader.GetMaterials();
        Asset asset;
        auto convert=[&](glm::vec3 v){return up=="Y"?glm::vec3(v.x,-v.z,v.y):v;};
        for(const auto& shape:reader.GetShapes()){
            size_t offset=0;
            for(size_t f=0;f<shape.mesh.num_face_vertices.size();f++){
                if(shape.mesh.num_face_vertices[f]!=3)throw std::runtime_error("OBJ triangulation failed");
                int material=shape.mesh.material_ids.at(f);
                if(material<0||size_t(material)>=objMaterials.size())throw std::runtime_error("Missing OBJ material mapping for asset: "+id);
                Triangle t{};t.ids.x=materials.index(objMaterials[material].name);glm::vec3 pos[3],norm[3];bool haveNormals=true;
                for(int k=0;k<3;k++){
                    const auto ix=shape.mesh.indices.at(offset+k);
                    if(ix.vertex_index<0||size_t(ix.vertex_index)*3+2>=attr.vertices.size())throw std::runtime_error("OBJ vertex index out of range");
                    pos[k]=convert({attr.vertices[ix.vertex_index*3],attr.vertices[ix.vertex_index*3+1],attr.vertices[ix.vertex_index*3+2]});
                    for(int d=0;d<3;d++)if(!std::isfinite(pos[k][d])||std::abs(pos[k][d])>1e5)throw std::runtime_error("OBJ position outside supported range");
                    if(ix.normal_index>=0){
                        if(size_t(ix.normal_index)*3+2>=attr.normals.size())throw std::runtime_error("OBJ normal index out of range");
                        norm[k]=convert({attr.normals[ix.normal_index*3],attr.normals[ix.normal_index*3+1],attr.normals[ix.normal_index*3+2]});
                        if(!std::isfinite(glm::length(norm[k]))||glm::length(norm[k])<1e-8f)throw std::runtime_error("Invalid OBJ normal");
                    }else haveNormals=false;
                }
                auto n=glm::cross(pos[1]-pos[0],pos[2]-pos[0]);
                if(glm::length(n)<1e-8f)throw std::runtime_error("Degenerate OBJ triangle");n=glm::normalize(n);
                t.a={pos[0],1};t.b={pos[1],1};t.c={pos[2],1};
                t.na={haveNormals?glm::normalize(norm[0]):n,0};t.nb={haveNormals?glm::normalize(norm[1]):n,0};t.nc={haveNormals?glm::normalize(norm[2]):n,0};
                asset.triangles.push_back(t);offset+=3;
            }
        }
        if(asset.triangles.empty()||asset.triangles.size()>2000000)throw std::runtime_error("asset triangle count outside 1..2000000");
        receipt["assets"].push_back({{"id",id},{"path",pathText(path)},{"sha256",digest},{"triangleCount",asset.triangles.size()},{"upAxis",up}});
        assets.push_back(path);loaded.emplace(id,std::move(asset));
    }
    glm::vec3 lower(std::numeric_limits<float>::max()),upper(-std::numeric_limits<float>::max());
    const auto& instances=input.at("instances");
    if(!instances.is_array()||instances.empty()||instances.size()>10000)throw std::runtime_error("mesh instances must contain 1..10000 entries");
    std::set<std::string> names;
    for(const auto& instance:instances){
        const auto id=identifier(instance);if(!names.insert(id).second)throw std::runtime_error("Duplicate instance id");
        const auto asset=instance.at("asset").get<std::string>();if(!loaded.count(asset))throw std::runtime_error("Missing instance asset: "+asset);
        const auto transform=instance.at("transform").get<std::vector<float>>();
        if(transform.size()!=16)throw std::runtime_error("instance transform requires 16 row-major values");
        glm::mat4 matrix(1);
        for(int i=0;i<16;i++){if(!std::isfinite(transform[i])||std::abs(transform[i])>1e5f)throw std::runtime_error("Invalid instance transform");matrix[i%4][i/4]=transform[i];}
        const auto det=glm::determinant(glm::mat3(matrix));
        if(transform[12]!=0||transform[13]!=0||transform[14]!=0||transform[15]!=1||!std::isfinite(det)||det<=1e-8f)
            throw std::runtime_error("instance transform must be nonsingular, affine and orientation-preserving");
        const auto normalMatrix=glm::inverseTranspose(glm::mat3(matrix));
        if(triangles.size()+loaded.at(asset).triangles.size()>2000000)throw std::runtime_error("Scene exceeds 2000000 triangle budget");
        for(auto t:loaded.at(asset).triangles){
            t.a=matrix*t.a;t.b=matrix*t.b;t.c=matrix*t.c;
            for(const auto v:{t.a,t.b,t.c})for(int k=0;k<3;k++){
                if(!std::isfinite(v[k])||std::abs(v[k])>1e5f)throw std::runtime_error("Transformed mesh outside supported world range");
                lower[k]=std::min(lower[k],v[k]);upper[k]=std::max(upper[k],v[k]);}
            t.na={glm::normalize(normalMatrix*glm::vec3(t.na)),0};t.nb={glm::normalize(normalMatrix*glm::vec3(t.nb)),0};t.nc={glm::normalize(normalMatrix*glm::vec3(t.nc)),0};
            t.ids.y=uint32_t(receipt["instances"].size());
            t.thermal={materials.values[t.ids.x].thermal.x,materials.values[t.ids.x].thermal.x,0,0};triangles.push_back(t);
        }
        receipt["instances"].push_back({{"id",id},{"asset",asset},{"transform",transform}});
    }
    receipt["worldBounds"]={{lower.x,lower.y,lower.z},{upper.x,upper.y,upper.z}};receipt["triangleCount"]=triangles.size();
    const auto& views=input.at("cameras");if(!views.is_array()||views.empty()||views.size()>32)throw std::runtime_error("cameras must contain 1..32 entries");
    names.clear();
    for(const auto& v:views){
        Camera c{identifier(v),vector(v.at("position")),vector(v.at("target")),vector(v.at("up")),value(v,"fovYDegrees",45,5,120)};
        auto filenameId=c.id;
        std::transform(filenameId.begin(),filenameId.end(),filenameId.begin(),[](unsigned char ch){return char(std::tolower(ch));});
        if(!names.insert(filenameId).second||glm::length(c.target-c.position)<1e-4f||glm::length(c.up)<1e-5f||glm::length(glm::cross(glm::normalize(c.target-c.position),glm::normalize(c.up)))<1e-4f)
            throw std::runtime_error("Invalid or duplicate mesh camera");
        cameras.push_back(c);
    }
    receipt["cameras"]=views;receipt["materials"]=input.at("materials");receipt["width"]=width;receipt["height"]=height;
    receipt["samplesPerPixel"]=samples*samples;
    if(input.contains("backgroundField"))background=std::make_shared<BackgroundField>(*this,source.parent_path());
    if(input.contains("thermalPerturbations")){
        std::set<std::string> changed;
        for(const auto& item:input.at("thermalPerturbations")){
            const auto id=item.at("instance").get<std::string>();const auto delta=item.at("deltaKelvin").get<float>();
            if(!std::isfinite(delta)||delta<0||delta>1500||!changed.insert(id).second)throw std::runtime_error("Invalid/duplicate supplied thermal perturbation");
            bool found=false;
            for(size_t i=0;i<receipt["instances"].size();i++)if(receipt["instances"][i]["id"]==id){
                found=true;for(auto& t:triangles)if(t.ids.y==i){
                    if(t.thermal.x+delta>2000||t.thermal.y+delta>2000)throw std::runtime_error("Perturbed surface temperature exceeds supported range");t.thermal.z=delta;
                }
            }
            if(!found)throw std::runtime_error("Unknown thermal perturbation instance");
        }
        receipt["thermalPerturbations"]=input.at("thermalPerturbations");
    }
}
}
