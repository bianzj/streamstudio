//
// Created by admin on 2024/1/25.
//

#include <io.h>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include "pipeline.h"
#include "base/vkcheck.h"


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

    // Get the shader group handles
    uint32_t dataSize = handleCount * handleSize;
    std::vector<uint8_t> handles(dataSize);
    auto result = vkGetRayTracingShaderGroupHandlesKHR(m_device, m_pipeline, 0, handleCount, dataSize, handles.data());
    assert(result == VK_SUCCESS);

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

bool Pipeline::createPipeline(std::shared_ptr<RaytracingIO> &raytracingio)
{


    auto & m_device = raytracingio->m_device;
    auto & m_pipelineLayout = raytracingio->m_pipelineLayout;
    auto & m_pipeline = raytracingio->m_pipeline;
    auto & m_sbtWrapper = raytracingio->m_sbtWrapper;
    auto & m_descSetLayout = raytracingio->m_descSetLayout;
    auto & useDeferred = raytracingio->useDeferred;
    auto & useSBTWrapper = raytracingio->useSBTWrapper;

    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    VkPushConstantRange pushConstantRange{VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                                          0, sizeof(RayRTSetting) };
    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
    pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;
    pipelineLayoutCreateInfo.setLayoutCount = static_cast<uint32_t>(1);
    pipelineLayoutCreateInfo.pSetLayouts = &m_descSetLayout;
    vkCheck(vkCreatePipelineLayout(m_device, &pipelineLayoutCreateInfo, nullptr, &m_pipelineLayout), "Raytracing vkCreatePipelineLayout");

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

    // emiss
    stage.module = nvvk::createShaderModule(m_device, nvh::loadFile(rayTracingRmissShader, true));
    stage.stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[eMiss] = stage;

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

    if (useSBTWrapper)
    {
        m_sbtWrapper.create(m_pipeline, rayPipelineInfo);
    }
    else
    {
        createShaderBindingTable(raytracingio);
    }

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    for (auto& s : stages)
    {
        vkDestroyShaderModule(m_device, s.module, nullptr);
    }


    return true;
}


void Pipeline::destroy(std::shared_ptr<RaytracingIO> &raytracingio) {

   // raytracingio->m_sbtWrapper.destroy();
  //  raytracingio->m_pAlloc->destroy((raytracingio->m_rtSBTBuffer));


    vkDestroyPipeline(raytracingio->m_device, raytracingio->m_pipeline, nullptr);
    vkDestroyPipelineLayout(raytracingio->m_device, raytracingio->m_pipelineLayout, nullptr);

    raytracingio->m_pipelineLayout = VkPipelineLayout();
    raytracingio->m_pipeline = VkPipeline();

}
