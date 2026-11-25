// ==========================================================================
// 文件：faceteb.cpp
// 功能：实现 FacetEB 后端门面 Faceteb：按阶段校验并转发到 Buffer/Descriptor/Pipeline/Command。
// 主要模块：
//   1. Faceteb::Faceteb：创建四个阶段对象
//   2. Faceteb::setup：校验输入路径与着色器目录非空
//   3. Faceteb::upload：要求 setup 已完成
//   4. Faceteb::create：依次创建缓冲区、描述符、管线、命令（任一失败即短路返回 false）
//   5. Faceteb::run：转发到 Command::runEB（真正执行 runFacetEBCore）
//   6. Faceteb::destroy：按创建的逆序撤销各阶段标志
// 数据流：状态与退出码均保存在 FacetebIO（facetebio.h），不在本类内部保存。
// 说明：除 run 外各阶段只维护布尔标志，不分配实际 GPU 资源；Vulkan 资源由
//   facetrtcore.cpp 在 runFacetEBCore 内自行创建与释放（待核对）。
// ==========================================================================

#include "faceteb.h"

#include "buffer.h"
#include "command.h"
#include "descriptor.h"
#include "pipeline.h"

// 作用：构造四个阶段子对象（均无状态，状态存放于 FacetebIO）。
Faceteb::Faceteb()
{
    m_pBuffer = std::make_shared<faceteb_model::Buffer>();
    m_pDescriptor = std::make_shared<faceteb_model::Descriptor>();
    m_pPipeline = std::make_shared<faceteb_model::Pipeline>();
    m_pCommand = std::make_shared<faceteb_model::Command>();
}

// 作用：setup 阶段；inputPath 或 shaderDirectory 为空时失败（outputPath 可为空，
//   由核心按输入路径推导默认输出位置）。
bool Faceteb::setup(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || modelio->inputPath.empty() || modelio->shaderDirectory.empty()) {
        return false;
    }
    modelio->setupReady = true;
    return true;
}

// 作用：upload 阶段；前置条件 setupReady。工程数据实际在 run 内由核心读取，这里只置标志。
bool Faceteb::upload(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || !modelio->setupReady) {
        return false;
    }
    modelio->uploadReady = true;
    return true;
}

// 作用：create 阶段；前置条件 uploadReady，依次创建 buffer → descriptor → pipeline → command，
//   &&  短路保证前一阶段失败时不再执行后续阶段。
bool Faceteb::create(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || !modelio->uploadReady) {
        return false;
    }
    return m_pBuffer->createBuffer(modelio) &&
           m_pDescriptor->createDescriptor(modelio) &&
           m_pPipeline->createPipeline(modelio) &&
           m_pCommand->create(modelio);
}

// 作用：运行 FacetEB；返回值只表示退出码是否为 0，细分结果（如 2 = 未收敛）
//   请读 modelio->exitCode。
bool Faceteb::run(std::shared_ptr<FacetebIO>& modelio)
{
    return m_pCommand->runEB(modelio);
}

// 作用：撤销所有阶段标志，顺序与创建相反（command → pipeline → descriptor → buffer）；
//   始终返回 true，modelio 为空时视为无需清理。
bool Faceteb::destroy(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio) {
        return true;
    }
    m_pCommand->destroy(modelio);
    m_pPipeline->destroy(modelio);
    m_pDescriptor->destroy(modelio);
    m_pBuffer->destroy(modelio);
    return true;
}
