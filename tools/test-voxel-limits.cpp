// ==========================================================================
// 文件：test-voxel-limits.cpp
// 功能：体元规模上限检查（src/base/voxellimits.h）的单元式自检，需要一个 Vulkan 设备（只读设备限制，不分配显存）。
//   覆盖：VoxelRT 辐射 SSBO（4 GiB）、VoxelEB TLAST SSBO、32 位下标、1D 派发组数；每项在“刚好够”时不抛错，
//   超限时抛出带体元数与上限的明确错误。打印本机的限制与各缓冲的最大体元数。
// 构建与运行（不属默认构建目标）：
//   cmake --build build\histream --config Release --target histream_voxel_limits_tests
//   或  cl /nologo /utf-8 /std:c++17 /EHsc /I<VulkanSDK>\Include /I models\histream\src tools\test-voxel-limits.cpp /link vulkan-1.lib
// 退出码 0：全部通过。
// ==========================================================================

#include "base/voxellimits.h"

#include <cstdio>
#include <functional>
#include <vector>

namespace {
int failures = 0;

void expectThrow(const char* name, const std::function<void()>& call, const char* mustContain)
{
    try {
        call();
        std::printf("[FAIL] %s: no exception\n", name);
        ++failures;
    } catch (const std::exception& error) {
        const std::string text = error.what();
        const bool ok = text.find(mustContain) != std::string::npos;
        std::printf("[%s] %s: %s\n", ok ? "ok" : "FAIL", name, text.c_str());
        if (!ok) ++failures;
    }
}

void expectNoThrow(const char* name, const std::function<void()>& call)
{
    try {
        call();
        std::printf("[ok] %s: no exception\n", name);
    } catch (const std::exception& error) {
        std::printf("[FAIL] %s: unexpected exception: %s\n", name, error.what());
        ++failures;
    }
}
}  // namespace

int main()
{
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&create, nullptr, &instance) != VK_SUCCESS) { std::printf("no Vulkan instance\n"); return 2; }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    if (devices.empty()) { std::printf("no Vulkan device\n"); return 2; }
    const VkPhysicalDevice device = devices[0];
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    const uint64_t range = properties.limits.maxStorageBufferRange;
    const uint64_t groups = properties.limits.maxComputeWorkGroupCount[0];
    std::printf("%s: maxStorageBufferRange %s, maxComputeWorkGroupCount[0] %s\n", properties.deviceName,
                voxellimits::grouped(range).c_str(), voxellimits::grouped(groups).c_str());

    // VoxelRT radiance buffer: 2*n_wave VoxelRad (8 B) per voxel.
    for (uint64_t bands : {3ull, 10ull, 100ull}) {
        const uint64_t perVoxel = 2 * bands * 8;
        std::printf("VoxelRT n_wave=%llu: %llu B/voxel, at most %s voxels\n", (unsigned long long)bands,
                    (unsigned long long)perVoxel, voxellimits::grouped(range / perVoxel).c_str());
    }
    const uint64_t rtMax = range / (2 * 3 * 8);
    expectNoThrow("VoxelRT radiance, n_wave=3, exactly at the limit",
                  [&] { voxellimits::requireStorageRange(device, "VoxelRT", "radiance (voxelRads)", rtMax, 48); });
    expectThrow("VoxelRT radiance, n_wave=3, one voxel over",
                [&] { voxellimits::requireStorageRange(device, "VoxelRT", "radiance (voxelRads)", rtMax + 1, 48); },
                "maxStorageBufferRange");
    expectThrow("VoxelRT radiance, n_wave=100, 4 M voxels (old layout 4.5 GB would also fail)",
                [&] { voxellimits::requireStorageRange(device, "VoxelRT", "radiance (voxelRads)", 4000000, 1600); }, "4,000,000 voxels");

    // VoxelEB TLAST: 8 layers x 8 B = 64 B per voxel.
    const uint64_t tlastMax = range / 64;
    std::printf("VoxelEB TLAST: at most %s voxels\n", voxellimits::grouped(tlastMax).c_str());
    expectNoThrow("VoxelEB TLAST, exactly at the limit",
                  [&] { voxellimits::requireStorageRange(device, "VoxelEB", "temperature history (TLAST)", tlastMax, 64); });
    expectThrow("VoxelEB TLAST, 70 M voxels",
                [&] { voxellimits::requireStorageRange(device, "VoxelEB", "temperature history (TLAST)", 70000000, 64); },
                "70,000,000 voxels");

    // 32-bit index of a table.
    expectNoThrow("32-bit index, 40 M voxels x 96 entries is rejected only above 44.7 M (here 44 M x 96)",
                  [&] { voxellimits::requireIndex32("VoxelEB", "sky cache", 44000000, 96); });
    expectThrow("32-bit index, 45 M voxels x 96 entries",
                [&] { voxellimits::requireIndex32("VoxelEB", "sky cache", 45000000, 96); }, "32-bit index");

    // 1D dispatch.
    expectNoThrow("dispatch, 4.19 M voxels", [&] { voxellimits::requireDispatch(device, "VoxelRT", 4000000, 64); });
    expectThrow("dispatch, voxels beyond groups x 64",
                [&] { voxellimits::requireDispatch(device, "VoxelRT", groups * 64 + 64, 64); }, "maxComputeWorkGroupCount");

    vkDestroyInstance(instance, nullptr);
    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
