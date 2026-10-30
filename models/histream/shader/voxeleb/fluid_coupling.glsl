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

#endif
