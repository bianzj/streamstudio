// ==========================================================================
// 文件：pipeline.cpp
// 功能：VoxelRT（体元辐射）模式的 Vulkan 计算管线创建与销毁。
// 主要模块：
//   1. Pipeline::createPipeline：建立管线布局（描述符集 + 推送常量），按 VoxelRTStage 读取
//      SPIR-V 着色器并创建 3 条计算管线（gap / diffuse / out）
//   2. Pipeline::destroy：销毁上述管线与管线布局
//   3. Pipeline::createShaderBindingTable：空实现（计算管线不需要 SBT）
//   4. 文件中间大段被注释掉的 createRTPipeline/ET/EB/Aero/Bio：VoxelEB 等模式遗留的旧实现，
//      当前未使用，仅保留作参考
// 数据流：VoxelrtIO（voxelrtio.h）提供 m_device、m_descSetLayout、definedDir、
//   periodicNeighborCount → 本文件写回 m_pipelineLayout 与 m_pipelines[stage]；
//   管线由 command.cpp 在 submit() 中绑定并 dispatch。
// 着色器：{definedDir}/shader/voxelrt/ 下的 voxelrad_solar / voxelrad_diffuse_single /
//   voxelrad_image 三个 .comp.spv（源码为同目录 .comp，由构建流程编译，待核对）。
// 约定：三条管线共用同一个管线布局；周期边界通过特化常量（constant_id 0/1）在管线创建时固化。
// ==========================================================================

//
// Created by admin on 2024/1/26.
//
#include <io.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include "pipeline.h"




// 模块：着色器绑定表（SBT）占位。VoxelRT 只使用计算管线，不需要 SBT，故为空实现；
// 仅为与其他模式的 Pipeline 接口保持一致（待核对）。
void Pipeline::createShaderBindingTable(std::shared_ptr<VoxelrtIO> &modelio)
{

}

// 作用：创建 VoxelRT 的管线布局与全部计算管线（VoxelRTStage：gap、diffuse、out）。
// 输入：modelio（设备、描述符集布局、着色器根目录 definedDir、periodicNeighborCount）。
// 输出：写入 modelio->m_pipelineLayout 与 modelio->m_pipelines；成功返回 true，
//       任一管线创建失败返回 false（已创建的管线由 destroy 统一回收）。
// 说明：下面的引用别名直接绑定 modelio 的成员，其中部分（SBT、m_pAlloc 等）当前未使用。
bool Pipeline::createPipeline(std::shared_ptr<VoxelrtIO> &modelio)
{
    auto & m_device = modelio->m_device;
    auto & m_pipelineLayout = modelio->m_pipelineLayout;
    auto & m_pipelines = modelio->m_pipelines;
    auto & m_sbtWrapper = modelio->m_sbtWrapper;
    auto & m_descSetLayout = modelio->m_descSetLayout;
    auto & useDeferred = modelio->useDeferred;
    auto & useSBTWrapper = modelio->useSBTWrapper;
    auto & m_pAlloc = modelio->m_pAlloc;
    auto & m_rtSBTBuffer = modelio->m_rtSBTBuffer;
    auto & m_debug = modelio->m_debug;

    // 步骤 1：先销毁旧管线布局（重复创建时避免泄漏；首次调用时句柄为 VK_NULL_HANDLE，合法）。
    //         再建立新布局：1 个描述符集 + 1 段计算阶段推送常量。
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);

    // 推送常量范围按 sizeof(VoxelLstSetting)（128 字节）声明，而 command.cpp 实际推送的是
    // VoxelRTSetting（约 96 字节，待核对）；范围不小于实际推送长度，因此可用，但类型不一致。
    VkPushConstantRange pushConstant{VK_SHADER_STAGE_COMPUTE_BIT,
                                     0, sizeof(VoxelLstSetting)};
    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
    pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstant;
    pipelineLayoutCreateInfo.setLayoutCount = static_cast<uint32_t>(1);
    pipelineLayoutCreateInfo.pSetLayouts = &m_descSetLayout;
    vkCreatePipelineLayout(m_device, &pipelineLayoutCreateInfo, nullptr, &m_pipelineLayout);

    // 步骤 2：按阶段登记 SPIR-V 路径：gap = 太阳直射/可见性预处理，diffuse = 单次散射
    //         漫射传输，out = 输出到图像（阶段含义依据 shader 文件名推断，待核对）。
    std::string baseDirectory = modelio->definedDir;
    std::map<VoxelRTStage, std::string> VoxelrtPaths{
            {VoxelRTStage::gap,         baseDirectory + "/shader/voxelrt/voxelrad_solar.comp.spv"},
            {VoxelRTStage::diffuse,  baseDirectory + "/shader/voxelrt/voxelrad_diffuse_single.comp.spv"},
            {VoxelRTStage::out,         baseDirectory + "/shader/voxelrt/voxelrad_image.comp.spv"}};


    // 只检查 gap 一个文件是否存在，仅记录日志、不中断；缺失时下面 loadFile 会失败。
    if (_access(VoxelrtPaths[VoxelRTStage::gap].c_str(), 0) == -1)
    {
        std::string error = "Error: no shader file (.spv) found in " + baseDirectory + "\n";
        LOGI(error.c_str());
    }

    // 步骤 3：准备周期边界的特化常量。periodicNeighborCount 限幅到 [0, 20]，
    //         0 表示不启用周期边界。
    // Specialization constants (periodic_walk.glsl): 0 = PERIODIC_BOUNDARY_ENABLED,
    // 1 = PERIODIC_CROSSING_LIMIT. Without periodic boundaries the driver
    // drops every periodic branch; with them the crossing limit is a
    // compile-time constant, so the periodic and non-periodic code paths of
    // a call site are no longer both kept.
    struct PeriodicSpecData { VkBool32 enabled; int32_t limit; };
    const PeriodicSpecData periodicData{
        modelio->periodicNeighborCount > 0 ? VK_TRUE : VK_FALSE,
        std::clamp<int32_t>(modelio->periodicNeighborCount, 0, 20)};
    const VkSpecializationMapEntry periodicEntries[2] = {
        {0, offsetof(PeriodicSpecData, enabled), sizeof(VkBool32)},
        {1, offsetof(PeriodicSpecData, limit), sizeof(int32_t)}};
    const VkSpecializationInfo periodicSpecialization{2, periodicEntries, sizeof(PeriodicSpecData), &periodicData};

    // 步骤 4：逐阶段加载 SPIR-V、绑定特化常量并创建计算管线。
    //         着色器模块创建管线后即可销毁（成功与失败路径都销毁）。
    for (const auto &stage : magic_enum::enum_values<VoxelRTStage>())
    {
        //int stageInt = (int)stage;
        VkComputePipelineCreateInfo computePipelineCreateInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        computePipelineCreateInfo.layout = m_pipelineLayout;
        computePipelineCreateInfo.stage =
                nvvk::createShaderStageInfo(m_device, nvh::loadFile(VoxelrtPaths[stage], true), VK_SHADER_STAGE_COMPUTE_BIT);
        computePipelineCreateInfo.stage.pSpecializationInfo = &periodicSpecialization;

        const VkResult result = vkCreateComputePipelines(
            m_device, {}, 1, &computePipelineCreateInfo, nullptr, &m_pipelines[stage]);
        if (result != VK_SUCCESS) {
            LOGE("Failed to create VoxelRT compute pipeline (%d) for %s\n",
                 static_cast<int>(result), VoxelrtPaths[stage].c_str());
            vkDestroyShaderModule(m_device, computePipelineCreateInfo.stage.module, nullptr);
            return false;
        }

        m_debug.setObjectName(m_pipelines[stage], "VoxelRT");
        vkDestroyShaderModule(m_device, computePipelineCreateInfo.stage.module, nullptr);
    }

    return true;
}

//
// 以下直到 destroy 之前，是被整段注释掉的旧管线创建函数（RT/ET/EB/Aero/Bio），
// 沿用自 VoxelEB 等模式，VoxelRT 不使用；保留仅供参考，不要在此处恢复（待核对）。
//bool Pipeline::createRTPipeline(std::shared_ptr<VoxelrtIO> &modelio)
//{
//
//
//    auto & m_device = modelio->m_device;
//    auto & m_pipelineLayout = modelio->m_pipelineLayout_rt;
//    auto & m_pipelines = modelio->m_pipelines_rt;
//    auto & m_sbtWrapper = modelio->m_sbtWrapper;
//    auto & m_descSetLayout = modelio->m_descSetLayout;
//    auto & useDeferred = modelio->useDeferred;
//    auto & useSBTWrapper = modelio->useSBTWrapper;
//    auto & m_pAlloc = modelio->m_pAlloc;
//    auto & m_rtSBTBuffer = modelio->m_rtSBTBuffer;
//    auto & m_debug = modelio->m_debug;
//
//    std::string baseDirectory = modelio->definedDir;
//    std::map<VoxelRadStage, std::string> VoxeltracingPaths{
//            {VoxelRadStage::gap, baseDirectory + "/shader/Voxeleb/voxelrad_solar.comp.spv"},
//            {VoxelRadStage::directVNIR, baseDirectory + "/shader/Voxeleb/voxelrad_direct_VNIR.comp.spv"},
//            {VoxelRadStage::directTIR, baseDirectory + "/shader/Voxeleb/voxelrad_direct_TIR.comp.spv"},
//            {VoxelRadStage::diffuseVNIR, baseDirectory + "/shader/Voxeleb/voxelrad_diffuse_VNIR_single.comp.spv"},
//            {VoxelRadStage::diffuseTIR, baseDirectory + "/shader/Voxeleb/voxelrad_diffuse_TIR_single.comp.spv"},
//            };
//
//
//    if (_access(VoxeltracingPaths[VoxelRadStage::gap].c_str(), 0) == -1)
//    {
//        std::string error = "Error: no shader file (.spv) found in " + baseDirectory + "\n";
//        LOGI(error.c_str());
//    }
//
//    for (const auto &stage : magic_enum::enum_values<VoxelRadStage>())
//    {
//        int stageInt = (int)stage;
//        VkComputePipelineCreateInfo computePipelineCreateInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
//        computePipelineCreateInfo.layout = m_pipelineLayout;
//        computePipelineCreateInfo.stage =
//                nvvk::createShaderStageInfo(m_device, nvh::loadFile(VoxeltracingPaths[stage], true), VK_SHADER_STAGE_COMPUTE_BIT);
//
//        vkCreateComputePipelines(m_device, {}, 1, &computePipelineCreateInfo, nullptr, &m_pipelines[stageInt]);
//
//        m_debug.setObjectName(m_pipelines[stageInt], "VoxelLST");
//        vkDestroyShaderModule(m_device, computePipelineCreateInfo.stage.module, nullptr);
//    }
//
//    return true;
//}
//
//
//bool Pipeline::createETPipeline(std::shared_ptr<VoxelrtIO> &modelio)
//{
//
//
//    auto & m_device = modelio->m_device;
//    auto & m_pipelineLayout = modelio->m_pipelineLayout_et;
//    auto & m_pipelines = modelio->m_pipelines_et;
//    auto & m_sbtWrapper = modelio->m_sbtWrapper;
//    auto & m_descSetLayout = modelio->m_descSetLayout;
//    auto & useDeferred = modelio->useDeferred;
//    auto & useSBTWrapper = modelio->useSBTWrapper;
//    auto & m_debug = modelio->m_debug;
//
//    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
//    VkPushConstantRange pushConstant{VK_SHADER_STAGE_COMPUTE_BIT,
//                                     0, sizeof(ETSetting)};
//
//    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
//    pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
//    pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstant;
//    pipelineLayoutCreateInfo.setLayoutCount = static_cast<uint32_t>(1);
//    pipelineLayoutCreateInfo.pSetLayouts = &m_descSetLayout;
//    vkCreatePipelineLayout(m_device, &pipelineLayoutCreateInfo, nullptr, &m_pipelineLayout);
//
//    std::string baseDirectory = modelio->definedDir;
//    std::map<ETStage, std::string> ETShaderPaths{
//            {ETStage::evapo, baseDirectory + "/shader/ET/evapo.comp.spv"},
//            {ETStage::updateTp, baseDirectory + "/shader/ET/updateTp.comp.spv"}};
//
//    for (const auto &stage : magic_enum::enum_values<ETStage>())
//    {
//        int stageInt = (int)stage;
//        VkComputePipelineCreateInfo computePipelineCreateInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
//        computePipelineCreateInfo.layout = m_pipelineLayout;
//        computePipelineCreateInfo.stage =
//                nvvk::createShaderStageInfo(m_device, nvh::loadFile(ETShaderPaths[stage], true), VK_SHADER_STAGE_COMPUTE_BIT);
//
//        vkCreateComputePipelines(m_device, {}, 1, &computePipelineCreateInfo, nullptr, &m_pipelines[stageInt]);
//        m_debug.setObjectName(m_pipelines[stageInt], "Evapo");
//        vkDestroyShaderModule(m_device, computePipelineCreateInfo.stage.module, nullptr);
//    }
//
//    return true;
//}
//
//
//bool Pipeline::createEBPipeline(std::shared_ptr<VoxelrtIO> &modelio)
//{
//
//
//    auto & m_device = modelio->m_device;
//    auto & m_pipelineLayout = modelio->m_pipelineLayout_et;
//    auto & m_pipelines = modelio->m_pipelines_eb;
//    auto & m_sbtWrapper = modelio->m_sbtWrapper;
//    auto & m_descSetLayout = modelio->m_descSetLayout;
//    auto & useDeferred = modelio->useDeferred;
//    auto & useSBTWrapper = modelio->useSBTWrapper;
//    auto & m_debug = modelio->m_debug;
//
//    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
//
//    VkPushConstantRange pushConstant{VK_SHADER_STAGE_COMPUTE_BIT,
//                                     0, sizeof(EBSetting)};
//    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
//    pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
//    pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstant;
//    pipelineLayoutCreateInfo.setLayoutCount = static_cast<uint32_t>(1);
//    pipelineLayoutCreateInfo.pSetLayouts = &m_descSetLayout;
//    vkCreatePipelineLayout(m_device, &pipelineLayoutCreateInfo, nullptr, &m_pipelineLayout);
//
//    std::string baseDirectory = modelio->definedDir;
//    std::map<EBStage, std::string> EBShaderPaths{
//            {EBStage::budget, baseDirectory + "/shader/EB/budget.comp.spv"},
//            {EBStage::updateL, baseDirectory + "/shader/EB/updateL.comp.spv"}};
//
//    for (const auto &stage : magic_enum::enum_values<EBStage>())
//    {
//        int stageInt = (int)stage;
//        VkComputePipelineCreateInfo computePipelineCreateInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
//        computePipelineCreateInfo.layout = m_pipelineLayout;
//        computePipelineCreateInfo.stage =
//                nvvk::createShaderStageInfo(m_device, nvh::loadFile(EBShaderPaths[stage], true), VK_SHADER_STAGE_COMPUTE_BIT);
//
//        vkCreateComputePipelines(m_device, {}, 1, &computePipelineCreateInfo, nullptr, &m_pipelines[stageInt]);
//        m_debug.setObjectName(m_pipelines[stageInt], "Budget");
//        vkDestroyShaderModule(m_device, computePipelineCreateInfo.stage.module, nullptr);
//    }
//
//    return true;
//}
//
//
//bool Pipeline::createAeroPipeline(std::shared_ptr<VoxelrtIO> &modelio)
//{
//
//    auto & m_device = modelio->m_device;
//    auto & m_pipelineLayout = modelio->m_pipelineLayout_et;
//    auto & m_pipeline = modelio->m_pipeline_aero;
//    auto & m_sbtWrapper = modelio->m_sbtWrapper;
//    auto & m_descSetLayout = modelio->m_descSetLayout;
//    auto & useDeferred = modelio->useDeferred;
//    auto & useSBTWrapper = modelio->useSBTWrapper;
//    auto & m_debug = modelio->m_debug;
//
//    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
//    VkPushConstantRange pushConstant{VK_SHADER_STAGE_COMPUTE_BIT,
//                                     0, sizeof(AeroSetting)};
//
//    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
//    pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
//    pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstant;
//    pipelineLayoutCreateInfo.setLayoutCount = static_cast<uint32_t>(1);
//    pipelineLayoutCreateInfo.pSetLayouts = &m_descSetLayout;
//    vkCreatePipelineLayout(m_device, &pipelineLayoutCreateInfo, nullptr, &m_pipelineLayout);
//
//    std::string baseDirectory = modelio->definedDir;
//    const std::string aeroShader = baseDirectory + "/shader/aero/aeresist.comp.spv";
//
//    VkComputePipelineCreateInfo computePipelineCreateInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
//    computePipelineCreateInfo.layout = m_pipelineLayout;
//    computePipelineCreateInfo.stage =
//            nvvk::createShaderStageInfo(m_device, nvh::loadFile(aeroShader, true), VK_SHADER_STAGE_COMPUTE_BIT);
//
//    vkCreateComputePipelines(m_device, {}, 1, &computePipelineCreateInfo, nullptr, &m_pipeline);
//
//    m_debug.setObjectName(m_pipeline, "Aero");
//    vkDestroyShaderModule(m_device, computePipelineCreateInfo.stage.module, nullptr);
//
//    return true;
//}
//
//
//bool Pipeline::createBioPipeline(std::shared_ptr<VoxelrtIO> &modelio)
//{
//
//
//    auto & m_device = modelio->m_device;
//    auto & m_pipelineLayout = modelio->m_pipelineLayout_bio;
//    auto & m_pipeline = modelio->m_pipeline_bio;
//    auto & m_sbtWrapper = modelio->m_sbtWrapper;
//    auto & m_descSetLayout = modelio->m_descSetLayout;
//    auto & useDeferred = modelio->useDeferred;
//    auto & useSBTWrapper = modelio->useSBTWrapper;
//    auto & m_debug = modelio->m_debug;
//
//    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
//    VkPushConstantRange pushConstant{VK_SHADER_STAGE_COMPUTE_BIT,
//                                     0, sizeof(BioSetting)};
//
//    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
//    pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
//    pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstant;
//    pipelineLayoutCreateInfo.setLayoutCount = static_cast<uint32_t>(1);
//    pipelineLayoutCreateInfo.pSetLayouts = &m_descSetLayout;
//    vkCreatePipelineLayout(m_device, &pipelineLayoutCreateInfo, nullptr, &m_pipelineLayout);
//
//    std::string baseDirectory =  modelio->definedDir;
//    const std::string bioShader = baseDirectory + "/shader/bio/biochemical.comp.spv";
//
//    VkComputePipelineCreateInfo computePipelineCreateInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
//    computePipelineCreateInfo.layout = m_pipelineLayout;
//    computePipelineCreateInfo.stage =
//            nvvk::createShaderStageInfo(m_device, nvh::loadFile(bioShader, true), VK_SHADER_STAGE_COMPUTE_BIT);
//
//    vkCreateComputePipelines(m_device, {}, 1, &computePipelineCreateInfo, nullptr, &m_pipeline);
//
//    m_debug.setObjectName(m_pipeline, "Bio");
//    vkDestroyShaderModule(m_device, computePipelineCreateInfo.stage.module, nullptr);
//
//    return true;
//}



// 作用：销毁 createPipeline 创建的所有计算管线与管线布局，并把句柄置空。
// 说明：下面被注释的部分为其他模式（EB/ET/RT/Aero/Bio）的旧销毁代码，当前不使用。
void Pipeline::destroy(std::shared_ptr<VoxelrtIO> &modelio) {

    // modelio->m_sbtWrapper.destroy();
    // modelio->m_pAlloc->destroy((modelio->m_rtSBTBuffer));
    // vkDestroyPipeline(modelio->m_device, modelio->m_pipeline_aero, nullptr);
    // vkDestroyPipelineLayout(modelio->m_device, modelio->m_pipelineLayout_aero, nullptr);
    // vkDestroyPipeline(modelio->m_device, modelio->m_pipeline_bio, nullptr);
    // vkDestroyPipelineLayout(modelio->m_device, modelio->m_pipelineLayout_bio, nullptr);
    //
    // for (auto &stage : magic_enum::enum_values<EBStage>())
    // {
    //     int stageInt = (int)stage;
    //     vkDestroyPipeline(modelio->m_device, modelio->m_pipelines_eb[stageInt], nullptr);
    //     modelio->m_pipelines_eb[stageInt] =  VkPipeline();
    // }
    // modelio->m_pipelineLayout_eb = VkPipelineLayout();


    // 逐阶段销毁管线；若 createPipeline 中途失败，未创建阶段的句柄为空（std::map 访问会
    // 补默认值），vkDestroyPipeline 对空句柄无操作。
    for (auto &stage : magic_enum::enum_values<VoxelRTStage>()) {
        vkDestroyPipeline(modelio->m_device, modelio->m_pipelines[stage], nullptr);
        modelio->m_pipelines[stage] = VkPipeline();
    }
    vkDestroyPipelineLayout(modelio->m_device, modelio->m_pipelineLayout, nullptr);
    modelio->m_pipelineLayout = VkPipelineLayout();


    //
    // for (auto &stage : magic_enum::enum_values<ETStage>())
    // {
    //     int stageInt = (int)stage;
    //     vkDestroyPipeline(modelio->m_device, modelio->m_pipelines_et[stageInt], nullptr);
    //     modelio->m_pipelines_et[stageInt] =  VkPipeline();
    // }
    // modelio->m_pipelineLayout_et = VkPipelineLayout();
    //
    //
    // for (auto &stage : magic_enum::enum_values<VoxelRadStage>())
    // {
    //     int stageInt = (int)stage;
    //     vkDestroyPipeline(modelio->m_device, modelio->m_pipelines_rt[stageInt], nullptr);
    //     modelio->m_pipelines_rt[stageInt] =  VkPipeline();
    // }
    // modelio->m_pipelineLayout_rt = VkPipelineLayout();



}






