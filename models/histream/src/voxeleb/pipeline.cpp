// ==========================================================================
// 文件：pipeline.cpp
// 功能：创建/销毁 VoxelEB（体元能量平衡）的全部计算管线（compute pipeline）
// 主要模块/步骤：
//   1. createPipeline：重建管线布局（push constant = VoxelLstSetting + 描述符集布局）
//   2. 按 VoxelEBStage 枚举为每个阶段定位 .spv 着色器（shader/voxeleb/*.comp.spv）
//   3. 设置特化常量（周期边界开关与穿越次数上限），逐阶段创建 VkPipeline
//   4. destroy：销毁所有阶段的管线与管线布局
//   （其余 createRT/ET/EB/Aero/BioPipeline 为旧版分管线实现，已整体注释掉，仅作历史保留）
// 数据流：输入 modelio->definedDir（着色器所在根目录）、m_descSetLayout（由 descriptor.cpp
//         创建）、stabilityCorrection / vegetationTemperatureMethod / periodicNeighborCount；
//         输出 modelio->m_pipelineLayout 与 m_pipelines[stage]
// 与其他文件的关系：command.cpp 用 m_pipelines[stage] 与 m_pipelineLayout 录制/提交各阶段；
//   所有阶段共用同一管线布局与描述符集
// 注意：stabilityCorrection 开启时 budget/evapo/aeresist 换用 *_most 版本（Monin-Obukhov
//   稳定度修正）；生化阶段按 vegetationTemperatureMethod 选择 biochemical 或 biochemical_m12
// ==========================================================================

//
// Created by admin on 2024/1/26.
//
#include <io.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include "pipeline.h"




// 作用：RT 管线 SBT（着色器绑定表）的占位实现。VoxelEB 全为计算管线，无需 SBT，函数体为空。
void Pipeline::createShaderBindingTable(std::shared_ptr<VoxelebIO> &modelio)
{




}

// 作用：创建 VoxelEB 所有阶段（VoxelEBStage）的计算管线。
// 输入：modelio（definedDir、m_descSetLayout、m_device 及若干开关，见文件头）；
// 输出：modelio->m_pipelineLayout、modelio->m_pipelines[stage]；全部成功返回 true，
//       任一阶段创建失败则记录错误并返回 false（此前已建的管线保留，由 destroy 清理）。
// 说明：每次调用先销毁旧的管线布局，因此可在更改开关后重建管线。
bool Pipeline::createPipeline(std::shared_ptr<VoxelebIO> &modelio)
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


    // 步骤 1：重建管线布局。push constant 范围覆盖整个 VoxelLstSetting（128 字节，仅计算阶段），
    // 描述符集布局只有一个集（descriptor.cpp 创建）。
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);

    VkPushConstantRange pushConstant{VK_SHADER_STAGE_COMPUTE_BIT,
                                     0, sizeof(VoxelLstSetting)};
    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
    pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstant;
    pipelineLayoutCreateInfo.setLayoutCount = static_cast<uint32_t>(1);
    pipelineLayoutCreateInfo.pSetLayouts = &m_descSetLayout;
    vkCreatePipelineLayout(m_device, &pipelineLayoutCreateInfo, nullptr, &m_pipelineLayout);


    // 步骤 2：确定每个阶段对应的 SPIR-V 文件（相对 definedDir）。
    // stabilityCorrection（Monin-Obukhov 稳定度修正）只影响 budget / evapo / aeresist 三个着色器，
    // 用 _most 后缀选择同一源码以 -DVOXELEB_MOST=1 编译的版本（见 CMakeLists.txt）；其余模块不变。
    std::string baseDirectory = modelio->definedDir;
    // control.stabilityCorrection selects the Monin-Obukhov builds of the
    // exchange shaders (-DVOXELEB_MOST); the neutral modules are unchanged.
    const std::string most = modelio->stabilityCorrection ? "_most" : "";
    std::map<VoxelEBStage, std::string> VoxellstPaths{
            // 阶段与着色器对应：gap=体元太阳可见度（voxelrad_solar）、directVNIR/directTIR=直射项、
            // scatteringVNIR / scatteringTrace=VNIR 多次散射（scatteringTrace 是同一份 voxelrad_scattering_VNIR.comp
            // 以 -DSCATTER_TRACE_STAGE=1 另行编译，见 CMakeLists.txt）、diffuseTIR=热红外漫射、budget=能量收支、
            // evapo=蒸散与热储、updateTp=提交温度历史、updateL=Monin-Obukhov 长度 L（旧版）、aero=空气动力阻抗、
            // bio=叶片生化、fluidLbm/fluidCommit=LBM 流体步进与提交、out=传感器成像输出。
            {VoxelEBStage::gap,         baseDirectory + "/shader/voxeleb/voxelrad_solar.comp.spv"},
            {VoxelEBStage::directVNIR,  baseDirectory + "/shader/voxeleb/voxelrad_direct_VNIR.comp.spv"},
            {VoxelEBStage::directTIR,   baseDirectory + "/shader/voxeleb/voxelrad_direct_TIR.comp.spv"},
            {VoxelEBStage::scatteringVNIR, baseDirectory + "/shader/voxeleb/voxelrad_scattering_VNIR.comp.spv"},
            {VoxelEBStage::scatteringTrace, baseDirectory + "/shader/voxeleb/voxelrad_scattering_trace.comp.spv"},
            {VoxelEBStage::diffuseTIR,  baseDirectory + "/shader/voxeleb/voxelrad_diffuse_TIR_single.comp.spv"},
            {VoxelEBStage::budget,      baseDirectory + "/shader/voxeleb/budget" + most + ".comp.spv"},
            {VoxelEBStage::evapo,       baseDirectory + "/shader/voxeleb/evapo" + most + ".comp.spv"},
            {VoxelEBStage::updateTp,    baseDirectory + "/shader/voxeleb/updateTp.comp.spv"},
            {VoxelEBStage::updateL,     baseDirectory + "/shader/voxeleb/updateL.comp.spv"},
            {VoxelEBStage::aero,        baseDirectory + "/shader/voxeleb/aeresist" + most + ".comp.spv"},
             // 植被温度方法 0 用 biochemical.comp，其余取值用 biochemical_m12.comp（方法编号含义待核对）。
             {VoxelEBStage::bio,         baseDirectory + (modelio->vegetationTemperatureMethod == 0
                 ? "/shader/voxeleb/biochemical.comp.spv"
                 : "/shader/voxeleb/biochemical_m12.comp.spv")},
            {VoxelEBStage::fluidLbm,         baseDirectory + "/shader/voxeleb/fluid_lbm.comp.spv"},
            {VoxelEBStage::fluidCommit,      baseDirectory + "/shader/voxeleb/fluid_lbm_commit.comp.spv"},
            {VoxelEBStage::out,         baseDirectory + "/shader/voxeleb/voxelrad_image.comp.spv"}};


    // 仅以 gap 阶段的 .spv 为代表检查着色器目录是否存在；缺失只打印日志，不中断
    // （后续 loadFile 对缺失文件的行为由 nvh 决定，待核对）。
    if (_access(VoxellstPaths[VoxelEBStage::gap].c_str(), 0) == -1)
    {
        std::string error = "Error: no shader file (.spv) found in " + baseDirectory + "\n";
        LOGI(error.c_str());
    }

    // Specialization constants (periodic_walk.glsl): 0 = PERIODIC_BOUNDARY_ENABLED,
    // 1 = PERIODIC_CROSSING_LIMIT. Without periodic boundaries the driver
    // drops every periodic branch; with them the crossing limit is a
    // compile-time constant, so the periodic and non-periodic code paths of
    // a call site are no longer both kept.
    // 步骤 3：特化常量，编号与 shader/periodic_walk.glsl 的 constant_id 对应：
    // 0 = PERIODIC_BOUNDARY_ENABLED（周期边界开关），1 = PERIODIC_CROSSING_LIMIT（穿越次数上限，
    // 此处限幅到 [0, 20]）。
    struct PeriodicSpecData { VkBool32 enabled; int32_t limit; };
    const PeriodicSpecData periodicData{
        modelio->periodicNeighborCount > 0 ? VK_TRUE : VK_FALSE,
        std::clamp<int32_t>(modelio->periodicNeighborCount, 0, 20)};
    const VkSpecializationMapEntry periodicEntries[2] = {
        {0, offsetof(PeriodicSpecData, enabled), sizeof(VkBool32)},
        {1, offsetof(PeriodicSpecData, limit), sizeof(int32_t)}};
    const VkSpecializationInfo periodicSpecialization{2, periodicEntries, sizeof(PeriodicSpecData), &periodicData};

    // 步骤 4：逐阶段加载 SPIR-V、创建计算管线；着色器模块在管线创建后立即销毁（管线已持有副本）。
    // 任一阶段失败即中止并返回 false。
    for (const auto &stage : magic_enum::enum_values<VoxelEBStage>())
    {
        //int stageInt = (int)stage;
        VkComputePipelineCreateInfo computePipelineCreateInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        computePipelineCreateInfo.layout = m_pipelineLayout;
        computePipelineCreateInfo.stage =
                nvvk::createShaderStageInfo(m_device, nvh::loadFile(VoxellstPaths[stage], true), VK_SHADER_STAGE_COMPUTE_BIT);
        computePipelineCreateInfo.stage.pSpecializationInfo = &periodicSpecialization;

        const VkResult result = vkCreateComputePipelines(
            m_device, {}, 1, &computePipelineCreateInfo, nullptr, &m_pipelines[stage]);
        if (result != VK_SUCCESS) {
            LOGE("Failed to create VoxelEB compute pipeline (%d) for %s\n",
                 static_cast<int>(result), VoxellstPaths[stage].c_str());
            vkDestroyShaderModule(m_device, computePipelineCreateInfo.stage.module, nullptr);
            return false;
        }

        m_debug.setObjectName(m_pipelines[stage], "VoxelEB");
        vkDestroyShaderModule(m_device, computePipelineCreateInfo.stage.module, nullptr);
    }

    return true;
}

//
//bool Pipeline::createRTPipeline(std::shared_ptr<VoxelebIO> &modelio)
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
//bool Pipeline::createETPipeline(std::shared_ptr<VoxelebIO> &modelio)
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
//bool Pipeline::createEBPipeline(std::shared_ptr<VoxelebIO> &modelio)
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
//bool Pipeline::createAeroPipeline(std::shared_ptr<VoxelebIO> &modelio)
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
//bool Pipeline::createBioPipeline(std::shared_ptr<VoxelebIO> &modelio)
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



// 作用：销毁 createPipeline 创建的所有阶段管线与管线布局，并把句柄复位为空。
// 说明：其余被注释掉的行是旧版分管线（RT/ET/EB/Aero/Bio）的销毁逻辑，已不使用。
void Pipeline::destroy(std::shared_ptr<VoxelebIO> &modelio) {

    // modelio->m_sbtWrapper.destroy();
   // modelio->m_pAlloc->destroy((modelio->m_rtSBTBuffer));

//    vkDestroyPipeline(modelio->m_device, modelio->m_pipeline_aero, nullptr);
//    vkDestroyPipelineLayout(modelio->m_device, modelio->m_pipelineLayout_aero, nullptr);
//    vkDestroyPipeline(modelio->m_device, modelio->m_pipeline_bio, nullptr);
//    vkDestroyPipelineLayout(modelio->m_device, modelio->m_pipelineLayout_bio, nullptr);
//
//    for (auto &stage : magic_enum::enum_values<EBStage>())
//    {
//        int stageInt = (int)stage;
//        vkDestroyPipeline(modelio->m_device, modelio->m_pipelines_eb[stageInt], nullptr);
//        modelio->m_pipelines_eb[stageInt] =  VkPipeline();
//    }
//    modelio->m_pipelineLayout_eb = VkPipelineLayout();


    for (auto &stage : magic_enum::enum_values<VoxelEBStage>()) {
        vkDestroyPipeline(modelio->m_device, modelio->m_pipelines[stage], nullptr);
        modelio->m_pipelines[stage] = VkPipeline();
    }
    vkDestroyPipelineLayout(modelio->m_device, modelio->m_pipelineLayout, nullptr);
    modelio->m_pipelineLayout = VkPipelineLayout();


//
//    for (auto &stage : magic_enum::enum_values<ETStage>())
//    {
//        int stageInt = (int)stage;
//        vkDestroyPipeline(modelio->m_device, modelio->m_pipelines_et[stageInt], nullptr);
//        modelio->m_pipelines_et[stageInt] =  VkPipeline();
//    }
//    modelio->m_pipelineLayout_et = VkPipelineLayout();
//
//
//    for (auto &stage : magic_enum::enum_values<VoxelRadStage>())
//    {
//        int stageInt = (int)stage;
//        vkDestroyPipeline(modelio->m_device, modelio->m_pipelines_rt[stageInt], nullptr);
//        modelio->m_pipelines_rt[stageInt] =  VkPipeline();
//    }
//    modelio->m_pipelineLayout_rt = VkPipelineLayout();



}






