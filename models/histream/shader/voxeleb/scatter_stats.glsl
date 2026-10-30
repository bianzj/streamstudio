#ifndef HISTREAM_SCATTER_STATS_GLSL
#define HISTREAM_SCATTER_STATS_GLSL

// CPU ABI: four records of 32 bytes (1/2/3 scattered orders, then sky zero).
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
    return setting.scatteringStage == 1 ? 3u
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

void recordScatteringAbsorption(uint slot, float shortwave, float par) {
    if(setting.scatteringStatsAddress == uint64_t(0)) return;
    atomicAdd(scatteringStats[slot].absorbedShortwave, shortwave,
              gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelaxed);
    atomicAdd(scatteringStats[slot].absorbedPar, par,
              gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelaxed);
}

#endif
