// ==========================================================================
// 文件：scatter_stats.glsl
// 功能：VoxelEB 多次散射/太阳可见度光线追踪的诊断统计。着色器把截断、透过率阈值停止、
//       无效值等计数以原子操作累加到 GPU 缓冲区，CPU 端读回后判定追踪是否可信。仅被 include。
// 主要模块：
//   1. ScatteringStatsRecord：1/2/3 阶散射与“天空零阶”共 4 条 32 字节记录
//   2. scatteringStatsSlot / checkedScatteringRadiance / recordScatteringRay / recordScatteringAbsorption
//   3. SolarVisibilityStatsRecord（仅 SOLAR_VISIBILITY_STATS 定义时）：64 字节太阳可见度统计，
//      紧跟在四条散射记录之后（偏移 128 字节），由 recordSolarRay 写入
// 数据流：着色器 → atomicAdd/atomicMax 写入 setting.scatteringStatsAddress 指向的缓冲区 → CPU 读回。
//         地址为 0 表示未开启统计，所有记录函数直接返回。
// 与其他文件：CPU 镜像结构与 SolarVisibilityStats 在 C++ 端；voxelrad_solar.comp 调用 recordSolarRay。
// 约定：残差（residual）为光线终止处无量纲通量（透过率）之和，是诊断上界，
//         不是被遗漏的吸收能量估计；CPU 必须拒绝 invalidValues > 0 的结果。
// ==========================================================================

#ifndef HISTREAM_SCATTER_STATS_GLSL
#define HISTREAM_SCATTER_STATS_GLSL

// 模块：记录结构（与 CPU 端字节布局严格一致，改动需同步 C++ 结构）。
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

// 当前散射阶段对应的统计槽：阶段 1 与阶段 4 共用“天空零阶”槽 3，其余按 scatteringOrder(1..3)-1。
uint scatteringStatsSlot() {
    // Stage 4 reuses the stage-1 sky trace: same (sky zero) record.
    return (setting.scatteringStage == 1 || setting.scatteringStage == 4) ? 3u
        : uint(clamp(setting.scatteringOrder, 1, 3) - 1);
}

// 辐亮度有效性检查：NaN/Inf 或小于 -1e-5 的值记入 invalidValues 并返回 0 以保持后续计算有限；
// 小的负值（浮点误差）截断为 0。
float checkedScatteringRadiance(float value) {
    if(isnan(value) || isinf(value) || value < -1.0e-5) {
        if(setting.scatteringStatsAddress != uint64_t(0))
            atomicAdd(scatteringStats[scatteringStatsSlot()].invalidValues, 1u);
        // Keep the remaining work finite; CPU must reject invalidValues>0.
        return 0.0;
    }
    return max(value, 0.0);
}

// 记录一条光线：reason==1 表示因透过率过低停止，reason==2 表示达到最大步数被截断；
// 两者分别累加终止处的残差通量。
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

// 模块：太阳可见度统计（见下方原注释）。*Max 字段用 floatBitsToUint 的位表示做 atomicMax
// （非负浮点的位序与无符号整数序一致）。
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

// 记录一条太阳光线：reason 取 SOLAR_RAY_*（逃逸/遮挡/阈值停止/达步数上限）；
// legacyTransmittance >= 0 表示旧版步数截断也会截断这条光线，其与新追踪透过率之差累入 legacyExcess。
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

// 累加某阶散射被吸收的短波与 PAR（光合有效辐射）能量，用于核对各阶贡献。
void recordScatteringAbsorption(uint slot, float shortwave, float par) {
    if(setting.scatteringStatsAddress == uint64_t(0)) return;
    atomicAdd(scatteringStats[slot].absorbedShortwave, shortwave,
              gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelaxed);
    atomicAdd(scatteringStats[slot].absorbedPar, par,
              gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelaxed);
}

#endif
