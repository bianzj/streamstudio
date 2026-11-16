layout(local_size_x=8,local_size_y=8) in;
layout(binding=0) uniform accelerationStructureEXT sceneAS;
struct Triangle {vec4 a,b,c,na,nb,nc;uvec4 ids;vec4 thermal;};
struct Material {vec4 colorEmissivity;vec4 thermal;};
struct Sample {vec4 hit;vec4 normalRadiance;uvec4 color;};
layout(std430,binding=1) readonly buffer Triangles {Triangle triangles[];};
layout(std430,binding=2) readonly buffer Materials {Material materials[];};
layout(std430,binding=3) writeonly buffer Output {Sample pixels[];};
layout(push_constant) uniform Push {
    ivec4 image;vec4 origin;vec4 forward;vec4 right;vec4 up;vec4 thermal;vec4 light;vec4 reserved;
} pc;
#ifdef MESH_SEQUENCE
#include "sequence_effects.glsl"
#endif
#ifdef MESH_GROUND_THERMAL
#include "ground_sampling.glsl"
#endif

bool trace(vec3 ro,vec3 rd,float far,out uint index,out vec2 bary,out float distance){
    rayQueryEXT q;
    rayQueryInitializeEXT(q,sceneAS,gl_RayFlagsOpaqueEXT,0xff,ro,0.002,rd,far);
    while(rayQueryProceedEXT(q)){}
    bool hit=rayQueryGetIntersectionTypeEXT(q,true)==gl_RayQueryCommittedIntersectionTriangleEXT;
    if(hit){index=rayQueryGetIntersectionPrimitiveIndexEXT(q,true);bary=rayQueryGetIntersectionBarycentricsEXT(q,true);distance=rayQueryGetIntersectionTEXT(q,true);
#ifdef MESH_INSTANCED
        // One BLAS per rigid part: the instance custom index is the part's first triangle in triangles[].
        index+=uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q,true));
#endif
    }
#ifdef MESH_SEQUENCE
    rayQueryEXT background;
    rayQueryInitializeEXT(background,backgroundAS,gl_RayFlagsOpaqueEXT,0xff,ro,.002,rd,hit?distance:far);
    while(rayQueryProceedEXT(background)){}
    if(rayQueryGetIntersectionTypeEXT(background,true)==gl_RayQueryCommittedIntersectionTriangleEXT){
        index=rayQueryGetIntersectionPrimitiveIndexEXT(background,true)|0x80000000u;bary=rayQueryGetIntersectionBarycentricsEXT(background,true);distance=rayQueryGetIntersectionTEXT(background,true);hit=true;
    }
#endif
    return hit;
}
bool blocked(vec3 ro,vec3 rd,float far){
    rayQueryEXT q;
    rayQueryInitializeEXT(q,sceneAS,gl_RayFlagsOpaqueEXT|gl_RayFlagsTerminateOnFirstHitEXT,0xff,ro,0.003,rd,far);
    while(rayQueryProceedEXT(q)){}
    bool hit=rayQueryGetIntersectionTypeEXT(q,true)==gl_RayQueryCommittedIntersectionTriangleEXT;
#ifdef MESH_SEQUENCE
    if(!hit){rayQueryEXT background;rayQueryInitializeEXT(background,backgroundAS,gl_RayFlagsOpaqueEXT|gl_RayFlagsTerminateOnFirstHitEXT,0xff,ro,.003,rd,far);while(rayQueryProceedEXT(background)){}hit=rayQueryGetIntersectionTypeEXT(background,true)==gl_RayQueryCommittedIntersectionTriangleEXT;}
#endif
    return hit;
}
float planck(float t){return 1.191043934e8/(pow(pc.thermal.x,5.0)*(exp(14388.29104/(pc.thermal.x*t))-1.0));}
vec3 sky(vec3 rd){return mix(vec3(.46,.53,.59),vec3(.13,.23,.34),pow(max(rd.z,0.),.6));}
vec3 shading(vec3 p,vec3 n,vec3 rd,Material m){
    if(dot(n,rd)>0)n=-n;
    vec3 light=normalize(pc.light.xyz);
    float visibility=0.;
    for(int i=0;i<4;i++){
        vec3 sun=normalize(light+vec3((i%2==0?-.012:.012),(i<2?-.012:.012),0));
        visibility+=blocked(p+n*.006,sun,500.)?0.:.25;
    }
    vec3 tangent=normalize(cross(abs(n.z)<.9?vec3(0,0,1):vec3(0,1,0),n));vec3 bitangent=cross(n,tangent);
    float ao=0.;
    for(int i=0;i<6;i++){
        float phi=6.2831853*(float(i)+.5)/6.;
        vec3 d=normalize(n*.65+tangent*cos(phi)*.76+bitangent*sin(phi)*.76);
        ao+=blocked(p+n*.006,d,3.)?.055:1./6.;
    }
    vec3 base=m.colorEmissivity.rgb;
    vec3 color=base*(vec3(.20,.235,.28)*ao+vec3(.94,.82,.65)*max(dot(n,light),0.)*visibility);
    float fresnel=pow(1.-max(dot(-rd,n),0.),5.);
    // Generic opaque grey glass; reflection is a sky approximation, not traced transmission.
    if(m.colorEmissivity.a<.9){
        float blend=m.colorEmissivity.a>.8?.24+.55*fresnel:.12;
        color=mix(color,sky(reflect(rd,n)),blend);
        color+=pow(max(dot(reflect(-light,n),-rd),0.),96.)*visibility*.45;
    }
    return color;
}
#ifdef MESH_SEQUENCE
#include "physical_appearance.glsl"
#endif
void main(){
    ivec2 p=ivec2(gl_GlobalInvocationID.xy)+ivec2(0,pc.image.w);
    if(any(greaterThanEqual(p,pc.image.xy)))return;
    uint idx=uint(p.y*pc.image.x+p.x);
    vec3 sumColor=vec3(0);float sumRadiance=0.;
#ifdef MESH_SEQUENCE
    vec3 sumOptical=vec3(0);bool optical=pc.reserved.w>.5;
#endif
    Sample result;result.hit=vec4(-1,-1,-1,pc.thermal.w);result.normalRadiance=vec4(0,0,0,0);result.color=uvec4(0,0xffffffffu,0,0);
    // Record a separate centre ray so diagnostic geometry is independent of AA.
    int n=pc.image.z;
    for(int sampleId=-1;sampleId<n*n;sampleId++){
        vec2 offset=sampleId<0?vec2(.5):(vec2(sampleId%n,sampleId/n)+.5)/float(n);
        vec2 uv=(vec2(p)+offset)/vec2(pc.image.xy)*2.-1.;uv.y=-uv.y;
        vec3 rd=normalize(pc.forward.xyz+pc.right.xyz*uv.x*pc.forward.w*pc.origin.w+pc.up.xyz*uv.y*pc.origin.w);
        uint triangleId;vec2 bary;float t;vec3 color;float radiance;
#ifdef MESH_SEQUENCE
        vec3 opticalLight=vec3(0),physicalColor=vec3(0);float opticalDistance=10000.;
#endif
        if(trace(pc.origin.xyz,rd,10000.,triangleId,bary,t)){
            Triangle tri;
#ifdef MESH_SEQUENCE
            if((triangleId&0x80000000u)!=0u)tri=backgroundTriangles[triangleId&0x7fffffffu];else tri=triangles[triangleId];
#else
            tri=triangles[triangleId];
#endif
            Material m=materials[tri.ids.x];
            vec3 normal=normalize(tri.na.xyz*(1.-bary.x-bary.y)+tri.nb.xyz*bary.x+tri.nc.xyz*bary.y);
            float surfaceK=(dot(cross(tri.b.xyz-tri.a.xyz,tri.c.xyz-tri.a.xyz),rd)<0.?tri.thermal.x:tri.thermal.y)+tri.thermal.z;
#ifdef MESH_GROUND_THERMAL
            surfaceK=groundSurfaceTemperature(pc.origin.xyz+rd*t,normal,tri.ids.x,surfaceK);
#endif
            #ifdef MESH_SEQUENCE
            surfaceK+=wallDelta(pc.origin.xyz+rd*t,tri.ids.w);
#endif
            radiance=m.colorEmissivity.a*planck(surfaceK)+(1.-m.colorEmissivity.a)*planck(pc.thermal.w);
            #ifdef MESH_SEQUENCE
            radiance=volumeTransfer(pc.origin.xyz,rd,t,radiance);
#endif
            if(sampleId<0){result.hit=vec4(t,float(tri.ids.x),float(tri.ids.y),surfaceK);result.normalRadiance=vec4(normal,radiance);result.color.y=triangleId;result.color.z=floatBitsToUint(radiance);continue;}
            color=shading(pc.origin.xyz+rd*t,normal,rd,m);
            // Mild distance haze affects visible presentation only.
            color=mix(color,vec3(.43,.48,.52),1.-exp(-t*.0012));
#ifdef MESH_SEQUENCE
            if(optical){opticalDistance=t;opticalLight=visibleFireLight(pc.origin.xyz+rd*t,normal,rd,m.colorEmissivity.rgb);physicalColor=pc.light.w>.5?physicalShade(pc.origin.xyz+rd*t,normal,rd,m,tri,bary,t):color;}
#endif
        }else{
            radiance=planck(pc.thermal.w);
#ifdef MESH_SEQUENCE
            radiance=volumeTransfer(pc.origin.xyz,rd,10000.,radiance);
#endif
            if(sampleId<0){result.normalRadiance.w=radiance;result.color.z=floatBitsToUint(radiance);continue;}
            color=sky(rd);
#ifdef MESH_SEQUENCE
            physicalColor=color;
#endif
        }
        sumColor+=color;sumRadiance+=radiance;
#ifdef MESH_SEQUENCE
        if(optical)sumOptical+=visibleVolumeTransfer(pc.origin.xyz,rd,opticalDistance,physicalColor+opticalLight);
#endif
    }
    sumColor=pow(clamp(sumColor/float(n*n),0.,1.),vec3(1./2.2));
    result.color.x=packUnorm4x8(vec4(sumColor,1));
#ifdef MESH_SEQUENCE
    if(optical){vec3 linear=max(sumOptical/float(n*n),vec3(0));linearRgb[idx]=vec4(linear,1);vec3 rgb=linear*1.6;rgb=pow(rgb/(1.+rgb),vec3(1./2.2));result.color.w=packUnorm4x8(vec4(rgb,1));}
#endif
    // Radiance includes AA samples, while hit records use the pixel-centre ray.
    result.normalRadiance.w=sumRadiance/float(n*n);
    pixels[idx]=result;
#ifdef MESH_SEQUENCE
    rawRadiance[idx]=result.normalRadiance.w;
#endif
}
