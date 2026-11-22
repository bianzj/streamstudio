#ifndef HISTREAM_FLUID_COUPLING_GLSL
#define HISTREAM_FLUID_COUPLING_GLSL

float localAmbientCo2Ppm(uint sparseIndex)
{
    return ubom.meteo.Ca + voxelAirs[sparseIndex].co2ExcessPpm;
}

float terrainBackgroundTemperatureC(float heightM)
{
    return ubom.meteo.Ta - (fluidParameters.heightProfile.x > 0.5
        ? fluidParameters.heightProfile.y * (heightM - fluidParameters.heightProfile.z) : 0.0);
}

float terrainSaturationHpa(float temperatureC)
{
    float t = clamp(temperatureC, -80.0, 80.0);
    return 6.107 * pow(10.0, 7.5 * t / (237.3 + t));
}

float terrainBackgroundVaporHpa(float heightM, float temperatureC)
{
    if (fluidParameters.heightProfile.x < 0.5) return ubom.meteo.ea;
    float exponent = -(heightM - fluidParameters.heightProfile.z)
        / max(fluidParameters.heightProfile.w, 0.001);
    return clamp(ubom.meteo.ea * exp(clamp(exponent, -80.0, 80.0)),
        0.0, terrainSaturationHpa(temperatureC));
}

float localSoilMoisture(uint sparseIndex, uint bioId)
{
    float local = voxelAirs[sparseIndex].soilMoisture;
    float material = soilSets[bioId].SMC;
    if (local < 0.0) return material > 1.0 ? material * 0.01 : material;
    return local;
}

float localSoilResistance(uint sparseIndex, uint bioId)
{
    float resistance = soilSets[bioId].rss;
    if (voxelAirs[sparseIndex].soilMoisture < 0.0) return resistance;
    // Keep material rss as the resistance at its configured reference SMC.
    // This inverse-moisture scaling is a prescribed availability approximation.
    float reference = soilSets[bioId].SMC;
    if (reference > 1.0) reference *= 0.01;
    return resistance * max(reference, 0.001)
        / max(localSoilMoisture(sparseIndex, bioId), 0.001);
}

int fluidLinearIndex(ivec3 cell)
{
    ivec3 size = fluidParameters.grid.xyz;
    if (cell.x < 0 || cell.y < 0 || cell.z < 0 ||
        cell.x >= size.x || cell.y >= size.y || cell.z >= size.z) return -1;
    return cell.x + size.x * (cell.z + size.z * cell.y);
}

// Air temperature adjacent to a sparse radiative voxel. Solid cells use
// their surrounding air. The return value is Celsius for EB shaders.
float localAirTemperatureC(uint sparseIndex)
{
    float background = terrainBackgroundTemperatureC(voxelAirs[sparseIndex].heightM);
    if (fluidParameters.grid.w == 0) return background;
    ivec3 sparseCell = voxelLinks[sparseIndex].voxelId;
    vec3 radiativeSpacing = max(
        fluidParameters.couplingSpacing.xyz, vec3(0.001));
    vec3 fluidSpacing = max(fluidParameters.spacingTime.xyz, vec3(0.001));
    vec3 position = (vec3(max(sparseCell, ivec3(0))) + vec3(0.5))
        * radiativeSpacing;
    if (sparseCell.y < 0) position.y = 0.5 * fluidSpacing.y;
    ivec3 cell = ivec3(floor(position / fluidSpacing));
    int center = fluidLinearIndex(cell);
    if (center < 0) return background;
    if (fluidMeta[center].kind != 1) {
        float temperature = fluidScalarA[center].x;
        return (isnan(temperature) || isinf(temperature))
            ? background : temperature - 273.15;
    }
    const ivec3 offsets[6] = ivec3[6](
        ivec3(1,0,0), ivec3(-1,0,0), ivec3(0,1,0),
        ivec3(0,-1,0), ivec3(0,0,1), ivec3(0,0,-1));
    float sum = 0.0;
    float count = 0.0;
    for (int i = 0; i < 6; ++i) {
        int neighbour = fluidLinearIndex(cell + offsets[i]);
        if (neighbour >= 0 && fluidMeta[neighbour].kind != 1) {
            float temperature = fluidScalarA[neighbour].x;
            if (!isnan(temperature) && !isinf(temperature)) {
                sum += temperature;
                count += 1.0;
            }
        }
    }
    return count > 0.0 ? sum / count - 273.15 : background;
}

float localVaporPressureHpa(uint sparseIndex)
{
    if (fluidParameters.heightProfile.x < 0.5) return ubom.meteo.ea;
    float temperature = localAirTemperatureC(sparseIndex);
    float background = terrainBackgroundVaporHpa(voxelAirs[sparseIndex].heightM, temperature);
    if (fluidParameters.grid.w == 0) return background;
    vec3 position = (vec3(max(voxelLinks[sparseIndex].voxelId, ivec3(0))) + vec3(0.5))
        * fluidParameters.couplingSpacing.xyz;
    if (voxelLinks[sparseIndex].voxelId.y < 0) position.y = 0.5 * fluidParameters.spacingTime.y;
    ivec3 cell = ivec3(floor(position / max(fluidParameters.spacingTime.xyz, vec3(0.001))));
    const ivec3 offsets[7] = ivec3[7](ivec3(0), ivec3(1,0,0), ivec3(-1,0,0),
        ivec3(0,1,0), ivec3(0,-1,0), ivec3(0,0,1), ivec3(0,0,-1));
    float sum = 0.0, count = 0.0;
    for (int i = 0; i < 7; ++i) {
        int index = fluidLinearIndex(cell + offsets[i]);
        if (index < 0 || fluidMeta[index].kind == 1) continue;
        float vapor = fluidScalarA[index].z;
        if (isnan(vapor) || isinf(vapor) || vapor < 0.0) continue;
        if (i == 0) return min(vapor, terrainSaturationHpa(temperature));
        sum += vapor; count += 1.0;
    }
    return count > 0.0 ? min(sum / count, terrainSaturationHpa(temperature)) : background;
}

// Local resolved air speed used by aerodynamic, sensible-heat and latent-heat
// resistance calculations. Solid surfaces sample their adjacent air cells.
float localWindSpeed(uint sparseIndex)
{
    if (fluidParameters.grid.w == 0) return ubom.meteo.u;
    ivec3 sparseCell = voxelLinks[sparseIndex].voxelId;
    vec3 radiativeSpacing = max(
        fluidParameters.couplingSpacing.xyz, vec3(0.001));
    vec3 fluidSpacing = max(fluidParameters.spacingTime.xyz, vec3(0.001));
    vec3 position = (vec3(max(sparseCell, ivec3(0))) + vec3(0.5))
        * radiativeSpacing;
    if (sparseCell.y < 0) position.y = 0.5 * fluidSpacing.y;
    ivec3 cell = ivec3(floor(position / fluidSpacing));
    int center = fluidLinearIndex(cell);
    if (center < 0) return ubom.meteo.u;
    if (fluidMeta[center].kind != 1) {
        float speed = length(fluidVelocityA[center].xyz);
        return (isnan(speed) || isinf(speed)) ? ubom.meteo.u : speed;
    }
    const ivec3 offsets[6] = ivec3[6](
        ivec3(1,0,0), ivec3(-1,0,0), ivec3(0,1,0),
        ivec3(0,-1,0), ivec3(0,0,1), ivec3(0,0,-1));
    float sum = 0.0;
    float count = 0.0;
    for (int i = 0; i < 6; ++i) {
        int neighbour = fluidLinearIndex(cell + offsets[i]);
        if (neighbour >= 0 && fluidMeta[neighbour].kind != 1) {
            float speed = length(fluidVelocityA[neighbour].xyz);
            if (!isnan(speed) && !isinf(speed)) {
                sum += speed;
                count += 1.0;
            }
        }
    }
    return count > 0.0 ? sum / count : ubom.meteo.u;
}

// Effective aerodynamic resistance of a ground-type surface (soil, water,
// building) including free convection. With the neutral wind-driven
// resistance (control.stabilityCorrection off) exchange collapses at low
// wind and left sunlit soil with G/Rn ~ 0.9. Under unstable conditions
// (surface warmer than air) the windless conductance of Kondo & Ishida
// (1997), as used in TSEB (Kustas & Norman 1999), is then added in
// parallel: g_free ~ 0.0025 (Ts - Ta)^(1/3) m/s. The form
// 0.0025 x (x^2 + 1)^(-1/3) keeps that asymptote but has a finite slope at
// x = 0, so the Newton update in budget.comp does not oscillate around
// Ts = Ta. Stable or neutral cases and vegetation are unchanged.
//
// With the Monin-Obukhov correction on (the *_most.comp.spv pipelines,
// compiled with -DVOXELEB_MOST), 1/raa already contains the buoyant
// enhancement of the scene surface layer; adding g_free in parallel would
// count buoyancy twice. The larger of the two conductances is used
// instead: MOST governs forced/mixed convection, the free-convection floor
// only takes over in the windless limit. The neutral pipelines keep the
// pre-MOST code token for token (bit-identical results).
bool groundFreeConvectionApplies(uint typeId)
{
    return typeId == TYPE_SOIL || typeId == TYPE_WATER || typeId == TYPE_BUILDING;
}

float groundFreeConductance(float excess)
{
    float x = clamp(excess, 0.0, 60.0);
    return 0.0025 * x * pow(x * x + 1.0, -1.0 / 3.0);
}

// d(g_free * excess)/d(excess) - g_free: the extra sensible-heat slope that
// the free-convection term adds beyond rho*cp/raa_eff.
float groundFreeConductanceExtraSlope(float excess)
{
    if (!(excess > 0.0) || excess >= 60.0) return 0.0;
    float x = excess;
    float s = x * x + 1.0;
    float dg = 0.0025 * (pow(s, -1.0 / 3.0) - (2.0 / 3.0) * x * x * pow(s, -4.0 / 3.0));
    return x * dg;
}

#ifdef VOXELEB_MOST
// y lane: MOST active for this node (cleared when the scene has no ground
// column to define a surface layer); z lane: scene 1/L [m-1].
bool stabilityCorrectionEnabled()
{
    return fluidParameters.localExchange.y > 0.5;
}

float groundExchangeRaa(float raa, float surfaceTemperatureK,
                        float airTemperatureC, uint typeId)
{
    raa = max(raa, 1.0);
    if (!groundFreeConvectionApplies(typeId)) return raa;
    float excess = surfaceTemperatureK - 273.15 - airTemperatureC;
    if (!(excess > 0.0)) return raa;
    if (stabilityCorrectionEnabled())
        return 1.0 / max(1.0 / raa, groundFreeConductance(excess));
    return 1.0 / (1.0 / raa + groundFreeConductance(excess));
}

// Extra Newton slope of the free-convection branch actually in use.
float groundFreeExchangeExtraSlope(float raa, float surfaceTemperatureK,
                                   float airTemperatureC)
{
    float excess = surfaceTemperatureK - 273.15 - airTemperatureC;
    if (stabilityCorrectionEnabled() &&
        !(groundFreeConductance(excess) > 1.0 / max(raa, 1.0))) return 0.0;
    return groundFreeConductanceExtraSlope(excess);
}

// Monin-Obukhov similarity functions, the GLSL port of
// src/facetrt/energyphysics.h (Paulson unstable / linear stable, valid
// for -5 <= zeta = z/L <= 1).
float mostPsiM(float zeta)
{
    zeta = clamp(zeta, -5.0, 1.0);
    if (zeta >= 0.0) return -5.0 * zeta;
    float x = pow(1.0 - 16.0 * zeta, 0.25);
    return 2.0 * log((1.0 + x) / 2.0) + log((1.0 + x * x) / 2.0)
        - 2.0 * atan(x) + 1.5707963267948966;
}

float mostPsiH(float zeta)
{
    zeta = clamp(zeta, -5.0, 1.0);
    return zeta >= 0.0 ? -5.0 * zeta : 2.0 * log((1.0 + sqrt(1.0 - 16.0 * zeta)) / 2.0);
}

float mostPhiH(float zeta)
{
    zeta = clamp(zeta, -5.0, 1.0);
    return zeta >= 0.0 ? 1.0 + 5.0 * zeta : 1.0 / sqrt(1.0 - 16.0 * zeta);
}
#else
float groundExchangeRaa(float raa, float surfaceTemperatureK,
                        float airTemperatureC, uint typeId)
{
    raa = max(raa, 1.0);
    if (!groundFreeConvectionApplies(typeId)) return raa;
    float excess = surfaceTemperatureK - 273.15 - airTemperatureC;
    if (!(excess > 0.0)) return raa;
    return 1.0 / (1.0 / raa + groundFreeConductance(excess));
}
#endif

#endif
