// Low-speed heat/tracer transport. Supplied sources; no explosion/damage model.
layout(local_size_x=4,local_size_y=4,local_size_z=4) in;
struct Cell {vec4 flow;vec4 matter;}; // velocity [m/s], T [K]; tracer, solid, pressure potential, divergence
layout(set=0,binding=0,std430) readonly buffer Input {Cell src[];};
layout(set=0,binding=1,std430) writeonly buffer Output {Cell dst[];};
layout(push_constant) uniform Parameters {ivec4 grid;vec4 domain;vec4 dynamics;vec4 settings;ivec4 style;
#ifdef MESH_TRANSPORT
vec4 sourcePoint;vec4 flightStart;vec4 flightSettings;
#endif
} pc;
const ivec3 directions[6]=ivec3[6](ivec3(1,0,0),ivec3(-1,0,0),ivec3(0,1,0),ivec3(0,-1,0),ivec3(0,0,1),ivec3(0,0,-1));
#ifdef MESH_TRANSPORT
layout(set=0,binding=2,std430) readonly buffer MeshObstacle {uint mask[];};
#define FIRE_BINDING 3
#include "fire_sources.glsl"
#ifdef IMPACT_EVENT
// impact-v1: heat share of the charge energy, injected once at the burst as a Gaussian, and the dust tracer,
// entrained into the growing fireball. burst.xyz burst point, burst.w fireball max radius (m); source.x heat
// amplitude K, source.y Gaussian sigma m, source.z tracer amplitude of that Gaussian, source.w fireball duration s.
layout(set=0,binding=4,std430) readonly buffer ImpactSource {vec4 burst;vec4 source;} impactSource;
float impactShape(vec3 p){vec3 q=p-impactSource.burst.xyz;float s=impactSource.source.y;return exp(-dot(q,q)/(2.*s*s));}
#endif
#define contact pc.sourcePoint.xyz
// sourcePoint.w: azimuth (rad) of the horizontal outward direction (sin a,-cos a); 0 keeps the -Y facade exactly.
vec3 effectOutward(){return pc.sourcePoint.w==0.?vec3(0,-1,0):vec3(sin(pc.sourcePoint.w),-cos(pc.sourcePoint.w),0);}
bool solidAt(vec3 p,float time){ivec3 i=ivec3(floor((p-pc.domain.xyz)/pc.domain.w));if(i.z<0)return true;if(any(lessThan(i,ivec3(0)))||any(greaterThanEqual(i,pc.grid.xyz)))return false;return mask[(i.z*pc.grid.y+i.y)*pc.grid.x+i.x]!=0;}
#else
const vec3 contact=vec3(0,-8.15,12);
vec3 effectOutward(){return vec3(0,-1,0);}
float box(vec3 p,vec3 b){vec3 q=abs(p)-b;return length(max(q,0))+min(max(q.x,max(q.y,q.z)),0);}
float obstacle(vec3 p,float time) {
    return min(geometryBuilding(p,time-pc.dynamics.z,pc.style.x!=0),geometryCity(p,pc.style.x!=0));
}
bool solidAt(vec3 p,float time){return p.z<0 || obstacle(p,time)<.866025404*pc.domain.w;}
#endif
uint indexOf(ivec3 i){return uint((i.z*pc.grid.y+i.y)*pc.grid.x+i.x);}
bool inside(ivec3 i){return all(greaterThanEqual(i,ivec3(0)))&&all(lessThan(i,pc.grid.xyz));}
vec3 position(ivec3 i){return pc.domain.xyz+(vec3(i)+.5)*pc.domain.w;}
vec3 wind(){return vec3(pc.dynamics.w,pc.settings.x,0);}
Cell ambient(){return Cell(vec4(wind(),300),vec4(0));}
Cell readCell(ivec3 i){if(inside(i))return src[indexOf(i)];Cell c=ambient();c.matter.y=i.z<0?1:0;return c;}
vec4 interpolate(vec3 point,out float tracer) {
    vec3 q=(point-pc.domain.xyz)/pc.domain.w-.5;
    ivec3 base=ivec3(floor(q));vec3 f=fract(q);
    vec4 total=vec4(0);float weights=0;tracer=0;
    for(int z=0;z<2;z++)for(int y=0;y<2;y++)for(int x=0;x<2;x++) {
        ivec3 offset=ivec3(x,y,z);Cell c=readCell(base+offset);
        if(c.matter.y>.5)continue;
        vec3 w=mix(1-f,f,vec3(offset));float weight=w.x*w.y*w.z;
        total+=vec4(c.flow.xyz,c.flow.w-300)*weight;tracer+=c.matter.x*weight;weights+=weight;
    }
    if(weights<1e-5){tracer=0;return vec4(wind(),300);}
    tracer/=weights;return vec4(total.xyz/weights,300+total.w/weights);
}
float pressure(ivec3 i,float centre){Cell c=readCell(i);return c.matter.y>.5?centre:c.matter.z;}
float face(ivec3 i,int axis) {
    if(!inside(i)) {
        ivec3 e=ivec3(0);e[axis]=1;
        if(axis==2 && i.z==-1)return 0; // impermeable ground
        if(i[axis]==-1 && inside(i+e)) {
            Cell n=readCell(i+e);
            return n.matter.y>.5?0:wind()[axis]-n.matter.z/pc.domain.w;
        }
        return wind()[axis];
    }
    Cell c=readCell(i);ivec3 e=ivec3(0);e[axis]=1;
    if(c.matter.y>.5 || readCell(i+e).matter.y>.5)return 0;
    return c.flow[axis];
}
float facePressure(ivec3 i,float centre){if(!inside(i))return i.z<0?centre:0;return pressure(i,centre);}
float sampleFace(vec3 p,int axis) {
    vec3 q=(p-pc.domain.xyz)/pc.domain.w-.5;q[axis]-=.5;
    ivec3 base=ivec3(floor(q));vec3 f=fract(q);float value=0;
    for(int z=0;z<2;z++)for(int y=0;y<2;y++)for(int x=0;x<2;x++) {
        ivec3 offset=ivec3(x,y,z);vec3 w=mix(1-f,f,vec3(offset));
        value+=w.x*w.y*w.z*face(base+offset,axis);
    }
    return value;
}
vec3 sampleVelocity(vec3 p){return vec3(sampleFace(p,0),sampleFace(p,1),sampleFace(p,2));}
#ifdef MESH_TRANSPORT
// Authored moving tracer/heat source. Initial 0.3 s history is an explicit
// initial condition; no motor flow or combustion calculation is implied.
float flightSource(vec3 p,float time){
    if(pc.flightStart.w<.5||time>=pc.dynamics.z||pc.dynamics.z<=0.)return 0.;
    vec3 end=pc.flightStart.w>1.5?intBitsToFloat(pc.style.yzw):contact;
    vec3 direction=normalize(end-pc.flightStart.xyz);
    vec3 nozzle=mix(pc.flightStart.xyz,end,time/pc.dynamics.z)-direction*pc.flightSettings.x;
    vec3 q=p-nozzle;return exp(-dot(q,q)/.72);
}
#endif
vec3 noPenetration(ivec3 i,vec3 velocity,bool provisional) {
    for(int axis=0;axis<3;axis++) {
        ivec3 offset=ivec3(0);offset[axis]=1;
        if(readCell(i+offset).matter.y>.5)velocity[axis]=0;
        else if(!inside(i+offset) && provisional)velocity[axis]=wind()[axis];
    }
    return velocity;
}
void main() {
    ivec3 i=ivec3(gl_GlobalInvocationID.xyz);if(!inside(i))return;
    uint index=indexOf(i);vec3 p=position(i);float dt=pc.dynamics.y,dx=pc.domain.w;
    int stage=pc.grid.w,scenario=int(pc.settings.y+.5);
    if(stage==0) {
        bool solid=solidAt(p,pc.dynamics.x);
        Cell c=ambient();c.matter.y=solid?1:0;if(solid)c.flow.xyz=vec3(0);
        if(scenario==2 && !solid)c.matter.x=exp(-dot(p-vec3(-18,-18,8),p-vec3(-18,-18,8))/8);
#ifdef MESH_TRANSPORT
        if(scenario==0&&!solid&&pc.flightStart.w>.5){for(int k=0;k<12;k++){float time=-pc.flightSettings.w*(float(k)+.5)/12.;float w=flightSource(p,time)*pc.flightSettings.w/12.;c.matter.x+=w*pc.flightSettings.y;c.flow.w+=w*pc.flightSettings.z;}}
#endif
#ifndef IMPACT_EVENT
        if(scenario==0 && pc.dynamics.z<=0) {
            if(solid) {
                bool exposed=false;for(int k=0;k<6;k++)exposed=exposed||!solidAt(p+vec3(directions[k])*pc.domain.w,0);
                if(exposed)c.flow.w+=120*exp(-dot(p-contact,p-contact)/32);
            }
            else {float pulse=exp(-dot(p-contact-effectOutward()*2.,p-contact-effectOutward()*2.)/5);c.flow.w+=550*pulse;c.matter.x+=pulse;}
        }
#endif
        dst[index]=c;return;
    }
    Cell c=src[index];
    if(stage==1) {
        bool solid=solidAt(p,pc.dynamics.x);
        // Newly exposed air starts at ambient rather than inheriting solid heat.
        if(!solid && c.matter.y>.5)c=ambient();
        c.matter.y=solid?1:0;
        bool initialHeat=scenario==0 && pc.settings.z==pc.settings.w;
        if(solid) {
            float temperature=c.flow.w;
#ifndef IMPACT_EVENT
            if(initialHeat)temperature+=120*exp(-dot(p-contact,p-contact)/32);
#endif
            float conduction=0,convection=0;int faces=0;
            for(int k=0;k<6;k++) {
                Cell n=readCell(i+directions[k]);
                if(n.matter.y>.5)conduction+=n.flow.w-temperature;
                else {convection+=12*(temperature-n.flow.w);faces++;}
            }
            if(initialHeat && faces==0)temperature=c.flow.w;
            float radiation=float(faces)*.94*5.670374419e-8*(pow(temperature,4)-pow(300.,4));
            // 3 mm effective surface layer; k/(rho cp) is concrete diffusivity.
            temperature+=dt*(1.4/(2300*880)*conduction/(dx*dx)-(convection+radiation)/(2300*880*.003));
            c.flow=vec4(0,0,0,max(300,temperature));c.matter=vec4(0,1,0,0);
        } else {
            float tracer;vec4 advected=interpolate(p-sampleVelocity(p)*dt,tracer);
            vec3 velocity=vec3(0);float temperature=advected.w;
            for(int axis=0;axis<3;axis++) {
                vec3 facePoint=p;facePoint[axis]+=.5*dx;
                velocity[axis]=sampleFace(facePoint-sampleVelocity(facePoint)*dt,axis);
            }
            float lapT=0,lapSmoke=0,exchange=0;vec3 lapU=vec3(0);
            for(int k=0;k<6;k++) {
                Cell n=readCell(i+directions[k]);
                if(n.matter.y>.5)exchange+=12*(n.flow.w-temperature)/(1206*dx);
                else {lapT+=n.flow.w-c.flow.w;lapSmoke+=n.matter.x-c.matter.x;}
                for(int axis=0;axis<3;axis++)lapU[axis]+=face(i+directions[k],axis)-face(i,axis);
            }
            temperature+=dt*(.12*lapT/(dx*dx)+exchange);
            tracer=max(0,tracer+dt*.08*lapSmoke/(dx*dx));
            velocity+=dt*(.05*lapU/(dx*dx)+.12*(wind()-velocity));
            if(scenario==0) {
#ifdef MESH_TRANSPORT
                float exhaust=flightSource(p,pc.dynamics.x);tracer+=dt*pc.flightSettings.y*exhaust;temperature+=dt*pc.flightSettings.z*exhaust;
                for(int k=0;k<min(fireHeader.x,8);k++){
                    FireSource f=fireSourceAt(k,pc.dynamics.x-pc.dynamics.z);float activity=fireActivity(f,pc.dynamics.x-pc.dynamics.z);if(activity<=0.)continue;
                    float r=f.positionRadius.w;vec3 q=(p-f.positionRadius.xyz-vec3(0,0,.45*r))/vec3(r,.8*r,max(.65,.55*r));
                    float shape=exp(-dot(q,q)*2.)*activity;temperature+=dt*f.flame.z*shape;
                    if(f.flame.w>=1000000.){
                        int profile=int(f.flame.w)/1000000;vec3 release=f.positionRadius.xyz+vec3(0,profile==3?0.:-.25*r,f.flame.x*.5);
                        vec3 smokeQ=(p-release)/vec3(r*1.15,r*.9,max(.75,r*.9));tracer+=dt*f.timing.w*exp(-dot(smokeQ,smokeQ)*2.)*activity;
                    }else tracer+=dt*f.timing.w*shape;
                }
#endif
#ifdef IMPACT_EVENT
                if(initialHeat)temperature+=impactSource.source.x*impactShape(p);
                // Dust is lofted while the fireball burns and cools: released at rate exp(-a/tau)/tau over
                // a in [0, 4 tau) (tau = fireball duration) into the current fireball volume with weight
                // 30/(4 pi R^3) (1 - r/R)^2 (unit integral), R as in the renderer, at least 1.5 cells. The total
                // equals the Gaussian amount the host computed (dust mass x extinction / 0.7).
                {float a=pc.dynamics.x-pc.dynamics.z,tau=max(impactSource.source.w,1e-3);
                 if(a>=0. && a<4.*tau){
                    float rmax=max(impactSource.burst.w,.5),R=max(rmax*(1.-exp(-a/(.1*tau))),1.5*pc.domain.w);
                    vec3 centre=impactSource.burst.xyz+effectOutward()*.3*R+vec3(0,0,.25*rmax*min(a/tau,1.));
                    float x=length(p-centre)/R,s2=impactSource.source.y*impactSource.source.y;
                    float total=impactSource.source.z*pow(6.28318530718*s2,1.5);
                    if(x<1.)tracer+=dt*total*exp(-a/tau)/(tau*(1.-exp(-4.)))*30./(12.5663706144*R*R*R)*(1.-x)*(1.-x);}}
#else
                float a=pc.dynamics.x-pc.dynamics.z;
                vec3 q=p-contact-effectOutward()*2.;float shape=exp(-dot(q,q)/5);
                if(initialHeat){temperature+=550*shape;tracer+=shape;}
                if(a>=0 && a<1.8) {
                    temperature+=dt*900*shape*exp(-a*1.8);
                    tracer+=dt*2*shape*exp(-a*1.8);
                }
#endif
                velocity.z+=dt*9.81*max(0,1-300/max(temperature,300));
                float extinction=.25*tracer;
                temperature-=dt*4*extinction*5.670374419e-8*(pow(max(temperature,300),4)-pow(300.,4))/1206;
            }
            c.flow=vec4(noPenetration(i,velocity,true),clamp(temperature,300,1500));
            c.matter=vec4(max(tracer,0),0,0,0);
        }
    } else if(stage==2) {
        c.matter.z=0;
        c.matter.w=c.matter.y>.5?0:(face(i,0)-face(i-directions[0],0)+
            face(i,1)-face(i-directions[2],1)+face(i,2)-face(i-directions[4],2))/dx;
    } else if(stage==3) {
        if(c.matter.y>.5)c.matter.z=0;
        else {
            float sum=0;for(int k=0;k<6;k++)sum+=facePressure(i+directions[k],c.matter.z);
            c.matter.z=(sum-c.matter.w*dx*dx)/6;
        }
    } else if(stage==4 && c.matter.y<.5) {
        vec3 grad=vec3(facePressure(i+directions[0],c.matter.z)-c.matter.z,
            facePressure(i+directions[2],c.matter.z)-c.matter.z,
            facePressure(i+directions[4],c.matter.z)-c.matter.z)/dx;
        c.flow.xyz=noPenetration(i,c.flow.xyz-grad,false);
    }
    dst[index]=c;
}
