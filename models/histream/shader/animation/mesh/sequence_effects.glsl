// Supplied appearance sources on top of low-speed GPU heat/tracer transport.
// No compressible shock, blast energy or chemical combustion is computed.
struct Cell {vec4 flow,matter;};
layout(binding=7) uniform accelerationStructureEXT backgroundAS;
layout(std430,binding=8) readonly buffer BackgroundTriangles {Triangle backgroundTriangles[];};
layout(std430,binding=4) readonly buffer Field {Cell cells[];};
layout(std430,binding=5) writeonly buffer RawRadiance {float rawRadiance[];};
layout(std430,binding=6) readonly buffer SequenceParameters {vec4 sourcePoint;vec4 fieldOrigin;ivec4 fieldGrid;vec4 wind;vec4 projectile;vec4 projectileStart;vec4 flightEnd;
#ifdef MESH_IMPACT
    // impact-v1: fireball (max radius m, duration s, peak K, growth time s); extra (ambient K, extinction /m, -, visible light scale); flags (powered).
    vec4 fireball;vec4 fireballExtra;vec4 impactFlags;
#endif
} fx;
// Effects are authored around the contact point. fx.wind.z is the azimuth (rad) of the horizontal outward direction, (sin a,-cos a); 0 keeps the original -Y facade bit-for-bit.
vec3 effectOutward(){float a=fx.wind.z;return a==0.?vec3(0,-1,0):vec3(sin(a),-cos(a),0);}
vec3 effectFrame(vec3 d){if(fx.wind.z==0.)return d;float c=cos(fx.wind.z),s=sin(fx.wind.z);return vec3(d.x*c+d.y*s,-d.x*s+d.y*c,d.z);}
#ifdef MESH_IMPACT
// Empirical fireball of the impact-v1 event: radius grows to the W^(1/3) diameter, the radiating temperature cools
// exponentially over the duration, the volume fades afterwards; it vents outward from the burst point by 0.3 R.
float fireballRadius(float age){return fx.fireball.x*(1.-exp(-age/max(fx.fireball.w,1e-4)));}
float fireballFade(float age){float d=fx.fireball.y;return age<=d?1.:exp(-(age-d)/(.3*d));}
float fireballTemperature(float age){return fx.fireballExtra.x+(fx.fireball.z-fx.fireballExtra.x)*exp(-2.*age/fx.fireball.y);}
vec3 fireballCentre(float age){return fx.sourcePoint.xyz+effectOutward()*.3*fireballRadius(age)+vec3(0,0,.25*fx.fireball.x*min(age/fx.fireball.y,1.));}
float fireballFill(vec3 p,float age){return max(0.,1.-length(p-fireballCentre(age))/max(fireballRadius(age),.01));}
bool impactJet(){return fx.impactFlags.x>.5;}
#endif
layout(std430,binding=9)writeonly buffer LinearOptical{vec4 linearRgb[];};
float planck(float t);
bool blocked(vec3 ro,vec3 rd,float far);
#ifdef MESH_FIRE
#ifdef MESH_NATURAL
Cell fieldCell(vec3 p);
#endif
#include "persistent_fire.glsl"
#endif
Cell fieldCell(vec3 p){
    vec3 q=(p-fx.fieldOrigin.xyz)/fx.fieldOrigin.w-.5;ivec3 base=ivec3(floor(q));vec3 f=fract(q);Cell sum=Cell(vec4(0),vec4(0));
    for(int z=0;z<2;z++)for(int y=0;y<2;y++)for(int x=0;x<2;x++){
        ivec3 o=ivec3(x,y,z),i=base+o;vec3 w=mix(1.-f,f,vec3(o));float weight=w.x*w.y*w.z;
        Cell c=Cell(vec4(0,0,0,300),vec4(0));if(all(greaterThanEqual(i,ivec3(0)))&&all(lessThan(i,fx.fieldGrid.xyz)))c=cells[(i.z*fx.fieldGrid.y+i.y)*fx.fieldGrid.x+i.x];sum.flow+=weight*c.flow;sum.matter+=weight*c.matter;
    }
    return sum;
}
float wallDelta(vec3 p,uint flags){
    if(flags!=0u)return 0.;vec3 q=(p-fx.fieldOrigin.xyz)/fx.fieldOrigin.w-.5;ivec3 base=ivec3(floor(q));vec3 f=fract(q);float sum=0.,weight=0.;
    // Interpolate only the solid layer perturbation. Hot air is not wall temperature.
    for(int z=0;z<2;z++)for(int y=0;y<2;y++)for(int x=0;x<2;x++){
        ivec3 o=ivec3(x,y,z),i=base+o;if(any(lessThan(i,ivec3(0)))||any(greaterThanEqual(i,fx.fieldGrid.xyz)))continue;
        Cell c=cells[(i.z*fx.fieldGrid.y+i.y)*fx.fieldGrid.x+i.x];if(c.matter.y<.5)continue;vec3 w=mix(1.-f,f,vec3(o));float a=w.x*w.y*w.z;sum+=a*max(c.flow.w-300.,0.);weight+=a;
    }
    return weight>1e-6?sum/weight:0.;
}
float billow(vec3 p,float age){
    p-=vec3(fx.wind.xy,1.)*age;
    return .68+.20*sin(p.x*1.7+sin(p.z*1.1))+.12*sin(p.y*2.1+p.z*.9+sin(p.x));
}
vec2 medium(vec3 p){
    float age=pc.reserved.x-pc.reserved.y;
    if(age<0.){
        if(fx.projectile.w<.5)return vec2(0,300);
        vec3 d=normalize(fx.sourcePoint.xyz-fx.projectileStart.xyz),a=fx.projectile.xyz-d*3.;float s=clamp(dot(p-a,d),0.,3.);float r=length(p-a-d*s);
        return vec2(.8*exp(-r*r/.032),650.);
    }
    vec3 warp=.65*vec3(sin(p.z*1.1-age*1.3+sin(p.y*.7)),sin(p.x*.9+age*.8+sin(p.z*.8)),sin(p.y*1.2-age*.8+sin(p.x*.6)));
    Cell c=fieldCell(p+warp);float noise=max(.18,billow(p,age));
    float density=c.matter.y>.65?0.:max(c.matter.x,0.)*.95*noise;
    float temp=max(295.,c.flow.w);
    // Authored initial fireball fills the near-field before the coarse plume
    // resolves; its supplied temperature/density are not an energy estimate.
    vec3 centre=fx.sourcePoint.xyz+(effectOutward()*1.6+vec3(0,0,min(age*1.2,3.)));
    float radius=.18+5.0*(1.-exp(-age*7.))*exp(-age*.8);
    float shape=length(effectFrame(p-centre)/vec3(1.,.82,1.05));float fill=max(0.,1.-shape/max(radius,.01));
    float fire=fill*fill*2.5*exp(-age*1.2)*noise;
    if(fire>density){temp=max(temp,330.+620.*exp(-age*.8)*pow(fill,.3));density+=fire;}
    // Fine cool dust appearance; the resolved tracer still comes from transport.
    vec3 dustCentre=fx.sourcePoint.xyz+(effectOutward()*2.8+vec3(0,0,-min(age*1.8,fx.sourcePoint.z-.5)));
    vec3 dq=effectFrame(p-dustCentre)/vec3(3.+age*.6,2.5+age*.35,1.4+age*.15);
    float dust=.22*exp(-dot(dq,dq)*2.)*(1.-exp(-age*4.))*exp(-age*.22)*noise;
    float combined=density+dust;if(combined>1e-6)temp=(density*temp+dust*296.)/combined;
    return vec2(combined,temp);
}
// V2 returns extinction and source radiance, mixing constituent emissions in
// radiance space. The supplied particulate temperature is distinct from hot air.
vec2 separatedMedium(vec3 p){
    float time=pc.reserved.x,age=time-pc.reserved.y;
    vec3 warp=.3*vec3(sin(p.z*1.8-time),sin(p.x*1.5+time),sin(p.y*1.8-time));
    Cell c=fieldCell(p+warp);float noise=max(.2,billow(p,time));
#ifdef MESH_NATURAL
    noise=naturalSmokeTexture(p,time);
#endif
    float smoke=c.matter.y>.65?0.:max(c.matter.x,0.)*.70*noise;
#ifdef MESH_VARIED_FIRE
    if(age>=0.)smoke*=2.1;
#endif
    float particleK=300.+min(max(c.flow.w-300.,0.),100.)*.35;
    float sigma=smoke,emission=smoke*planck(particleK);
#ifdef MESH_IMPACT
    // Fine dust inside the fireball is heated to the local gas temperature (an optically thick dusty fireball
    // radiates at that temperature); outside it keeps the transported-smoke particle temperature.
    if(age>=0.){float f=fireballFill(p,age);if(f>0.){float T=fireballTemperature(age);float local=mix(fx.fireballExtra.x+.3*(T-fx.fireballExtra.x),T,pow(f,.3));
        emission=smoke*planck(mix(particleK,local,smoothstep(0.,.15,f)*fireballFade(age)));}}
#endif
#ifdef MESH_IMPACT
    if(age<0.&&fx.projectile.w>.5&&impactJet()){
#else
    if(age<0.&&fx.projectile.w>.5){
#endif
        vec3 direction=normalize((fx.flightEnd.w>.5?fx.flightEnd.xyz:fx.sourcePoint.xyz)-fx.projectileStart.xyz);
        vec3 nozzle=fx.projectile.xyz-direction*fx.projectileStart.w;
        vec3 q=p-nozzle;float s=-dot(q,direction);
        if(s>0.&&s<2.1){
            float radius=.045+.14*s/2.1;
            float r=length(q+direction*s),taper=1.-smoothstep(1.45,2.1,s);
            // Tapered jet with fine unsteady structure, behind the nozzle only.
            float jet=2.8*exp(-r*r/(radius*radius))*taper*(.82+.18*sin(s*21.-time*47.));
            float temperature=mix(1250.,560.,s/2.1);sigma+=jet;emission+=jet*planck(temperature);
        }
    }else if(age>=0.){
#ifdef MESH_IMPACT
        // Empirical fireball; dust comes from the transported tracer (failed volume x dust fraction).
        float fill=fireballFill(p,age),T=fireballTemperature(age);
        float fire=fill*fill*fx.fireballExtra.y*fireballFade(age)*noise;
        sigma+=fire;emission+=fire*planck(mix(fx.fireballExtra.x+.3*(T-fx.fireballExtra.x),T,pow(fill,.3)));
#else
        vec3 centre=fx.sourcePoint.xyz+(effectOutward()*1.3+vec3(0,0,min(age*.9,2.)));
        float radius=.18+3.2*(1.-exp(-age*8.))*exp(-age*1.1);
        float fill=max(0.,1.-length(effectFrame(p-centre)/vec3(1.,.85,1.1))/max(radius,.01));
        float fire=fill*fill*3.*exp(-age*1.4)*noise;
        sigma+=fire;emission+=fire*planck(350.+650.*exp(-age*1.2)*pow(fill,.3));
#ifndef MESH_IMPACT
        vec3 dustCentre=fx.sourcePoint.xyz+(effectOutward()*2.8+vec3(0,0,-min(age*1.8,fx.sourcePoint.z-.5)));
        vec3 dq=effectFrame(p-dustCentre)/vec3(3.+age*.6,2.5+age*.35,1.4+age*.15);
        float dust=.22*exp(-dot(dq,dq)*2.)*(1.-exp(-age*4.))*exp(-age*.22)*noise;
        sigma+=dust;emission+=dust*planck(296.);
#endif
#endif
    }
#ifdef MESH_FIRE
    vec2 continuous=sustainedInfrared(p);sigma+=continuous.x;emission+=continuous.y;
#endif
    return vec2(sigma,sigma>1e-8?emission/sigma:0.);
}
float volumeTransfer(vec3 ro,vec3 rd,float surfaceDistance,float background){
    if(pc.reserved.z<.5)return background;
    float age=pc.reserved.x-pc.reserved.y;
    vec3 lo,hi;
    bool enhanced=fx.fieldGrid.w!=0;
    if(enhanced){lo=min(fx.projectileStart.xyz,fx.sourcePoint.xyz)-vec3(10,14,12);hi=max(fx.projectileStart.xyz,fx.sourcePoint.xyz)+vec3(18,14,32);}
    else if(age<0.){lo=fx.projectile.xyz-vec3(4);hi=fx.projectile.xyz+vec3(4);}
    else {lo=fx.sourcePoint.xyz+vec3(-10,-14,-12);hi=fx.sourcePoint.xyz+vec3(18,14,32);}
#ifdef MESH_IMPACT
    lo-=vec3(1.3*fx.fireball.x);hi+=vec3(1.3*fx.fireball.x);
#endif
#ifdef MESH_FIRE
    fireBounds(lo,hi);
#endif
    vec3 inv=1./rd;vec3 a=(lo-ro)*inv,b=(hi-ro)*inv;vec3 near=min(a,b),far=max(a,b);
    float start=max(0.,max(near.x,max(near.y,near.z))),finish=min(surfaceDistance,min(far.x,min(far.y,far.z)));
    if(finish<=start)return background;
    if(enhanced){
#ifdef MESH_NATURAL
        float s=start,T=1.,L=0.;
        for(int i=0;i<1536&&s<finish&&T>.008;i++){
            float ds=min(finish-s,max(naturalStepAt(ro+rd*s),(finish-s)/float(1536-i)));
            vec2 m=separatedMedium(ro+rd*(s+.5*ds));float alpha=1.-exp(-max(m.x,0.)*ds);L+=T*alpha*m.y;T*=1.-alpha;s+=ds;
        }return L+T*background;
#else
        float ds=age<0.?.16:.4,T=1.,L=0.;int steps=min(384,int(ceil((finish-start)/ds)));ds=(finish-start)/float(max(steps,1));
        for(int i=0;i<steps&&T>.008;i++){vec3 p=ro+rd*(start+(float(i)+.5)*ds);vec2 m=separatedMedium(p);float alpha=1.-exp(-max(m.x,0.)*ds);L+=T*alpha*m.y;T*=1.-alpha;}
        return L+T*background;
#endif
    }
    float ds=.5,T=1.,L=0.;int steps=min(128,int(ceil((finish-start)/ds)));ds=(finish-start)/float(max(steps,1));
    for(int i=0;i<steps&&T>.008;i++){
        vec3 p=ro+rd*(start+(float(i)+.5)*ds);vec2 m=medium(p);float alpha=1.-exp(-max(m.x,0.)*ds);
        L+=T*alpha*planck(clamp(m.y,150.,1800.));T*=1.-alpha;
    }
    return L+T*background;
}

// Optional visible RGB illustration. These source colors and extinction are
// supplied appearance parameters, not a spectral optical/combustion solution.
bool blocked(vec3 ro,vec3 rd,float far);
vec3 visibleFireLight(vec3 p,vec3 n,vec3 rd,vec3 albedo){
#ifdef MESH_FIRE
    vec3 persistent=sustainedFireLight(p,n,rd,albedo);
#else
    vec3 persistent=vec3(0);
#endif
#ifdef MESH_IMPACT
    float age=pc.reserved.x-pc.reserved.y;if(pc.reserved.z<.5||age<0.||age>3.*fx.fireball.y)return persistent;
    if(dot(n,rd)>0.)n=-n;
    vec3 source=fireballCentre(age);vec3 delta=source-p;float distance=length(delta);
    if(distance<.02||distance>2.*fx.fireball.x+12.)return persistent;vec3 light=delta/distance;
    float cosine=max(dot(n,light),0.);if(cosine==0.||blocked(p+n*.006,light,max(.01,distance-.03)))return persistent;
    float ratio=fireballTemperature(age)/fx.fireball.z;
    float intensity=fx.fireballExtra.w*ratio*ratio*ratio*ratio*fireballFade(age)*(1.-exp(-age*70.))*(1.+fireballRadius(age)*fireballRadius(age)/9.)/(1.+distance*distance);
    return albedo*vec3(1.,.32,.035)*intensity*cosine+persistent;
#else
    float age=pc.reserved.x-pc.reserved.y;if(pc.reserved.z<.5||age<0.||age>2.5)return persistent;
    if(dot(n,rd)>0.)n=-n;
    vec3 source=fx.sourcePoint.xyz+(effectOutward()*1.3+vec3(0,0,min(age*.9,2.)));vec3 delta=source-p;float distance=length(delta);
    if(distance<.02||distance>12.)return persistent;vec3 light=delta/distance;
    float cosine=max(dot(n,light),0.);if(cosine==0.||blocked(p+n*.006,light,max(.01,distance-.03)))return persistent;
    float intensity=18.*exp(-age*2.1)*(1.-exp(-age*70.))/(1.+distance*distance);
    return albedo*vec3(1.,.32,.035)*intensity*cosine+persistent;
#endif
}
vec4 visibleMedium(vec3 p,vec3 rd){
    float time=pc.reserved.x,age=time-pc.reserved.y;
    vec3 warp=.3*vec3(sin(p.z*1.8-time),sin(p.x*1.5+time),sin(p.y*1.8-time));Cell c=fieldCell(p+warp);
    float noise=max(.2,billow(p,time));float smoke=c.matter.y>.65?0.:max(c.matter.x,0.)*.95*noise;
#ifdef MESH_NATURAL
    noise=naturalSmokeTexture(p,time);smoke=c.matter.y>.65?0.:max(c.matter.x,0.)*.95*noise;
#ifdef MESH_VARIED_FIRE
    if(age>=0.)smoke*=2.;
#endif
#endif
    float mu=dot(rd,normalize(pc.light.xyz)),g=.3;float phase=(1.-g*g)/pow(max(.1,1.+g*g-2.*g*mu),1.5);
    vec3 tint=age<0.?vec3(.62,.63,.61):vec3(.31,.29,.26);
    vec3 smokeLight=tint*(vec3(.32,.37,.45)+vec3(.92,.82,.67)*phase*.55);
#ifdef MESH_NATURAL
    if(age>=0.)smokeLight=naturalSmokeLight(p,rd,smoke);
#endif
    float sigma=smoke;vec3 emission=smoke*smokeLight;
#ifdef MESH_IMPACT
    if(age>=0.){float f=fireballFill(p,age);if(f>0.){float ratio=fireballTemperature(age)/fx.fireball.z;
        vec3 flame=mix(vec3(5.,.38,.025),vec3(9.,5.5,.8),smoothstep(.05,.8,f))*ratio*ratio*ratio*ratio;
        emission=smoke*mix(smokeLight,flame,smoothstep(0.,.15,f)*fireballFade(age));}}
#endif
#ifdef MESH_IMPACT
    if(age<0.&&fx.projectile.w>.5&&impactJet()){
#else
    if(age<0.&&fx.projectile.w>.5){
#endif
        vec3 direction=normalize((fx.flightEnd.w>.5?fx.flightEnd.xyz:fx.sourcePoint.xyz)-fx.projectileStart.xyz);
        vec3 nozzle=fx.projectile.xyz-direction*fx.projectileStart.w,q=p-nozzle;float s=-dot(q,direction);
        if(s>0.&&s<2.1){
            float radius=.045+.14*s/2.1,r=length(q+direction*s);
            float jet=2.8*exp(-r*r/(radius*radius))*(1.-smoothstep(1.45,2.1,s))*(.82+.18*sin(s*21.-time*47.));
            vec3 flame=mix(vec3(2.4,3.3,5.),vec3(6.,1.9,.16),smoothstep(.1,.7,s));
            sigma+=jet;emission+=jet*flame;
        }
    }else if(age>=0.){
#ifdef MESH_IMPACT
        float fill=fireballFill(p,age),ratio=fireballTemperature(age)/fx.fireball.z;
        float fire=fill*fill*fx.fireballExtra.y*fireballFade(age)*noise;
        vec3 flame=mix(vec3(5.,.38,.025),vec3(9.,5.5,.8),smoothstep(.05,.8,fill))*ratio*ratio*ratio*ratio;
        sigma+=fire;emission+=fire*flame;
#else
        vec3 centre=fx.sourcePoint.xyz+(effectOutward()*1.3+vec3(0,0,min(age*.9,2.)));
        float radius=.18+3.2*(1.-exp(-age*8.))*exp(-age*1.1);
        float fill=max(0.,1.-length(effectFrame(p-centre)/vec3(1.,.85,1.1))/max(radius,.01));
        float fire=fill*fill*3.*exp(-age*1.4)*noise;
        vec3 flame=mix(vec3(5.,.38,.025),vec3(9.,5.5,.8),smoothstep(.05,.8,fill))*exp(-age*.9);
        sigma+=fire;emission+=fire*flame;
#ifndef MESH_IMPACT
        vec3 dustCentre=fx.sourcePoint.xyz+(effectOutward()*2.8+vec3(0,0,-min(age*1.8,fx.sourcePoint.z-.5)));
        vec3 dq=effectFrame(p-dustCentre)/vec3(3.+age*.6,2.5+age*.35,1.4+age*.15);
        float dust=.45*exp(-dot(dq,dq)*2.)*(1.-exp(-age*4.))*exp(-age*.22)*noise;
        sigma+=dust;emission+=dust*vec3(.44,.38,.29)*(.45+.35*phase);
#endif
#endif
    }
#ifdef MESH_FIRE
    vec4 continuous=sustainedVisible(p);sigma+=continuous.w;emission+=continuous.xyz;
#endif
    return vec4(sigma>1e-8?emission/sigma:vec3(0),sigma);
}
vec3 visibleVolumeTransfer(vec3 ro,vec3 rd,float surfaceDistance,vec3 background){
    if(pc.reserved.z<.5)return background;
    vec3 lo=min(fx.projectileStart.xyz,fx.sourcePoint.xyz)-vec3(10,14,12),hi=max(fx.projectileStart.xyz,fx.sourcePoint.xyz)+vec3(18,14,32);
#ifdef MESH_IMPACT
    lo-=vec3(1.3*fx.fireball.x);hi+=vec3(1.3*fx.fireball.x);
#endif
#ifdef MESH_FIRE
    fireBounds(lo,hi);
#endif
    vec3 inv=mix(vec3(1),vec3(-1),lessThan(rd,vec3(0)))/max(abs(rd),vec3(1e-8));
    vec3 a=(lo-ro)*inv,b=(hi-ro)*inv,near=min(a,b),far=max(a,b);
    float start=max(0.,max(near.x,max(near.y,near.z))),finish=min(surfaceDistance,min(far.x,min(far.y,far.z)));
    if(finish<=start)return background;
#ifdef MESH_NATURAL
    float s=start,T=1.;vec3 L=vec3(0);
    for(int i=0;i<1536&&s<finish&&T>.008;i++){
        float ds=min(finish-s,max(naturalStepAt(ro+rd*s),(finish-s)/float(1536-i)));
        vec4 m=visibleMedium(ro+rd*(s+.5*ds),rd);float alpha=1.-exp(-max(m.w,0.)*ds);L+=T*alpha*m.xyz;T*=1.-alpha;s+=ds;
    }return L+T*background;
#else
    float ds=pc.reserved.x<pc.reserved.y?.16:.4;
    int steps=min(384,int(ceil((finish-start)/ds)));ds=(finish-start)/float(max(steps,1));float T=1.;vec3 L=vec3(0);
    for(int i=0;i<steps&&T>.008;i++){vec4 m=visibleMedium(ro+rd*(start+(float(i)+.5)*ds),rd);float alpha=1.-exp(-max(m.w,0.)*ds);L+=T*alpha*m.xyz;T*=1.-alpha;}
    return L+T*background;
#endif
}
