#ifndef HISTREAM_SCATTER_STATS_GLSL
#define HISTREAM_SCATTER_STATS_GLSL

// CPU ABI: four records of 32 bytes (1/2/3 scattered orders, then sky zero),
// followed by the 64-byte solar-visibility record (SOLAR_VISIBILITY_STATS).
// Byte offsets per record: 0,4,8,12 uint; 16,20,24,28 float.
// Residual fields sum the dimensionless ray throughput at a termination;
// they are diagnostic bounds, not estimated omitted absorbed energy.
struct ScatteringStatsRecord {
    uint maxStepTruncations;
    uint transmittanceStops;
    uint tracedRays;
    uint invalidValues;
    float absorbedShortwave;
    float absorbedPar;
    float transmittanceResidual;
    float truncatedResidual;
};

layout(buffer_reference, scalar, buffer_reference_align = 16)
buffer ScatteringStatsBuffer {
    ScatteringStatsRecord records[4];
};

#define scatteringStats ScatteringStatsBuffer(setting.scatteringStatsAddress).records

uint scatteringStatsSlot() {
    // Stage 4 reuses the stage-1 sky trace: same (sky zero) record.
    return (setting.scatteringStage == 1 || setting.scatteringStage == 4) ? 3u
        : uint(clamp(setting.scatteringOrder, 1, 3) - 1);
}

float checkedScatteringRadiance(float value) {
    if(isnan(value) || isinf(value) || value < -1.0e-5) {
        if(setting.scatteringStatsAddress != uint64_t(0))
            atomicAdd(scatteringStats[scatteringStatsSlot()].invalidValues, 1u);
        // Keep the remaining work finite; CPU must reject invalidValues>0.
        return 0.0;
    }
    return max(value, 0.0);
}

void recordScatteringRay(uint slot, int reason, float residual) {
    if(setting.scatteringStatsAddress == uint64_t(0)) return;
    atomicAdd(scatteringStats[slot].tracedRays, 1u);
    if(reason == 1) {
        atomicAdd(scatteringStats[slot].transmittanceStops, 1u);
        atomicAdd(scatteringStats[slot].transmittanceResidual, residual,
                  gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelaxed);
    } else if(reason == 2) {
        atomicAdd(scatteringStats[slot].maxStepTruncations, 1u);
        atomicAdd(scatteringStats[slot].truncatedResidual, residual,
                  gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelaxed);
    }
}

#ifdef SOLAR_VISIBILITY_STATS
// Solar-visibility record (voxelrad_solar.comp, gap stage), 64 bytes right
// after the four scattering records; CPU mirror SolarVisibilityStats.
// Transmittances are the dimensionless sun visibility a ray returned: the
// direct irradiance error of that voxel is at most residual * E_beam.
//   truncated*  rays that reached the geometric step budget (SolarStepLimit)
//   threshold*  rays stopped at transmittance <= 1e-4 (value kept)
//   legacy*     rays that needed more than radiationMaxSteps: excess is the
//               transmittance the pre-2026-11-23 cut kept minus the traced one
// *Max fields hold float bits (non-negative floats order like uints).
struct SolarVisibilityStatsRecord {
    uint tracedRays;
    uint truncations;
    uint thresholdStops;
    uint legacyTruncations;
    float truncatedResidual;
    uint truncatedResidualMax;
    float thresholdResidual;
    uint thresholdResidualMax;
    float legacyExcess;
    uint legacyExcessMax;
    uint maxStepsUsed;
    uint stepLimit;
    uint reserved0;
    uint reserved1;
    uint reserved2;
    uint reserved3;
};

layout(buffer_reference, scalar, buffer_reference_align = 16)
buffer SolarVisibilityStatsBuffer {
    SolarVisibilityStatsRecord record;
};

const int SOLAR_RAY_ESCAPED = 0;
const int SOLAR_RAY_BLOCKED = 1;
const int SOLAR_RAY_THRESHOLD = 2;
const int SOLAR_RAY_TRUNCATED = 3;

void recordSolarRay(int reason, float transmittance, float legacyTransmittance,
                    int steps, int stepLimit) {
    if(setting.scatteringStatsAddress == uint64_t(0)) return;
    SolarVisibilityStatsBuffer stats =
        SolarVisibilityStatsBuffer(setting.scatteringStatsAddress + uint64_t(128));
    float value = max(transmittance, 0.0);
    atomicAdd(stats.record.tracedRays, 1u);
    atomicMax(stats.record.maxStepsUsed, uint(steps));
    atomicMax(stats.record.stepLimit, uint(stepLimit));
    if(reason == SOLAR_RAY_TRUNCATED) {
        atomicAdd(stats.record.truncations, 1u);
        atomicAdd(stats.record.truncatedResidual, value,
                  gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelaxed);
        atomicMax(stats.record.truncatedResidualMax, floatBitsToUint(value));
    } else if(reason == SOLAR_RAY_THRESHOLD) {
        atomicAdd(stats.record.thresholdStops, 1u);
        atomicAdd(stats.record.thresholdResidual, value,
                  gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelaxed);
        atomicMax(stats.record.thresholdResidualMax, floatBitsToUint(value));
    }
    if(legacyTransmittance >= 0.0) {
        float excess = max(legacyTransmittance - value, 0.0);
        atomicAdd(stats.record.legacyTruncations, 1u);
        atomicAdd(stats.record.legacyExcess, excess,
                  gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelaxed);
        atomicMax(stats.record.legacyExcessMax, floatBitsToUint(excess));
    }
}
#endif // SOLAR_VISIBILITY_STATS

void recordScatteringAbsorption(uint slot, float shortwave, float par) {
    if(setting.scatteringStatsAddress == uint64_t(0)) return;
    atomicAdd(scatteringStats[slot].absorbedShortwave, shortwave,
              gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelaxed);
    atomicAdd(scatteringStats[slot].absorbedPar, par,
              gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelaxed);
}

#endif
