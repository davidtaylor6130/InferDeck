#include "llama_cpp/VulkanDevice.hpp"
#include "core/Logger.hpp"
#include <algorithm>

namespace inferdeck::core {

VulkanDevice& VulkanDevice::Get() {
    static VulkanDevice instance;
    return instance;
}

bool VulkanDevice::IsVulkanAvailable() {
    return false;
}

std::vector<GpuInfo> VulkanDevice::EnumerateGpus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        gpus_.clear();
        initialized_ = true;
    }
    return gpus_;
}

GpuInfo VulkanDevice::GetBestGpu() const {
    std::vector<GpuInfo> gpus = EnumerateGpus();
    if (gpus.empty()) {
        return GpuInfo{};
    }
    auto discrete = std::find_if(gpus.begin(), gpus.end(),
        [](const GpuInfo& g) { return g.is_discrete; });
    if (discrete != gpus.end()) {
        return *discrete;
    }
    return *std::max_element(gpus.begin(), gpus.end(),
        [](const GpuInfo& a, const GpuInfo& b) {
            return a.memory_total < b.memory_total;
        });
}

GpuInfo VulkanDevice::GetGpuByIndex(uint32_t index) const {
    std::vector<GpuInfo> gpus = EnumerateGpus();
    if (index < gpus.size()) {
        return gpus[index];
    }
    return GpuInfo{};
}

uint64_t VulkanDevice::GetTotalVramMb() const {
    GpuInfo gpu = GetBestGpu();
    return gpu.memory_total / (1024 * 1024);
}

uint64_t VulkanDevice::GetFreeVramMb() const {
    GpuInfo gpu = GetBestGpu();
    return gpu.memory_free / (1024 * 1024);
}

} // namespace inferdeck::core
