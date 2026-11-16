#define GROUND_PARAMS_BINDING 12
#define GROUND_FIELD_BINDING 11
#define GROUND_FIELD_ACCESS readonly
#include "../ground_layout.glsl"
float groundSurfaceTemperature(vec3 p,vec3 normal,uint material,float fallback){
    if(material>=uint(groundSize.z)||groundMaterials[material].flags.y<.5||normal.z<.95||p.z>groundSettings.x)return fallback;
    vec2 q=(p.xy-groundDomain.xy)/groundDomain.z-.5;ivec2 a=ivec2(floor(q));vec2 f=fract(q);float total=0.,weight=0.;
    for(int y=0;y<2;y++)for(int x=0;x<2;x++){ivec2 c=a+ivec2(x,y);if(any(lessThan(c,ivec2(0)))||any(greaterThanEqual(c,groundSize.xy)))continue;
        GroundCell g=groundCells[c.y*groundSize.x+c.x];if(g.point.w<0.||uint(g.point.w)!=material||abs(g.point.z-p.z)>.35)continue;
        float w=(x==0?1.-f.x:f.x)*(y==0?1.-f.y:f.y);total+=w*g.state.y;weight+=w;
    }return weight>1e-6?total/weight:fallback;
}
