// Opt-in appearance detail driven by existing low-speed fields. This is not
// reactive combustion or a measured flame/spectral scattering model.
float fireHash(vec3 p){p=fract(p*.1031);p+=dot(p,p.yzx+33.33);return fract((p.x+p.y)*p.z);}
float fireNoise(vec3 p){
    vec3 i=floor(p),f=fract(p);f=f*f*(3.-2.*f);
    return mix(mix(mix(fireHash(i),fireHash(i+vec3(1,0,0)),f.x),mix(fireHash(i+vec3(0,1,0)),fireHash(i+vec3(1,1,0)),f.x),f.y),mix(mix(fireHash(i+vec3(0,0,1)),fireHash(i+vec3(1,0,1)),f.x),mix(fireHash(i+vec3(0,1,1)),fireHash(i+vec3(1)),f.x),f.y),f.z);
}
float fireFbm(vec3 p){return .57*fireNoise(p)+.29*fireNoise(p*2.07+vec3(5.2,1.1,3.7))+.14*fireNoise(p*4.21+vec3(2.8,7.1,1.9));}
float naturalSmokeTexture(vec3 p,float time){return .45+.90*fireFbm(p*1.15-vec3(fx.wind.xy,1.5)*time);}
float naturalStepAt(vec3 p){
    float step=pc.reserved.x<pc.reserved.y?.16:.35;
    if(pc.reserved.x<pc.reserved.y&&fx.projectile.w>.5&&fx.projectileStart.w>.5){
        vec3 direction=normalize((fx.flightEnd.w>.5?fx.flightEnd.xyz:fx.sourcePoint.xyz)-fx.projectileStart.xyz);
        vec3 nozzle=fx.projectile.xyz-direction*fx.projectileStart.w,q=p-nozzle;float s=-dot(q,direction);
        if(s>-.3&&s<2.4&&length(q+direction*s)<.45)step=min(step,.025+.025*clamp(s/2.1,0.,1.));
    }
    float top=1.7;
#ifdef MESH_VARIED_FIRE
    top=2.2;
#endif
    for(int i=0;i<min(fireHeader.x,8);i++){FireSource f=fireSourceAt(i,pc.reserved.x-pc.reserved.y);if(fireActivity(f,pc.reserved.x-pc.reserved.y)<=0.)continue;vec3 q=p-f.positionRadius.xyz;float r=f.positionRadius.w;
        if(fireGrowthCode(i)>0u&&fireGrowth(i,pc.reserved.x-pc.reserved.y)<1.){if(q.z>-.35&&q.z<f.flame.x*top+.35&&length(q.xy)<3.*r+.35)step=min(step,max(.01,.065*r));}
        else if(q.z>-.35&&q.z<f.flame.x*top&&dot(q.xy,q.xy)<9.*r*r)step=min(step,.065*r);}
    return step;
}
vec3 naturalSmokeLight(vec3 p,vec3 rd,float density){
    float tau=0.;vec3 sun=normalize(pc.light.xyz);
    if(density>.01)for(int k=0;k<6;k++){Cell c=fieldCell(p+sun*(.6+float(k)*2.));if(c.matter.y<.5)tau+=max(c.matter.x,0.)*1.9;}
#ifdef MESH_VARIED_FIRE
    tau*=2.;
#endif
    float mu=dot(rd,sun),g=.3,phase=(1.-g*g)/pow(max(.1,1.+g*g-2.*g*mu),1.5);
    return vec3(.23,.22,.20)*(vec3(.18,.20,.24)+vec3(.70,.60,.45)*phase*.35*exp(-min(tau,12.)));
}
#ifdef MESH_VARIED_FIRE
#include "varied_fire.glsl"
#else
vec4 flameSample(FireSource f,vec3 p,int id){
    float time=pc.reserved.x,age=time-pc.reserved.y,activity=fireActivity(f,age);if(activity<=0.)return vec4(0,300,0,700);
    vec3 q=p-f.positionRadius.xyz;float r=f.positionRadius.w,h=f.flame.x;
    if(q.z<-.2||q.z>h*1.6||dot(q.xy,q.xy)>9.*r*r)return vec4(0,300,0,700);
    vec3 flow=fieldCell(p).flow.xyz;float z=max(q.z,0.),height=z/h;
    vec3 advected=vec3(q.xy/r,(z-(2.8+.4*clamp(flow.z,0.,3.))*time)/h)*2.8+vec3(float(id)*3.7,0,0);
    float broad=fireFbm(advected),fine=fireFbm(advected*1.73+vec3(3.1,7.5,1.2));
    vec2 drift=(fx.wind.xy*.16+clamp(flow.xy,vec2(-3),vec2(3))*.10)*z;
    vec2 curl=.32*r*vec2(sin(z*3.1-time*5.+float(id)),sin(z*2.7-time*4.3+float(id)*1.7))*smoothstep(0.,.4,height);
    vec2 radial=(q.xy-drift+curl+r*1.6*vec2(broad-.5,fine-.5))/r;
    float spread=.45+.45*sqrt(height+.05);
    float fuel=max(exp(-dot(radial-vec2(.22,.12),radial-vec2(.22,.12))/(spread*spread)),.86*exp(-dot(radial+vec2(.30,.13),radial+vec2(.30,.13))/(spread*spread*.78)))*exp(-height*.85);
    float threshold=.14+.48*broad+.12*(fine-.5);
    // Irregular emitting sheets and intermittent tips rather than a solid cone.
    float sheet=exp(-pow((fuel-threshold)/.15,2.))*smoothstep(.05,.24,fuel);
    float envelope=1.-smoothstep(1.,1.6,height);
    float flame=activity*envelope*(1.5*sheet*(.55+.45*fine)+.38*fuel*smoothstep(.2,.7,fine));
    float temperature=clamp(mix(540.,f.flame.y,sheet*(.76+.24*fine))-height*65.,350.,f.flame.y);
    float ember=activity*.55*exp(-dot(q.xy,q.xy)/(r*r*.65)-pow(q.z/.14,2.))*(.55+.45*fireFbm(q*5.+vec3(0,0,time*.4)));
    return vec4(flame,temperature,ember,710.+100.*fine);
}
#endif
vec2 sustainedInfrared(vec3 p){
    float extinction=0.,emission=0.;
    for(int i=0;i<min(fireHeader.x,8);i++){vec4 s=flameSample(fireSourceAt(i,pc.reserved.x-pc.reserved.y),p,i);extinction+=s.x+s.z;emission+=s.x*planck(s.y)+s.z*planck(s.w);}
    return vec2(extinction,emission);
}
vec4 sustainedVisible(vec3 p){
    float extinction=0.;vec3 emission=vec3(0);
    for(int i=0;i<min(fireHeader.x,8);i++){vec4 s=flameSample(fireSourceAt(i,pc.reserved.x-pc.reserved.y),p,i);vec3 color=mix(vec3(1.4,.06,.003),vec3(5.2,1.95,.12),smoothstep(650.,1200.,s.y));extinction+=s.x+s.z;emission+=s.x*color+s.z*vec3(1.2,.045,.002);}
    return vec4(emission,extinction);
}
vec3 sustainedFireLight(vec3 p,vec3 n,vec3 rd,vec3 albedo){
    if(pc.reserved.z<.5)return vec3(0);if(dot(n,rd)>0.)n=-n;vec3 sum=vec3(0);
    for(int i=0;i<min(fireHeader.x,8);i++){
        FireSource f=fireSourceAt(i,pc.reserved.x-pc.reserved.y);float a=fireActivity(f,pc.reserved.x-pc.reserved.y);if(a<=0.)continue;if(fireGrowthCode(i)>0u)a*=fireReleaseScale(i,pc.reserved.x-pc.reserved.y);
        vec3 q=f.positionRadius.xyz+vec3(0,0,f.flame.x*.32)-p;float distance=length(q);if(distance<.03||distance>14.)continue;
        vec3 d=q/distance;float nl=max(dot(n,d),0.);if(nl<=0.||blocked(p+n*.006,d,max(.01,distance-.04)))continue;
        float flicker=.82+.11*sin(pc.reserved.x*5.7+float(i))+.07*sin(pc.reserved.x*11.3+float(i)*1.7);
#ifdef MESH_VARIED_FIRE
        int packed=int(f.flame.w+.5),profile=packed/1000000;float seed=float(packed-profile*1000000),phase=seed*.017;
        float frequency=profile==1?4.7:profile==2?1.9:3.1;
        flicker=.78+.15*sin((pc.reserved.x-pc.reserved.y-f.timing.x)*frequency+phase)+.07*sin((pc.reserved.x-pc.reserved.y-f.timing.x)*frequency*1.71+phase*1.3);
#endif
        sum+=albedo*vec3(1.,.23,.01)*(7.*f.positionRadius.w*a*flicker*nl/(1.+distance*distance));
    }return sum;
}
