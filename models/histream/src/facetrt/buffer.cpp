// ==========================================================================
// 文件：buffer.cpp（facetrt）
// 功能：实现 FacetRT “缓冲区”阶段：校验上传阶段已完成，并置 bufferReady 标志。
// 主要模块：
//   1. Buffer::createBuffer：要求 uploadReady，成功后 bufferReady = true
//   2. Buffer::destroy：bufferReady = false
// 说明：仅为阶段状态机，不创建真实 GPU 缓冲；实际 Vulkan 资源由 facetrtcore.cpp 内部创建。
// ==========================================================================

#include "buffer.h"

namespace facetrt_model {

// 作用：缓冲区阶段；前置条件 uploadReady，否则返回 false；成功时置 bufferReady。
bool Buffer::createBuffer(std::shared_ptr<FacetrtIO>& modelio)
{
    if (!modelio || !modelio->uploadReady) {
        return false;
    }
    modelio->bufferReady = true;
    return true;
}

// 作用：撤销缓冲区阶段标志（modelio 为空时忽略）；由 Facetrt::destroy 逆序调用。
void Buffer::destroy(std::shared_ptr<FacetrtIO>& modelio)
{
    if (modelio) {
        modelio->bufferReady = false;
    }
}

} // namespace facetrt_model
