// ==========================================================================
// 文件：buffer.cpp（faceteb）
// 功能：实现 FacetEB “缓冲区”阶段：校验上传阶段已完成，并置 bufferReady 标志。
// 主要模块：
//   1. Buffer::createBuffer：要求 uploadReady，成功后 bufferReady = true
//   2. Buffer::destroy：bufferReady = false
// 说明：这里只是对 setup→upload→create 阶段顺序的状态机封装，不创建真实 GPU 缓冲；
//   实际的 Vulkan 资源由 facetrt/facetrtcore.cpp 在 runFacetEBCore 内部创建。
// ==========================================================================

#include "buffer.h"

namespace faceteb_model {

// 作用：缓冲区阶段；前置条件 uploadReady（由 Faceteb::upload 置位），否则返回 false。
// 输出：成功时置 modelio->bufferReady = true。
bool Buffer::createBuffer(std::shared_ptr<FacetebIO>& modelio)
{
    if (!modelio || !modelio->uploadReady) {
        return false;
    }
    modelio->bufferReady = true;
    return true;
}

// 作用：撤销缓冲区阶段标志（modelio 为空时忽略）；由 Faceteb::destroy 逆序调用。
void Buffer::destroy(std::shared_ptr<FacetebIO>& modelio)
{
    if (modelio) {
        modelio->bufferReady = false;
    }
}

} // namespace faceteb_model
