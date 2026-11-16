#include "animation/collision/rigid_scene.h"
#include <iostream>
#include <cmath>
#include <stdexcept>
using namespace animation;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
Triangle tri(glm::vec3 a,glm::vec3 b,glm::vec3 c){Triangle t{};t.a={a,1};t.b={b,1};t.c={c,1};return t;}
int main(){try{
    std::vector<Triangle> wall{tri({0,-5,-5},{0,5,-5},{0,5,5}),tri({0,-5,-5},{0,5,5},{0,-5,5})};
    std::vector<glm::vec3> hull;for(int z:{-1,1})for(int y:{-1,1})for(int x:{-1,1})hull.emplace_back(x*.1f,y*.1f,z*.1f);
    auto hit=sweepContact(wall,hull,{-2,0,0},{2,0,0});check(hit.hit,"Missing thin-wall convex contact");check(std::abs(hit.origin.x+.1f)<.001,"Convex first contact exceeds1mm analytic tolerance");check(hit.normal.x<-.99,"Wrong contact normal");
    check(!sweepContact(wall,hull,{-2,7,0},{2,7,0}).hit,"Grazing miss became false contact");
    RigidBodySpec drop;drop.center={0,0,10};drop.size={1,1,1};
    RigidOptions coarse;coarse.step=1./120;RigidOptions fine;fine.step=1./240;
    RigidScene a({}, {drop},coarse),b({}, {drop},fine);double expected=10-4.905*.25*.25;
    auto pa=a.sample(.25)[0],pb=b.sample(.25)[0];double ea=std::abs(pa.position.z-expected),eb=std::abs(pb.position.z-expected);check(eb<ea&&eb<.01,"Free-fall time-step refinement failed");
    std::vector<Triangle> ground{tri({-10,-10,0},{10,-10,0},{10,10,0}),tri({-10,-10,0},{10,10,0},{-10,10,0})};
    RigidBodySpec lower;lower.center={0,0,.5};lower.size={1,1,1};RigidBodySpec upper=lower;upper.center.z=1.5;
    RigidScene stack(ground,{lower,upper});auto rest=stack.sample(3);
    if(std::abs(rest[0].position.z-.5)>=.01||std::abs(rest[1].position.z-1.5)>=.01)std::cerr<<"Stack positions "<<rest[0].position.x<<','<<rest[0].position.y<<','<<rest[0].position.z<<" / "<<rest[1].position.x<<','<<rest[1].position.y<<','<<rest[1].position.z<<'\n'<<stack.statistics().dump(2)<<'\n';
    check(std::abs(rest[0].position.z-.5)<.01&&std::abs(rest[1].position.z-1.5)<.01,"Stack support/gap exceeds1cm");
    RigidBodySpec thin;thin.center={2,0,.12};thin.size={.03,.07,.18};thin.velocity={0,0,-18};thin.angularVelocity={.7,.5,.4};
    RigidScene thinFall(ground,{thin});auto thinPose=thinFall.sample(.2)[0];auto rotation=glm::mat3_cast(thinPose.orientation);float bottom=1e9f;
    for(int x:{-1,1})for(int y:{-1,1})for(int z:{-1,1})bottom=std::min(bottom,(thinPose.position+rotation*(thin.size*.5f*glm::vec3(x,y,z))).z);
    check(bottom>-.01,"Thin rotating fragment tunnels through static ground");
    std::vector<Triangle> floorWall=ground;floorWall.insert(floorWall.end(),wall.begin(),wall.end());
    RigidBodySpec sliding=thin;sliding.center={-.5,0,.09};sliding.velocity={18,0,0};sliding.angularVelocity={0,.3,.2};RigidOptions largeStep;largeStep.step=1./120;
    RigidScene curb(floorWall,{sliding},largeStep);check(curb.sample(.12)[0].position.x<.01,"Existing ground contact hides later wall sweep");
    bool overlapRejected=false;try{RigidScene invalid({}, {lower,lower});}catch(const std::runtime_error&){overlapRejected=true;}check(overlapRejected,"Initial overlapping fragment proxies accepted");
    RigidBodySpec left=lower,right=lower;left.center={-1,0,0};right.center={1,0,0};left.velocity={3,0,0};right.velocity={-3,0,0};RigidOptions zero;zero.gravity={0,0,0};
    RigidScene pair({}, {left,right},zero),repeat({}, {left,right},zero);auto p=pair.sample(.75),q=repeat.sample(.75);
    check(p[0].position.x+.99f<=p[1].position.x,"Body-body collision interpenetrates");
    check(pair.statistics()["fragmentContactPointSamples"].get<double>()>0,"Missing body-body contacts");
    for(size_t i=0;i<p.size();i++)check(p[i].position==q[i].position&&p[i].orientation==q[i].orientation,"Repeated dynamics not deterministic");
    std::cout<<nlohmann::json{{"status","passed"},{"planeContactErrorM",std::abs(hit.origin.x+.1f)},{"coarseFreeFallErrorM",ea},{"fineFreeFallErrorM",eb},{"stack",stack.statistics()},{"pair",pair.statistics()}}.dump(2)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
