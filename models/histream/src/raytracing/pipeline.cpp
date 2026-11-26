// ==========================================================================
// 文件：pipeline.cpp
// 功能：创建 RayTracing 模式使用的 Vulkan 光线追踪管线（VK_KHR_ray_tracing_pipeline）
//       及其着色器绑定表（SBT）。
// 主要模块：
//   1. createShaderBindingTable  手工计算 SBT 各区域的步长/大小并写入着色器组句柄
//   2. createPipeline            管线布局（含推送常量 RayRTSetting）、4 个着色器阶段、
//                                4 个着色器组、管线创建（可选 deferred operation）、SBT 创建
//   3. destroy                   销毁管线和管线布局
// 着色器（由 RaytracingIO::definedDir + /shader/raytracing/ 加载，SPIR-V）：
//   offline.rgen（raygen）、rmiss.rmiss（miss）、shadow.rmiss（阴影 miss）、
//   closestHit.rchit（三角形最近命中）
// 数据流：RaytracingIO 提供 m_device、m_descSetLayout、m_pAlloc、m_rtProperties 等；
//         本文件写回 m_pipeline、m_pipelineLayout、m_rtSBTBuffer 及 m_*Region。
// 关系：raytracing.cpp 调用 createPipeline；着色器组枚举（eRaygen/eMiss/eMiss2/eClosestHit/
//       eShaderGroupCount）定义在 base/structs.h。
// 约定：SBT 布局依次为 raygen(1) | miss(missCount=2) | hit(hitCount=1)，无 callable 区域。
// ==========================================================================

//
// Created by admin on 2024/1/25.
//

#include <io.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include "pipeline.h"
#include "base/vkcheck.h"


// 模块：手工构建着色器绑定表（SBT），仅在 useSBTWrapper=false 时被 createPipeline 调用。
// 输入：raytracingio（读 m_pipeline、missCount、hitCount、m_rtProperties）；
// 输出：写 m_rtSBTBuffer 以及 m_rgenRegion / m_missRegion / m_hitRegion 的 stride、size、地址。
// 说明：句柄大小与对齐取自设备属性；raygen 区 stride 必须按 shaderGroupBaseAlignment 对齐，
//       且 size 必须等于 stride（Vulkan 规范要求）；缓冲为主机可见，直接映射写入。
void Pipeline::createShaderBindingTable(std::shared_ptr<RaytracingIO> &raytracingio)
{
    auto & m_device = raytracingio->m_device;
    auto & m_pipelineLayout = raytracingio->m_pipelineLayout;
    auto & m_pipeline = raytracingio->m_pipeline;
    auto & m_sbtWrapper = raytracingio->m_sbtWrapper;
    auto & m_descSetLayout = raytracingio->m_descSetLayout;
    auto & useDeferred = raytracingio->useDeferred;
    auto & useSBTWrapper = raytracingio->useSBTWrapper;
    auto & m_pAlloc = raytracingio->m_pAlloc;
    auto & m_rtSBTBuffer = raytracingio->m_rtSBTBuffer;
    auto & m_debug = raytracingio->m_debug;

    auto & missCount = raytracingio->missCount;
    auto & hitCount = raytracingio->hitCount;
    auto & m_rgenRegion = raytracingio->m_rgenRegion;
    auto & m_missRegion = raytracingio->m_missRegion;
    auto & m_hitRegion = raytracingio->m_hitRegion;
    auto & m_callRegion = raytracingio->m_callRegion;
    auto & m_rtProperties = raytracingio->m_rtProperties;

    // 步骤 1：计算各区域的 stride/size（句柄数 = raygen 1 个 + miss + hit）。
    auto handleCount = 1 + missCount + hitCount;
    uint32_t handleSize = m_rtProperties.shaderGroupHandleSize;
    // the SBT need to have starting groups to be aligned and handles in the group to be aligned.
    uint32_t handleSizeAligned = nvh::align_up(handleSize, m_rtProperties.shaderGroupHandleAlignment);

    m_rgenRegion.stride = nvh::align_up(handleSizeAligned, m_rtProperties.shaderGroupBaseAlignment);
    m_rgenRegion.size = m_rgenRegion.stride; // the size member of pRayGenShaderBindingTable must be equal to its stride member.
    m_missRegion.stride = handleSizeAligned;
    m_missRegion.size = nvh::align_up(missCount * handleSizeAligned, m_rtProperties.shaderGroupBaseAlignment);
    m_hitRegion.stride = handleSizeAligned;
    m_hitRegion.size = nvh::align_up(hitCount * handleSizeAligned, m_rtProperties.shaderGroupBaseAlignment);

    // 步骤 2：从管线取出全部着色器组句柄（顺序与 createPipeline 里 groups 的顺序一致）。
    // Get the shader group handles
    uint32_t dataSize = handleCount * handleSize;
    std::vector<uint8_t> handles(dataSize);
    auto result = vkGetRayTracingShaderGroupHandlesKHR(m_device, m_pipeline, 0, handleCount, dataSize, handles.data());
    assert(result == VK_SUCCESS);

    // 步骤 3：分配 SBT 缓冲并取设备地址，各区域按 raygen → miss → hit 顺序首尾相接。
    // Allocate a buffer for storing the SBT.
    VkDeviceSize sbtSize = m_rgenRegion.size + m_missRegion.size + m_hitRegion.size + m_callRegion.size;
    m_rtSBTBuffer = m_pAlloc->createBuffer(sbtSize,
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    m_debug.setObjectName(m_rtSBTBuffer.buffer, std::string("SBT")); // give it a debug name for NSight.

    // Find the SBT addresses of each group
    VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, m_rtSBTBuffer.buffer };
    VkDeviceAddress sbtAddress = vkGetBufferDeviceAddress(m_device, &info);
    m_rgenRegion.deviceAddress = sbtAddress;
    m_missRegion.deviceAddress = sbtAddress + m_rgenRegion.size;
    m_hitRegion.deviceAddress = sbtAddress + m_rgenRegion.size + m_missRegion.size;

    // Helper to retrieve the handle data
    auto getHandle = [&](int i)
    { return handles.data() + i * handleSize; };

    // 步骤 4：映射缓冲，把句柄按各区域 stride 依次写入后解除映射。
    // Map the SBT buffer and write in the handles.
    auto* pSBTBuffer = reinterpret_cast<uint8_t*>(m_pAlloc->map(m_rtSBTBuffer));
    uint8_t* pData{ nullptr };
    uint32_t handleIdx{ 0 };
    // Raygen
    pData = pSBTBuffer;
    memcpy(pData, getHandle(handleIdx++), handleSize);
    // Miss
    pData = pSBTBuffer + m_rgenRegion.size;
    for (uint32_t c = 0; c < missCount; c++)
    {
        memcpy(pData, getHandle(handleIdx++), handleSize);
        pData += m_missRegion.stride;
    }
    // Hit
    pData = pSBTBuffer + m_rgenRegion.size + m_missRegion.size;
    for (uint32_t c = 0; c < hitCount; c++)
    {
        memcpy(pData, getHandle(handleIdx++), handleSize);
        pData += m_hitRegion.stride;
    }

    m_pAlloc->unmap(m_rtSBTBuffer);
    m_pAlloc->finalizeAndReleaseStaging();
}

// 模块：创建光线追踪管线（含管线布局、着色器阶段/组、SBT）。
// 输入：raytracingio（读 m_device、m_descSetLayout、definedDir、useDeferred、useSBTWrapper）；
// 输出：写 m_pipelineLayout、m_pipeline 以及 SBT；返回 true（失败时由 vkCheck 或异常抛出）。
// 副作用：先销毁已有的管线布局与管线再重建，因此可在重新加载场景时重复调用；
//         着色器文件缺失时抛出 std::runtime_error。
// 步骤：1 管线布局 → 2 着色器阶段 → 3 着色器组 → 4 创建管线 → 5 创建 SBT → 6 释放着色器模块。
bool Pipeline::createPipeline(std::shared_ptr<RaytracingIO> &raytracingio)
{


    auto & m_device = raytracingio->m_device;
    auto & m_pipelineLayout = raytracingio->m_pipelineLayout;
    auto & m_pipeline = raytracingio->m_pipeline;
    auto & m_sbtWrapper = raytracingio->m_sbtWrapper;
    auto & m_descSetLayout = raytracingio->m_descSetLayout;
    auto & useDeferred = raytracingio->useDeferred;
    auto & useSBTWrapper = raytracingio->useSBTWrapper;

    // 步骤 1：重建管线布局：一个描述符集 + 一段推送常量（RayRTSetting），
    //         三个着色器阶段（raygen / closest hit / miss）都能读取该推送常量。
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    VkPushConstantRange pushConstantRange{VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                                          0, sizeof(RayRTSetting) };
    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
    pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;
    pipelineLayoutCreateInfo.setLayoutCount = static_cast<uint32_t>(1);
    pipelineLayoutCreateInfo.pSetLayouts = &m_descSetLayout;
    vkCheck(vkCreatePipelineLayout(m_device, &pipelineLayoutCreateInfo, nullptr, &m_pipelineLayout), "Raytracing vkCreatePipelineLayout");

    // 步骤 2：加载 SPIR-V 并建立 4 个着色器阶段（先检查文件是否存在，缺失即报错）。
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// <summary>
    /// stages in pipeline
    /// </summary>
    vkDestroyPipeline(m_device, m_pipeline, nullptr);
    std::array<VkPipelineShaderStageCreateInfo, eShaderGroupCount> stages{};
    VkPipelineShaderStageCreateInfo stage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stage.pName = "main";

    std::string baseDirectory = raytracingio->definedDir;
    std::string offlineRaytracingRgenShader = baseDirectory + "/shader/raytracing/offline.rgen.spv";
    std::string rayTracingRchitShader = baseDirectory + "/shader/raytracing/closestHit.rchit.spv";
    std::string rayTracingRmissShader = baseDirectory + "/shader/raytracing/rmiss.rmiss.spv";
    std::string rayTracingShadowRmissShader = baseDirectory + "/shader/raytracing/shadow.rmiss.spv";

    for (const std::string& shader : {offlineRaytracingRgenShader, rayTracingRchitShader, rayTracingRmissShader, rayTracingShadowRmissShader})
    {
        if (access(shader.c_str(), 0) == -1)
            throw std::runtime_error("Raytracing shader file not found: " + shader);
    }

    // raygen
    vk::ShaderModule rgen = nvvk::createShaderModule(m_device, nvh::loadFile(offlineRaytracingRgenShader, true));
    stage.module = rgen;
    stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[eRaygen] = stage;
    // Specialization constants of offline.rgen: 8 = sampler (sampler_qmc.glsl), 9 = RT_MAX_WAVE,
    // the capacity of the per-thread Radiance/RadianceSum/Throughput arrays. The smallest of
    // {16,32,64,128,800} >= n_wave keeps the arrays small enough to live in registers or a
    // small local-memory block (800 floats x 3 = 9.6 KB per thread otherwise).
    // Sampler 0 keeps the historical PCG code path (folded by the driver at pipeline creation).
    struct RaygenSpecialization { int32_t samplerSobol; int32_t maxWave; int32_t adaptive; };
    RaygenSpecialization raygenValues{ raytracingio->rtSampler != 0 ? 1 : 0, 800, raytracingio->rtAdaptive ? 1 : 0 };
    for (const int32_t capacity : { 16, 32, 64, 128, 800 })
    {
        if (raytracingio->n_wave <= capacity) { raygenValues.maxWave = capacity; break; }
    }
    // Experiment override: HISTREAM_RT_MAX_WAVE=<n> forces the capacity (never below n_wave).
    if (const char* forced = std::getenv("HISTREAM_RT_MAX_WAVE"); forced && *forced)
        raygenValues.maxWave = std::clamp<int32_t>(static_cast<int32_t>(std::strtol(forced, nullptr, 10)),
                                                   raytracingio->n_wave, 800);
    const std::array<VkSpecializationMapEntry, 3> raygenEntries{ {
        { 8, static_cast<uint32_t>(offsetof(RaygenSpecialization, samplerSobol)), sizeof(int32_t) },
        { 9, static_cast<uint32_t>(offsetof(RaygenSpecialization, maxWave)), sizeof(int32_t) },
        { 10, static_cast<uint32_t>(offsetof(RaygenSpecialization, adaptive)), sizeof(int32_t) } } };
    const VkSpecializationInfo samplerSpecialization{ static_cast<uint32_t>(raygenEntries.size()), raygenEntries.data(),
                                                      sizeof(raygenValues), &raygenValues };
    stages[eRaygen].pSpecializationInfo = &samplerSpecialization;

    // emiss
    stage.module = nvvk::createShaderModule(m_device, nvh::loadFile(rayTracingRmissShader, true));
    stage.stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[eMiss] = stage;

    // 说明：stages 下标须与下面 groups 里的 generalShader / closestHitShader 取值一致
    //       （eRaygen、eMiss、eMiss2、eClosestHit）。
    // emiss2;
    /// the second miss shader is invoked when a shadow ray misses the geometry.
    /// it simply indicates that no occlusion has been found
    stage.module = nvvk::createShaderModule(m_device, nvh::loadFile(rayTracingShadowRmissShader, true));
    stage.stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[eMiss2] = stage;

    // Hit group - Closest hit
    stage.module = nvvk::createShaderModule(m_device, nvh::loadFile(rayTracingRchitShader, true));
    stage.stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[eClosestHit] = stage;

    // 步骤 3：建立着色器组：raygen、miss、shadow miss 为 GENERAL 组，
    //         最近命中为 TRIANGLES_HIT_GROUP 组（没有 any-hit 与 intersection）。
    //         组顺序决定 SBT 里句柄顺序，须与 missCount=2、hitCount=1 对应。
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// <summary>
    /// groups
    /// </summary>
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
    VkRayTracingShaderGroupCreateInfoKHR group{ VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
    group.anyHitShader = VK_SHADER_UNUSED_KHR;
    group.closestHitShader = VK_SHADER_UNUSED_KHR;
    group.generalShader = VK_SHADER_UNUSED_KHR;
    group.intersectionShader = VK_SHADER_UNUSED_KHR;
    // raygen
    group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    group.generalShader = eRaygen;
    groups.push_back(group);
    // Miss
    group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    group.generalShader = eMiss;
    groups.push_back(group);
    // shadow miss
    group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    group.generalShader = eMiss2;
    groups.push_back(group);
    // closest hit shader
    group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    group.generalShader = VK_SHADER_UNUSED_KHR;
    group.closestHitShader = eClosestHit;
    groups.push_back(group);

    // 步骤 4：创建光线追踪管线；useDeferred 为真时用 deferred operation 并行编译
    //         （最多 8 个线程加入），默认关闭（useDeferred=false）。
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// <summary>
    /// pipeline
    /// </summary>
    VkRayTracingPipelineCreateInfoKHR rayPipelineInfo{ VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
    rayPipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    rayPipelineInfo.pStages = stages.data();
    rayPipelineInfo.groupCount = static_cast<uint32_t>(groups.size());
    rayPipelineInfo.pGroups = groups.data();
    rayPipelineInfo.maxPipelineRayRecursionDepth = 2; /// ray depth, no use
    rayPipelineInfo.layout = m_pipelineLayout;

    // deferred operations: spreading work fot a single command across multiple CPU cores.
    VkResult result;
    VkDeferredOperationKHR deferredOp{ VK_NULL_HANDLE };
    if (useDeferred)
    {
        vkCheck(vkCreateDeferredOperationKHR(m_device, nullptr, &deferredOp), "vkCreateDeferredOperationKHR");
    }

    result = vkCreateRayTracingPipelinesKHR(m_device, deferredOp, {}, 1, &rayPipelineInfo, nullptr, &m_pipeline);
    if (!useDeferred || (result != VK_OPERATION_DEFERRED_KHR && result != VK_OPERATION_NOT_DEFERRED_KHR))
        vkCheck(result, "vkCreateRayTracingPipelinesKHR");

    if (useDeferred)
    {
        // Query the maximum amount of concurrency and clamp to the desired maximum
        uint32_t maxThreads{ 8 };
        uint32_t numLaunches = std::min(vkGetDeferredOperationMaxConcurrencyKHR(m_device, deferredOp), maxThreads);

        std::vector<std::future<void>> joins;
        for (uint32_t i = 0; i < numLaunches; i++)
        {
            VkDevice device{ m_device };
            joins.emplace_back(std::async(std::launch::async, [device, deferredOp]()
            { vkDeferredOperationJoinKHR(device, deferredOp); }));
        }

        for (auto& f : joins)
        {
            f.get();
        }

        vkCheck(vkGetDeferredOperationResultKHR(m_device, deferredOp), "vkCreateRayTracingPipelinesKHR (deferred)");
        vkDestroyDeferredOperationKHR(m_device, deferredOp, nullptr);
    }

    // 步骤 5：创建 SBT：默认使用 nvvk::SBTWrapper，否则用上面手工实现的版本。
    if (useSBTWrapper)
    {
        m_sbtWrapper.create(m_pipeline, rayPipelineInfo);
    }
    else
    {
        createShaderBindingTable(raytracingio);
    }

    // 步骤 6：管线创建完成后着色器模块不再需要，统一销毁。
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    for (auto& s : stages)
    {
        vkDestroyShaderModule(m_device, s.module, nullptr);
    }


    return true;
}


// 模块：销毁管线与管线布局，并把句柄置为空。
// 说明：SBT 缓冲/SBTWrapper 的释放被注释掉了，未在此处销毁（待核对：是否在别处释放）。
void Pipeline::destroy(std::shared_ptr<RaytracingIO> &raytracingio) {

   // raytracingio->m_sbtWrapper.destroy();
  //  raytracingio->m_pAlloc->destroy((raytracingio->m_rtSBTBuffer));


    vkDestroyPipeline(raytracingio->m_device, raytracingio->m_pipeline, nullptr);
    vkDestroyPipelineLayout(raytracingio->m_device, raytracingio->m_pipelineLayout, nullptr);

    raytracingio->m_pipelineLayout = VkPipelineLayout();
    raytracingio->m_pipeline = VkPipeline();

}
