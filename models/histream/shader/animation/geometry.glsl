// Shared render/transport geometry. Breach and removed members are prescribed.
const int CITY_COUNT=13;
const vec3 CITY_CENTRE[CITY_COUNT]=vec3[CITY_COUNT](
    vec3(-43,30,7),vec3(41,35,9),vec3(-7,63,6),vec3(-30,12,8),
    vec3(30,9,10),vec3(-55,7,12),vec3(63,8,8),vec3(-65,40,15),
    vec3(64,57,12),vec3(-41,72,13),vec3(39,82,15),vec3(-9,101,17),vec3(78,25,11));
const vec3 CITY_HALF[CITY_COUNT]=vec3[CITY_COUNT](
    vec3(12,10,7),vec3(13,11,9),vec3(17,9,6),vec3(7,7,8),
    vec3(7,7,10),vec3(10,9,12),vec3(10,9,8),vec3(10,10,15),
    vec3(11,9,12),vec3(10,8,13),vec3(11,10,15),vec3(15,10,17),vec3(9,9,11));
float geometryBox(vec3 p,vec3 b){vec3 q=abs(p)-b;return length(max(q,0))+min(max(q.x,max(q.y,q.z)),0);}
float geometryOpening(vec3 p){return length((p-vec3(0,-8.15,12))/vec3(1,1.25,1.15))-3.9;}
float geometryBreach(vec3 p,float elapsed,bool detailed) {
    if(!detailed)return geometryOpening(p);
    vec3 q=p-vec3(-.35,-8,12.1);
    // A staged damaged asset, formed from intersecting planar cuts. These
    // shapes are prescribed; they are not stress/fracture calculations.
    q.xz=mat2(.9968,-.0799,.0799,.9968)*q.xz;
    float d=geometryBox(q,vec3(2.9,4.8,2.45));
    if(elapsed>.045)d=min(d,geometryBox(p-vec3(-2.9,-8.2,14.25),vec3(1.15,4.1,1.30)));
    if(elapsed>.10)d=min(d,geometryBox(p-vec3(2.65,-7.9,10.35),vec3(1.15,4.2,1.50)));
    if(elapsed>.16)d=min(d,geometryBox(p-vec3(.7,-7.6,15),vec3(1.5,3.7,.9)));
    if(elapsed>.22)d=min(d,geometryBox(p-vec3(-3.6,-8.0,11.1),vec3(.8,3.8,.85)));
    if(elapsed>.1)for(int i=0;i<12;i++) {
        float side=i%2==0?-1:1,angle=.35*sin(float(i)*1.9);
        vec3 local=p-vec3(side*(2.8+.25*sin(float(i)*2.7)),-8,9.7+float(i/2)*.92);
        local.xz=mat2(cos(angle),-sin(angle),sin(angle),cos(angle))*local.xz;
        d=min(d,geometryBox(local,vec3(.42+.12*sin(float(i)*3.2),4,.28+.12*cos(float(i)*1.2))));
    }
    return d;
}
float geometryBuilding(vec3 p,float elapsed,bool detailed) {
    // Hollow interior with visible slabs/columns, rather than a solid block.
    float d=max(geometryBox(p-vec3(0,0,13),vec3(12,8,13)),
               -geometryBox(p-vec3(0,0,13),vec3(11.2,7.2,12.2)));
    float z=clamp(round(p.z/4.3)*4.3,0,25.8);
    d=min(d,geometryBox(p-vec3(0,0,z),vec3(12.18,8.18,.16)));
    for(int x=-1;x<=1;x++)for(int y=-1;y<=1;y+=2)
        d=min(d,geometryBox(p-vec3(float(x)*8,float(y)*5.5,13),vec3(.45,.45,13)));
    // Preset opening removes a local section of facade, slab and columns.
    if(elapsed>=0)d=max(d,-geometryBreach(p,elapsed,detailed));
    d=min(d,geometryBox(p-vec3(-6,2,26.65),vec3(2.6,1.8,.8)));
    d=min(d,geometryBox(p-vec3(5,3,26.5),vec3(2,2,.65)));
    if(detailed) {
        float parapet=max(geometryBox(p-vec3(0,0,26.45),vec3(12,8,.45)),
                          -geometryBox(p-vec3(0,0,26.45),vec3(11.65,7.65,.8)));
        d=min(d,parapet);
        d=min(d,geometryBox(p-vec3(-5,4.5,27.6),vec3(2.2,2.3,1.6)));
        d=min(d,geometryBox(p-vec3(8,3.5,27.05),vec3(1.1,1.8,.8)));
        d=min(d,geometryBox(p-vec3(8,-3,26.35),vec3(1.1,.8,.28)));
        // Entrance canopy and a low service annex break up the block outline.
        d=min(d,geometryBox(p-vec3(-8,-9.1,3.2),vec3(2.5,1.6,.18)));
        d=min(d,geometryBox(p-vec3(-13.1,3,2.5),vec3(1.5,3,2.5)));
    }
    return d;
}
float geometryBuilding(vec3 p,float elapsed){return geometryBuilding(p,elapsed,false);}
float geometryCity(vec3 p,bool detailed) {
    float distance=1e6;
    for(int i=0;i<CITY_COUNT;i++) {
        vec3 c=CITY_CENTRE[i],h=CITY_HALF[i],q=p-c;
        float d=geometryBox(q,h);
        if(detailed) {
            float roof=2*h.z;
            if(i%3==0)d=min(d,geometryBox(p-vec3(c.xy+vec2(-2,1),roof+2.2),vec3(h.x*.58,h.y*.62,2.2)));
            if(i%3==1)d=min(d,geometryBox(p-vec3(c.xy+vec2(h.x*.35,0),roof+1.35),vec3(h.x*.30,h.y*.80,1.35)));
            float parapet=max(geometryBox(p-vec3(c.xy,roof+.35),vec3(h.xy,.35)),
                              -geometryBox(p-vec3(c.xy,roof+.35),vec3(h.xy-.32,.8)));
            d=min(d,parapet);
            d=min(d,geometryBox(p-vec3(c.xy+vec2(h.x*.35,h.y*.2),roof+.75),vec3(1.2,1.6,.75)));
            if(i%4==0)d=min(d,geometryBox(p-vec3(c.x+h.x,c.y+1,2),vec3(2,h.y*.7,2)));
        }
        distance=min(distance,d);
    }
    return distance;
}
float geometryCity(vec3 p){return geometryCity(p,false);}
