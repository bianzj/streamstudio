// Generic microfacet RGB material/light model. Values/textures are illustrative;
// it is not measured spectral material data or full multiple-bounce transport.
float materialHash(vec3 p){p=fract(p*.1031);p+=dot(p,p.yzx+33.33);return fract((p.x+p.y)*p.z);}
float materialNoise(vec3 p){
    vec3 i=floor(p),f=fract(p);f=f*f*(3.-2.*f);
    return mix(mix(mix(materialHash(i),materialHash(i+vec3(1,0,0)),f.x),mix(materialHash(i+vec3(0,1,0)),materialHash(i+vec3(1,1,0)),f.x),f.y),mix(mix(materialHash(i+vec3(0,0,1)),materialHash(i+vec3(1,0,1)),f.x),mix(materialHash(i+vec3(0,1,1)),materialHash(i+vec3(1)),f.x),f.y),f.z);
}
float tracerSunVisibility(vec3 p,vec3 light){
    if(pc.reserved.z<.5||pc.reserved.x<pc.reserved.y||distance(p,fx.sourcePoint.xyz)>28.)return 1.;
    float tau=0.;for(int i=0;i<8;i++){Cell c=fieldCell(p+light*(.4+(float(i)+.5)*2.));if(c.matter.y<.5)tau+=max(c.matter.x,0.)*.95*2.;}
    return exp(-min(tau,12.));
}
vec3 physicalShade(vec3 p,vec3 n,vec3 rd,Material m,Triangle tri,vec2 bary,float distance){
    if(dot(n,rd)>0.)n=-n;vec3 v=-rd,l=normalize(pc.light.xyz),halfVector=v+l;
    vec3 h=dot(halfVector,halfVector)>1e-12?normalize(halfVector):n;
    float rough=m.thermal.y>0.?m.thermal.y:.6,metal=clamp(m.thermal.z,0.,1.),ior=m.thermal.w>1.?m.thermal.w:1.5;
    vec3 coordinates=p;
    if((tri.ids.w&2u)!=0u){
        // Intrinsic triangle metric coordinates stay attached to moving chunks.
        vec3 e=tri.b.xyz-tri.a.xyz,f=tri.c.xyz-tri.a.xyz;float lengthE=max(length(e),1e-5),x=dot(f,e)/lengthE;
        coordinates=vec3(bary.x*lengthE+bary.y*x,bary.y*sqrt(max(0.,dot(f,f)-x*x)),float(tri.ids.z)*.731);
    }
    float footprint=2.*distance*pc.origin.w/float(pc.image.y);
    float broad=materialNoise(coordinates*1.4)-.5;
    float grain=(materialNoise(coordinates*14.)-.5)*(1.-smoothstep(.3,.8,footprint*14.));
    float amount=tri.ids.w==4u?.015:((tri.ids.w&2u)!=0u?.025:.10);
    vec3 albedo=clamp(m.colorEmissivity.rgb*(1.+amount*broad+.025*grain),vec3(.005),vec3(.98));
    rough=clamp(rough*(1.+.12*broad),.04,1.);float a=rough*rough,a2=a*a;
    float nl=max(dot(n,l),0.),nv=max(dot(n,v),.001),nh=max(dot(n,h),0.),vh=max(dot(v,h),0.);
    vec3 f0=mix(vec3(pow((ior-1.)/(ior+1.),2.)),albedo,metal);vec3 fresnel=f0+(1.-f0)*pow(1.-vh,5.);
    float denominator=nh*nh*(a2-1.)+1.;float D=a2/(3.14159265*denominator*denominator);
    float k=(rough+1.)*(rough+1.)/8.;float G=(nv/(nv*(1.-k)+k))*(nl/(nl*(1.-k)+k));
    vec3 specular=D*G*fresnel/max(4.*nv*max(nl,.001),.001),diffuse=(1.-fresnel)*(1.-metal)*albedo/3.14159265;
    float shadow=0.;for(int i=0;i<4;i++){vec3 s=normalize(l+vec3(i%2==0?-.0045:.0045,i<2?-.0045:.0045,0));shadow+=blocked(p+n*.006,s,500.)?0.:.25;}
    vec3 tangent=normalize(cross(abs(n.z)<.9?vec3(0,0,1):vec3(0,1,0),n)),bitangent=cross(n,tangent);float ao=0.;
    for(int i=0;i<4;i++){float phi=6.2831853*(float(i)+.5)/4.;vec3 d=normalize(n*.72+tangent*cos(phi)*.7+bitangent*sin(phi)*.7);ao+=blocked(p+n*.006,d,3.)?.07:.25;}
    vec3 skyAmbient=mix(vec3(.16,.18,.21),vec3(.27,.34,.45),max(n.z,0.));
    vec3 reflection=sky(reflect(rd,n))*(f0+(1.-f0)*pow(1.-nv,5.))*(1.-.65*rough);
    vec3 direct=(diffuse+specular)*vec3(3.8,3.45,2.95)*nl*shadow*tracerSunVisibility(p+n*.02,l);
    vec3 color=direct+albedo*skyAmbient*ao*(1.-metal)+reflection*ao;
#ifdef MESH_NATURAL
    if((tri.ids.w&16u)!=0u){
        // Thin-leaf proxy for diffuse backlighting and unresolved canopy scatter.
        float back=max(dot(-n,l),0.),visibility=blocked(p+l*.015,l,500.)?.15:1.;
        color+=albedo*vec3(3.8,3.45,2.95)*back*.22*visibility;
        color+=albedo*skyAmbient*max(.45-ao,0.);
    }
#endif
    // Homogeneous aerial attenuation in RGB scene units, with sky in-scatter.
    vec3 transmittance=exp(-vec3(.0014,.0019,.0027)*distance);return color*transmittance+sky(rd)*(1.-transmittance);
}
