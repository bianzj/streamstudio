#define FIRE_BINDING 10
#include "../fire_sources.glsl"
void fireBounds(inout vec3 lo,inout vec3 hi){
    for(int i=0;i<min(fireHeader.x,8);i++){FireSource f=fireSources[i];if(pc.reserved.x-pc.reserved.y<f.timing.x)continue;float r=f.positionRadius.w,h=f.flame.x;
        float top=2.;
#ifdef MESH_VARIED_FIRE
        top=2.3;
#endif
        lo=min(lo,f.positionRadius.xyz-vec3(3.*r+h,3.*r+h,.25));hi=max(hi,f.positionRadius.xyz+vec3(3.*r+h,3.*r+h,top*h));}
}
#ifdef MESH_NATURAL
#include "natural_fire.glsl"
#else
vec4 flameSample(FireSource f,vec3 p,int id){
    float age=pc.reserved.x-pc.reserved.y,activity=fireActivity(f,age);if(activity<=0.)return vec4(0,300,0,700);
    vec3 q=p-f.positionRadius.xyz;float r=f.positionRadius.w,h=f.flame.x,time=pc.reserved.x;
    if(q.z<-.2||q.z>h*1.3||dot(q.xy,q.xy)>9.*r*r)return vec4(0,300,0,700);
    float seed=float(id)*2.37;h*=.92+.08*sin(time*5.3+seed)+.06*sin(time*11.7+seed*1.3);
    float z=max(q.z,0.),height=z/max(h,.1);
    vec2 centre=fx.wind.xy*z*.23+.13*r*vec2(sin(z*4.-time*6.+seed),sin(z*3.1-time*5.1+seed));
    float radius=r*max(.14,1.-height*.8),radial=length(q.xy-centre)/radius;
    float turbulence=clamp(.65+.22*sin(q.x*5.+q.y*3.-time*9.+z*5.)+.18*sin(q.y*7.+z*6.-time*13.+seed),.15,1.);
    float flame=activity*3.5*exp(-radial*radial*2.)*pow(max(0.,1.-height),.55)*turbulence;
    float hotCore=exp(-radial*radial)*pow(max(0.,1.-height),.3);
    float temperature=mix(520.,f.flame.y,hotCore);
    float ember=activity*.7*exp(-dot(q.xy,q.xy)/(r*r*.65)-pow(q.z/.18,2.));
    return vec4(flame,temperature,ember,780.+60.*turbulence);
}
vec2 sustainedInfrared(vec3 p){
    float extinction=0.,emission=0.;
    for(int i=0;i<min(fireHeader.x,8);i++){vec4 s=flameSample(fireSources[i],p,i);extinction+=s.x+s.z;emission+=s.x*planck(s.y)+s.z*planck(s.w);}
    return vec2(extinction,emission);
}
vec4 sustainedVisible(vec3 p){
    float extinction=0.;vec3 emission=vec3(0);
    for(int i=0;i<min(fireHeader.x,8);i++){vec4 s=flameSample(fireSources[i],p,i);
        vec3 color=mix(vec3(3.,.22,.008),vec3(9.,5.,.6),smoothstep(650.,1150.,s.y));
        extinction+=s.x+s.z;emission+=s.x*color+s.z*vec3(2.,.12,.008);}
    return vec4(emission,extinction);
}
vec3 sustainedFireLight(vec3 p,vec3 n,vec3 rd,vec3 albedo){
    if(pc.reserved.z<.5)return vec3(0);if(dot(n,rd)>0.)n=-n;vec3 sum=vec3(0);
    for(int i=0;i<min(fireHeader.x,8);i++){
        FireSource f=fireSources[i];float a=fireActivity(f,pc.reserved.x-pc.reserved.y);if(a<=0.)continue;
        vec3 q=f.positionRadius.xyz+vec3(0,0,f.flame.x*.35)-p;float distance=length(q);if(distance<.03||distance>14.)continue;
        vec3 d=q/distance;float nl=max(dot(n,d),0.);if(nl<=0.||blocked(p+n*.006,d,max(.01,distance-.04)))continue;
        sum+=albedo*vec3(1.,.27,.018)*(8.*f.positionRadius.w*a*nl/(1.+distance*distance));
    }return sum;
}
#endif
