// Independent source profiles. The facade profiles use the synthetic scene's
// X horizontal / -Y outward convention; parameters are supplied appearance.
vec4 flameSample(FireSource f,vec3 p,int id){
    float age=pc.reserved.x-pc.reserved.y,activity=fireActivity(f,age);if(activity<=0.)return vec4(0,300,0,700);
    int packed=int(f.flame.w),profile=packed/1000000;float seed=float(packed%1000000);
    if(profile<1)profile=id%3+1;float phase=6.2831853*fireHash(vec3(seed*.017,float(profile)*2.3,seed*.003));
    float time=max(0.,age-f.timing.x),r=f.positionRadius.w,h=f.flame.x;vec3 q=p-f.positionRadius.xyz;
    float frequency=profile==1?4.7:profile==2?1.9:3.1;
    float breath=.78+.15*sin(time*frequency+phase)+.07*sin(time*frequency*1.71+phase*1.3);
    h*=profile==1?(1.05+.20*breath):profile==2?(.72+.22*breath):(.45+.22*breath);
    if(q.z<-.2||q.z>h*1.65||dot(q.xy,q.xy)>9.*r*r)return vec4(0,300,0,700);
    float z=max(q.z,0.),height=z/h;vec3 velocity=fieldCell(p).flow.xyz;
    vec3 advected=vec3(q.xy/r,(z-(2.0+float(profile)*.45+.25*clamp(velocity.z,0.,3.))*time)/h)*vec3(2.5,3.1,3.3)+vec3(phase,seed*.003,phase*1.7);
    float broad=fireFbm(advected),fine=fireFbm(advected*1.83+vec3(7.2,2.4,5.1));
    vec2 wind=(fx.wind.xy*(.10+.035*float(profile))+clamp(velocity.xy,vec2(-3),vec2(3))*.08)*z;
    vec2 curl=r*(profile==1?.42:profile==2?.24:.31)*vec2(sin(z*3.7-time*frequency+phase),sin(z*2.3-time*frequency*.79+phase*1.6))*smoothstep(0.,.4,height);
    vec2 axes=profile==1?vec2(1.45,.52):profile==2?vec2(1.12,.90):vec2(1.55,1.25);
    vec2 q2=(q.xy-wind+curl+r*1.45*vec2(broad-.5,fine-.5))/(r*axes);float fuel=0.;
    if(profile==1){for(int k=0;k<3;k++){vec2 d=q2-vec2((float(k)-1.)*.44,.10*sin(phase+float(k)));fuel=max(fuel,exp(-dot(d,d)/(.16+.13*height)));}fuel*=exp(-height*.75);}
    else if(profile==2){vec2 d=q2-vec2(.22*sin(phase+time),.16*cos(time*.7+phase));fuel=exp(-dot(d,d)/(.55+.30*height))*exp(-height*.66);}
    else {for(int k=0;k<5;k++){float angle=float(k)*2.39996+phase;vec2 d=q2-vec2(cos(angle),sin(angle))*.42;fuel+=.43*exp(-dot(d,d)/.14);}fuel*=exp(-height*.95);}
    float threshold=(profile==2?.10:.15)+(.36+.07*float(profile))*broad+.12*(fine-.5);
    float sheet=exp(-pow((fuel-threshold)/(profile==2?.22:.14),2.))*smoothstep(.04,.22,fuel);
    float envelope=1.-smoothstep(1.05,1.65,height);
    float flame=activity*envelope*((profile==2?1.8:1.45)*sheet*(.5+.5*fine)+.33*fuel*smoothstep(.18,.7,fine))*breath;
    float temperature=clamp(mix(520.,f.flame.y,sheet*(.70+.30*fine))-height*(45.+float(profile)*18.),350.,f.flame.y);
    float ember=activity*.75*exp(-dot(q.xy/(r*axes),q.xy/(r*axes))/.7-pow(q.z/.16,2.))*(.45+.55*fireFbm(q*4.+vec3(phase,0,time*.5)));
    return vec4(flame,temperature,ember,700.+110.*fine);
}
