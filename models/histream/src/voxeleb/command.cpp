// ==========================================================================
// 文件：command.cpp
// 功能：VoxelEB（体元能量平衡）的 GPU 命令调度：按时间节点依次录制/提交各计算阶段，
//       组织短波辐射、热红外辐射、能量平衡迭代、MOST 稳定度迭代和 LBM 流体步进，并读回诊断。
// 主要模块：
//   A. 匿名命名空间辅助函数：短波缓冲清零、EBState 清零/读回、MOST 开关与 1/L 上传、
//      船体水温上传、目标 1/L 估计、散射/太阳可见度统计的清零与读回、诊断 JSON 输出
//   B. Command::create / destroy：fence 与信号量的创建和销毁
//   C. Command::runEB：单节点能量平衡全流程（跳过短波 → gap → 直射 → 多次散射 → 直射 TIR →
//      气动阻抗 → ≤50 次 [diffuseTIR, bio, evapo, budget] 迭代 → 收尾 → updateTp）
//   D. Command::runRT：传感器成像（out 阶段，二维派发）
//   E. Command::runFluid：LBM 流体步进（fluidLbm + fluidCommit，32 步一批）
//   F. Command::submit / recordCommandBuffer / waitFence：单阶段录制、提交与同步
// 数据流：VoxelebIO（setting 推送常量、气象、各 GPU 缓冲）→ submit 逐阶段派发 → GPU 写体元
//   温度/通量/EBState → readEnergyState 读回未收敛计数与稳定度累加量 → 决定是否继续迭代；
//   诊断写到 <projectDir>/diagnostics/scattering_node=<k>.json 与控制台
// 与其他文件的关系：pipeline.cpp 提供 m_pipelines[stage]/m_pipelineLayout；descriptor.cpp 提供
//   描述符集；shader/voxeleb/*.comp 为各阶段着色器；voxeleb.cpp 逐节点调用 runEB/runRT/runFluid
// 重要约定：温度 K（气象 Ta 为 °C）；通量 W m-2；场景 1/L 单位 m-1；EBState 为定点整数累加
//   （sensible 0.1 W m-2、ustar 1e-4 m s-1、height 0.01 m、水温 0.01 K）；每次 submit 后
//   必须 waitFence（否则下一次 submit 会复用同一个 fence）
// ==========================================================================

//
// Created by admin on 2024/1/26.
//

#include "command.h"
#include "base/vkcheck.h"
#include "base/voxelflags.h"
#include "base/voxellimits.h"
#include <cstdlib>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>

// 模块 A：文件内部辅助函数（匿名命名空间）。多数函数各自新建临时 nvvk::CommandPool，
// 录入一次填充/拷贝命令并 submitAndWait 同步执行；主流程 Command::runEB 见后文。
namespace {

// HISTREAM_VOXEL_PASS_TIMING=1: wall-clock time (submit to fence) summed per stage over the whole run.
// Printed as PASS_TIMING lines when the run's last node ends (see Command::runEB) so that the
// per-pass effect of shader changes can be compared without a GPU profiler.
struct PassTiming {
    std::array<double, 24> milliseconds{};
    std::array<int, 24> calls{};
    std::array<double, 24> minimum{};   // fastest single call: the least disturbed by other GPU users
    int pendingStage{-1};
    std::chrono::steady_clock::time_point start{};
};
PassTiming& passTiming() { static PassTiming timing; return timing; }

void printPassTiming()
{
    static const char* names[] = {"gap", "directVNIR", "directTIR", "scatteringVNIR", "diffuseTIR", "bio",
        "aero", "evapo", "budget", "updateL", "updateTp", "fluidLbm", "fluidCommit", "out", "scatteringTrace",
        "scatTrace_s1_sky", "scatTrace_s2", "scatTrace_s3_order", "scatTrace_s4_skycache"};
    PassTiming& timing = passTiming();
    for (size_t stage = 0; stage < 19; ++stage) {
        if (timing.calls[stage] == 0) continue;
        std::cout << "PASS_TIMING\tstage=" << names[stage] << "\tcalls=" << timing.calls[stage]
                  << "\tms=" << timing.milliseconds[stage] << "\tminCallMs=" << timing.minimum[stage] << std::endl;
    }
}

// 多次散射统计的 CPU 端汇总（uint64/double 累加），与 GPU 端 ScatteringOrderStats（32 字节，
// uint32/float）字段一一对应；按光谱分组读回后累加，以避免 GPU 计数溢出和精度损失。
struct ScatteringAggregate {
    uint64_t maxStepTruncations{0}, transmittanceStops{0}, tracedRays{0}, reserved{0};
    double absorbedShortwave{0}, absorbedPar{0}, transmittanceResidual{0}, truncatedResidual{0};
};

// 作用：把只有短波 pass 才会写的体元缓冲清零（方向 VoxelDir、净辐射 VoxelNetRad、Pnet、
//       辐射场 VoxelRad 及图像环境项）。用于无太阳且无实测短波的节点（夜间等被跳过的节点）。
// 输入：modelio（n_voxel、各缓冲、setting.imageEnvironment、n_wave）；无返回值。
// 副作用：同步提交一次 vkCmdFillBuffer + 内存屏障（TRANSFER → COMPUTE）。
// Zeroes every voxel field that only a shortwave pass would write. A node with no
// sun and no measured shortwave contributes exactly zero to all of them, so a
// skipped node has to clear them explicitly instead of inheriting the previous
// node's radiance.
void clearShortwaveFields(const std::shared_ptr<VoxelebIO>& modelio)
{
    nvvk::CommandPool commandPool(modelio->m_device, modelio->m_queueIndex);
    vk::CommandBuffer command = commandPool.createCommandBuffer();
    const VkDeviceSize voxelCount = static_cast<VkDeviceSize>(modelio->n_voxel);
    // Buffer sizes are not stored on nvvk::Buffer, so they follow the layouts the
    // buffer builder uses: VoxelDir, VoxelNetRad and VoxelPnet are per voxel and
    // the radiance field holds DIFFUSENUM directions per voxel.
    // 步骤 1：列出需要清零的缓冲及其字节数（缓冲大小未存于 nvvk::Buffer，按创建时的布局推算）。
    const std::array<std::pair<nvvk::Buffer*, VkDeviceSize>, 4> ranges{{
        {modelio->m_voxelio->m_pDirBuffer.get(), voxelCount * sizeof(VoxelDir)},
        {modelio->m_voxelio->m_pNetRadBuffer.get(), voxelCount * sizeof(VoxelNetRad)},
        {modelio->m_voxelio->m_pPnetBuffer.get(), voxelCount * sizeof(VoxelPnet)},
        {modelio->m_voxelio->m_pRadsBuffer.get(),
         voxelCount * DIFFUSENUM * static_cast<VkDeviceSize>(sizeof(VoxelRad))}}};
    // 步骤 2：逐个缓冲清零（缓冲未创建或字节数为 0 时跳过）。
    for (const auto& [buffer, bytes] : ranges) {
        if (buffer && buffer->buffer && bytes > 0) {
            vkCmdFillBuffer(command, buffer->buffer, 0, bytes, 0U);
        }
    }
    // The optical image's multiple-scattering environment follows the field
    // and the sky gap transmittances (buffer.cpp): no shortwave, no environment.
    if (modelio->setting.imageEnvironment != 0 && modelio->m_voxelio->m_pRadsBuffer
        && modelio->m_voxelio->m_pRadsBuffer->buffer) {
        // 步骤 3：光学成像用的多次散射环境项存放在辐射场缓冲 1.5×DIFFUSENUM×体元数 的偏移之后，
        // 每个 VoxelRad 容纳 2 个 float，所以字节数按 (floats+1)/2 向上取整（布局见 buffer.cpp，待核对）。
        const VkDeviceSize layoutVoxels = static_cast<VkDeviceSize>(voxelflags::testRadianceVoxels(modelio->n_voxel));
        const VkDeviceSize environmentOffset = layoutVoxels * DIFFUSENUM * 3 / 2 * sizeof(VoxelRad);
        const VkDeviceSize environmentFloats =
            layoutVoxels * static_cast<VkDeviceSize>(std::max(modelio->n_wave, 0));
        vkCmdFillBuffer(command, modelio->m_voxelio->m_pRadsBuffer->buffer, environmentOffset,
                        (environmentFloats + 1) / 2 * sizeof(VoxelRad), 0U);
    }
    // 步骤 4：填充（传输写）→ 后续计算着色器读写之间的内存屏障，然后同步提交等待完成。
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier,
                         0, nullptr, 0, nullptr);
    commandPool.submitAndWait(command);
}

// 作用：把 EBState（能量平衡的 MOST 定点累加状态与未收敛计数）整块清零，
//       在每次 budget 派发前调用，让 GPU 原子累加从 0 开始。同步提交。
void clearEnergyState(const std::shared_ptr<VoxelebIO>& modelio)
{
    // DEFERRED[低] 性能: 每次 EB 迭代都新建/销毁一个 nvvk::CommandPool 并 submitAndWait 一个 4 字节 fill（readEnergyState 同样），50 次迭代 × 每节点产生大量池创建与同步往返；建议：把 vkCmdFillBuffer 直接录入 budget 派发的命令缓冲（submit 中已有 TRANSFER_WRITE→SHADER 屏障）。
    nvvk::CommandPool commandPool(modelio->m_device, modelio->m_queueIndex);
    vk::CommandBuffer command = commandPool.createCommandBuffer();
    vkCmdFillBuffer(command, modelio->m_voxelio->m_pStateBuffer->buffer,
                    0, sizeof(EBState), 0);
    commandPool.submitAndWait(command);
}

// 作用：把 GPU 上的 EBState 拷到主机可见的 readback 缓冲并读出。
// 输入：readback（至少 sizeof(EBState) 字节的主机可见缓冲）；full 非空时拷出完整 EBState。
// 输出：返回 EBState.count（超出能量残差阈值的状态数，runEB 据此判断收敛）。
// 步骤：计算写 → 传输读屏障 → 拷贝 → 传输写 → 主机读屏障 → 同步等待 → map 读取。
uint32_t readEnergyState(const std::shared_ptr<VoxelebIO>& modelio,
                         const nvvk::Buffer& readback, EBState* full = nullptr)
{
    nvvk::CommandPool commandPool(modelio->m_device, modelio->m_queueIndex);
    vk::CommandBuffer command = commandPool.createCommandBuffer();
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    VkBufferCopy copy{};
    copy.size = sizeof(EBState);
    vkCmdCopyBuffer(command, modelio->m_voxelio->m_pStateBuffer->buffer,
                    readback.buffer, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    commandPool.submitAndWait(command);

    const void* mapped = modelio->m_pAlloc->map(readback);
    const uint32_t count = *static_cast<const uint32_t*>(mapped);
    if (full != nullptr) std::memcpy(full, mapped, sizeof(EBState));
    modelio->m_pAlloc->unmap(readback);
    return count;
}

// 作用：把 MOST 稳定度修正开关（enabled）与场景 1/L（inverseL，单位 m-1）写入 GPU 上的
//       FluidParameters UBO 的预留分量 localExchange.y/.z，供 budget/evapo/aeresist 的
//       _most 版本读取；关闭时两个分量都写 0。副作用：同步提交 vkCmdUpdateBuffer。
// Publish the MOST switch and the current scene 1/L to the shaders through
// the reserved localExchange.y/z lanes of FluidParameters.
void uploadStability(const std::shared_ptr<VoxelebIO>& modelio, bool enabled, double inverseL)
{
    modelio->fluidParameters.localExchange.y = enabled ? 1.0f : 0.0f;
    modelio->fluidParameters.localExchange.z = enabled ? static_cast<float>(inverseL) : 0.0f;
    if (!modelio->m_pFluidParameters)
        throw std::runtime_error("VoxelEB stability correction needs the FluidParameters buffer");
    nvvk::CommandPool commandPool(modelio->m_device, modelio->m_queueIndex);
    vk::CommandBuffer command = commandPool.createCommandBuffer();
    vkCmdUpdateBuffer(command, modelio->m_pFluidParameters->buffer, 0,
                      sizeof(FluidParameters), &modelio->fluidParameters);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    commandPool.submitAndWait(command);
}

// 作用：把船体水线接触用的场景水温（kelvin，K）写入 FluidParameters 的 localExchange.w，
//       0 表示尚未设置。与 uploadStability 一样用 vkCmdUpdateBuffer 上传整个结构体。
// Ship waterline contact: publish the scene water temperature [K] to the
// reserved localExchange.w lane (0 = unset; only written when a Ship material
// has a waterline, so every other run keeps the lane at 0).
void uploadShipWaterTemperature(const std::shared_ptr<VoxelebIO>& modelio, double kelvin)
{
    modelio->fluidParameters.localExchange.w = static_cast<float>(kelvin);
    if (!modelio->m_pFluidParameters)
        throw std::runtime_error("VoxelEB ship waterline needs the FluidParameters buffer");
    nvvk::CommandPool commandPool(modelio->m_device, modelio->m_queueIndex);
    vk::CommandBuffer command = commandPool.createCommandBuffer();
    vkCmdUpdateBuffer(command, modelio->m_pFluidParameters->buffer, 0,
                      sizeof(FluidParameters), &modelio->fluidParameters);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    commandPool.submitAndWait(command);
}

// 作用：由场景显热 H 与摩擦速度 u* 估计目标 1/L：1/L = -κ g H / (ρ cp Ta u*³)。
// 限幅：u* 下限 0.001 m/s（避免除零）；z 下限 0.1 m；(z-d)/L 限制在 [-5, 1]，
//       这是 MOST 公式的适用范围，超出后稳定度函数不可靠。常数 ρ、cp 与 shader/global.glsl 一致。
// Desired scene 1/L from H (W m-2 per ground area) and u*, clamped to the
// MOST applicability range -5 <= (z-d)/L <= 1 (energyphysics.h form;
// rho and cp as in shader/global.glsl).
double desiredInverseObukhov(double sensibleHeat, double ustar, double airTemperatureK, double height)
{
    constexpr double rho = 1.2047, cp = 1004.0, kappa = 0.4, gravity = 9.81;
    const double u = std::max(ustar, 0.001);
    const double raw = -kappa * gravity * sensibleHeat / (rho * cp * airTemperatureK * u * u * u);
    const double z = std::max(height, 0.1);
    return std::clamp(raw * z, -5.0, 1.0) / z;
}

// 作用：清零统计缓冲中前 4 条 ScatteringOrderStats（天空未散射 + 3 个散射阶），
//       每个光谱分组开始前调用（GPU 计数器按分组复位，CPU 侧再累加）。
void resetScatteringStats(const std::shared_ptr<VoxelebIO>& modelio)
{
    nvvk::CommandPool pool(modelio->m_device, modelio->m_queueIndex);
    VkCommandBuffer command = pool.createCommandBuffer();
    vkCmdFillBuffer(command, modelio->m_pScatteringStats->buffer, 0,
                    4 * sizeof(ScatteringOrderStats), 0);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    pool.submitAndWait(command);
}

// 作用：同步读回 4 条 ScatteringOrderStats。经临时主机可见 staging 缓冲拷贝并读取后销毁。
std::array<ScatteringOrderStats, 4> readScatteringStats(
    const std::shared_ptr<VoxelebIO>& modelio)
{
    std::array<ScatteringOrderStats, 4> stats{};
    const VkDeviceSize bytes = sizeof(stats);
    nvvk::Buffer staging = modelio->m_pAlloc->createBuffer(bytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    nvvk::CommandPool pool(modelio->m_device, modelio->m_queueIndex);
    VkCommandBuffer command = pool.createCommandBuffer();
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    VkBufferCopy copy{};
    copy.size = bytes;
    vkCmdCopyBuffer(command, modelio->m_pScatteringStats->buffer, staging.buffer, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    pool.submitAndWait(command);
    const void* mapped = modelio->m_pAlloc->map(staging);
    std::memcpy(stats.data(), mapped, static_cast<size_t>(bytes));
    modelio->m_pAlloc->unmap(staging);
    modelio->m_pAlloc->destroy(staging);
    return stats;
}

// 作用：清零统计缓冲中紧随散射统计之后的 SolarVisibilityStats（偏移 kSolarVisibilityStatsOffset），
//       每个能量平衡节点的 gap（太阳可见度）阶段之前调用。
void resetSolarStats(const std::shared_ptr<VoxelebIO>& modelio)
{
    nvvk::CommandPool pool(modelio->m_device, modelio->m_queueIndex);
    VkCommandBuffer command = pool.createCommandBuffer();
    vkCmdFillBuffer(command, modelio->m_pScatteringStats->buffer,
                    kSolarVisibilityStatsOffset, sizeof(SolarVisibilityStats), 0);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    pool.submitAndWait(command);
}

// 作用：同步读回 SolarVisibilityStats（64 字节），方式同 readScatteringStats。
SolarVisibilityStats readSolarStats(const std::shared_ptr<VoxelebIO>& modelio)
{
    SolarVisibilityStats stats{};
    const VkDeviceSize bytes = sizeof(stats);
    nvvk::Buffer staging = modelio->m_pAlloc->createBuffer(bytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    nvvk::CommandPool pool(modelio->m_device, modelio->m_queueIndex);
    VkCommandBuffer command = pool.createCommandBuffer();
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    VkBufferCopy copy{};
    copy.srcOffset = kSolarVisibilityStatsOffset;
    copy.size = bytes;
    vkCmdCopyBuffer(command, modelio->m_pScatteringStats->buffer, staging.buffer, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    pool.submitAndWait(command);
    const void* mapped = modelio->m_pAlloc->map(staging);
    std::memcpy(&stats, mapped, static_cast<size_t>(bytes));
    modelio->m_pAlloc->unmap(staging);
    modelio->m_pAlloc->destroy(staging);
    return stats;
}

// 作用：把 GPU 里以 uint 位型存放的 float 取回（*Max 统计字段用 floatBitsToUint 做原子最大）。
float floatFromBits(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// 作用：计算垂直于太阳光线平面上的直射辐照度 [W m-2] = Rin × 直射比例 / cos(sza)，
//       用于把太阳可见度的无量纲残差换算为 W m-2 误差上界；cos ≤ 0.01 时太阳 pass 被跳过，返回 0。
// Beam irradiance on a plane normal to the sun, W/m2: Rin x direct fraction
// / cos(theta_s), zero when the solar pass is skipped (cos <= 0.01).
double solarBeamNormalIrradiance(const std::shared_ptr<VoxelebIO>& modelio)
{
    if (modelio->angles.empty()) return 0.0;
    const double cosine = std::cos(modelio->angles.front().sza * 3.14159265358979323846 / 180.0);
    if (!(cosine > 0.01)) return 0.0;
    return std::max(0.0, double(modelio->meteo.Rin)) *
        std::clamp(double(modelio->light.direct), 0.0, 1.0) / cosine;
}

// 作用：把单节点的太阳可见度诊断拼成 JSON 字符串，写入 scattering_node=*.json 的
//       solarVisibility 字段。jsonNumber 把非有限值输出为 null；误差上界 = 束辐照度 × 最大残差。
// JSON object and log fields of the per-node solar-visibility diagnostics.
std::string solarStatsJson(const std::shared_ptr<VoxelebIO>& modelio,
                           const std::function<std::string(double)>& jsonNumber)
{
    const auto& s = modelio->solarStats;
    const double beam = modelio->solarBeamIrradiance;
    std::ostringstream out;
    out << std::setprecision(10)
        << "{\"tracedRays\": " << s.tracedRays
        << ", \"stepLimit\": " << s.stepLimit
        << ", \"maxStepsUsed\": " << s.maxStepsUsed
        << ", \"solarTruncations\": " << s.truncations
        << ", \"truncatedResidualSum\": " << jsonNumber(s.truncatedResidual)
        << ", \"truncatedResidualMax\": " << jsonNumber(floatFromBits(s.truncatedResidualMax))
        << ", \"thresholdStops\": " << s.thresholdStops
        << ", \"transmittanceCutoff\": 0.0001"
        << ", \"thresholdResidualSum\": " << jsonNumber(s.thresholdResidual)
        << ", \"thresholdResidualMax\": " << jsonNumber(floatFromBits(s.thresholdResidualMax))
        << ", \"beyondRadiationMaxSteps\": " << s.legacyTruncations
        << ", \"legacyCutExcessSum\": " << jsonNumber(s.legacyExcess)
        << ", \"legacyCutExcessMax\": " << jsonNumber(floatFromBits(s.legacyExcessMax))
        << ", \"beamNormalIrradiance\": " << jsonNumber(beam)
        << ", \"truncationErrorBoundMaxWm2\": "
        << jsonNumber(beam * floatFromBits(s.truncatedResidualMax))
        << ", \"thresholdErrorBoundMaxWm2\": "
        << jsonNumber(beam * floatFromBits(s.thresholdResidualMax)) << "}";
    return out.str();
}

// 作用：把单节点的短波多次散射诊断写成 <projectDir>/diagnostics/scattering_node=<k>.json。
// 输入：stats（4 个阶的 CPU 汇总）、milliseconds（短波总耗时）、orderMilliseconds（各阶耗时）。
// 说明：残差是射线透过率之和（无量纲），不是遗漏的能量；通量按体元求和，不是区域总功率。
// 副作用：创建目录并写文件；打开失败则抛 std::runtime_error。
void writeScatteringStats(const std::shared_ptr<VoxelebIO>& modelio,
    const std::array<ScatteringAggregate, 4>& stats, double milliseconds,
    const std::array<double, 3>& orderMilliseconds)
{
    // 步骤 1：确定输出路径并打开文件（文件名含节点号 k_node）。
    const auto directory = std::filesystem::path(modelio->projectDir) / "diagnostics";
    std::filesystem::create_directories(directory);
    const auto file = directory / ("scattering_node=" + std::to_string(modelio->k_node) + ".json");
    std::ofstream output(file);
    if (!output) throw std::runtime_error("Cannot write scattering diagnostics: " + file.string());
    const auto jsonNumber = [](double value) {
        if (!std::isfinite(value)) return std::string("null");
        std::ostringstream text;
        text << std::setprecision(10) << value;
        return text.str();
    };
    // 步骤 2：写 JSON 头部：格式标识、请求/完成的散射阶数、体元数、射线步数上限、耗时，
    // 以及各统计量的定义说明和单节点太阳可见度诊断。
    output << std::setprecision(10)
        << "{\n  \"format\": \"histream-shortwave-scattering-v1\",\n"
        << "  \"node\": " << modelio->k_node << ",\n"
        << "  \"requestedOrders\": " << modelio->shortwaveScatteringOrders << ",\n"
        << "  \"completedOrders\": " << modelio->shortwaveScatteringOrders << ",\n"
        << "  \"orderContributionEarlyStop\": false,\n"
        << "  \"orderDefinition\": \"sky orders 0..K plus solar orders 1..K; "
           "higher orders as a spectral-invariant leaf tail; direct solar absorption is separate\",\n"
        << "  \"voxelCount\": " << modelio->n_voxel << ",\n"
        << "  \"directions\": 64,\n"
        << "  \"maxRaySteps\": " << modelio->setting.maxStep << ",\n"
        << "  \"transmittanceCutoff\": 0.01,\n"
        << "  \"elapsedMs\": " << milliseconds << ",\n"
        << "  \"statsAvailable\": true,\n"
        << "  \"statsScope\": \"perOrder: diffuse shortwave paths; solarVisibility: direct-sun visibility rays\",\n"
        << "  \"counterAggregation\": \"GPU counters reset per spectral group; CPU uint64 totals\",\n"
        << "  \"residualAggregation\": \"sum of dimensionless ray throughputs at cutoff; not omitted energy\",\n"
        << "  \"fluxAggregation\": \"sum of per-voxel flux densities; not total domain power\",\n"
        << "  \"solarVisibilityDefinition\": \"rays followed until they leave the scene (stepLimit); "
           "per-voxel direct irradiance error <= residual * beamNormalIrradiance; "
           "beyondRadiationMaxSteps/legacyCutExcess describe the former radiationMaxSteps cut\",\n"
        << "  \"solarVisibility\": " << solarStatsJson(modelio, jsonNumber) << ",\n"
        << "  \"perOrder\": [\n";
    // 步骤 3：逐条写 perOrder。index 0..2 对应第 1..3 阶散射，index 3 对应第 0 阶
    // （未散射的天空项，无独立计时）；任一统计量非有限或 reserved（无效值计数）非 0 则 valid=false。
    bool valid = true;
    for (int index = 0; index < 4; ++index) {
        const auto& value = stats[index];
        valid = valid && value.reserved == 0 && std::isfinite(value.absorbedShortwave) &&
            std::isfinite(value.absorbedPar) && std::isfinite(value.transmittanceResidual) &&
            std::isfinite(value.truncatedResidual);
        output << "    {\"order\": " << (index == 3 ? 0 : index + 1)
            << ", \"elapsedMs\": " << (index == 3 ? 0.0 : orderMilliseconds[index])
            << ", \"maxStepTruncations\": " << value.maxStepTruncations
            << ", \"transmittanceStops\": " << value.transmittanceStops
            << ", \"tracedRays\": " << value.tracedRays
            << ", \"invalidValues\": " << value.reserved
            << ", \"absorbedShortwave\": " << jsonNumber(value.absorbedShortwave)
            << ", \"absorbedPar\": " << jsonNumber(value.absorbedPar)
            << ", \"transmittanceResidual\": " << jsonNumber(value.transmittanceResidual)
            << ", \"truncatedResidual\": " << jsonNumber(value.truncatedResidual) << "}"
            << (index == 3 ? "\n" : ",\n");
    }
    output << "  ],\n  \"valid\": " << (valid ? "true" : "false") << "\n}\n";
    output.close();
    // 步骤 4：在控制台输出一行 SHORTWAVE_SCATTERING 摘要（供日志/回归脚本解析）；
    // 诊断不合法时在文件写完之后才抛异常，保证现场文件可供排查。
    uint64_t truncations = 0;
    for (const auto& value : stats) truncations += value.maxStepTruncations;
    const auto& solar = modelio->solarStats;
    std::cout << "SHORTWAVE_SCATTERING\tnode=" << modelio->k_node
        << "\trequested=" << modelio->shortwaveScatteringOrders
        << "\telapsedMs=" << milliseconds
        << "\ttruncations=" << truncations
        << "\tsolarTruncations=" << solar.truncations
        << "\tsolarResidualSum=" << solar.truncatedResidual
        << "\tsolarResidualMax=" << floatFromBits(solar.truncatedResidualMax)
        << "\tsolarBeyondMaxSteps=" << solar.legacyTruncations
        << "\tsolarMaxSteps=" << solar.maxStepsUsed
        << "\tdiagnostics=" << file.string() << std::endl;
    if (!valid) throw std::runtime_error("Invalid shortwave scattering state: " + file.string());
}

} // namespace




// 模块 B：Command 类实现 —— 录制并提交 VoxelEB 各计算阶段，同步等待完成。
// 作用：创建公用 fence 与 n_pipeline+1 个信号量（信号量目前仅保留，submit 中的等待/
//       信号已被注释掉，实际同步靠 fence + 内存屏障）。返回 true；Vulkan 调用失败时由 vkCheck 报错。
bool Command::create(std::shared_ptr<VoxelebIO> &modelio){
    // for the firest init
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCheck(vkCreateFence(modelio->m_device, &fci, nullptr, &modelio->m_fence), "vkCreateFence");
//    vkWaitForFences(modelio->m_device, 1, &(modelio->m_fence), VK_TRUE, UINT64_MAX);
    vkResetFences(modelio->m_device, 1,  &(modelio->m_fence));

    // Here 4 means number of pipelines + 1
    modelio->m_semaphores.resize(modelio->n_pipeline+1);
    std::generate_n( modelio->m_semaphores.begin(), modelio->n_pipeline+1,
                     [&]
                     {
                         VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                         VkSemaphore semaphore;
                         vkCheck(vkCreateSemaphore( modelio->m_device, &sci, nullptr, &semaphore), "vkCreateSemaphore");
                         return semaphore;
                     });


    // if (vkCreateFence(modelio->m_device, &fci, nullptr, &modelio->m_fence) != VK_SUCCESS)
    //     return false;

    return true;
}

// 模块 C：runEB —— 单个时间节点的 VoxelEB 能量平衡完整流程（GPU 阶段的主调度）。
// 输入：modelio（setting 推送常量、气象 meteo、太阳角 angles、各开关与缓冲）；
// 输出：体元温度/通量缓冲（GPU 内）、energyIterations / energyFinalUnconvergedStateCount /
//       stabilityXxx / shipWaterK 等诊断写回 modelio；固定返回 true，失败以异常报告。
// 步骤：1 准备；2 判定是否跳过短波；3 短波（太阳可见度 → 直射 → 多次散射 → 诊断文件）；
//       4 直射热红外；5 初始化迭代状态；6 气动阻抗/稳定度；7 能量平衡主迭代（最多 50 次）；
//       8 用最终温度刷新并 finalize；9 updateTp 提交温度历史。每个阶段 submit 后 waitFence。
bool Command::runEB(std::shared_ptr<VoxelebIO> &modelio){

    modelio->m_currentSemaphore = 1;

    // 步骤 1：一维派发组数 = ceil(n_voxel / GROUP_SIZEX)，每线程处理一个体元。
    glm::ivec3 voxelSize1D = glm::ivec3((modelio->n_voxel + (GROUP_SIZEX - 1)) / GROUP_SIZEX, 1, 1);
    // DEFERRED[中] GPU同步: 与 VoxelRT 相同，1D 派发组数 n_voxel/64 可能超过 maxComputeWorkGroupCount[0]（部分 GPU 为 65535，即 >4.19M 体元），属未定义行为；VoxelEB 面向城市场景更易触发；建议：查询限制并改为 2D 派发或分批。
    auto & descSet = modelio->m_descSet;
    auto & setting = modelio->setting;


    //--------------------------------------------------

    int stageInt = 0;

    std::vector<VkSemaphore> beginSemaph;
    beginSemaph.emplace_back(modelio->m_semaphores[0]);

    // 步骤 2：判定“无短波节点”：实测入射短波 Rin 不超过 1e-3 W m-2 且太阳天顶角 ≥ 90°
    // （无太阳角信息时按 180° 即夜间处理）。此类节点跳过 gap/直射/散射三类 pass，改为清零短波缓冲。
    // A node with the sun below the horizon and no measured shortwave has no
    // sunbeam and no sky VNIR source: the gap, direct and every scattering order
    // would accumulate exactly zero. Clearing those fields and skipping the three
    // passes is numerically identical to tracing them and removes the dominant
    // cost of night nodes.
    constexpr float kShortwaveFloor = 1.0e-3f;
    const float solarZenith = modelio->angles.empty() ? 180.0f : modelio->angles.front().sza;
    const bool noShortwaveNode = !(modelio->meteo.Rin > kShortwaveFloor) && solarZenith >= 90.0f;

    // 散射阶数上限：XML/JSON 读取处（fileio.cpp、fileio_json.cpp）已限定 shortwaveScatteringOrders 为 1~3，
    // 这里再做一次防御性检查，保证下面按阶累加的 orderMilliseconds（3 项）不会越界。
    if (modelio->shortwaveScatteringOrders < 1 || modelio->shortwaveScatteringOrders > 3)
        throw std::runtime_error("shortwaveScatteringOrders must be 1, 2 or 3");
    std::array<double, 3> orderMilliseconds{};
    std::array<ScatteringAggregate, 4> scatteringStats{};
    modelio->solarStats = SolarVisibilityStats{};
    modelio->solarBeamIrradiance = 0.0;
    if (noShortwaveNode) {
        const auto skipBegin = std::chrono::steady_clock::now();
        clearShortwaveFields(modelio);
        const double skipMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - skipBegin).count();
        writeScatteringStats(modelio, scatteringStats, skipMs, orderMilliseconds);
        std::cout << "SHORTWAVE_SKIPPED\tnode=" << modelio->k_node
                  << "\trin=" << modelio->meteo.Rin
                  << "\tsza=" << solarZenith << std::endl;
    } else {
    // 步骤 3.1：太阳可见度（gap 阶段）：先清零统计，派发后读回统计并换算束辐照度。
    resetSolarStats(modelio);
    submit(modelio, VoxelEBStage::gap, voxelSize1D, std::nullopt, std::nullopt);
    waitFence(modelio);
    modelio->solarStats = readSolarStats(modelio);
    modelio->solarBeamIrradiance = solarBeamNormalIrradiance(modelio);

    // 步骤 3.2：VNIR 直射短波吸收。
    submit(modelio, VoxelEBStage::directVNIR, voxelSize1D, std::nullopt, std::nullopt);
    waitFence(modelio);

    const auto shortwaveBegin = std::chrono::steady_clock::now();
    if (uint64_t(modelio->n_voxel) * 64 > UINT32_MAX) {
        throw std::runtime_error("VoxelEB: " + voxellimits::grouped(uint64_t(modelio->n_voxel))
            + " voxels exceed the 32-bit scattering diagnostic counters of one spectral group (at most "
            + voxellimits::grouped(UINT32_MAX / 64) + " voxels). Reduce the scene size.");
    }
    // 步骤 3.3：多次散射。scatteringStage 取值：0 清零漫射吸收累加量；1 追踪未碰撞天空场并缓存
    // 间隙透过率；4 复用缓存、仅按本组天空辐照缩放；2 构造第 order 阶散射源；3 追踪第 order 阶
    // 入射场并累计吸收（定义见 voxelrad_scattering_VNIR.comp 文件头）。
    setting.scatteringStage = 0;
    setting.scatteringOrders = modelio->shortwaveScatteringOrders;
    submit(modelio, VoxelEBStage::scatteringTrace, voxelSize1D, std::nullopt, std::nullopt);
    waitFence(modelio);
    // 按光谱分组循环 VNIR 波段下标 0..2000（共 2001 个，1 nm 间隔）；每组宽度为 n_jump（至少 1）。
    // 分组在下标 301 处强制断开，使 PAR（含 700 nm）落在同一积分域。
    // Each spectral group owns the entire directional field until all its
    // orders finish. Reusing two fields keeps memory independent of K.
    const int width = std::max(setting.n_jump, 1);
    bool skyTransmittanceReady = false;
    for (int first = 0; first < 2001;) {
        resetScatteringStats(modelio);
        int end = std::min(first + width, 2001);
        // PAR includes 700 nm: indices 0..300 form one integration domain.
        if (first < 301 && end > 301) end = 301;
        setting.scatteringBandStart = first;
        setting.scatteringBandEnd = end;
        // Stage 1 traces the spectrally neutral uncollided sky field once per
        // node and stores its gap transmittance; later groups (stage 4) only
        // rescale it by their sky irradiance instead of retracing 64 rays.
        setting.scatteringStage = skyTransmittanceReady ? 4 : 1;
        setting.scatteringOrder = 0;
        submit(modelio, VoxelEBStage::scatteringTrace, voxelSize1D, std::nullopt, std::nullopt);
        waitFence(modelio);
        skyTransmittanceReady = true;
        // 逐阶散射：先 stage 2 由上一阶入射场构造源，再 stage 3 追踪本阶入射场；
        // orderMilliseconds[order-1] 累加各阶耗时（诊断用，数组只有 3 项，对应最多 3 阶）。
        for (int order = 1; order <= modelio->shortwaveScatteringOrders; ++order) {
            const auto orderBegin = std::chrono::steady_clock::now();
            setting.scatteringOrder = order;
            setting.scatteringStage = 2;
            submit(modelio, VoxelEBStage::scatteringVNIR, voxelSize1D, std::nullopt, std::nullopt);
            waitFence(modelio);
            setting.scatteringStage = 3;
            submit(modelio, VoxelEBStage::scatteringTrace, voxelSize1D, std::nullopt, std::nullopt);
            waitFence(modelio);
            orderMilliseconds[order - 1] += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - orderBegin).count();
        }
        // 每组读回 GPU 统计并累加到 CPU 的 uint64/double 汇总中。
        // Reset/read per group so 2001-band jobs cannot wrap uint32 GPU
        // counters. The inexpensive 128-byte transfer feeds uint64 totals.
        // （计数器按光谱分组在 GPU 端清零，故每组读回一次；CPU 端用 64 位累加。）
        const auto groupStats = readScatteringStats(modelio);
        for (size_t index = 0; index < scatteringStats.size(); ++index) {
            auto& total = scatteringStats[index];
            const auto& value = groupStats[index];
            total.maxStepTruncations += value.maxStepTruncations;
            total.transmittanceStops += value.transmittanceStops;
            total.tracedRays += value.tracedRays;
            total.reserved += value.reserved;
            total.absorbedShortwave += value.absorbedShortwave;
            total.absorbedPar += value.absorbedPar;
            total.transmittanceResidual += value.transmittanceResidual;
            total.truncatedResidual += value.truncatedResidual;
        }
        first = end;
    }
    const double shortwaveMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - shortwaveBegin).count();
    // 步骤 3.4：写散射诊断文件并打印摘要。
    writeScatteringStats(modelio, scatteringStats, shortwaveMs, orderMilliseconds);
    }

    // 步骤 4：直射热红外 pass（当前着色器已禁用：清零 directTrad 后即返回，
    // 天空向下长波由后面的 diffuseTIR 一并计入，见 voxelrad_direct_TIR.comp 文件头）。
    submit(modelio, VoxelEBStage::directTIR, voxelSize1D, std::nullopt, std::nullopt);
    waitFence(modelio);

    // 步骤 5：创建 EBState 的主机读回缓冲，并复位本节点的迭代诊断量。
    nvvk::Buffer stateReadback = modelio->m_pAlloc->createBuffer(
        sizeof(EBState), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);


    setting.energyFinalize = 0;
    modelio->energyIterations = 0;
    modelio->energyFinalUnconvergedStateCount = 0;
    modelio->energyFinalized = false;
    // 生理（生化）过程只在前 25 次迭代更新；之后冻结生理，仅迭代温度。
    // Stomatal state reacts to leaf-surface humidity outside the temperature
    // Newton step; later iterations solve temperature with physiology frozen.
    constexpr int physiologyIterations = 25;
    // 步骤 6：气动阻抗与 MOST 稳定度的初始状态。stability 表示是否启用 Monin-Obukhov 修正；
    // domainColumns = 水平格数（x×z），用于把全场显热换算为单位地面面积 [W m-2]；
    // airTemperatureK = Ta(°C)+273.15；inverseL 为场景 1/L [m-1]，每个节点从 0（中性）开始。
    // Aerodynamic resistances depend on meteorology, canopy structure, the
    // (frozen during EB) flow field and - with control.stabilityCorrection -
    // the scene Obukhov length, never on surface temperature directly: the
    // free-convection term is applied per state in evapo/budget. Neutral
    // runs evaluate them once per node; the MOST iteration re-evaluates
    // them whenever it moves 1/L.
    const bool stability = modelio->stabilityCorrection;
    // 限幅：至少 1 列，避免后面除零。
    const int domainColumns = std::max(1, modelio->setting.voxelSize.x * modelio->setting.voxelSize.z);
    const double airTemperatureK = modelio->meteo.Ta + 273.15;
    double inverseL = 0.0; // each node starts neutral (reproducible per node)
    modelio->stabilityAvailable = false;
    modelio->stabilityAeroUpdates = 0;
    modelio->stabilityChange = 0.0;
    modelio->stabilitySensibleHeat = 0.0;
    // 局部函数：评估一次气动阻抗 raa。MOST 开启时先上传当前 1/L 并清零 EBState，
    // aero 着色器会累加地面列的 u* 与有效高度；读回后取地面列均值作为摩擦速度与 z-d。
    // 没有土壤/水体地面列时无法定义表面层，改回中性模式重算阻抗。
    const auto evaluateAerodynamics = [&]() {
        if (stability) {
            uploadStability(modelio, true, inverseL);
            clearEnergyState(modelio);
        }
        submit(modelio, VoxelEBStage::aero, voxelSize1D, std::nullopt, std::nullopt);
        waitFence(modelio);
        if (!stability) return;
        ++modelio->stabilityAeroUpdates;
        EBState state{};
        readEnergyState(modelio, stateReadback, &state);
        int64_t ustarSum = 0, heightSum = 0;
        for (int bin = 0; bin < kEnergyStateBins; ++bin) {
            ustarSum += state.ustar[bin];
            heightSum += state.height[bin];
        }
        modelio->stabilityAvailable = state.groundCount > 0;
        if (modelio->stabilityAvailable) {
            modelio->frictionVelocity = 1.0e-4 * double(ustarSum) / state.groundCount;
            modelio->stabilityHeight = 0.01 * double(heightSum) / state.groundCount;
        } else {
            // No soil/water column to define a surface layer: run this node
            // exactly as the neutral model.
            uploadStability(modelio, false, 0.0);
            submit(modelio, VoxelEBStage::aero, voxelSize1D, std::nullopt, std::nullopt);
            waitFence(modelio);
        }
    };
    // 步骤 6.1：本节点的首次气动阻抗评估（中性或 1/L=0 起点）。
    evaluateAerodynamics();
    modelio->inverseObukhov = inverseL;
    // 船体水线接触：水温由上一节点继承，首个节点（shipWaterK ≤ 1 K 视为未设置）取气温。
    // Ship waterline contact: the water temperature carries over from the
    // previous node; the first node starts from its air temperature (also the
    // value used when the scene has no water).
    if (modelio->shipWaterline) {
        if (!(modelio->shipWaterK > 1.0)) modelio->shipWaterK = airTemperatureK;
        uploadShipWaterTemperature(modelio, modelio->shipWaterK);
    }
    // 稳定度收敛容差：|Δ(z/L)| ≤ 0.02 + 0.02·|z/L|（经验阈值，来源待核对）。
    // |delta(z/L)| below which 1/L counts as converged.
    const auto stabilityTolerance = [](double zeta) { return 0.02 + 0.02 * std::abs(zeta); };
    // 步骤 7：能量平衡主迭代，最多 50 次。每次：diffuseTIR（长波入射）→ bio（前 25 次）→ evapo
    // （H/LE/G 通量）→ 清零 EBState → budget（牛顿更新温度，并原子统计未收敛状态数）→ 可选的
    // 船体水温更新 / MOST 稳定度更新 → 收敛判断。收敛条件：迭代 ≥ 3 次（kiter ≥ 2）且
    // 未收敛状态数为 0（MOST 开启时还要求 1/L 收敛）。
    // DEFERRED[中] 数值: 能量平衡迭代上限 50 写死且前 25 次才更新生理；未收敛时只记录 energyFinalUnconvergedStateCount，后续仍 updateTp 推进时间节点，若调用方不检查该计数，未收敛（甚至振荡）的温度会被当作正常结果传播到下一时间步；建议：将上限与生理冻结轮数做成配置项，并在未收敛计数>0 时输出警告/在结果元数据中标记。
    for(int kiter = 0;kiter < 50; kiter++) {
        // Also read by budget.comp for its damping schedule; the biochemical
        // pass only runs (and reads it) while kiter < physiologyIterations.
        setting.energyIteration = kiter;

        // 7.1 热红外漫射：用当前温度重算体元入射长波。
        submit(modelio, VoxelEBStage::diffuseTIR, voxelSize1D, std::nullopt, std::nullopt);
        waitFence(modelio);

        //--------------------------------------------------

        // 7.2 叶片生化/气孔（仅前 physiologyIterations 次迭代）。
        if(kiter < physiologyIterations) {
            setting.energyIteration = kiter;
            submit(modelio, VoxelEBStage::bio, voxelSize1D, std::nullopt, std::nullopt);
            waitFence(modelio);
        }


        //--------------------------------------------------


        // 7.3 蒸散与显热/潜热/热储通量。
        submit(modelio, VoxelEBStage::evapo, voxelSize1D, std::nullopt, std::nullopt);
        waitFence(modelio);

        //--------------------------------------------------
        // 7.4 清零 EBState 后派发 budget：更新温度，并累计未收敛个数与稳定度所需的显热等。
        clearEnergyState(modelio);

        submit(modelio, VoxelEBStage::budget, voxelSize1D, std::nullopt, std::nullopt);
        waitFence(modelio);
        modelio->energyIterations = kiter + 1;
        // 7.5 船体水线：budget 把水体体元数与水温偏差（0.01 K 定点数）累计进 EBState，
        // 这里取均值累加到场景水温 shipWaterK，并上传给下一次迭代（语义见 budget.comp 文件头）。
        if (modelio->shipWaterline) {
            // Scene mean water temperature after this pass for the next one.
            EBState waterState{};
            readEnergyState(modelio, stateReadback, &waterState);
            if (waterState.waterCount > 0) {
                modelio->shipWaterVoxels = waterState.waterCount;
                modelio->shipWaterK += 0.01 * double(waterState.waterTemperature) / waterState.waterCount;
                uploadShipWaterTemperature(modelio, modelio->shipWaterK);
            }
        }
        // 7.6 MOST 稳定度更新：由本次 pass 的场景显热（EBState.sensible 以 0.1 W m-2 定点累加，
        // 除以水平格数得单位面积显热）与 u* 求目标 1/L；以 |Δ(z/L)| 判断是否收敛，
        // 未收敛则 1/L 按 0.5 欠松弛逼近目标并重新评估气动阻抗（FacetEB 用同一系数）。
        if (stability && modelio->stabilityAvailable) {
            // Fluxes of this pass (evapo at the pre-update temperatures).
            EBState state{};
            const uint32_t outside = readEnergyState(modelio, stateReadback, &state);
            int64_t sensibleSum = 0;
            for (int bin = 0; bin < kEnergyStateBins; ++bin) sensibleSum += state.sensible[bin];
            const double sensible = 0.1 * double(sensibleSum) / domainColumns;
            const double desired = desiredInverseObukhov(sensible, modelio->frictionVelocity,
                                                         airTemperatureK, modelio->stabilityHeight);
            const double height = modelio->stabilityHeight;
            modelio->stabilitySensibleHeat = sensible;
            modelio->stabilityChange = std::abs(desired - inverseL) * height;
            const bool stabilityConverged =
                modelio->stabilityChange <= stabilityTolerance(desired * height);
            if (kiter >= 2 && outside == 0U && stabilityConverged) break;
            if (!stabilityConverged) {
                // Under-relaxed fixed point (FacetEB uses the same 0.5).
                inverseL += 0.5 * (desired - inverseL);
                modelio->inverseObukhov = inverseL;
                evaluateAerodynamics();
            }
        // 中性（或无地面列）情形：迭代 ≥ 3 次且无未收敛状态即收敛退出。
        } else if(kiter >= 2 && readEnergyState(modelio, stateReadback) == 0U) {
            break;
        }



        //--------------------------------------------------

    }
    // 步骤 8：收尾。budget 最后一次更新了温度，因此用最终温度刷新长波/生理/通量，
    // 再以 energyFinalize=1 派发 budget（只写 G 并检查残差，不再更新温度），
    // 最后一次读回的 EBState.count 记为最终未收敛状态数。
    // budget updated temperature last. Refresh radiation, physiology and
    // exchange fluxes at that final temperature before advancing history.
    // The final budget dispatch only writes G/checks residuals; it cannot
    // make another temperature update or advance the physical time node.
    submit(modelio, VoxelEBStage::diffuseTIR, voxelSize1D, std::nullopt, std::nullopt);
    waitFence(modelio);
    if(modelio->energyIterations < physiologyIterations) {
        setting.energyIteration = modelio->energyIterations;
        submit(modelio, VoxelEBStage::bio, voxelSize1D, std::nullopt, std::nullopt);
        waitFence(modelio);
    }
    submit(modelio, VoxelEBStage::evapo, voxelSize1D, std::nullopt, std::nullopt);
    waitFence(modelio);
    clearEnergyState(modelio);
    setting.energyFinalize = 1;
    submit(modelio, VoxelEBStage::budget, voxelSize1D, std::nullopt, std::nullopt);
    waitFence(modelio);
    modelio->energyFinalUnconvergedStateCount = readEnergyState(modelio, stateReadback);
    modelio->energyFinalized = true;
    setting.energyFinalize = 0;
    modelio->m_pAlloc->destroy(stateReadback);

    // 步骤 9：提交本节点的温度历史（土壤/水体/船体），供下一节点的热储通量使用。
    submit(modelio, VoxelEBStage::updateTp, voxelSize1D, std::nullopt, std::nullopt);
    waitFence(modelio);

    if (voxelflags::passTimingEnabled()) printPassTiming();
    return true;
}


// 模块 D：runRT —— 传感器成像。按图像尺寸二维派发 out 阶段（voxelrad_image.comp），
// 每线程一个像素；依赖 runEB 已得到的温度/日照/漫射场。返回 true。
bool Command::runRT(std::shared_ptr<VoxelebIO> &modelio){

    modelio->m_currentSemaphore = 1;

    glm::ivec3 voxelSize2D = glm::ivec3((modelio->setting.imageSize.x + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY,
                                        (modelio->setting.imageSize.y + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY, 1);
    auto & descSet = modelio->m_descSet;
    auto & setting = modelio->setting;


    //--------------------------------------------------

//    nvmath::vec3i size = nvmath::vec3i((m_setting.size.x + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY,
//                                       (m_setting.size.y + (GROUP_SIZEXY - 1)) / GROUP_SIZEXY, 1);

    submit(modelio, VoxelEBStage::out, voxelSize2D, std::nullopt, std::nullopt);
    waitFence(modelio);



    return true;
}

// 模块 E：runFluid —— 运行 iterations 步 LBM 流体求解（fluidLbm + fluidCommit 交替）。
// 输入：iterations；流体未启用、网格为空或 iterations ≤ 0 时直接返回 true。
// 说明：每 32 步合并为一个命令缓冲提交一次并等待，既避免 Windows GPU 看门狗超时，
//       又避免每步一次提交与 fence 等待；步间用计算→计算的内存屏障保证可见性。
bool Command::runFluid(std::shared_ptr<VoxelebIO> &modelio, int iterations)
{
    if (!modelio->fluid.enabled || modelio->fluidCellCount == 0 || iterations <= 0) return true;
    const glm::ivec3 dispatch(
        static_cast<int>((modelio->fluidCellCount + GROUP_SIZEX - 1) / GROUP_SIZEX), 1, 1);

    // Keep command buffers short enough for Windows' GPU watchdog while
    // avoiding a queue submission and fence wait for every LBM pass.
    // 每批 32 步：原注释说明这样既不超过 Windows GPU 看门狗限制，又省去每步一次提交与等待。
    constexpr int iterationsPerBatch = 32;
    nvvk::CommandPool commandPool(modelio->m_device, modelio->m_queueIndex);
    for (int first = 0; first < iterations; first += iterationsPerBatch) {
        // 步骤：每批录入 batchSize 步 [fluidLbm → 屏障 → fluidCommit → 屏障]，结束后提交并等待。
        const int batchSize = std::min(iterationsPerBatch, iterations - first);
        VkCommandBuffer command = commandPool.createCommandBuffer();
        VkMemoryBarrier memoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

        // DEFERRED[中] GPU同步: 与 submit() 不同，这里每批第一个 fluidLbm 派发前没有任何屏障；VoxelEB 的 recordCommandBuffer 也不含屏障，因此上一次提交（budget/updateTp 或上一批的 fluidCommit）写入的 SSBO 对本批首个派发不保证可见（fence 只保证 host 侧可见）；建议：循环前先插入一次 srcAccess=SHADER_WRITE|TRANSFER_WRITE、src 阶段 ALL_COMMANDS 的内存屏障，与 submit() 一致。
        for (int iteration = 0; iteration < batchSize; ++iteration) {
            recordCommandBuffer(command, modelio->m_descSet, modelio->m_pipelineLayout,
                                modelio->m_pipelines[VoxelEBStage::fluidLbm], modelio->setting);
            vkCmdDispatch(command, dispatch.x, dispatch.y, dispatch.z);
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 1, &memoryBarrier, 0, nullptr, 0, nullptr);

            recordCommandBuffer(command, modelio->m_descSet, modelio->m_pipelineLayout,
                                modelio->m_pipelines[VoxelEBStage::fluidCommit], modelio->setting);
            vkCmdDispatch(command, dispatch.x, dispatch.y, dispatch.z);
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 1, &memoryBarrier, 0, nullptr, 0, nullptr);
        }

        vkEndCommandBuffer(command);
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &command;
        vkCheck(vkQueueSubmit(modelio->m_queue, 1, &submitInfo, modelio->m_fence), "VoxelEB fluid vkQueueSubmit");
        waitFence(modelio);
    }
    return true;
}

//bool Command::run(std::shared_ptr<VoxelebIO> &modelio){
//
//    modelio->m_currentSemaphore = 1;
//
//    glm::ivec3 voxelSize1D = glm::ivec3((modelio->n_voxel + (GROUP_SIZEX - 1)) / GROUP_SIZEX, 1, 1);
//    auto & descSet = modelio->m_descSet;
//    auto & setting = modelio->setting;
//
//
//    auto & piplineLayout_rt = modelio->m_pipelineLayout_rt;
//    auto & piplines_rt = modelio->m_pipelines_rt;
//    auto & piplineLayout_eb = modelio->m_pipelineLayout_eb;
//    auto & piplines_eb = modelio->m_pipelines_eb;
//    auto & piplineLayout_et = modelio->m_pipelineLayout_et;
//    auto & piplines_et = modelio->m_pipelines_et;
//    auto & piplineLayout_bio = modelio->m_pipelineLayout_bio;
//    auto & pipline_bio = modelio->m_pipeline_bio;
//    auto & piplineLayout_aero = modelio->m_pipelineLayout_aero;
//    auto & pipline_aero = modelio->m_pipeline_aero;
//    //--------------------------------------------------
//
//    int stageInt = 0;
//
//    stageInt = (int)VoxelRadStage::gap;
//    submit(modelio, voxelSize1D,descSet,piplineLayout_rt,piplines_rt[stageInt],setting,nullptr, nullptr);
//    waitFence(modelio);
//
//    stageInt = (int)VoxelRadStage::directVNIR;
//    submit(modelio, voxelSize1D,descSet,piplineLayout_rt,piplines_rt[stageInt],setting,nullptr, nullptr);
//    waitFence(modelio);
//
//    stageInt = (int)VoxelRadStage::diffuseVNIR;
//    submit(modelio, voxelSize1D,descSet,piplineLayout_rt,piplines_rt[stageInt],setting,nullptr, nullptr);
//    waitFence(modelio);
//
//
//
//
//    //--------------------------------------------------
//
//
//    submit(modelio, voxelSize1D,descSet,piplineLayout_bio,pipline_bio,setting,nullptr, nullptr);
//    waitFence(modelio);
//
//    //--------------------------------------------------
//
//    submit(modelio, voxelSize1D,descSet,piplineLayout_aero,pipline_aero,setting,nullptr, nullptr);
//    waitFence(modelio);
//
//
//    //--------------------------------------------------
//
//
//    stageInt = (int)ETStage::evapo;
//    submit(modelio, voxelSize1D,descSet,piplineLayout_et,piplines_et[stageInt],setting,nullptr, nullptr);
//    waitFence(modelio);
//
//    //--------------------------------------------------
//
//    stageInt = (int)EBStage::budget;
//    submit(modelio, voxelSize1D,descSet,piplineLayout_eb,piplines_eb[stageInt],setting,nullptr, nullptr);
//    waitFence(modelio);
//
//
//    //--------------------------------------------------
//
//
//
//
//
//    return true;
//}


// 模块 F：submit —— 录制并提交单个阶段：全局内存屏障 → 绑定管线/描述符/push constant →
// vkCmdDispatch → 提交（带 m_fence，调用者随后须 waitFence）。
// 输入：stage（选择 m_pipelines[stage]）、dispatchSize（工作组数）；push constant 取
//       modelio->setting 的当前值（按值拷贝，所以改 setting 后须重新 submit 才生效）。
// 说明：inSemaphores/outSemaphore 参数目前未使用（等待/信号语句已被注释掉）。
//       环境变量 HISTREAM_GPU_DIAGNOSTICS 存在时打印每个阶段号。
void Command::submit(std::shared_ptr<VoxelebIO> &modelio, VoxelEBStage stage, glm::ivec3 dispatchSize,
                     const std::optional<std::vector<VkSemaphore>> &inSemaphores, const std::optional<VkSemaphore> &outSemaphore)
{

    auto & descSet = modelio->m_descSet;
    auto & setting = modelio->setting;
    auto & pipelineLayout = modelio->m_pipelineLayout;
    // 步骤 1：取出本阶段的管线（m_pipelines 为 std::map，operator[] 对未创建的阶段会得到空句柄）。
    auto & pipeline = modelio->m_pipelines[stage];
    if (std::getenv("HISTREAM_GPU_DIAGNOSTICS")) std::cout << "GPU_STAGE\t" << static_cast<int>(stage) << std::endl;
    if (voxelflags::passTimingEnabled()) {
        passTiming().pendingStage = stage == VoxelEBStage::scatteringTrace && setting.scatteringStage >= 1 && setting.scatteringStage <= 4 ? 14 + setting.scatteringStage : static_cast<int>(stage);
        passTiming().start = std::chrono::steady_clock::now();
    }


    auto & m_currentSemaphore = modelio->m_currentSemaphore;
    // Preparing for the compute shader
    VkCommandBuffer cmdBuf = modelio->m_genCmdBuf.createCommandBuffer();
    m_inFlight.push_back(cmdBuf);

//    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
//    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
//    beginInfo.pNext = nullptr;
//    beginInfo.pInheritanceInfo = nullptr;
//    vkBeginCommandBuffer(cmdBuf, &beginInfo);

    // 步骤 2：派发前的全局内存屏障（上一个阶段的着色器/传输写 → 本阶段读写）。
    // Queue/fence completion alone does not make previous SSBO writes visible
    // to this dispatch. This also orders the previous scattering pass globally.
    VkMemoryBarrier memoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
    // Dispatching the shader only for the other;
    recordCommandBuffer(cmdBuf, descSet, pipelineLayout, pipeline, setting );
    // 步骤 3：绑定并派发；push constant 在录制时按值拷贝进命令缓冲。
    vkCmdDispatch(cmdBuf, dispatchSize.x, dispatchSize.y, dispatchSize.z);

    auto & semaphores = modelio->m_semaphores;
    // new semaphores
    std::array<VkPipelineStageFlags, 1> waitStages{VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
    VkSubmitInfo submitInfoCompute{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfoCompute.commandBufferCount = 1;
    submitInfoCompute.pCommandBuffers = &cmdBuf;
//    submitInfoCompute.waitSemaphoreCount =  static_cast<uint32_t>(1);
//    submitInfoCompute.pWaitSemaphores =  inSemaphores.has_value() ? inSemaphores->data() : &semaphores[m_currentSemaphore-1];
//    submitInfoCompute.pWaitDstStageMask = waitStages.data();
//    submitInfoCompute.signalSemaphoreCount = 1;
//    submitInfoCompute.pSignalSemaphores = outSemaphore.has_value() ? &outSemaphore.value() : & semaphores[m_currentSemaphore];


    vkEndCommandBuffer(cmdBuf);
    // 步骤 4：提交到队列并用 m_fence 通知完成（调用者须 waitFence）。
    vkCheck(vkQueueSubmit( modelio->m_queue, 1, &submitInfoCompute,  modelio->m_fence), "VoxelEB vkQueueSubmit");
    m_currentSemaphore++;
}


//void Command::submit(std::shared_ptr<VoxelebIO> &modelio,glm::ivec3 dispatchSize,
//                     VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
//                     VkPipeline pipeline, VoxelLstSetting setting,
//                     const std::optional<VkSemaphore> &inSemaphore, const std::optional<VkSemaphore> &outSemaphore)
//{
//    auto & m_currentSemaphore = modelio->m_currentSemaphore;
//    // Preparing for the compute shader
//    VkCommandBuffer cmdBuf = modelio->m_genCmdBuf.createCommandBuffer();
//
////    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
////    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
////    vkBeginCommandBuffer(cmdBuf, &beginInfo);
//
//    // Dispatching the shader only for the other;
//    recordCommandBuffer(cmdBuf, descSet, pipelineLayout, pipeline, setting );
//    vkCmdDispatch(cmdBuf, dispatchSize.x, dispatchSize.y, dispatchSize.z);
//
//    // new semaphores
//    std::array<VkPipelineStageFlags, 1> waitStages{VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
//    VkSubmitInfo submitInfoCompute{VK_STRUCTURE_TYPE_SUBMIT_INFO};
//    submitInfoCompute.waitSemaphoreCount =  static_cast<uint32_t>(1);
//    submitInfoCompute.pWaitSemaphores =  inSemaphore.has_value() ? &inSemaphore.value() : & modelio->m_semaphores[m_currentSemaphore - 1];
//    submitInfoCompute.pWaitDstStageMask = waitStages.data();
//    submitInfoCompute.commandBufferCount = 1;
//    submitInfoCompute.pCommandBuffers = &cmdBuf;
//    submitInfoCompute.signalSemaphoreCount = 1;
//    submitInfoCompute.pSignalSemaphores = outSemaphore.has_value() ? &outSemaphore.value() : & modelio->m_semaphores[m_currentSemaphore];
//
//
//
//    // old using only fense
////    const VkPipelineStageFlags waitStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
////    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
////    submitInfo.pWaitDstStageMask = &waitStageMask;
////    submitInfo.pCommandBuffers = &cmdBuf;
////    submitInfo.commandBufferCount = 1;
//
//    vkEndCommandBuffer(cmdBuf);
//    vkQueueSubmit( modelio->m_queue, 1, &submitInfoCompute,  modelio->m_fence);
//    m_currentSemaphore++;
//}

//void Command::recordCommandBuffer(VkCommandBuffer cmdBuf, VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
//                     std::map<VoxelEBStage, VkPipeline> pipelines, VoxelEBStage stage, VoxelLstSetting setting)
//{
//    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[stage]);
//    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0,
//                            static_cast<uint32_t>(1), &descSet, 0, nullptr);
//    // Sending the push constant information
//    vkCmdPushConstants(cmdBuf, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VoxelLstSetting), &setting);
//}

// 作用：向命令缓冲录入“绑定计算管线 + 绑定描述符集 + 推送 VoxelLstSetting 常量”三步；
//       push constant 大小为 sizeof(VoxelLstSetting)（128 字节，与 pipeline.cpp 的范围一致）。
void Command::recordCommandBuffer(VkCommandBuffer cmdBuf, VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
                                  VkPipeline pipeline,  VoxelLstSetting setting)
{
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0,
                            static_cast<uint32_t>(1), &descSet, 0, nullptr);
    // Sending the push constant information
    vkCmdPushConstants(cmdBuf, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VoxelLstSetting), &setting);
}



// 作用：等待 fence 完成（无超时），复位 fence 并释放已完成的命令缓冲 m_inFlight。
//       等待失败抛 std::runtime_error（通常是 GPU 设备丢失/超时）。
void Command::waitFence(std::shared_ptr<VoxelebIO> &modelio)
{
    const VkResult result = vkWaitForFences(modelio->m_device, 1, &(modelio->m_fence), VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) throw std::runtime_error("VoxelEB GPU execution failed: " + std::to_string(result));
    if (voxelflags::passTimingEnabled() && passTiming().pendingStage >= 0) {
        PassTiming& timing = passTiming();
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - timing.start).count();
        timing.milliseconds[timing.pendingStage] += elapsed;
        if (timing.calls[timing.pendingStage] == 0 || elapsed < timing.minimum[timing.pendingStage])
            timing.minimum[timing.pendingStage] = elapsed;
        ++timing.calls[timing.pendingStage];
        timing.pendingStage = -1;
    }
    vkResetFences(modelio->m_device, 1,  &(modelio->m_fence));
    modelio->m_genCmdBuf.destroy(m_inFlight);
    m_inFlight.clear();
}


// 作用：销毁 fence 与信号量；若仍有未释放的命令缓冲，先 vkDeviceWaitIdle 再释放。
void Command::destroy(std::shared_ptr<VoxelebIO> &modelio) {

    if (!m_inFlight.empty()) {
        vkDeviceWaitIdle(modelio->m_device);
        modelio->m_genCmdBuf.destroy(m_inFlight);
        m_inFlight.clear();
    }
    vkDestroyFence(modelio->m_device, modelio->m_fence, nullptr);
       // vkFreeCommandBuffers(m_device, m_cmdPool, 1, &m_commandBuffers[i]);

    for (auto& semaphore : modelio->m_semaphores)
    {
        vkDestroySemaphore(modelio->m_device, semaphore, nullptr);
    }

}
