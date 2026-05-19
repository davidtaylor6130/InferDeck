#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace inferdeck::core {

struct GpuInfo {
    uint32_t device_index = 0;
    std::string name;
    uint64_t memory_total = 0;
    uint64_t memory_free = 0;
    uint32_t compute_units = 0;
    bool is_discrete = true;
};

class VulkanDevice {
public:
    static VulkanDevice& Get();
    std::vector<GpuInfo> EnumerateGpus() const;
    GpuInfo GetBestGpu() const;
    GpuInfo GetGpuByIndex(uint32_t index) const;
    uint64_t GetTotalVramMb() const;
    uint64_t GetFreeVramMb() const;
    static bool IsVulkanAvailable();

private:
    VulkanDevice() = default;
    ~VulkanDevice() = default;
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    mutable std::vector<GpuInfo> gpus_;
    mutable bool initialized_ = false;
    mutable std::mutex mutex_;
};

} // namespace inferdeck::core
