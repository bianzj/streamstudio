// Shared supplied-source schedule. No fuel/oxygen/ignition calculation.
struct FireSource {vec4 positionRadius,timing,flame;};
layout(std430,binding=FIRE_BINDING)readonly buffer FireSources {ivec4 fireHeader;FireSource fireSources[];};
uint fireGrowthCode(int id){return (uint(fireHeader[1+id/4])>>uint(8*(id%4)))&255u;}
float fireGrowth(int id,float age){uint code=fireGrowthCode(id);if(code==0u)return 1.;return smoothstep(0.,float(code)*.1,max(age-fireSources[id].timing.x,0.));}
float fireReleaseScale(int id,float age){return fireGrowthCode(id)==0u?1.:mix(.03,1.,fireGrowth(id,age));}
FireSource fireSourceAt(int id,float age){
    FireSource f=fireSources[id];if(fireGrowthCode(id)==0u)return f;
    float g=fireGrowth(id,age),rate=mix(.03,1.,g);f.positionRadius.w*=mix(.20,1.,g);f.flame.x*=mix(.15,1.,g);f.flame.z*=rate;f.timing.w*=rate;return f;
}
float fireActivity(FireSource f,float age){
    float start=f.timing.x,end=start+f.timing.y;
    return smoothstep(start,start+min(.5,.3*f.timing.y),age)*(1.-smoothstep(end-f.timing.z,end,age));
}
