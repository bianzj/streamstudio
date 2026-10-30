#ifndef FUNCTIONS_GLSL
#define FUNCTIONS_GLSL

#include "global.glsl"
#include "parameters.h"
//--------------------------------------------------------------------------------------------
// Common functions
//--------------------------------------------------------------------------------------------

// Central domain is index 0. Indices 1..20 identify successive boundary
// passages. Geometry buffers are never duplicated; ray tracers remap every
// query into the same acceleration structure.
ivec2 periodicTileOffset(int index)
{
  const ivec2 offsets[20] = ivec2[20](
    ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1),
    ivec2(-1, -1), ivec2(-1, 1), ivec2(1, -1), ivec2(1, 1),
    ivec2(-2, 0), ivec2(2, 0), ivec2(0, -2), ivec2(0, 2),
    ivec2(-2, -1), ivec2(-2, 1), ivec2(2, -1), ivec2(2, 1),
    ivec2(-1, -2), ivec2(1, -2), ivec2(-1, 2), ivec2(1, 2));
  return index <= 0 ? ivec2(0) : offsets[clamp(index - 1, 0, 19)];
}

// Returns the periodic tile and the ray-parameter interval over which the ray
// is physically inside that tile. Restricting intersection tests to this
// interval prevents a wrapped query from replacing a nearer hit that still
// belongs to the central study area.
ivec2 periodicRayTileSegment(vec3 origin, vec3 direction,
                             int traversalIndex, vec2 requestedTileSize,
                             out float segmentMinimum, out float segmentMaximum)
{
  const float huge = 1.0e30;
  vec2 tileSize = max(requestedTileSize, vec2(0.01));
  vec2 position = vec2(origin.x, origin.z) / tileSize + vec2(0.5);
  vec2 ray = vec2(direction.x, direction.z) / tileSize;
  ivec2 cell = ivec2(floor(position));
  ivec2 stepDirection = ivec2(
    ray.x > 0.0 ? 1 : (ray.x < 0.0 ? -1 : 0),
    ray.y > 0.0 ? 1 : (ray.y < 0.0 ? -1 : 0));
  vec2 nextBoundary = vec2(
    stepDirection.x > 0 ? float(cell.x + 1) : float(cell.x),
    stepDirection.y > 0 ? float(cell.y + 1) : float(cell.y));
  vec2 nextCrossing = vec2(
    stepDirection.x == 0 ? huge : (nextBoundary.x - position.x) / ray.x,
    stepDirection.y == 0 ? huge : (nextBoundary.y - position.y) / ray.y);
  vec2 crossingStep = vec2(
    stepDirection.x == 0 ? huge : abs(1.0 / ray.x),
    stepDirection.y == 0 ? huge : abs(1.0 / ray.y));

  segmentMinimum = 0.0;
  for(int crossing = 0; crossing < traversalIndex; ++crossing)
  {
    segmentMinimum = min(nextCrossing.x, nextCrossing.y);
    float difference = abs(nextCrossing.x - nextCrossing.y);
    float tolerance = 1.0e-5 * max(1.0, min(nextCrossing.x, nextCrossing.y));
    if(difference <= tolerance)
    {
      cell += stepDirection;
      nextCrossing += crossingStep;
    }
    else if(nextCrossing.x < nextCrossing.y)
    {
      cell.x += stepDirection.x;
      nextCrossing.x += crossingStep.x;
    }
    else
    {
      cell.y += stepDirection.y;
      nextCrossing.y += crossingStep.y;
    }
  }
  segmentMaximum = min(nextCrossing.x, nextCrossing.y);
  return cell;
}

// Deterministic low-frequency sky used only for sensor-image misses. It is a
// visual background and is intentionally excluded from shadow and scattering
// queries.
float proceduralSkyCloudAmount(vec3 direction)
{
  vec3 d = normalize(direction);
  float azimuth = atan(d.z, d.x);
  float elevation = asin(clamp(d.y, -1.0, 1.0));
  float pattern = 0.5
    + 0.23 * sin(3.1 * azimuth + 1.7 * sin(2.2 * elevation))
    + 0.17 * sin(6.7 * azimuth - 3.4 * elevation + 0.8)
    + 0.10 * sin(13.0 * azimuth + 5.0 * elevation);
  float aboveHorizon = smoothstep(-0.04, 0.18, d.y);
  return smoothstep(0.58, 0.76, pattern) * aboveHorizon;
}

float proceduralSkyReflectance(float wavelength, vec3 direction)
{
  float wavelengthUm = wavelength > 50.0 ? wavelength / 1000.0 : wavelength;
  vec3 d = normalize(direction);
  float height = clamp(d.y * 0.5 + 0.5, 0.0, 1.0);
  float blueResponse = exp(-pow((wavelengthUm - 0.47) / 0.19, 2.0));
  float clearSky = mix(0.34, 0.10, height)
    + blueResponse * mix(0.16, 0.38, height);
  float cloud = proceduralSkyCloudAmount(d);
  return clamp(mix(clearSky, 0.82, cloud), 0.0, 0.95);
}

float proceduralSkyTemperature(vec3 direction, float clearSkyTemperature)
{
  vec3 d = normalize(direction);
  float cloud = proceduralSkyCloudAmount(d);
  float horizonWarmth = 8.0 * (1.0 - clamp(d.y, 0.0, 1.0));
  return clearSkyTemperature + horizonWarmth + cloud * 22.0;
}


//-------------------------------------------------------------------------------------------------
// Avoiding self intersections (see Ray Tracing Gems, Ch. 6)
//-------------------------------------------------------------------------------------------------
vec3 OffsetRay(in vec3 p, in vec3 n)
{
  const float intScale   = 256.0f;
  const float floatScale = 1.0f / 65536.0f;
  const float origin     = 1.0f / 32.0f;

  ivec3 of_i = ivec3(intScale * n.x, intScale * n.y, intScale * n.z);

  vec3 p_i = vec3(intBitsToFloat(floatBitsToInt(p.x) + ((p.x < 0) ? -of_i.x : of_i.x)),
                  intBitsToFloat(floatBitsToInt(p.y) + ((p.y < 0) ? -of_i.y : of_i.y)),
                  intBitsToFloat(floatBitsToInt(p.z) + ((p.z < 0) ? -of_i.z : of_i.z)));

  return vec3(abs(p.x) < origin ? p.x + floatScale * n.x : p_i.x,  //
              abs(p.y) < origin ? p.y + floatScale * n.y : p_i.y,  //
              abs(p.z) < origin ? p.z + floatScale * n.z : p_i.z);
}

//-------------------------------------------------------------------------------------------------
// From tangent space/local space to world space and from world space to tangent space/ local space
//-------------------------------------------------------------------------------------------------
vec3 World2Tangent(vec3 normal,vec3 tangent,vec3 binormal,vec3 v)
{
    return vec3(dot(tangent,v),dot(binormal,v),dot(normal,v));
}
vec3 Tangent2World(vec3 normal,vec3 tangent,vec3 binormal,vec3 v)
{
    // The basis vectors are expressed in world coordinates.  Reconstruct the
    // world vector from the tangent-space components; dot products perform the
    // opposite (world-to-tangent) transform and rotate sampled light paths.
    return normalize(tangent * v.x + binormal * v.y + normal * v.z);
}


// Ray-Sphere intersection
// http://viclw17.github.io/2018/07/16/raytracing-ray-sphere-intersection/
float hitSphere(const Sphere s, const Ray r)
{
  vec3  oc           = r.origin - s.center;
  float a            = dot(r.direction, r.direction);
  float b            = 2.0 * dot(oc, r.direction);
  float c            = dot(oc, oc) - s.radius * s.radius;
  float discriminant = b * b - 4 * a * c;
  if(discriminant < 0)
  {
    return -1.0;
  }
  else
  {
    return (-b - sqrt(discriminant)) / (2.0 * a);
  }
}

// Ray-AABB intersection
float hitAabb(const AllAabb aabb, const Ray r)
{
  vec3  invDir = 1.0 / r.direction;
  vec3  tbot   = invDir * (aabb.minimum - r.origin);
  vec3  ttop   = invDir * (aabb.maximum - r.origin);
  vec3  tmin   = min(ttop, tbot);
  vec3  tmax   = max(ttop, tbot);
  float t0     = max(tmin.x, max(tmin.y, tmin.z));
  float t1     = min(tmax.x, min(tmax.y, tmax.z));
  if(t1 > max(t0, 0.0))
  {
    if(t0<=0) return t1;
    else return t0;
  }
  else return -1.0;
//   return t1 > max(t0, 0.0) ? t0 : -1.0;
}

//--------------------------------------------------------------------------------------------
//  SATE VAPOR USING A TEMPERATURE  T 300K
//--------------------------------------------------------------------------------------------
float es_fun(float T)
{
    float a = 7.5;
    float b = 237.3;
    float temp = a*T/(b+T);
    return 6.107*pow(10,temp);
}
//slope of the saturated pressure function
float s_fun(float es, float T)
{
    return es*2.3026*7.5*237.3/((237.3+T)*(237.3+T));
}


//-------------------------------------------
// thermal inertial
//-------------------------------------------
float thermal_inertial(float cs,float rhos,float lambdas)
{
    return sqrt(cs*rhos*lambdas);
}


vec2 BallBerry(float Cs, float RH,float A,float BallBerrySlope, float minCi,float CiInput)
{
    float Ci = max(minCi * Cs, Cs *(1-1.6/(BallBerrySlope * RH)) );
    float gs = 0;
    return vec2(Ci,gs);
}

//----------------------------------------------------------
// quadratic formula, root of least magnitude: AX2 + BX + C = 0
//    for the eqn ax^2 + bx + c, 
//    if dsign is:
//       -1, 0: choose the smaller root
//       +1: choose the larger root
//----------------------------------------------------------

float sel_root(float a,float b,float c,float design)
{
    float x;
    if(a ==0)
    {
        x = -c/b;
    }else
    {
        if(design ==0)
        {
            design = -1;
        }
        x = (-b + design *sqrt(b*b - 4*a*c))/(2*a);
    }
    return x;
}

// Scalar helpers for the Farquhar/von Caemmerer solver. Invalid equations
// remain missing values rather than being replaced with plausible carbon flux.
float biochemicalMissing()
{
    return uintBitsToFloat(0x7fc00000u);
}

bool biochemicalFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

float biochemicalQuadraticRoot(float a, float b, float c, float design)
{
    if(!biochemicalFinite(a) || !biochemicalFinite(b) || !biochemicalFinite(c))
        return biochemicalMissing();
    if(a == 0.0)
        return b != 0.0 ? -c / b : biochemicalMissing();
    float coefficientScale = max(abs(a), max(abs(b), abs(c)));
    a /= coefficientScale;
    b /= coefficientScale;
    c /= coefficientScale;
    float bSquared = b * b;
    float fourAC = 4.0 * a * c;
    float discriminant = bSquared - fourAC;
    // Only roundoff-sized negative discriminants may become a double root.
    float tolerance = 8.0 * 1.192092896e-7 * (bSquared + abs(fourAC));
    if(discriminant < -tolerance) return biochemicalMissing();
    float radical = sqrt(max(discriminant, 0.0));
    if(radical == 0.0) return -b / (2.0 * a);
    float q = -0.5 * (b + (b < 0.0 ? -radical : radical));
    // q/a is the non-cancelling root; c/q is its complementary root.
    bool useQ = (design > 0.0) == (b < 0.0);
    return useQ ? q / a : c / q;
}

float biochemicalElectronTransport(float absorbedExcitation, float capacity, float theta)
{
    if(!biochemicalFinite(absorbedExcitation) || !biochemicalFinite(capacity) ||
       !biochemicalFinite(theta) || absorbedExcitation < 0.0 || capacity < 0.0 ||
       theta < 0.0 || theta > 1.0) return biochemicalMissing();
    if(absorbedExcitation == 0.0 || capacity == 0.0) return 0.0;
    float scale = max(absorbedExcitation, capacity);
    float excitation = absorbedExcitation / scale;
    float maximum = capacity / scale;
    float sum = excitation + maximum;
    float radical = sqrt(max(sum * sum - 4.0 * theta * excitation * maximum, 0.0));
    // Rationalized smaller root. At theta=0 this is Q2*Jms/(Q2+Jms),
    // the exact linear limit of theta*J^2-(Q2+Jms)*J+Q2*Jms=0.
    return scale * (2.0 * excitation * maximum / (sum + radical));
}

float biochemicalIntercellularCO2(float surfaceCO2, float lowerBound,
                                 float slope, float relativeHumidity, float stress)
{
    float product = slope * relativeHumidity * stress;
    return product > 0.0 ? max(lowerBound, surfaceCO2 * (1.0 - 1.6 / product)) : lowerBound;
}

float biochemicalResistance(float surfaceCO2, float intercellularCO2,
                             float netAssimilation, float airDensity,
                             float molarAirMass, float pressure)
{
    if(netAssimilation <= 0.0) return 625000.0;
    // The difference is nonnegative for a photosynthesizing leaf. Do not
    // divide by A until after testing the closed-stomata/respiration limit.
    return 0.625 * max(surfaceCO2 - intercellularCO2, 0.0) / netAssimilation *
           airDensity / molarAirMass * 1e3 * 1e6 / pressure * 1e5;
}

float computeA(float Ci, int Type, float g_m,float Vs_C3,float MM_consts,float Rd,float Vcmax,
                float Gamma_star, float Je,float effcon, float atheta,float kpepcase)
{
    float Vc,Ve,Vs;
    if(Type == 3)
    {
        float Vs = Vs_C3;
        float Vc = Vcmax *(Ci-Gamma_star)/(MM_consts + Ci);
        float CO2_per_electron =(Ci-Gamma_star)/(Ci+2*Gamma_star) * effcon;
        float Ve = Je * CO2_per_electron;
    }else if(Type ==4)
    {
        float Vc = Vcmax;
        float Vs = kpepcase * Ci;
        float CO2_per_electron = effcon;
        float Ve = Je * CO2_per_electron;
    }

    float V = sel_root(atheta, -(Vc+Ve), Vc*Ve, 1);
    float Ag = sel_root(0.98,-(V+Vs),V*Vs, -1);
    float A = Ag - Rd;

    return A;
}




//--------------------------------------------------------------------------------------------
// planck and invplanck functions to convert temperature to radiance
//--------------------------------------------------------------------------------------------
 float Planck(float wavelength, float temperature)
{
    float c1 = 11910.439340652*10000;
    float c2 = 14388.291040407;
    if(wavelength > 50) wavelength = wavelength / 1000.0;
    // double c1 = 11910.439340* 10000;
    // double c2 = 14388.291040;

    //if (temperature < 100) temperature = temperature + 273.15;
    double radiance = c1 / (pow(wavelength, 5) * (exp(c2 / temperature / wavelength) - 1));
    
    return float(radiance);
}

 float PlanckT(float wavelength, float temperature)
{
    float c1 = 11910.439340652;
    float c2 = 14388.291040407;
    if(wavelength > 50) wavelength = wavelength / 1000.0;
    // double c1 = 11910.439340* 10000;
    // double c2 =14388.291040;

    double radiance = c1 / (pow(wavelength, 5) * (exp(c2 / temperature / wavelength) - 1)) ;
    
    return float(radiance);
}

float InvPlanck(float wavelength,float radiance)
{
    // double c1 = 11910.439340 * 10000; 
    // double c2 = 14388.291040;
    float c1 = 11910.439340 * 10000; 
    float c2 = 14388.291040;
    if(wavelength > 50) wavelength = wavelength / 1000.0;
    // double temp = c1 / (radiance * pow((wavelength),5)) + 1;
    // double tempp = c2 / (wavelength * log(temp));
    float temp = c1 / (radiance * pow((wavelength),5)) + 1;
    float tempp = c2 / (wavelength * log(temp));
    return float(tempp);
}

//--------------------------------------------------------------------------------------------
//  hapke directional emissivity. {0 < w < 1; 1 < K < 10}
//--------------------------------------------------------------------------------------------
float Emissivity_Hapke(float cosTheta, float w, float K)
{
    float emissivity;
    emissivity = sqrt(1-w) * (1 + 2 * cosTheta/K)/(1 + 2 * cosTheta/K * sqrt(1-w));
    return emissivity;
}


//--------------------------------------------------------------------------------------------
//  From temperature to radiance T i.e. 300 K
//--------------------------------------------------------------------------------------------
float Stefen_Boltzmann(float T,float emis)
{
    return T/100*T/100*T/100*T/100*emis*5.6704;
}

float Inv_Stefen_Boltzmann(float rad, float emis)
{
    return pow(rad / emis /5.6704 , 0.25) * 100;
}

//-------------------------------------------------------------------------------------------------
//  Return the tangent and binormal from the incoming normal
//-------------------------------------------------------------------------------------------------
int createCoordinateSystem(in vec3 N, out vec3 Nt, out vec3 Nb)
{
  Nt = normalize(((abs(N.z) > 0.99999f) ? vec3(-N.x * N.y, 1.0f - N.y * N.y, -N.y * N.z) :
                                          vec3(-N.x * N.z, -N.y * N.z, 1.0f - N.z * N.z)));
  Nb = cross(Nt, N);
  return 1;
}

//--------------------------------------------------------------------------------------------
// Vector Distance
//--------------------------------------------------------------------------------------------
float Vector2Distance(vec3 vector)
{
    float pathLength = sqrt(vector.x*vector.x+vector.y*vector.y+vector.z*vector.z);
    return pathLength;
}


//--------------------------------------------------------------------------------------------
// hotspot function proposed by Kuusk 
//--------------------------------------------------------------------------------------------

// get passed voxel ID (ivec3)
ivec3 getPassedVoxelId(vec3 position, float scale,vec3 direction)
{
    float minTemp = 0.001;
    int xb = int(floor((position.x-direction.x*minTemp *scale)/scale));
    int yb = int(floor((position.y-direction.y*minTemp *scale)/scale));
    int zb = int(floor((position.z-direction.z*minTemp *scale)/scale)); 

    ivec3 voxelId = ivec3(xb,yb,zb);
    return voxelId;
}

// WITH SEMI
// foward direction 
ivec3 getVoxelIdSemi(vec3 position,vec3 semiRange, float scale,vec3 direction, out ivec3 alignVoxelId)
{
    float minTemp = 0.05;

    // int xf = int((position.x+semiRange.x+direction.x*minTemp*scale)/scale);
    // int yf = int((position.y+direction.y*minTemp*scale)/scale);
    // int zf = int((position.z+semiRange.z+direction.z*minTemp*scale)/scale); 
    // if (position.y+direction.y*minTemp*scale<0) yf = -1;
    // int xb = int((position.x+semiRange.x-direction.x*minTemp *scale)/scale);
    // int yb = int((position.y-direction.y*minTemp*scale)/scale);
    // int zb = int((position.z+semiRange.z-direction.z*minTemp*scale)/scale); 
    int xf = int(floor(position.x+semiRange.x+direction.x*minTemp));
    int yf = int(floor(position.y+direction.y*minTemp));
    int zf = int(floor(position.z+semiRange.z+direction.z*minTemp));
    if (position.y+direction.y*minTemp<0) yf = -1;
    int xb = int(floor(position.x+semiRange.x-direction.x*minTemp));
    int yb = int(floor(position.y-direction.y*minTemp));
    int zb = int(floor(position.z+semiRange.z-direction.z*minTemp));
    ivec3 voxelId = ivec3(xb,yb,zb);
    alignVoxelId = ivec3(xf,yf,zf);
    
    return voxelId;
}

ivec3 getVoxelIdSemi(vec3 position,vec3 semiRange,vec3 direction, out ivec3 alignVoxelId)
{
     float minTemp = 0.05;

    int xf = int(floor(position.x+semiRange.x+direction.x*minTemp));
    int yf = int(floor(position.y+direction.y*minTemp));
    int zf = int(floor(position.z+semiRange.z+direction.z*minTemp));
    int xb = int(floor(position.x+semiRange.x-direction.x*minTemp));
    int yb = int(floor(position.y-direction.y*minTemp));
    int zb = int(floor(position.z+semiRange.z-direction.z*minTemp));
    ivec3 voxelId = ivec3(xb,yb,zb);
    alignVoxelId = ivec3(xf,yf,zf);
    
    return voxelId;
}

ivec3 getVoxelIdSimple(vec3 position,vec3 semiRange, float scale)
{

    int x = int((position.x+semiRange.x)/scale);
    int y = int(position.y/scale);
    int z = int((position.z+semiRange.z)/scale);
    ivec3 voxelId = ivec3(x,y,z);
    return voxelId;
}

///----------------------------------------
// To check the relationship between posistion and semiRange+VoxelId
///---------------------------------------

int getFaceIdSimple(vec3 position,vec3 semiRange,ivec3 voxelId)
{
    vec3 voxelPos = vec3(voxelId.x - semiRange.x, voxelId.y,voxelId.z - semiRange.z);
    if(abs(position.y - voxelPos.y-1)<0.1) return 4;
    if(abs(position.z - voxelPos.z-1)<0.01) return 0;
    else if(abs(position.z - voxelPos.z)<0.01) return 1;
    else if(abs(position.x - voxelPos.x)<0.01) return 2;
    else if(abs(position.x - voxelPos.x-1)<0.01) return 3;
    return 7;
}

int getFacetIdPos(int faceid0, inout vec3 voxelPos0)
{
        if (faceid0 == 1){
        voxelPos0 = vec3(voxelPos0.x + 0.5, voxelPos0.y + 0.5, voxelPos0.z +1.0);
        }
        else if (faceid0 == 2){
        voxelPos0 = vec3(voxelPos0.x + 0.5, voxelPos0.y + 0.5, voxelPos0.z);
        }
        else if (faceid0 == 3){
        voxelPos0 = vec3(voxelPos0.x, voxelPos0.y + 0.5, voxelPos0.z + 0.5);
        }
        else if (faceid0 == 4){
        voxelPos0 = vec3(voxelPos0.x + 1.0, voxelPos0.y + 0.5, voxelPos0.z + 0.5);
        }
        else if (faceid0 == 5){
        voxelPos0 = vec3(voxelPos0.x + 0.5, voxelPos0.y+1, voxelPos0.z + 0.5);
        }
        else if (faceid0 == 0){
            voxelPos0 = vec3(voxelPos0.x + 0.5, voxelPos0.y+0.5, voxelPos0.z + 0.5);
        }

        return 1;
}

int SurfFlatten(ivec3 voxelId, ivec3 voxelRes)
{
    int surf1DId = voxelId.x + voxelId.z * voxelRes.x;
    return surf1DId;
};

#if defined(VOXELRT) || defined(VOXELLST)
// OBJ cells have one state (faceId=0). Legacy generated buildings have five
// contiguous states (faceId=1..5). Never offset a one-state cell into its neighbour.
int ResolveVoxelSurfaceState(int base, vec3 normal)
{
    if(base < 0 || base >= setting.voxelCount) return base;
    if(voxelLinks[base].faceId == 0) return base;
    vec3 magnitude = abs(normal);
    int offset = magnitude.y >= max(magnitude.x, magnitude.z) ? 4
        : magnitude.x >= magnitude.z ? (normal.x >= 0.0 ? 3 : 2)
        : (normal.z >= 0.0 ? 0 : 1);
    int candidate = base + offset;
    if(candidate >= setting.voxelCount || voxelLinks[candidate].faceId != offset + 1
        || any(notEqual(voxelLinks[candidate].voxelId, voxelLinks[base].voxelId))) return base;
    return candidate;
}

// Outward normal of a solid (soil/building/water) cell. Legacy generated
// buildings carry it in faceId 1..5; OBJ cells store the area-weighted facet
// normal; cells without one (e.g. DEM ground) are horizontal.
vec3 solidSurfaceNormal(int bufferId)
{
    int face = voxelLinks[bufferId].faceId;
    if(face == 1) return vec3(0.0, 0.0, 1.0);
    if(face == 2) return vec3(0.0, 0.0, -1.0);
    if(face == 3) return vec3(-1.0, 0.0, 0.0);
    if(face == 4) return vec3(1.0, 0.0, 0.0);
    if(face == 5) return vec3(0.0, 1.0, 0.0);
    vec3 n = vec3(voxelLinks[bufferId].normalX, voxelLinks[bufferId].normalY,
                  voxelLinks[bufferId].normalZ);
    float len = length(n);
    return len > 0.5 ? n / len : vec3(0.0, 1.0, 0.0);
}

// OBJ 异质性参数优先于二维 LAD 和冠层默认密度；hexId=-1 时保持原算法。
float ResolveTurbidDensity(int bufferId, uint canopyId, ivec3 voxelId, ivec3 voxelRes)
{
    int mediumId = voxelLinks[bufferId].hexId;
    if (mediumId >= 0) return max(voxelHexs[mediumId].rho, 0.0);
    if (setting.islad >= 1) return max(lads[SurfFlatten(voxelId, voxelRes)], 0.0);
    // Objects with geometry-derived leaf density carry it per voxel.
    float leafDensity = voxelLinks[bufferId].leafDensity;
    return leafDensity > 0.0 ? leafDensity : max(canopies[canopyId].density, 0.0);
}

float ResolveClumpingIndex(int bufferId, vec3 direction)
{
    int mediumId = voxelLinks[bufferId].hexId;
    if (mediumId < 0) return 1.0;
    float length2 = dot(direction, direction);
    if (length2 <= 1.0e-12) return 1.0;
    VoxelHex medium = voxelHexs[mediumId];
    vec3 weights = abs(direction) * inversesqrt(length2);
    if (medium.model == 10) {
        weights /= max(weights.x + weights.y + weights.z, 1.0e-6);
        return 2.0 * dot(weights, vec3(medium.ax, medium.ay, medium.az));
    }
    return max(dot(weights, vec3(medium.ax, medium.ay, medium.az))
               / max(weights.x + weights.y + weights.z, 1.0e-6), 0.0);
}

float HexHotspotDensity(int bufferId, vec3 direction, float density, float G)
{
    int mediumId = voxelLinks[bufferId].hexId;
    if (mediumId >= 0 && voxelHexs[mediumId].model == 10) {
        vec3 weights = abs(direction) /
            max(dot(abs(direction), vec3(1.0)), 1.0e-12);
        return density * dot(
            weights, vec3(voxelHexs[mediumId].ax,
                          voxelHexs[mediumId].ay,
                          voxelHexs[mediumId].az)) / max(G, 1.0e-6);
    }
    return density * ResolveClumpingIndex(bufferId, direction);
}

// x 为透过率，y 为拦截率，二者使用相同的 rho*G*CI(direction)。
vec2 ResolveTurbidInteraction(int bufferId, float density, float G,
                              vec3 direction, float pathLength, float sceneScale)
{
    int mediumId = voxelLinks[bufferId].hexId;
    if (mediumId >= 0 && voxelHexs[mediumId].model == 10) {
        vec3 weights = abs(direction) /
            max(dot(abs(direction), vec3(1.0)), 1.0e-12);
        float extinction = voxelHexs[mediumId].rho * dot(
            weights, vec3(voxelHexs[mediumId].ax,
                          voxelHexs[mediumId].ay,
                          voxelHexs[mediumId].az));
        float transmission = exp(-max(extinction, 0.0) *
                                  max(pathLength, 0.0) *
                                  max(sceneScale, 0.0));
        return vec2(transmission, 1.0 - transmission);
    }
    float opticalDepth = max(density, 0.0) * max(G, 0.0)
        * ResolveClumpingIndex(bufferId, direction)
        * max(pathLength, 0.0) * max(sceneScale, 0.0);
    float transmission = exp(-opticalDepth);
    return vec2(transmission, 1.0 - transmission);
}

// Negative aperture values are internal HEX render-mode tags.  They do not
// alter the public camera aperture used by the ordinary perspective path.
vec3 coneOrigin = vec3(0.0);
vec3 coneAxis = vec3(0.0, 1.0, 0.0);
float coneBaseDiameter = 0.0;
float coneDiameterSlope = 0.0;
bool overlapHasPrevious = false;
vec3 overlapPreviousMin = vec3(0.0);
vec3 overlapPreviousEnd = vec3(0.0);
vec3 overlapPreviousSpan = vec3(0.0);

// Turbid cells are represented by interior integration planes, so consecutive
// hits along a ray lie in different cells (or the first one starts at the ray
// origin). The previous turbid cell is kept to integrate its exit segment.
bool turbidHasPrevious = false;
vec3 turbidPreviousMin = vec3(0.0);
float turbidPreviousExtinction = 0.0;

void ResetHexOverlap()
{
    overlapHasPrevious = false;
}

void ResetHexRay()
{
    overlapHasPrevious = false;
    turbidHasPrevious = false;
}

bool ConeProjectionEnabled()
{
    return voxelHexs.length() > 0 && voxelHexs[0].model == 5 &&
           sensorMatrix.aperture == -4.0;
}

bool ConeLODEnabled()
{
    return ConeProjectionEnabled();
}

void ConfigureCone(vec3 origin, vec3 axis, float diameter, float halfAngle)
{
    overlapHasPrevious = false;
    turbidHasPrevious = false;
    coneOrigin = origin;
    coneAxis = normalize(axis);
    coneBaseDiameter = diameter;
    coneDiameterSlope = 2.0 * tan(halfAngle);
}

// DDA through one mipmap level.  The segment is clipped to the parent
// voxel, so empty child cells remain empty instead of being filled by an
// enclosing bounding-box approximation.
float HierarchyTransmission(int rootId, int level, vec3 start, vec3 end,
                            vec3 parentMin, float sceneScale)
{
    vec4 metadata = voxelHexs[rootId].children[level];
    int offset = int(metadata.x);
    int n = int(metadata.y);
    vec3 delta = end - start;
    float enter = 0.0;
    float leave = 1.0;
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(delta[axis]) < 1.0e-12) {
            if (start[axis] < parentMin[axis] ||
                start[axis] >= parentMin[axis] + 1.0) return 1.0;
        } else {
            float a = (parentMin[axis] - start[axis]) / delta[axis];
            float b = (parentMin[axis] + 1.0 - start[axis]) / delta[axis];
            enter = max(enter, min(a, b));
            leave = min(leave, max(a, b));
        }
    }
    if (leave <= enter) return 1.0;

    vec3 directionSign = sign(delta);
    ivec3 cell = clamp(ivec3(floor(
        (start + enter * delta - parentMin) * float(n) +
        directionSign * 1.0e-5)), ivec3(0), ivec3(n - 1));
    vec3 nextBoundary = vec3(1.0e30);
    vec3 stride = vec3(1.0e30);
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(delta[axis]) < 1.0e-12) continue;
        float boundary = parentMin[axis] +
            float(cell[axis] + (delta[axis] > 0.0 ? 1 : 0)) / float(n);
        nextBoundary[axis] = (boundary - start[axis]) / delta[axis];
        stride[axis] = 1.0 / (float(n) * abs(delta[axis]));
    }

    vec3 weights = abs(delta) /
        max(dot(abs(delta), vec3(1.0)), 1.0e-12);
    float opticalDepth = 0.0;
    float t = enter;
    float distanceMetres = length(delta) * max(sceneScale, 0.0);
    for (int visit = 0; visit < 3 * n + 3; ++visit) {
        if (any(lessThan(cell, ivec3(0))) ||
            any(greaterThanEqual(cell, ivec3(n)))) break;
        float nextT = min(leave, min(nextBoundary.x,
                                     min(nextBoundary.y, nextBoundary.z)));
        int index = offset + (cell.x * n + cell.y) * n + cell.z;
        vec3 k = vec3(voxelHexs[index].ax,
                       voxelHexs[index].ay,
                       voxelHexs[index].az);
        opticalDepth += max(nextT - t, 0.0) * distanceMetres * dot(weights, k);
        if (nextT >= leave) break;
        for (int axis = 0; axis < 3; ++axis) {
            if (nextBoundary[axis] <= nextT + 1.0e-7) {
                cell[axis] += int(directionSign[axis]);
                nextBoundary[axis] += stride[axis];
            }
        }
        t = nextT;
    }
    return exp(-max(opticalDepth, 0.0));
}

float ResolveHierarchyTransmission(int rootId, vec3 start, vec3 end,
                                   vec3 parentMin, float sceneScale)
{
    int maxDepth = int(voxelHexs[rootId].a);
    vec3 midpoint = 0.5 * (start + end);
    float distance = max(dot(midpoint - coneOrigin, coneAxis), 0.0);
    float diameter = coneBaseDiameter + distance * coneDiameterSlope;
    float depth = ConeLODEnabled()
        ? clamp(-log2(max(diameter, 1.0e-12)), 0.0, float(maxDepth))
        : float(maxDepth);
    int coarse = int(floor(depth));
    int fine = min(coarse + 1, maxDepth);
    float blend = depth - float(coarse);
    float coarseTransmission = HierarchyTransmission(
        rootId, coarse, start, end, parentMin, sceneScale);
    if (fine == coarse || blend < 1.0e-6) return coarseTransmission;
    float fineTransmission = HierarchyTransmission(
        rootId, fine, start, end, parentMin, sceneScale);
    return mix(coarseTransmission, fineTransmission, blend);
}

// Apply the six-neighbour gap correction to a segment crossing a compact
// current-scale HEX voxel.  Non-HEX media retain the original transport.
// Fraction of the segment start + t*delta, t in [0,1], inside a unit cell.
float SegmentFractionInCell(vec3 start, vec3 delta, vec3 cellMin)
{
    float entry = 0.0;
    float exitPoint = 1.0;
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(delta[axis]) < 1.0e-12) {
            if (start[axis] < cellMin[axis] - 1.0e-5
                || start[axis] > cellMin[axis] + 1.0 + 1.0e-5) return 0.0;
        } else {
            float a = (cellMin[axis] - start[axis]) / delta[axis];
            float b = (cellMin[axis] + 1.0 - start[axis]) / delta[axis];
            entry = max(entry, min(a, b));
            exitPoint = min(exitPoint, max(a, b));
        }
    }
    return max(exitPoint - entry, 0.0);
}

#ifdef NANOVDB_INFO
// VoxelRT: turbid cells are crossed planes, so an oblique ray can clip a cell
// without hitting any of its planes. Integrate segments cell by cell instead.
NanoVDBLink turbidGridLink;
bool turbidGridReady = false;

void ConfigureTurbidGrid(NanoVDBLink link)
{
    turbidGridLink = link;
    turbidGridReady = true;
}

float TurbidCellExtinction(int bufferId, ivec3 cell, vec3 direction)
{
    if (bufferId < 0 || bufferId >= setting.voxelCount) return 0.0;
    uint meshId = instanceLinks[voxelLinks[bufferId].instanceId].meshId;
    if (meshLinks[meshId].type != TYPE_VEGETATION) return 0.0;
    uint canopyId = meshLinks[meshId].canopyId;
    if (isParticipatingMedium(canopies[canopyId])) return 0.0;
    int mediumId = voxelLinks[bufferId].hexId;
    if (mediumId >= 0 && voxelHexs[mediumId].model == 10) {
        vec3 weights = abs(direction) / max(dot(abs(direction), vec3(1.0)), 1.0e-12);
        return max(voxelHexs[mediumId].rho * dot(weights, vec3(voxelHexs[mediumId].ax,
            voxelHexs[mediumId].ay, voxelHexs[mediumId].az)), 0.0) * setting.scale;
    }
    float density = ResolveTurbidDensity(bufferId, canopyId, cell, ivec3(setting.voxelSize));
    return density * max(canopies[canopyId].G, 0.0)
        * ResolveClumpingIndex(bufferId, direction) * setting.scale;
}

// Optical depth of the turbid cells along start..end (scene cell units).
float TurbidSegmentOpticalDepth(vec3 start, vec3 end)
{
    vec3 semi = vec3(floor(setting.voxelSize.x * 0.5 + 0.5), 0.0,
                     floor(setting.voxelSize.z * 0.5 + 0.5));
    vec3 a = start + semi;
    vec3 delta = (end + semi) - a;
    float len = length(delta);
    if (len < 1.0e-8) return 0.0;
    vec3 dir = delta / len;
    vec3 lo = vec3(0.0);
    vec3 hi = vec3(setting.voxelSize);
    float t0 = 0.0;
    float t1 = len;
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(dir[axis]) < 1.0e-9) {
            if (a[axis] < lo[axis] || a[axis] > hi[axis]) return 0.0;
        } else {
            float ta = (lo[axis] - a[axis]) / dir[axis];
            float tb = (hi[axis] - a[axis]) / dir[axis];
            t0 = max(t0, min(ta, tb));
            t1 = min(t1, max(ta, tb));
        }
    }
    if (t1 <= t0) return 0.0;

    vec3 p = a + dir * t0;
    ivec3 cell = ivec3(floor(p + dir * 1.0e-5));
    ivec3 stepCell = ivec3(sign(dir));
    vec3 tMax = vec3(1.0e30);
    vec3 tDelta = vec3(1.0e30);
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(dir[axis]) >= 1.0e-9) {
            float boundary = float(cell[axis] + (dir[axis] > 0.0 ? 1 : 0));
            tMax[axis] = t0 + (boundary - p[axis]) / dir[axis];
            tDelta[axis] = 1.0 / abs(dir[axis]);
        }
    }
    float t = t0;
    float tau = 0.0;
    for (int i = 0; i < 1024 && t < t1; ++i) {
        float next = min(min(tMax.x, tMax.y), tMax.z);
        float segmentEnd = min(next, t1);
        int bufferId = nanovdbGet(turbidGridLink, cell);
        tau += TurbidCellExtinction(bufferId, cell, dir) * max(segmentEnd - t, 0.0);
        t = segmentEnd;
        if (t >= t1) break;
        if (tMax.x <= tMax.y && tMax.x <= tMax.z) { cell.x += stepCell.x; tMax.x += tDelta.x; }
        else if (tMax.y <= tMax.z) { cell.y += stepCell.y; tMax.y += tDelta.y; }
        else { cell.z += stepCell.z; tMax.z += tDelta.z; }
    }
    return tau;
}
#endif

// A ray that leaves the scene after its last turbid hit still crosses the rest
// of the canopy grid; integrate it (positions are in cell units).
float TurbidEscapeTransmittance(vec3 lastPosition, vec3 direction)
{
#ifdef NANOVDB_INFO
    if (turbidGridReady)
        return exp(-TurbidSegmentOpticalDepth(lastPosition,
            lastPosition + normalize(direction) * 4096.0));
#endif
    if (!turbidHasPrevious) return 1.0;
    vec3 delta = normalize(direction) * 2.0;
    float exitLength = 2.0 * SegmentFractionInCell(lastPosition, delta, turbidPreviousMin);
    turbidHasPrevious = false;
    return exp(-turbidPreviousExtinction * exitLength);
}

vec2 ResolveSpatialInteraction(int bufferId, float density, float G,
                               vec3 start, vec3 end, float sceneScale)
{
    vec3 delta = end - start;
    float distance = length(delta);
    int mediumId = voxelLinks[bufferId].hexId;

    if (mediumId >= 0 && voxelHexs[mediumId].model == 5) {
        if (distance < 1.0e-10) return vec2(1.0, 0.0);
        vec3 semi = vec3(floor(setting.voxelSize.x * 0.5 + 0.5), 0.0,
                         floor(setting.voxelSize.z * 0.5 + 0.5));
        vec3 parentMin = vec3(voxelLinks[bufferId].voxelId) - semi;
        float transmission = ResolveHierarchyTransmission(
            mediumId, start, end, parentMin, sceneScale);

        // Consecutive face neighbours use the same beta correction as the
        // compact HEX path.  The beta values are stored in children[6/7].
        float entry = 0.0;
        float exitPoint = 1.0;
        for (int axis = 0; axis < 3; ++axis) {
            if (abs(delta[axis]) > 1.0e-12) {
                float a = (parentMin[axis] - start[axis]) / delta[axis];
                float b = (parentMin[axis] + 1.0 - start[axis]) / delta[axis];
                entry = max(entry, min(a, b));
                exitPoint = min(exitPoint, max(a, b));
            }
        }
        if (exitPoint > entry) {
            vec3 first = start + entry * delta;
            vec3 last = start + exitPoint * delta;
            vec3 span = abs(last - first);
            vec3 difference = parentMin - overlapPreviousMin;
            vec3 stepCell = round(difference);
            if (overlapHasPrevious && length(first - overlapPreviousEnd) < 0.002 &&
                length(difference - stepCell) < 0.001 &&
                abs(dot(abs(stepCell), vec3(1.0)) - 1.0) < 0.001 &&
                voxelHexs[mediumId].children[6].w > 0.5) {
                int axis = abs(stepCell.x) > 0.5 ? 0 :
                           (abs(stepCell.y) > 0.5 ? 1 : 2);
                int face = axis * 2 + (stepCell[axis] < 0.0 ? 1 : 0);
                float beta = voxelHexs[mediumId].children[6 + face / 3][face % 3];
                float exposure = clamp(overlapPreviousSpan[axis], 0.0, 1.0) *
                                 clamp(span[axis], 0.0, 1.0);
                transmission = exp(min(0.0, log(max(transmission, 1.0e-30)) +
                                             beta * exposure));
            }
            overlapHasPrevious = true;
            overlapPreviousMin = parentMin;
            overlapPreviousEnd = last;
            overlapPreviousSpan = span;
        } else {
            overlapHasPrevious = false;
        }
        return vec2(transmission, 1.0 - transmission);
    }

    if (mediumId < 0 || voxelHexs[mediumId].model != 10) {
        ResetHexOverlap();
        if (distance < 1.0e-10) return vec2(1.0, 0.0);
#ifdef NANOVDB_INFO
        if (turbidGridReady) {
            float gridTransmission = exp(-TurbidSegmentOpticalDepth(start, end));
            return vec2(gridTransmission, 1.0 - gridTransmission);
        }
#endif
        // The segment runs from the previous hit (often the ray origin or the
        // far side of an empty gap) to an interior plane of this cell. Only
        // the parts inside turbid cells attenuate: this cell's entry part and
        // the previous turbid cell's exit part.
        vec3 semi = vec3(floor(setting.voxelSize.x * 0.5 + 0.5), 0.0,
                         floor(setting.voxelSize.z * 0.5 + 0.5));
        vec3 cellMin = vec3(voxelLinks[bufferId].voxelId) - semi;
        float extinction = max(density, 0.0) * max(G, 0.0)
            * ResolveClumpingIndex(bufferId, delta) * max(sceneScale, 0.0);
        float opticalDepth = extinction * distance
            * SegmentFractionInCell(start, delta, cellMin);
        if (turbidHasPrevious
            && any(greaterThan(abs(turbidPreviousMin - cellMin), vec3(0.5))))
            opticalDepth += turbidPreviousExtinction * distance
                * SegmentFractionInCell(start, delta, turbidPreviousMin);
        turbidHasPrevious = true;
        turbidPreviousMin = cellMin;
        turbidPreviousExtinction = extinction;
        float transmission = exp(-opticalDepth);
        return vec2(transmission, 1.0 - transmission);
    }
    if (distance < 1.0e-10) return vec2(1.0, 0.0);

    vec3 semi = vec3(floor(setting.voxelSize.x * 0.5 + 0.5), 0.0,
                     floor(setting.voxelSize.z * 0.5 + 0.5));
    vec3 lower = vec3(voxelLinks[bufferId].voxelId) - semi;
    float entry = 0.0;
    float exitPoint = 1.0;
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(delta[axis]) < 1.0e-12) {
            if (start[axis] < lower[axis] ||
                start[axis] >= lower[axis] + 1.0) {
                ResetHexOverlap();
                return vec2(1.0, 0.0);
            }
        } else {
            float a = (lower[axis] - start[axis]) / delta[axis];
            float b = (lower[axis] + 1.0 - start[axis]) / delta[axis];
            entry = max(entry, min(a, b));
            exitPoint = min(exitPoint, max(a, b));
        }
    }
    if (exitPoint <= entry) {
        ResetHexOverlap();
        return vec2(1.0, 0.0);
    }

    vec3 first = start + entry * delta;
    vec3 last = start + exitPoint * delta;
    vec3 span = abs(last - first);
    vec3 weights = abs(delta) /
        max(dot(abs(delta), vec3(1.0)), 1.0e-12);
    float extinction = voxelHexs[mediumId].rho * dot(
        weights, vec3(voxelHexs[mediumId].ax,
                      voxelHexs[mediumId].ay,
                      voxelHexs[mediumId].az));
    float opticalDepth = max(extinction, 0.0) * length(last - first) *
                         max(sceneScale, 0.0);

    vec3 difference = lower - overlapPreviousMin;
    vec3 stepCell = round(difference);
    if (overlapHasPrevious && length(first - overlapPreviousEnd) < 0.002 &&
        length(difference - stepCell) < 0.001 &&
        abs(dot(abs(stepCell), vec3(1.0)) - 1.0) < 0.001) {
        int axis = abs(stepCell.x) > 0.5 ? 0 :
                   (abs(stepCell.y) > 0.5 ? 1 : 2);
        int face = axis * 2 + (stepCell[axis] < 0.0 ? 1 : 0);
        float beta = voxelHexs[mediumId].children[face / 3][face % 3];
        opticalDepth -= beta * clamp(overlapPreviousSpan[axis], 0.0, 1.0) *
                        clamp(span[axis], 0.0, 1.0);
    }

    overlapHasPrevious = true;
    overlapPreviousMin = lower;
    overlapPreviousEnd = last;
    overlapPreviousSpan = span;
    float transmission = exp(-max(opticalDepth, 0.0));
    return vec2(transmission, 1.0 - transmission);
}
#endif

// uint SurfFlatten(ivec3 voxelId, ivec3 voxelRes)
// {
//     uint surf1DId = voxelId.x*voxelRes.z + voxelId.z ;
//     return surf1DId;
// };

//--------------------------------------------------------------------------------------------
// Geometry function
//--------------------------------------------------------------------------------------------
// wo: from camera to position
// wi: from position to sun
// if negative, the same side; vice versa

// bool isSameSide(vec3 wi,vec3 wo,vec3 normal)
// {
//     float wio = dot(wi,normal)*dot(wi,normal);
//     if(wio <= 0) return false;
//     else return true;
// }
bool IsSameSide(vec3 wi,vec3 wo,vec3 normal)
{
    float wio = dot(wi,normal)*dot(wo,normal);
    if(wio < 0) return true;
    else return false;
}


float atan2(float y,float x)
{
    float phi;
    if(x > 0)
    {
        phi = atan(y/x);
    }else if(x<0 && y>=0)
    {
        phi = atan(y/x) + PI;
    }else if(x<0 && y<0)
    {
        phi = atan(y/x) - PI;
    }else if(x == 0 && y>0)
    {
        phi = PI/2.0;
    }else if( x==0 && y<0)
    {
        phi = -PI/2.0;
    }else
    {
        phi = 0;
    }
    return phi;
}

// // a direction
// vec2 scattering_phase_function_direct_XYZ(vec3 directioni,vec3 directionj)
// {
//     vec2 Gamma;
//     float thetai = acos(directioni.z);
//     float thetaj = acos(directionj.z);
//     float alphai = 0,alphaj = 0;
//     alphai = atan2(directioni.y,directioni.x);
//     alphaj = atan2(directionj.y,directionj.x);
//     float cosa = directioni.z * directionj.z + sin(thetai) * sin(thetaj)*cos(alphai-alphaj);
//     float a = acos(cosa);
//     float sina = sin(a);
//     Gamma.x = abs(sina + (PI - a)*cosa)/(3*PI);
//     Gamma.y = abs(sina + a*cosa)/(3*PI);
    
//     return Gamma;
// }

float cross0(vec3 A, vec3 B){
    return A.x*B.x+A.y*B.y+A.z*B.z;
}

/////--------------------------------------------
//-- A Fast, Invertible Canopy Reflectance Model, Andres Kuusk, RSE, 1995
// Gamma.x for reflectance
// Gamma.y for transmittance
///----------------------------------------------
vec2 scattering_phase_function_direct_XZY(vec3 directioni,vec3 directionj)
{
    // vec2 Gamma;
    // float thetai = acos(directioni.y);
    // float thetaj = acos(directionj.y);
    // float alphai = 0,alphaj = 0;
    // alphai = atan2(directioni.z,directioni.x);
    // alphaj = atan2(directionj.z,directionj.x);
    // //入射方向与出射方向的夹角余弦值; Cosine value of scattering angle between incident direction and exit direction
    // float cosa = directioni.y * directionj.y + sin(thetai) * sin(thetaj)*cos(alphai-alphaj);
    // //scattering angle 
    // float a = acos(cosa);
    // float sina = sin(a);
    // Gamma.x = (sina + (PI - a)*cosa)/(3*PI);
    // Gamma.y = (sina + a*cosa)/(3*PI);
    // return Gamma;

    vec2 Gamma;
    float thetai = acos(directioni.y);
    float thetaj = acos(directionj.y);
    float alphai = 0,alphaj = 0;
    alphai = atan2(directioni.z,directioni.x);
    alphaj = atan2(directionj.z,directionj.x);
    //入射方向与出射方向的夹角余弦值; Cosine value of scattering angle between incident direction and exit direction
    float cosa = cos(thetai) * cos(thetaj) + sin(thetai) * sin(thetaj)*cos(alphai-alphaj);
    //scattering angle 
    float a = acos(cosa);
    float sina = sin(a);
    Gamma.x = abs(sina + (PI - a)*cosa)/(3*PI);
    Gamma.y = abs(sina + a*cosa)/(3*PI);
    return Gamma;
}
/////--------------------------------------------
//-- leaf scattering phase function
///----------------------------------------------
vec2 scattering_phase_function_direct_XZY_paper(vec3 directioni,vec3 directionj)
{
    // vec2 Gamma;
    // float thetai = acos(directioni.y);
    // float thetaj = acos(directionj.y);
    // float alphai = 0,alphaj = 0;
    // alphai = atan2(directioni.z,directioni.x);
    // alphaj = atan2(directionj.z,directionj.x);
    // //入射方向与出射方向的夹角余弦值; Cosine value of scattering angle between incident direction and exit direction
    // float cosa = directioni.y * directionj.y + sin(thetai) * sin(thetaj)*cos(alphai-alphaj);
    // //scattering angle 
    // float a = acos(cosa);
    // float sina = sin(a);
    // Gamma.x = (sina - a * cosa) / 3 * PI;
    // Gamma.y = cosa / 3;
    // return Gamma;

     vec2 Gamma;
    float thetai = acos(directioni.y);
    float thetaj = acos(directionj.y);
    float alphai = 0,alphaj = 0;
    alphai = atan2(directioni.z,directioni.x);
    alphaj = atan2(directionj.z,directionj.x);
    //入射方向与出射方向的夹角余弦值; Cosine value of scattering angle between incident direction and exit direction
    float cosa = cos(thetai) * cos(thetaj) + sin(thetai) * sin(thetaj)*cos(alphai-alphaj);
    //scattering angle 
    float a = acos(cosa);
    float sina = sin(a);
    // Ross (1981) bi-Lambertian spherical leaves; a is the angle between the
    // incident and scattered propagation directions (a = pi: backscatter):
    //   Gamma = (rho+tau)/(3 pi) (sin a - a cos a) + tau/3 cos a
    // Gamma.y is signed so that backscatter carries rho/3 only.
    Gamma.x = (sina - a * cosa) / (3 * PI);
    Gamma.y = cosa / 3.0;
    return Gamma;
}


// Kuusk (1991, 1995) bidirectional gap probability of the sun and view paths
// from a scattering point, both directions pointing away from the scene:
//   P = exp(-G rho (ls + lv) + G rho sqrt(ls lv) (1 - exp(-a)) / a)
// with a = z * dso / hs, z the vertical depth, hs the leaf size in metres and
//   dso = sqrt(tan^2 ts + tan^2 tv - 2 tan ts tan tv cos(phi_s - phi_v)),
// so a vanishes in the exact backscatter direction (full correlation).
float Hotspot_Kuusk(vec3 directioni,vec3 directionj, float pl_from, float pl_to, float density, float hs)
{
    float mus = max(directioni.y, 1.0e-3);
    float muv = max(directionj.y, 1.0e-3);
    float tans = sqrt(max(1.0 - mus * mus, 0.0)) / mus;
    float tanv = sqrt(max(1.0 - muv * muv, 0.0)) / muv;
    vec2 sunAzimuth = directioni.xz;
    vec2 viewAzimuth = directionj.xz;
    float cosphi = (dot(sunAzimuth, sunAzimuth) > 1.0e-12 && dot(viewAzimuth, viewAzimuth) > 1.0e-12)
        ? dot(normalize(sunAzimuth), normalize(viewAzimuth)) : 1.0;
    float dso = sqrt(max(tans * tans + tanv * tanv - 2.0 * tans * tanv * cosphi, 0.0));
    float depth = sqrt(max(pl_from * mus, 0.0) * max(pl_to * muv, 0.0));
    float ks = dso * depth / max(hs, 1.0e-6);
    float xtemp = ks <= 0.1 ? 1.0 - ks * 0.5 : (1.0 - exp(-ks)) / ks;
    float chs = sqrt(max(pl_from * pl_to, 0.0)) * xtemp;
    float xx = (chs - pl_from - pl_to) * GLEAF;
    float hotspot = exp(xx * density);
    return hotspot;
}



bool checkSameDirection(vec3 directioni, vec3 directionj)
{
    bool a = false;
    
    if (abs(directioni.x - directionj.x) < EPSION_2)
    {
        if (abs(directioni.y - directionj.y) < EPSION_2)
        {
            if (abs(directioni.z - directionj.z) < EPSION_2)
            {
                a = true;
                return a;
            }
        }
    }
    return a;
}


float calReCor(float lai)
{
    float reCor = 0.88 * (1 - exp(-0.7 * pow(lai, 0.75)));
    return reCor;
}


// Material SMC accepts a volumetric fraction or a percentage.  The pore-space
// limit belongs to this material, rather than the global meteorological state.
float soilMoistureFraction(float moisture, float saturatedWater)
{
    float fraction = moisture > 1.0 ? moisture * 0.01 : moisture;
    return clamp(fraction, 0.0, clamp(saturatedWater, 0.0, 1.0));
}

// Implicit, conservative finite-volume conduction on a nonuniform nodal grid.
// Temperatures are Celsius; G is positive downward [W m-2].  cs [J kg-1 K-1]
// and rhos [kg m-3] give dry volumetric capacity; pore water adds its capacity.
void Soilheatflux(in float Tprofile[TLASTNUM], in float Mprofile[TLASTNUM], float Tsi,
                  float lowerBoundaryTemperature, inout float G,
                  inout float T[TLASTNUM], float dtime,
                  float cs, float rhos, float lambdas, float saturatedWater)
{
    float depth[] = {0.0, 0.02, 0.04, 0.10, 0.20, 0.40, 0.60, 1.0};
    float timestep = max(dtime, 1.0);
    float conductivity = max(lambdas, 0.0);
    float dryCapacity = max(cs, 0.0) * max(rhos, 0.0);
    float capacity[TLASTNUM];
    float TA[TLASTNUM], TB[TLASTNUM], TC[TLASTNUM], TD[TLASTNUM];
    for(int i = 0; i < TLASTNUM; ++i)
    {
        capacity[i] = max(dryCapacity + 4.195e6 *
            soilMoistureFraction(Mprofile[i], saturatedWater), 1.0);
    }
    for(int k = 0; k < TLASTNUM; ++k)
    {
        if(k == 0 || k == TLASTNUM - 1)
        {
            TA[k] = 1.0;
            TB[k] = 0.0;
            TC[k] = 0.0;
            TD[k] = k == 0 ? Tsi : lowerBoundaryTemperature;
        }
        else
        {
            float volumeWidth = 0.5 * (depth[k + 1] - depth[k - 1]);
            float storageRate = capacity[k] * volumeWidth / timestep;
            TB[k] = conductivity / (depth[k + 1] - depth[k]);
            TC[k] = conductivity / (depth[k] - depth[k - 1]);
            TA[k] = storageRate + TB[k] + TC[k];
            TD[k] = storageRate * Tprofile[k];
        }
    }

    float P[TLASTNUM], Q[TLASTNUM];
    P[0] = TB[0] / TA[0];
    Q[0] = TD[0] / TA[0];
    for(int i = 1; i < TLASTNUM; ++i)
    {
        float denominator = TA[i] - TC[i] * P[i - 1];
        P[i] = TB[i] / denominator;
        Q[i] = (TD[i] + TC[i] * Q[i - 1]) / denominator;
    }
    T[TLASTNUM - 1] = Q[TLASTNUM - 1];
    for(int i = TLASTNUM - 2; i >= 0; --i)
    {
        T[i] = P[i] * T[i + 1] + Q[i];
    }

    // Surface half-cell storage plus conduction into the first interior cell.
    // Telescoping the interior balances gives the identical quantity as total
    // column storage plus conduction through the prescribed 1 m boundary.
    float surfaceStorage = capacity[0] * 0.5 * (depth[1] - depth[0]) *
        (T[0] - Tprofile[0]) / timestep;
    G = surfaceStorage + conductivity * (T[0] - T[1]) /
        (depth[1] - depth[0]);
}

// The conduction system is linear in surface temperature.  A unit response
// uses exactly the same capacities, grid and timestep as the flux evaluation.
float soilHeatFluxSurfaceDerivative(in float Mprofile[TLASTNUM], float dtime,
                                   float cs, float rhos, float lambdas,
                                   float saturatedWater)
{
    float previous[TLASTNUM], response[TLASTNUM];
    for(int i = 0; i < TLASTNUM; ++i) previous[i] = 0.0;
    float derivative = 0.0;
    Soilheatflux(previous, Mprofile, 1.0, 0.0, derivative, response, dtime,
                 cs, rhos, lambdas, saturatedWater);
    return derivative;
}




#endif
