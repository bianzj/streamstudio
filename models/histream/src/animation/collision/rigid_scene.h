#pragma once
#include "animation/scene/mesh_scene.h"
#include <glm/gtc/quaternion.hpp>
#include <memory>

namespace animation {
struct GeometricContact {
    bool hit=false;double fraction=0;int triangle=-1;
    glm::vec3 point{},normal{},origin{};
};
// Convex translation against actual triangles; not a penetration/failure model.
GeometricContact sweepContact(const std::vector<Triangle>&,const std::vector<glm::vec3>& localHull,glm::vec3 from,glm::vec3 to);
struct RigidBodySpec {glm::vec3 center{},size{},velocity{},angularVelocity{};float releaseTime=0,density=2300;};
struct RigidPose {glm::vec3 position{};glm::quat orientation{1,0,0,0};};
// maxLinearSpeed>0 enables a per-substep limiter (m/s) for contact-solver artefacts; 0 disables it.
// trackCacheDirectory (non-empty) enables the rigid track cache: the first run records every base-step
// pose and statistic; later runs with an identical input hash replay them without creating Bullet
// bodies. trackHorizonSeconds is the largest elapsed time that will be sampled (needed to decide
// whether an existing track is long enough); <=0 disables the cache.
struct RigidOptions {double step=1./1920;glm::vec3 gravity{0,0,-9.81f};float friction=.65f,restitution=.12f;double maxLinearSpeed=0;
    std::string trackCacheDirectory;double trackHorizonSeconds=0;
    // When true, any static triangle that cannot be represented as a closed convex 12-triangle
    // part is rejected instead of falling back to a thin triangle skin (checked for replays too).
    bool requireSolidStatic=false;};
class RigidScene {
public:
    RigidScene(const std::vector<Triangle>& staticTriangles,const std::vector<RigidBodySpec>&,const RigidOptions& = {});
    ~RigidScene();
    std::vector<RigidPose> sample(double elapsed);
    nlohmann::json statistics()const;
    // Writes a recording track now (otherwise at destruction) so trackCache.writeOutcome is known
    // before receipts are written. Later samples are no longer added to the file.
    void flushTrack();
private:
    struct Impl;std::unique_ptr<Impl> m;
};
}
