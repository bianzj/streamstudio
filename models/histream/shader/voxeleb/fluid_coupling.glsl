// ==========================================================================
// 文件：fluid_coupling.glsl
// 功能：VoxelEB 体元（稀疏辐射体元）与流体网格（LBM 风场/温度场/水汽场）之间的耦合辅助函数库，
//       也包含船体、地表自由对流和 Monin–Obukhov（MOST）相似函数。仅被 include，不含 main。
// 主要模块：
//   1. 局部大气量：局部 CO2、地形背景气温/水汽、土壤含水量与土壤表面阻抗
//   2. 流体网格索引 fluidLinearIndex，及从流体格取体元邻近的气温、水汽压、风速
//   3. 船体：吃水线判断、水侧交换阻抗、钢板隐式多层导热（线性通量 G(Ts)）
//   4. 地表自由对流有效阻抗 groundExchangeRaa（MOST 与非 MOST 两个版本）
//   5. MOST 稳定度函数 mostPsiM / mostPsiH / mostPhiH（仅 VOXELEB_MOST 编译）
// 数据流：体元坐标 voxelId → 流体网格格点（按两套网格间距换算）→ 读 fluidScalarA/fluidVelocityA；
//         固体格（fluidMeta.kind == 1）取其六邻居非固体格的平均；越界/非有限值回退到背景值。
// 与其他文件：被 biochemical_m12/budget/evapo 等体元着色器与 fluid_common.glsl 包含；
//         参数来自 fluidParameters（grid、couplingSpacing、spacingTime、heightProfile、localExchange）。
// 约定：fluidScalarA.x 为温度（K）、.z 为水汽压（hPa）；fluidVelocityA.xyz 为风速（m/s）；
//         网格线性索引 = x + nx·(z + nz·y)（y 为竖直方向）；EB 着色器取气温时返回 °C。
// ==========================================================================

#ifndef HISTREAM_FLUID_COUPLING_GLSL
#define HISTREAM_FLUID_COUPLING_GLSL

// 局部环境 CO2（ppm）= 气象背景 Ca + 体元所在位置的 CO2 超额（流体输运得到）。
float localAmbientCo2Ppm(uint sparseIndex)
{
    return ubom.meteo.Ca + voxelAirs[sparseIndex].co2ExcessPpm;
}

// 地形背景气温（°C）：打开 heightProfile 时按直减率 y 从参考高度 z 外推，否则取气象站 Ta。
// terrainSaturationHpa 为 Magnus 式饱和水汽压（hPa），温度限幅 ±80 °C。
// terrainBackgroundVaporHpa 让水汽压随高度按标高 w 指数衰减，并不超过饱和值。
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

// 土壤体积含水量：体元无局部值（<0）时用材质 SMC（>1 视为百分数，乘 0.01）。
// localSoilResistance：有局部含水量时按“材质参考 rss × 参考SMC / 局部SMC”缩放，
// 是一个预设的水分可用性近似（下限 0.001 防除零）。
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

// 流体网格线性索引（越界返回 -1）；与 fluid_common.glsl 的 fluidCellFromLinear 互逆。
int fluidLinearIndex(ivec3 cell)
{
    ivec3 size = fluidParameters.grid.xyz;
    if (cell.x < 0 || cell.y < 0 || cell.z < 0 ||
        cell.x >= size.x || cell.y >= size.y || cell.z >= size.z) return -1;
    return cell.x + size.x * (cell.z + size.z * cell.y);
}

// 以下三个 local* 函数的共同做法：
//   步骤 1：体元中心（voxelId+0.5）×辐射网格间距 → 流体网格格点（voxelId.y<0 的地下体元取第一层中心）；
//   步骤 2：若该格为流体格，直接读取；若为固体格，取六邻居中非固体、值有效者的平均；
//   步骤 3：都失败则回退到背景/气象值。
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

// 局部水汽压（hPa）：未启用 heightProfile 时用气象 ea；否则在中心格及其六邻居中取有效水汽，
// 中心格有效则直接返回，否则邻居平均，结果不超过当地饱和水汽压。
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

// 模块：船体（BUILDING 类型的 Ship 材质）。吃水线判断 → 水侧/风侧换热阻抗 → 钢板导热。
// Ship hull (BUILDING voxels of a Ship material; SoilSet ship* fields, all 0
// = off and the code below returns its input unchanged).
// Voxels whose centre lies below the material waterline exchange heat with
// the scene water temperature (host: FluidParameters.localExchange.w, K;
// 0 = not set, then the air) at waterHeatTransferCoefficient.
bool shipSubmerged(uint sparseIndex, uint bioId)
{
    if (!(soilSets[bioId].waterHeatTransferCoefficient > 0.0)) return false;
    return (float(voxelLinks[sparseIndex].voxelId.y) + 0.5) * setting.scale
        < soilSets[bioId].waterlineHeight;
}

// 场景水温（°C）：宿主未设置（<=1 K）时退回气温。
float shipWaterTemperatureC(float airTemperatureC)
{
    float kelvin = fluidParameters.localExchange.w;
    return kelvin > 1.0 ? kelvin - 273.15 : airTemperatureC;
}

// Aerodynamic resistance of a ship hull voxel: wetted voxels use the water
// coefficient; with a speed the forced convection of the relative wind
// |u_wind - v_ship| competes with the scene exchange (larger conductance wins).
float shipHullRaa(uint sparseIndex, uint bioId, float raa)
{
    if (shipSubmerged(sparseIndex, bioId))
        return max(RHOA * CP / soilSets[bioId].waterHeatTransferCoefficient, 1.0);
    float speed = soilSets[bioId].shipSpeed;
    if (!(speed > 0.0)) return raa;
    float wind = max(localWindSpeed(sparseIndex), 0.05);
    float relative = sqrt(max(wind * wind + speed * speed
        + 2.0 * wind * speed * soilSets[bioId].shipWindCosine, 0.0));
    return min(raa, max(RHOA * CP / (8.3 + 4.1 * max(relative, 0.1)), 1.0));
}

// Ship hull steel plate (SoilSet plate* fields; plateThickness 0 = off).
// TLASTNUM equal implicit (backward Euler) layers; cell k holds T_k in degC
// in the Tlast profile (both sunlit and shaded slots carry the same column,
// like soil method 2). The surface node sits half a layer from the outer
// face; the inner layer exchanges with the indoor temperature through the
// half layer plus plateIndoorH, and takes the internal heat source.
// Eliminating from the inner side gives T_1 = e_0 + (1 - h_0) Ts, so the
// flux into the plate is linear in the new surface temperature:
//   G(Ts) = gs (h_0 Ts - e_0),  dG/dTs = gs h_0,  gs = 2 lambda / dx.
bool shipPlateOn(uint bioId)
{
    return soilSets[bioId].plateThickness > 0.0;
}

// 钢板隐式导热的追赶（消元）：从内侧向外表面消元，得到系数 e_k、h_k，使表面通量线性于表面温度，
// k=0 层半格距外表面，故用 2g（半格导热）；a 为热容项，gin 为内侧总换热导纳。
void shipPlateElimination(uint bioId, in float Told[TLASTNUM], float dtime,
                          out float e[TLASTNUM], out float h[TLASTNUM])
{
    float dx = soilSets[bioId].plateThickness / float(TLASTNUM);
    float lambdaPlate = max(soilSets[bioId].plateLambda, 1.0e-6);
    float a = soilSets[bioId].plateCapacity * dx / max(dtime, 1.0);
    float g = lambdaPlate / dx;
    float gin = 1.0 / (dx / (2.0 * lambdaPlate) + 1.0 / max(soilSets[bioId].plateIndoorH, 1.0e-6));
    float denominator = a + g + gin;
    e[TLASTNUM - 1] = (a * Told[TLASTNUM - 1] + gin * soilSets[bioId].plateIndoorC
                       + soilSets[bioId].internalHeatFlux) / denominator;
    h[TLASTNUM - 1] = (a + gin) / denominator;
    for (int k = TLASTNUM - 2; k >= 0; k--)
    {
        float gl = k == 0 ? 2.0 * g : g;
        float d = a + gl + g * h[k + 1];
        e[k] = (a * Told[k] + g * e[k + 1]) / d;
        h[k] = (a + g * h[k + 1]) / d;
    }
}

// Heat flux (W m-2) from the surface into the plate for surface temperature
// Ts (degC), and its slope with Ts.
float shipPlateSurfaceFlux(uint bioId, float Ts, float e0, float h0)
{
    float gs = 2.0 * max(soilSets[bioId].plateLambda, 1.0e-6) * float(TLASTNUM)
               / soilSets[bioId].plateThickness;
    return gs * (h0 * Ts - e0);
}

float shipPlateSurfaceSlope(uint bioId, float h0)
{
    float gs = 2.0 * max(soilSets[bioId].plateLambda, 1.0e-6) * float(TLASTNUM)
               / soilSets[bioId].plateThickness;
    return gs * h0;
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
// 自由对流适用的类别：土壤、水体、建筑；植被不适用。
// groundFreeConductance(x) = 0.0025·x·(x²+1)^(-1/3)，x 为表面-气温差（K，限幅 [0,60]）。
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

// MOST 版本：stabilityCorrectionEnabled 由宿主在 localExchange.y 中打开；
// groundExchangeRaa 在 MOST 开启时取 1/ra 与自由对流导纳的较大者，避免浮力重复计入。
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
// MOST 稳定度修正函数：zeta = z/L 限幅在 [-5, 1]；不稳定时 psi_M 用 Paulson 形式，
// 稳定时用线性（-5ζ）；psiH、phiH 为对应的热量修正与无量纲梯度函数。
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
// 非 MOST（中性）版本：始终把自由对流导纳与 1/ra 并联（见上方长注释）。
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
