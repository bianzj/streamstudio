//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_COMMAND_H
#define FIELD_COMMAND_H

#include "voxelrtio.h"
#include <vector>

class Command {
public:
    Command();

    bool create(std::shared_ptr<VoxelrtIO> &VoxellstIO);
//    bool run(std::shared_ptr<VoxelrtIO> &VoxelrtIO);

    bool runRT(std::shared_ptr<VoxelrtIO> &modelio);
    bool runRTAccelerated(std::shared_ptr<VoxelrtIO> &modelio,
                          int spectralBatchSize = 4);

//    void submit(std::shared_ptr<VoxelrtIO> &VoxelrtIO,glm::ivec3 dispatchSize,
//                VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
//                std::map<VoxelRadStage, VkPipeline> pipelines, VoxelRadStage stage,VoxelLstSetting setting,
//                const std::optional<VkSemaphore> &inSemaphore, const std::optional<VkSemaphore> &outSemaphore);

    void submit(std::shared_ptr<VoxelrtIO> &VoxellstIO, VoxelRTStage stage, glm::ivec3 dispatchSize,
                const std::optional<std::vector<VkSemaphore>> &inSemaphore, const std::optional<VkSemaphore> &outSemaphore);


//    void submit(std::shared_ptr<VoxelrtIO> &VoxelrtIO,glm::ivec3 dispatchSize,
//                VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
//                VkPipeline pipeline, VoxelLstSetting setting,
//                const std::optional<VkSemaphore> &inSemaphore, const std::optional<VkSemaphore> &outSemaphore);

    void Command::recordCommandBuffer(VkCommandBuffer cmdBuf,
                                      VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
                                      VkPipeline pipeline,  VoxelRTSetting setting);

//    void Command::recordCommandBuffer(VkCommandBuffer cmdBuf, VkDescriptorSet descSet, VkPipelineLayout pipelineLayout,
//                                      std::map<VoxelEBStage, VkPipeline> pipelines, VoxelEBStage stage, VoxelLstSetting setting);

//    void recordCommandBuffer(const VkCommandBuffer& cmdBuf,std::shared_ptr<VoxelrtIO> &VoxelrtIO);
    void waitFence(std::shared_ptr<VoxelrtIO> &VoxellstIO);
    void destroy(std::shared_ptr<VoxelrtIO> &VoxellstIO);

private:
    // Submitted command buffers, freed once waitFence() observes completion.
    std::vector<VkCommandBuffer> m_inFlight;
};


#endif //FIELD_COMMAND_H
