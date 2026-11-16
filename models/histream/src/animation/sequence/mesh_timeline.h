#pragma once
#include "animation/scene/mesh_scene.h"
#include <glm/gtc/quaternion.hpp>

namespace animation {
class RigidScene;
namespace impact {struct Event;}
class MeshTimeline {
public:
    MeshTimeline(const std::filesystem::path&);
    ~MeshTimeline();
    MeshScene base,shell;
    int fps=24,count=288,first=1,last=288;float impact=3;
    // Physical seconds per presentation second (impact-v1 presentationTimeScale; 1 for the other backends).
    // Frame n is sampled at physical time (n-1)/fps*timeScale.
    double timeScale=1;
    // impact-v1 event (penetration, burst, blast, failure, energy budget); null for authored/rigid-v1.
    const impact::Event* impactEvent()const{return event.get();}
    // Constant authored flight speed (m/s): supplied directly or derived from impactTime.
    float flightSpeed=0;bool flightSpeedSupplied=false;
    // Azimuth (rad) of the horizontal outward direction (sin a,-cos a) used by fireball, dust and heat pulse; 0 = -Y facade.
    float outwardAzimuth=0;
    // Wall-clock seconds spent in the rigid solver (or its cached replay); diagnostic only.
    double rigidSampleSeconds=0;
    Camera camera;
    glm::vec3 contact{},projectileStart{},flowOrigin{};glm::vec2 wind{};
    bool sensor=true,media=true,projectileV2=false;
    std::vector<int> exports;
    // parts (optional): rigid groups of the triangles appended after the base/shell scene (projectile,
    // fragments), with first relative to the first appended triangle. Geometry is unchanged.
    MeshScene frame(double time,bool recordDiagnostics=true,std::vector<MeshPart>* parts=nullptr);
    glm::vec3 projectile(double time)const;
    glm::vec3 flightDirection()const{return glm::normalize(projectileEnd-projectileStart);}
    // A camera with its optional projectile tracking or body mount (the primary rig comes from sequence.camera).
    struct CameraRig {Camera camera;bool tracking=false;glm::vec3 trackingOffset{};bool mounted=false;glm::vec3 mountOffset{0,0,.025f};};
    CameraRig primaryRig()const{return {camera,tracking,trackingOffset,mounted,mountOffset};}
    // Parses {"camera": id, "trackingCameraOffset": [x,y,z]?, "cameraMount": {"body":"projectile","offsetMeters":[..]}?}.
    CameraRig rigFrom(const nlohmann::json&)const;
    Camera view(const CameraRig&,double time)const;
    Camera view(double time)const{return view(primaryRig(),time);}
    bool bodyMountedCamera()const{return mounted;}
    nlohmann::json cameraMountDescription()const;
    uint32_t targetIndex()const{return target;}
    nlohmann::json receipt()const;
    bool rigid()const{return bool(dynamics);}
    nlohmann::json rigidStatistics()const;
    // Writes a recording rigid track before receipts are finalised (no-op otherwise).
    void flushRigidTrack();
    nlohmann::json snapshot(const std::filesystem::path&)const;
private:
    struct Chunk {
        std::vector<Triangle> triangles;glm::vec3 center{},size{},velocity{},landing{};
        glm::quat rest{1,0,0,0};float delay=0,exit=0,contactTime=0;glm::vec2 lower{},upper{};
        std::array<glm::vec2,4> footprint{};float bottom=0,restHeight=0;
        glm::vec2 exitPoint{}; // authored exit-plane point (custom fragmentExit only)
    };
    // Authored fragment exit plane {p : dot(exitNormal,p.xy)=exitOffset}; default is the canonical
    // -Y facade (normal (0,-1), offset 2.8 -> y=-2.8), evaluated with the original literals.
    bool customExit=false;glm::vec2 exitNormal{0,-1};float exitOffset=2.8f;
    std::vector<Chunk> chunks;
    // Chunk index of each rigid body (identity for rigid-v1; failed, non-pulverised blocks for impact-v1).
    std::vector<size_t> bodyChunk;
    std::unique_ptr<impact::Event> event;
    // Rendered projectile part temperatures (K): the authored values unless impact-v1 computes them.
    float noseK=342,bodyK=334,nozzleK=600,finK=330;
    std::vector<Triangle> ground;
    std::vector<std::filesystem::path> paths;
    std::vector<std::string> hashes,keys;
    std::vector<Triangle> remainder;
    std::unique_ptr<RigidScene> dynamics;
    glm::vec3 projectileEnd{};
    nlohmann::json firstContact;
    nlohmann::json groundIssue;
    uint32_t target=0,fractureMaterial=0,projectileMaterial=0;
    uint32_t bodyMaterial=0,noseMaterial=0,nozzleMaterial=0,finMaterial=0;
    bool tracking=false;glm::vec3 trackingOffset{};
    bool mounted=false;glm::vec3 mountOffset{0,0,.025f};
    double maxPenetration=0;
    double maxRestGap=0;uint32_t overlapPairs=0;
    // Authored breach bounds (union of fragment boxes); contact outside them is reported.
    glm::vec3 breachLower{},breachUpper{};bool contactInBreach=true;nlohmann::json breachReceipt,releaseReceipt;
    double outsideSupportSamples=0,speedBound=0,speedGuard=0,minChunkDimension=0;int sliverChunks=0;
    float support(glm::vec3 center)const;
    float footprintSupport(const std::array<glm::vec2,4>&,bool strict=true)const;
    glm::mat3 rotation(const Chunk&,double freeTime)const;
};
}
