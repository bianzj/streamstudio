#include "rigid_scene.h"
#include "animation/scene/path_text.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#ifdef HISTREAM_ANIMATION_BULLET
#include <btBulletDynamicsCommon.h>

namespace animation {
namespace {
btVector3 bt(glm::vec3 p){return {p.x,p.y,p.z};}
glm::vec3 vec(const btVector3& p){return {float(p.x()),float(p.y()),float(p.z())};}
bool finite(glm::vec3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&glm::length(p)<100000;}
// A closed convex hexahedron given as 12 triangles: 8 welded vertices, every edge shared by
// exactly two triangles, no degenerate face and every vertex behind every face plane.
// allowInverted additionally accepts a consistently inward-wound group (used only by the
// fallback partition so the ordered fast path keeps its original acceptance rule).
bool closedConvexHex(const std::vector<const Triangle*>& group,std::vector<glm::vec3>& vertices,bool allowInverted){
    vertices.clear();if(group.size()!=12)return false;std::map<std::pair<int,int>,int> edges;
    for(const auto* t:group){
        std::array<int,3> indices;int index=0;
        for(auto q:{t->a,t->b,t->c}){glm::vec3 p(q);auto it=std::find_if(vertices.begin(),vertices.end(),[&](auto v){return glm::length(v-p)<1e-5f;});if(it==vertices.end()){indices[index++]=int(vertices.size());vertices.push_back(p);}else indices[index++]=int(it-vertices.begin());}
        for(int n=0;n<3;n++)edges[std::minmax(indices[n],indices[(n+1)%3])]++;
    }
    if(vertices.size()!=8||std::any_of(edges.begin(),edges.end(),[](auto& e){return e.second!=2;}))return false;
    bool outward=true,inward=allowInverted;
    for(const auto* t:group){
        auto a=glm::vec3(t->a),c=glm::cross(glm::vec3(t->b)-a,glm::vec3(t->c)-a);
        // A degenerate face would normalize to NaN and every NaN comparison passes the test below.
        if(!(glm::length(c)>1e-10f))return false;
        auto n=glm::normalize(c);
        for(auto p:vertices){const float d=glm::dot(n,p-a);if(d>1e-4f)outward=false;if(d<-1e-4f)inward=false;}
    }
    return outward||inward;
}
// Static collider partition. Canonical architecture/preset assets contain consecutive
// closed convex 12-triangle boxes: that ordered layout is accepted unchanged (fast path).
// Otherwise every 12-triangle window that forms a closed convex box is kept as a solid part,
// remaining triangles are grouped by welded-vertex connectivity and each 12-triangle
// component is tested again; only what still fails becomes a thin triangle skin.
struct StaticLayout {
    std::vector<std::vector<glm::vec3>> hulls;std::vector<size_t> skin;
    bool ordered=false;size_t windowParts=0,componentParts=0;
    bool solid()const{return skin.empty()&&!hulls.empty();}
    const char* kind()const{return hulls.empty()?(skin.empty()?"none":"triangleMeshSkin"):skin.empty()?"closedConvexParts":"mixed";}
};
StaticLayout classifyStatic(const std::vector<Triangle>& triangles){
    StaticLayout layout;if(triangles.empty())return layout;
    std::vector<const Triangle*> group;std::vector<glm::vec3> vertices;
    auto window=[&](size_t first,bool allowInverted){group.clear();for(size_t k=first;k<first+12;k++)group.push_back(&triangles[k]);return closedConvexHex(group,vertices,allowInverted);};
    if(triangles.size()%12==0){
        layout.ordered=true;
        for(size_t first=0;first<triangles.size()&&layout.ordered;first+=12){if(window(first,false))layout.hulls.push_back(vertices);else layout.ordered=false;}
        if(layout.ordered)return layout;
        layout.hulls.clear();
    }
    std::vector<size_t> rest;
    for(size_t i=0;i<triangles.size();){
        if(i+12<=triangles.size()&&window(i,true)){layout.hulls.push_back(vertices);layout.windowParts++;i+=12;}
        else rest.push_back(i++);
    }
    // Connected components of the remaining triangles over welded (1e-5 m) vertices.
    std::vector<size_t> parent(rest.size());for(size_t i=0;i<parent.size();i++)parent[i]=i;
    auto find=[&](size_t x){while(parent[x]!=x){parent[x]=parent[parent[x]];x=parent[x];}return x;};
    std::map<std::array<long long,3>,size_t> owner;
    for(size_t i=0;i<rest.size();i++){
        const auto& t=triangles[rest[i]];
        for(auto q:{t.a,t.b,t.c}){
            const std::array<long long,3> key{std::llround(double(q.x)*1e5),std::llround(double(q.y)*1e5),std::llround(double(q.z)*1e5)};
            auto [it,inserted]=owner.emplace(key,i);if(!inserted){auto a=find(i),b=find(it->second);if(a!=b)parent[std::max(a,b)]=std::min(a,b);}
        }
    }
    std::map<size_t,std::vector<size_t>> components;for(size_t i=0;i<rest.size();i++)components[find(i)].push_back(rest[i]);
    for(auto& [root,members]:components){
        if(members.size()==12){group.clear();for(auto k:members)group.push_back(&triangles[k]);if(closedConvexHex(group,vertices,true)){layout.hulls.push_back(vertices);layout.componentParts++;continue;}}
        layout.skin.insert(layout.skin.end(),members.begin(),members.end());
    }
    std::sort(layout.skin.begin(),layout.skin.end());
    return layout;
}
struct MeshCollider {
    btTriangleMesh mesh;
    std::vector<std::unique_ptr<btConvexHullShape>> parts;
    std::unique_ptr<btCollisionShape> skinShape;
    std::unique_ptr<btCollisionShape> shape;
    std::unique_ptr<btRigidBody> body;
    size_t skinTriangles=0;
    // Plain thin triangle skin (projectile sweep and fully non-solid scenes).
    explicit MeshCollider(const std::vector<Triangle>& triangles){
        for(const auto& t:triangles)mesh.addTriangle(bt(glm::vec3(t.a)),bt(glm::vec3(t.b)),bt(glm::vec3(t.c)),false);
        skinTriangles=triangles.size();shape=std::make_unique<btBvhTriangleMeshShape>(&mesh,true);shape->setMargin(0);finish();
    }
    // Solid convex parts retained; only the triangles the layout could not close stay a skin.
    MeshCollider(const std::vector<Triangle>& triangles,const StaticLayout& layout){
        if(layout.hulls.empty()){
            for(const auto& t:triangles)mesh.addTriangle(bt(glm::vec3(t.a)),bt(glm::vec3(t.b)),bt(glm::vec3(t.c)),false);
            skinTriangles=triangles.size();shape=std::make_unique<btBvhTriangleMeshShape>(&mesh,true);shape->setMargin(0);finish();return;
        }
        for(const auto& vertices:layout.hulls){auto hull=std::make_unique<btConvexHullShape>();hull->setMargin(0);for(auto p:vertices)hull->addPoint(bt(p),false);hull->recalcLocalAabb();parts.push_back(std::move(hull));}
        auto compound=std::make_unique<btCompoundShape>(true,int(parts.size()+(layout.skin.empty()?0:1)));for(auto& p:parts)compound->addChildShape(btTransform::getIdentity(),p.get());
        if(!layout.skin.empty()){
            for(auto k:layout.skin){const auto& t=triangles[k];mesh.addTriangle(bt(glm::vec3(t.a)),bt(glm::vec3(t.b)),bt(glm::vec3(t.c)),false);}
            skinTriangles=layout.skin.size();skinShape=std::make_unique<btBvhTriangleMeshShape>(&mesh,true);skinShape->setMargin(0);compound->addChildShape(btTransform::getIdentity(),skinShape.get());
        }
        shape=std::move(compound);finish();
    }
private:
    void finish(){btRigidBody::btRigidBodyConstructionInfo info(0,nullptr,shape.get());body=std::make_unique<btRigidBody>(info);body->setUserIndex(-1);}
};
struct World {
    btDefaultCollisionConfiguration config;btCollisionDispatcher dispatcher{&config};btDbvtBroadphase broad;
    btSequentialImpulseConstraintSolver solver;btDiscreteDynamicsWorld world{&dispatcher,&broad,&solver,&config};
};
struct Sweep : btCollisionWorld::ClosestConvexResultCallback {
    int triangle=-1;
    Sweep(btVector3 a,btVector3 b):ClosestConvexResultCallback(a,b){}
    btScalar addSingleResult(btCollisionWorld::LocalConvexResult& r,bool normalWorld)override{
        if(r.m_hitFraction<=m_closestHitFraction&&r.m_localShapeInfo)triangle=r.m_localShapeInfo->m_triangleIndex;
        return ClosestConvexResultCallback::addSingleResult(r,normalWorld);
    }
};
struct StaticSweep : btCollisionWorld::ClosestConvexResultCallback {
    const btCollisionObject* target;
    StaticSweep(btVector3 a,btVector3 b,const btCollisionObject* only):ClosestConvexResultCallback(a,b),target(only){}
    bool needsCollision(btBroadphaseProxy* p)const override{return p->m_clientObject==target&&ClosestConvexResultCallback::needsCollision(p);}
    btScalar addSingleResult(btCollisionWorld::LocalConvexResult& r,bool normalWorld)override{
        // Reject existing contacts before they can set closestFraction=0 and
        // hide a later positive wall/curb hit in the same compound query.
        if(r.m_hitFraction<=1e-5)return 1;
        return ClosestConvexResultCallback::addSingleResult(r,normalWorld);
    }
};
}
GeometricContact sweepContact(const std::vector<Triangle>& triangles,const std::vector<glm::vec3>& points,glm::vec3 from,glm::vec3 to){
    if(points.size()<4||!finite(from)||!finite(to)||glm::length(to-from)<1e-6)throw std::runtime_error("Invalid convex sweep");
    if(triangles.empty())return {};
    for(auto p:points)if(!finite(p))throw std::runtime_error("Invalid sweep hull");
    World w;MeshCollider mesh(triangles);w.world.addRigidBody(mesh.body.get());
    btConvexHullShape hull;hull.setMargin(0);for(auto p:points)hull.addPoint(bt(p),false);hull.recalcLocalAabb();
    btTransform a=btTransform::getIdentity(),b=a;a.setOrigin(bt(from));b.setOrigin(bt(to));Sweep callback(a.getOrigin(),b.getOrigin());
    w.world.convexSweepTest(&hull,a,b,callback,0);w.world.removeRigidBody(mesh.body.get());
    if(!callback.hasHit())return {};
    return {true,double(callback.m_closestHitFraction),callback.triangle,vec(callback.m_hitPointWorld),vec(callback.m_hitNormalWorld.normalized()),glm::mix(from,to,float(callback.m_closestHitFraction))};
}
namespace {
// Rigid track cache. Bump kSolverRevision whenever stepping, contact handling or the limiter changes,
// so stale tracks are never replayed.
constexpr const char* kSolverRevision="rigid-solver-r3";
struct Stats {double maxPenetration=0,staticSamples=0,fragmentSamples=0,maxSpeed=0,rawPeakSpeed=0,rawPeakBody=-1,rawPeakTime=0,worstA=-1,worstB=-1,worstTime=0,fullShapeClamps=0,internalSteps=0,maximumSubdivisions=1,minimumStep=1,speedGuardClamps=0,speedGuardBodies=0,sleeping=0,kinetic=0;};
static_assert(std::is_trivially_copyable<Stats>::value&&sizeof(Stats)==18*sizeof(double),"Stats is stored raw in track files");
struct TrackHeader {char magic[8];uint32_t version,bodies;uint64_t steps,templateBytes;double stepSeconds;char key[40];};
static_assert(std::is_trivially_copyable<TrackHeader>::value&&sizeof(TrackHeader)==80,"TrackHeader is stored raw in track files");
// Two independent 64-bit FNV-style hashes; a cache key, not a security digest.
struct Hasher {
    uint64_t a=14695981039346656037ull,b=0x9E3779B97F4A7C15ull;
    void bytes(const void* data,size_t size){auto* p=static_cast<const unsigned char*>(data);for(size_t i=0;i<size;i++){a=(a^p[i])*1099511628211ull;b=(b+p[i]+1)*0xFF51AFD7ED558CCDull;b^=b>>29;}}
    template<class T> void value(const T& v){bytes(&v,sizeof(T));}
    std::string hex()const{char text[33];std::snprintf(text,sizeof text,"%016llx%016llx",static_cast<unsigned long long>(a),static_cast<unsigned long long>(b));return text;}
};
long processId(){
#ifdef _WIN32
    return long(_getpid());
#else
    return long(getpid());
#endif
}
std::string trackKey(const std::vector<Triangle>& triangles,const std::vector<RigidBodySpec>& specs,const RigidOptions& o,const StaticLayout& layout){
    Hasher h;h.bytes(kSolverRevision,std::strlen(kSolverRevision));const int bullet=BT_BULLET_VERSION;h.value(bullet);
    // The ordered all-box and the all-skin layouts collide exactly as before this partition
    // existed, so their keys are unchanged; a partially solid layout is a different collider.
    if(!layout.ordered&&!layout.hulls.empty()){const char* tag="static-layout-r1";h.bytes(tag,std::strlen(tag));uint64_t parts=layout.hulls.size(),skin=layout.skin.size();h.value(parts);h.value(skin);}
    h.value(o.step);h.value(o.gravity.x);h.value(o.gravity.y);h.value(o.gravity.z);h.value(o.friction);h.value(o.restitution);h.value(o.maxLinearSpeed);
    uint64_t n=triangles.size();h.value(n);for(const auto& t:triangles)for(auto p:{t.a,t.b,t.c}){h.value(p.x);h.value(p.y);h.value(p.z);}
    n=specs.size();h.value(n);
    for(const auto& s:specs){for(auto v:{s.center,s.size,s.velocity,s.angularVelocity}){h.value(v.x);h.value(v.y);h.value(v.z);}h.value(s.releaseTime);h.value(s.density);}
    return h.hex();
}
// Writes the per-step values into a statistics object; shared by live runs and replays so both report identical numbers.
void applyStats(nlohmann::json& j,const Stats& s,int step,double stepSeconds){
    j["maximumSubdivisions"]=int(s.maximumSubdivisions);j["minimumInternalStepSeconds"]=s.internalSteps>0?s.minimumStep:stepSeconds;j["internalStepCount"]=uint64_t(s.internalSteps);
    j["simulationTimeSeconds"]=step*stepSeconds;j["maxContactPenetrationM"]=s.maxPenetration;j["worstContactBodyA"]=int(s.worstA);j["worstContactBodyB"]=int(s.worstB);j["worstContactTimeSeconds"]=s.worstTime;
    j["staticContactPointSamples"]=s.staticSamples;j["fragmentContactPointSamples"]=s.fragmentSamples;j["fullShapeStaticCcdClamps"]=uint64_t(s.fullShapeClamps);
    j["maximumSpeedMPerSecond"]=s.maxSpeed;j["rawPeakSpeedMPerSecond"]=s.rawPeakSpeed;j["rawPeakSpeedBody"]=int(s.rawPeakBody);j["rawPeakSpeedTimeSeconds"]=s.rawPeakTime;
    j["speedGuardClamps"]=uint64_t(s.speedGuardClamps);j["speedGuardBodyCount"]=size_t(s.speedGuardBodies);j["sleepingBodies"]=int(s.sleeping);j["translationalKineticEnergyJ"]=s.kinetic;
}
}
struct RigidScene::Impl {
    World context;std::unique_ptr<MeshCollider> mesh;
    std::vector<std::unique_ptr<btBoxShape>> shapes;
    std::vector<std::unique_ptr<btRigidBody>> bodies;
    std::vector<RigidBodySpec> specs;std::vector<bool> released;
    std::vector<RigidPose> previous,current;RigidOptions options;
    int step=0;double lastTime=0,maxPenetration=0,staticSamples=0,fragmentSamples=0,maxSpeed=0;
    int rawPeakBody=-1;double rawPeakTime=0,rawPeakSpeed=0;
    uint64_t speedGuardLinear=0;std::set<int> speedGuardBodies;
    int worstA=-1,worstB=-1;double worstTime=0;
    uint64_t fullShapeClamps=0;
    uint64_t internalSteps=0;int maximumSubdivisions=1;double minimumStep=1;
    // Rigid track cache state (see RigidOptions::trackCacheDirectory).
    bool replay=false,recording=false;size_t trackSteps=0;std::vector<float> trackPoses;std::vector<Stats> trackStats;
    nlohmann::json trackTemplate,trackInfo;std::filesystem::path trackPath;std::string trackKeyText;bool trackWritten=false;
    StaticLayout layout;
    ~Impl(){writeTrack();for(auto& b:bodies)context.world.removeRigidBody(b.get());if(mesh)context.world.removeRigidBody(mesh->body.get());}
    nlohmann::json layoutDescription()const{
        return {{"kind",layout.kind()},{"orderedFastPath",layout.ordered},{"convexParts",layout.hulls.size()},{"windowParts",layout.windowParts},{"componentParts",layout.componentParts},{"skinTriangles",layout.skin.size()},
            {"note","closed convex 12-triangle parts keep their solid volume; only skinTriangles are a thin two-sided surface through which thin fragments may enter"}};
    }
    Stats snapshot()const{
        Stats s;s.maxPenetration=maxPenetration;s.staticSamples=staticSamples;s.fragmentSamples=fragmentSamples;s.maxSpeed=maxSpeed;s.rawPeakSpeed=rawPeakSpeed;s.rawPeakBody=rawPeakBody;s.rawPeakTime=rawPeakTime;
        s.worstA=worstA;s.worstB=worstB;s.worstTime=worstTime;s.fullShapeClamps=double(fullShapeClamps);s.internalSteps=double(internalSteps);s.maximumSubdivisions=maximumSubdivisions;s.minimumStep=minimumStep;
        s.speedGuardClamps=double(speedGuardLinear);s.speedGuardBodies=double(speedGuardBodies.size());
        int sleeping=0;double kinetic=0;for(size_t i=0;i<bodies.size();i++)if(released[i]){const auto& b=*bodies[i];sleeping+=!b.isActive();kinetic+=.5/double(b.getInvMass())*double(b.getLinearVelocity().length2());}
        s.sleeping=sleeping;s.kinetic=kinetic;return s;
    }
    nlohmann::json constants()const{
        return {{"engine","Bullet3.25 double precision"},{"computeDevice","CPU"},{"fixedStepSeconds",options.step},{"solverIterations",60},{"staticClosedConvexParts",mesh?mesh->parts.size():0},{"speedGuardLimitMPerSecond",options.maxLinearSpeed},
            {"speedGuard","per-substep limiter of linear speed at the supplied bound; raw peaks are recorded before limiting"},{"staticCollider",mesh?layout.kind():"none"},{"staticLayout",layoutDescription()},{"requireSolidStatic",options.requireSolidStatic},{"bodyCount",bodies.size()},
            {"initialConditions","authored release times, velocities, spin, density and preset fracture; no impact energy calculation"},{"fragmentCCD","Bullet inscribed swept sphere plus full-box static sweep/TOI and normal impulse; dynamic rotational CCD not guaranteed"}};
    }
    // Track recording: float32 position + quaternion (w,x,y,z) of every body after each base step, plus the statistics snapshot.
    void record(){
        for(const auto& p:current){const float v[7]={p.position.x,p.position.y,p.position.z,p.orientation.w,p.orientation.x,p.orientation.y,p.orientation.z};trackPoses.insert(trackPoses.end(),v,v+7);}
        trackStats.push_back(snapshot());
    }
    std::vector<RigidPose> unpack(size_t at)const{
        std::vector<RigidPose> out(specs.size());const float* v=trackPoses.data()+at*specs.size()*7;
        for(size_t i=0;i<out.size();i++,v+=7){out[i].position={v[0],v[1],v[2]};out[i].orientation=glm::quat(v[3],v[4],v[5],v[6]);}
        return out;
    }
    // Steps recorded in an existing valid track file for this key (0 if absent/invalid); header-only check.
    uint64_t existingSteps()const{
        std::error_code ec;if(!std::filesystem::is_regular_file(trackPath,ec))return 0;
        std::ifstream f(trackPath,std::ios::binary);TrackHeader h{};f.read(reinterpret_cast<char*>(&h),sizeof h);h.key[sizeof h.key-1]=0;
        if(!f||std::memcmp(h.magic,"HSRTRK01",8)!=0||h.version!=1||h.bodies!=specs.size()||h.stepSeconds!=options.step||trackKeyText!=h.key||h.steps>20000000||h.templateBytes>1000000)return 0;
        const uint64_t count=h.steps+1,expected=sizeof h+h.templateBytes+count*specs.size()*7*sizeof(float)+count*sizeof(Stats);
        if(std::filesystem::file_size(trackPath,ec)!=expected||ec)return 0;return h.steps;
    }
    // Writes the recorded track once (explicit flush before the receipt, or at destruction).
    // An existing valid file holding at least as many steps is kept (identical deterministic
    // content for the same key; a shorter recording must not replace a longer one). The rename
    // is retried because a concurrent reader can briefly hold the target open on Windows.
    void writeTrack(){
        if(!recording||trackWritten)return;trackWritten=true;
        auto outcome=[&](const std::string& value,const std::string& detail={}){trackInfo["writeOutcome"]=value;if(!detail.empty())trackInfo["writeDetail"]=detail;};
        if(trackStats.size()<2){outcome("skipped-empty");return;}
        const uint64_t steps=trackStats.size()-1;
        if(existingSteps()>=steps){outcome("skipped-existing","a valid track with at least as many steps already exists for this key");return;}
        auto temporary=trackPath;
        temporary+="."+std::to_string(processId())+"."+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".tmp";
        try{
            std::filesystem::create_directories(trackPath.parent_path());
            {
                std::ofstream f(temporary,std::ios::binary);const std::string meta=constants().dump();TrackHeader h{};std::memcpy(h.magic,"HSRTRK01",8);h.version=1;h.bodies=uint32_t(specs.size());
                h.steps=steps;h.templateBytes=meta.size();h.stepSeconds=options.step;std::snprintf(h.key,sizeof h.key,"%s",trackKeyText.c_str());
                f.write(reinterpret_cast<const char*>(&h),sizeof h);f.write(meta.data(),std::streamsize(meta.size()));
                f.write(reinterpret_cast<const char*>(trackPoses.data()),std::streamsize(trackPoses.size()*sizeof(float)));
                f.write(reinterpret_cast<const char*>(trackStats.data()),std::streamsize(trackStats.size()*sizeof(Stats)));f.flush();if(!f)throw std::runtime_error("write failed");
            }
            std::string last;
            for(int attempt=0;attempt<4;attempt++){
                if(attempt)std::this_thread::sleep_for(std::chrono::milliseconds(50<<(attempt-1)));
                // Another process may have finished the same (or a longer) track meanwhile.
                if(attempt&&existingSteps()>=steps){std::error_code ec;std::filesystem::remove(temporary,ec);outcome("skipped-existing","written concurrently by another process");return;}
                std::error_code ec;std::filesystem::rename(temporary,trackPath,ec);
                if(!ec){outcome("written",attempt?"after "+std::to_string(attempt)+" retries":"");return;}
                last=ec.message();
            }
            throw std::runtime_error("rename failed: "+last);
        }catch(const std::exception& e){std::error_code ec;std::filesystem::remove(temporary,ec);outcome("failed",e.what());std::cerr<<"WARNING rigid track cache not written: "<<e.what()<<'\n';}
    }
    // Orphaned temporaries of killed writers (older than one day) are removed before recording.
    void cleanStaleTemporaries()const{
        std::error_code ec;const auto directory=trackPath.parent_path();if(!std::filesystem::is_directory(directory,ec))return;
        const auto now=std::filesystem::file_time_type::clock::now();
        for(std::filesystem::directory_iterator it(directory,ec),end;!ec&&it!=end;it.increment(ec)){
            const auto name=it->path().filename().string();
            if(name.rfind("rigid_",0)!=0||name.size()<4||name.compare(name.size()-4,4,".tmp")!=0)continue;
            std::error_code e2;auto time=std::filesystem::last_write_time(it->path(),e2);
            if(!e2&&now-time>std::chrono::hours(24))std::filesystem::remove(it->path(),e2);
        }
    }
    bool load(size_t neededSteps){
        std::error_code ec;if(!std::filesystem::is_regular_file(trackPath,ec))return false;
        std::ifstream f(trackPath,std::ios::binary);TrackHeader h{};f.read(reinterpret_cast<char*>(&h),sizeof h);h.key[sizeof h.key-1]=0;
        if(!f||std::memcmp(h.magic,"HSRTRK01",8)!=0||h.version!=1||h.bodies!=specs.size()||h.stepSeconds!=options.step||trackKeyText!=h.key||h.steps<neededSteps||h.steps>20000000||h.templateBytes>1000000)return false;
        const uint64_t count=h.steps+1,expected=sizeof h+h.templateBytes+count*specs.size()*7*sizeof(float)+count*sizeof(Stats);
        if(std::filesystem::file_size(trackPath,ec)!=expected||ec)return false;
        std::string meta(size_t(h.templateBytes),'\0');f.read(meta.data(),std::streamsize(meta.size()));
        trackPoses.resize(size_t(count)*specs.size()*7);trackStats.resize(size_t(count));
        f.read(reinterpret_cast<char*>(trackPoses.data()),std::streamsize(trackPoses.size()*sizeof(float)));f.read(reinterpret_cast<char*>(trackStats.data()),std::streamsize(trackStats.size()*sizeof(Stats)));
        if(!f)return false;trackTemplate=nlohmann::json::parse(meta,nullptr,false);if(trackTemplate.is_discarded())return false;trackSteps=size_t(h.steps);return true;
    }
    std::vector<RigidPose> poses()const{
        std::vector<RigidPose> out;out.reserve(bodies.size());
        for(const auto& b:bodies){const auto& t=b->getWorldTransform();const auto& q=t.getRotation();out.push_back({vec(t.getOrigin()),glm::quat(float(q.w()),float(q.x()),float(q.y()),float(q.z()))});}return out;
    }
    void advance(){
        previous=current;double now=step*options.step;
        for(size_t i=0;i<bodies.size();i++)if(!released[i]&&now+1e-10>=specs[i].releaseTime){
            auto& b=*bodies[i];context.world.removeRigidBody(&b);
            btScalar mass=double(specs[i].density)*specs[i].size.x*specs[i].size.y*specs[i].size.z;btVector3 inertia;shapes[i]->calculateLocalInertia(mass,inertia);
            b.setMassProps(mass,inertia);b.updateInertiaTensor();b.setCollisionFlags(b.getCollisionFlags()&~btCollisionObject::CF_STATIC_OBJECT);
            b.setLinearVelocity(bt(specs[i].velocity));b.setAngularVelocity(bt(specs[i].angularVelocity));b.setActivationState(ACTIVE_TAG);
            context.world.addRigidBody(&b);released[i]=true;
        }
        double ratio=1;
        for(size_t i=0;i<bodies.size();i++)if(released[i]&&bodies[i]->isActive()){
            double speed=bodies[i]->getLinearVelocity().length()+bodies[i]->getAngularVelocity().length()*.5*glm::length(specs[i].size)+glm::length(options.gravity)*options.step;
            double thickness=std::min({specs[i].size.x,specs[i].size.y,specs[i].size.z});ratio=std::max(ratio,options.step*speed/(.05*thickness));
        }
        int subdivisions=std::clamp(int(std::ceil(std::min(ratio,16.))),1,16);maximumSubdivisions=std::max(maximumSubdivisions,subdivisions);
        double dt=options.step/subdivisions;minimumStep=std::min(minimumStep,dt);
        for(int sub=0;sub<subdivisions;sub++)integrate(dt,now+(sub+1)*dt);
        step++;current=poses();if(recording)record();
    }
    void integrate(double dt,double endTime){
        std::vector<btTransform> before;before.reserve(bodies.size());for(const auto& b:bodies)before.push_back(b->getWorldTransform());
        context.world.stepSimulation(dt,0,dt);internalSteps++;
        // Bullet's built-in swept sphere is inscribed in the thinnest dimension;
        // it cannot protect a rotating plate's outer corners. Sweep the complete
        // box against static scene triangles before accepting its new pose.
        if(mesh)for(size_t i=0;i<bodies.size();i++)if(released[i]&&bodies[i]->isActive()){
            auto& body=*bodies[i];auto after=body.getWorldTransform();
            double travel=(after.getOrigin()-before[i].getOrigin()).length()+after.getRotation().angleShortestPath(before[i].getRotation())*.5*glm::length(specs[i].size);
            if(travel<.1*std::min({specs[i].size.x,specs[i].size.y,specs[i].size.z}))continue;
            StaticSweep hit(before[i].getOrigin(),after.getOrigin(),mesh->body.get());
            context.world.convexSweepTest(shapes[i].get(),before[i],after,hit,0);
            // Existing/resting contacts belong to the coupled constraint solver;
            // freezing their poses atTOI0 would suppress rolling and spoil stacks.
            if(hit.hasHit()&&hit.m_closestHitFraction>1e-5&&hit.m_closestHitFraction<1.-1e-8){
                btScalar fraction=btMax(btScalar(0),hit.m_closestHitFraction-btScalar(1e-6));btTransform accepted;
                accepted.setOrigin(before[i].getOrigin().lerp(after.getOrigin(),fraction));accepted.setRotation(before[i].getRotation().slerp(after.getRotation(),fraction));
                body.proceedToTransform(accepted);body.setInterpolationWorldTransform(accepted);context.world.updateSingleAabb(&body);
                auto normal=hit.m_hitNormalWorld.normalized(),r=hit.m_hitPointWorld-accepted.getOrigin();btScalar vn=normal.dot(body.getVelocityInLocalPoint(r));
                if(vn<0){btScalar denominator=body.getInvMass()+normal.dot((body.getInvInertiaTensorWorld()*(r.cross(normal))).cross(r));if(denominator>1e-12)body.applyImpulse(normal*(-(1.+options.restitution)*vn/denominator),r);}
                fullShapeClamps++;
            }
        }
        auto& dispatcher=context.dispatcher;
        for(int i=0;i<dispatcher.getNumManifolds();i++){
            auto* manifold=dispatcher.getManifoldByIndexInternal(i);bool fragments=manifold->getBody0()->getUserIndex()>=0&&manifold->getBody1()->getUserIndex()>=0;
            for(int j=0;j<manifold->getNumContacts();j++){double depth=manifold->getContactPoint(j).getDistance();if(depth>0)continue;if(-depth>maxPenetration){maxPenetration=-depth;worstA=manifold->getBody0()->getUserIndex();worstB=manifold->getBody1()->getUserIndex();worstTime=endTime;}if(fragments)fragmentSamples++;else staticSamples++;}
        }
        // Speed guard. No energy source exists after release except gravity and the
        // supplied initial velocity, so a body far above the free-fall bound is a
        // contact-solver artefact (typically a thin sliver pinched between heavy
        // blocks). Only linear speed is limited: also limiting angular velocity was tried and
        // made thin bodies sink into the ground (45 mm vs 0.7 mm). Raw peaks (the transient value
        // before limiting) are recorded.
        for(size_t i=0;i<bodies.size();i++){
            if(!released[i])continue;auto& body=*bodies[i];double speed=body.getLinearVelocity().length();
            if(speed>rawPeakSpeed){rawPeakSpeed=speed;rawPeakBody=int(i);rawPeakTime=endTime;}
            if(options.maxLinearSpeed>0){
                const double cap=options.maxLinearSpeed;
                if(speed>cap){body.setLinearVelocity(body.getLinearVelocity()*(cap/speed));speedGuardLinear++;speedGuardBodies.insert(int(i));}
            }
            maxSpeed=std::max(maxSpeed,double(body.getLinearVelocity().length()));
        }
    }
};
RigidScene::RigidScene(const std::vector<Triangle>& triangles,const std::vector<RigidBodySpec>& specs,const RigidOptions& options):m(std::make_unique<Impl>()){
    if(!std::isfinite(options.step)||options.step<1./2000||options.step>1./30||!finite(options.gravity)||!std::isfinite(options.friction)||options.friction<0||options.friction>2||!std::isfinite(options.restitution)||options.restitution<0||options.restitution>1||!std::isfinite(options.maxLinearSpeed)||options.maxLinearSpeed<0||options.maxLinearSpeed>1000||specs.empty()||specs.size()>3000)throw std::runtime_error("Invalid rigid options/bodies");
    m->options=options;m->specs=specs;m->released.resize(specs.size(),false);
    // Classify the static collider before any cache lookup so requireSolidStatic is enforced
    // for replayed tracks too (a replay never builds the Bullet world).
    m->layout=classifyStatic(triangles);
    if(!triangles.empty()&&!m->layout.solid()){
        const std::string detail=std::to_string(m->layout.skin.size())+" of "+std::to_string(triangles.size())+" static triangles are not closed convex 12-triangle parts";
        if(options.requireSolidStatic)throw std::runtime_error("rigid-v1: "+detail+"; refusing the thin triangle-skin fallback (rigidRequireSolidStatic=true)");
        std::cerr<<"WARNING rigid-v1: "<<detail<<"; they use a thin triangle-skin collider (thin fragments may enter through back faces)\n";
    }
    if(!options.trackCacheDirectory.empty()&&options.trackHorizonSeconds>0){
        // Identical input hash and a long enough recorded track: replay it and never build the Bullet world.
        m->trackKeyText=trackKey(triangles,specs,options,m->layout);m->trackPath=pathFrom(options.trackCacheDirectory)/("rigid_"+m->trackKeyText+".track");
        const size_t needed=size_t(std::ceil(options.trackHorizonSeconds/options.step-1e-8)); // same rounding as sample()
        if(m->load(needed)){
            m->replay=true;m->trackInfo={{"mode","replay"},{"file",pathText(m->trackPath)},{"key",m->trackKeyText},{"steps",m->trackSteps},{"note","poses and statistics replayed from a recorded run with an identical input hash; Bullet was not executed"}};
            return;
        }
        m->cleanStaleTemporaries();
        m->recording=true;m->trackInfo={{"mode","record"},{"file",pathText(m->trackPath)},{"key",m->trackKeyText},{"neededSteps",needed},{"note","this run recorded its poses for later replay; the file holds only the steps this run simulated; see writeOutcome"}};
    }
    auto& world=m->context.world;world.setGravity(bt(options.gravity));
    // Bullet's default4cm permitted CCD advance is larger than a full step of
    // a thin fragment. Zero keeps those swept contacts from being discarded.
    world.getDispatchInfo().m_allowedCcdPenetration=0;
    auto& solver=world.getSolverInfo();solver.m_numIterations=60;solver.m_splitImpulse=true;solver.m_splitImpulsePenetrationThreshold=-.002;solver.m_solverMode&=~SOLVER_RANDMIZE_ORDER;
    if(!triangles.empty()){m->mesh=std::make_unique<MeshCollider>(triangles,m->layout);m->mesh->body->setFriction(options.friction);m->mesh->body->setRestitution(options.restitution);world.addRigidBody(m->mesh->body.get());}
    for(size_t i=0;i<specs.size();i++)for(size_t j=0;j<i;j++){glm::vec3 overlap=(specs[i].size+specs[j].size)*.5f-glm::abs(specs[i].center-specs[j].center);if(std::min({overlap.x,overlap.y,overlap.z})>1e-4f)throw std::runtime_error("Initial rigid fragments overlap; regenerate preset with --disjoint");}
    for(size_t i=0;i<specs.size();i++){
        const auto& s=specs[i];if(!finite(s.center)||!finite(s.size)||!finite(s.velocity)||!finite(s.angularVelocity)||glm::any(glm::lessThanEqual(s.size,glm::vec3(.0001f)))||!std::isfinite(s.releaseTime)||s.releaseTime<0||s.releaseTime>30||!std::isfinite(s.density)||s.density<=0||s.density>100000)throw std::runtime_error("Invalid rigid body");
        auto shape=std::make_unique<btBoxShape>(bt(s.size*.5f));float thickness=std::min({s.size.x,s.size.y,s.size.z});shape->setMargin(std::min(.001f,thickness*.05f));
        btRigidBody::btRigidBodyConstructionInfo info(0,nullptr,shape.get());info.m_friction=options.friction;info.m_restitution=options.restitution;
        auto body=std::make_unique<btRigidBody>(info);btTransform t=btTransform::getIdentity();t.setOrigin(bt(s.center));body->setWorldTransform(t);body->setUserIndex(int(i));body->setSleepingThresholds(.04,.04);
        body->setCcdMotionThreshold(thickness*.5);body->setCcdSweptSphereRadius(thickness*.2);world.addRigidBody(body.get());m->shapes.push_back(std::move(shape));m->bodies.push_back(std::move(body));
    }
    m->current=m->poses();m->previous=m->current;if(m->recording)m->record();
}
RigidScene::~RigidScene()=default;
std::vector<RigidPose> RigidScene::sample(double elapsed){
    if(!std::isfinite(elapsed)||elapsed<0||elapsed>120||elapsed+1e-8<m->lastTime)throw std::runtime_error("Invalid/nonmonotonic rigid sample time");m->lastTime=elapsed;
    int target=int(std::ceil(elapsed/m->options.step-1e-8));
    if(m->replay){
        if(size_t(target)>m->trackSteps)throw std::runtime_error("Rigid track cache is shorter than the requested time");
        if(target!=m->step||m->current.empty()){m->step=target;m->current=m->unpack(size_t(target));m->previous=m->unpack(size_t(target>0?target-1:0));}
    }else while(m->step<target)m->advance();
    if(m->step==0)return m->current;float alpha=float(std::clamp((elapsed-(m->step-1)*m->options.step)/m->options.step,0.,1.));auto out=m->current;
    for(size_t i=0;i<out.size();i++){out[i].position=glm::mix(m->previous[i].position,m->current[i].position,alpha);out[i].orientation=glm::normalize(glm::slerp(m->previous[i].orientation,m->current[i].orientation,alpha));}return out;
}
nlohmann::json RigidScene::statistics()const{
    nlohmann::json j=m->replay?m->trackTemplate:m->constants();
    applyStats(j,m->replay?m->trackStats[size_t(m->step)]:m->snapshot(),m->step,m->options.step);
    // The collider classification and the guard of *this* run (a replayed template may predate them).
    j["requireSolidStatic"]=m->options.requireSolidStatic;j["staticLayout"]=m->layoutDescription();j["staticCollider"]=m->layout.kind();
    if(!m->trackInfo.is_null())j["trackCache"]=m->trackInfo;
    return j;
}
void RigidScene::flushTrack(){m->writeTrack();}
}
#else
namespace animation {
struct RigidScene::Impl{};
GeometricContact sweepContact(const std::vector<Triangle>&,const std::vector<glm::vec3>&,glm::vec3,glm::vec3){throw std::runtime_error("rigid-v1 requires HISTREAM_ENABLE_ANIMATION_COLLISION");}
RigidScene::RigidScene(const std::vector<Triangle>&,const std::vector<RigidBodySpec>&,const RigidOptions&){throw std::runtime_error("rigid-v1 requires HISTREAM_ENABLE_ANIMATION_COLLISION");}
RigidScene::~RigidScene()=default;
std::vector<RigidPose> RigidScene::sample(double){throw std::runtime_error("Collision module disabled");}
nlohmann::json RigidScene::statistics()const{return {};}
void RigidScene::flushTrack(){}
}
#endif
