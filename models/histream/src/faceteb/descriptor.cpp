// ==========================================================================
// 文件：descriptor.cpp（faceteb）
// 功能：实现 FacetEB “描述符”阶段：校验缓冲区阶段已完成，并置 descriptorReady 标志。
// 主要模块：
//   1. Descriptor::createDescriptor：要求 bufferReady，成功后 descriptorReady = true
//   2. Descriptor::destroy：descriptorReady = false
// 说明：只是阶段状态机，不创建真实的 Vulkan 描述符集。
// ==========================================================================

#include "descriptor.h"

namespace faceteb_model {

// 作用：描述符阶段；前置条件 bufferReady，否则返回 false；成功时置 descriptorReady。
bool Descriptor::createDescriptor(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || !modelio->bufferReady) {
        return false;
    }
    modelio->descriptorReady = true;
    return true;
}

// 作用：撤销描述符阶段标志（modelio 为空时忽略）。
void Descriptor::destroy(std::shared_ptr<FacetebIO>& modelio)
{
    if (modelio) {
        modelio->descriptorReady = false;
    }
}

} // namespace faceteb_model
