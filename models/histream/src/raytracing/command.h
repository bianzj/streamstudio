//
// Created by admin on 2024/1/26.
//

#ifndef FIELD_COMMAND_H
#define FIELD_COMMAND_H

#include "raytracingio.h"
#include <vector>

class Command {
public:
    Command();

    bool create(std::shared_ptr<RaytracingIO> &raytracingio);

    bool run(std::shared_ptr<RaytracingIO> &raytracingio);
//    void submit(std::shared_ptr<RaytracingIO> &raytracingio,
//                const std::optional<std::vector<VkSemaphore>> &inSemaphore,
//                const std::optional<VkSemaphore> &outSemaphore);

    void submit(std::shared_ptr<RaytracingIO> &raytracingio,
                const std::optional<VkSemaphore> &inSemaphore,
                const std::optional<VkSemaphore> &outSemaphore);

    void recordCommandBuffer(const VkCommandBuffer& cmdBuf,std::shared_ptr<RaytracingIO> &raytracingio);
    void waitFence(std::shared_ptr<RaytracingIO> &raytracingio);
    void destroy(std::shared_ptr<RaytracingIO> &raytracingio);

private:
    // Submitted command buffers, freed once waitFence() observes completion.
    std::vector<VkCommandBuffer> m_inFlight;
};


#endif //FIELD_COMMAND_H
