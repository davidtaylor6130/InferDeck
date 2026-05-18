/// @file VulkanDevice.cpp
/// @brief VulkanDevice implementation.

#include "llama_cpp/VulkanDevice.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <vulkan/vulkan_core.h>

namespace inferdeck::core {

VulkanDevice& VulkanDevice::Get() {
    static VulkanDevice instance;
    return instance;
}

VulkanDevice::VulkanDevice() = default;
VulkanDevice::~VulkanDevice() {
    if (instance_) {
        instance_.destroy();
    }
}

bool VulkanDevice::IsVulkanAvailable() {
    try {
        vk::ApplicationInfo app_info("InferDeck", VK_MAKE_VERSION(1, 0, 0), "InferDeck", VK_MAKE_VERSION(1, 0, 0), VK_API_VERSION_1_3);
        vk::InstanceCreateInfo create_info({}, &app_info);
        auto instance = vk::createInstance(create_info);
        instance.destroy();
        return true;
    } catch (const vk::system_error&) {
        return false;
    }
}

std::vector<GpuInfo> VulkanDevice::EnumerateGpus() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        instance_ = CreateInstance();
        physical_devices_ = EnumeratePhysicalDevices(instance_);
        initialized_ = true;
    }

    std::vector<GpuInfo> gpus;
    for (uint32_t i = 0; i < physical_devices_.size(); i++) {
        gpus.push_back(PhysicalDeviceToGpuInfo(physical_devices_[i], i));
    }
    return gpus;
}

GpuInfo VulkanDevice::GetBestGpu() const {
    std::vector<GpuInfo> gpus = EnumerateGpus();
    if (gpus.empty()) {
        return GpuInfo{};
    }

    // Prefer discrete GPUs, then by memory
    auto discrete = std::find_if(gpus.begin(), gpus.end(),
        [](const GpuInfo& g) { return g.is_discrete; });
    if (discrete != gpus.end()) {
        return *discrete;
    }

    // Return GPU with most memory
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

vk::PhysicalDevice VulkanDevice::GetPhysicalDevice(uint32_t index) const {
    std::vector<GpuInfo> gpus = EnumerateGpus();
    if (index < gpus.size()) {
        return physical_devices_[index];
    }
    return vk::PhysicalDevice();
}

uint64_t VulkanDevice::GetTotalVramMb() const {
    GpuInfo gpu = GetBestGpu();
    return gpu.memory_total / (1024 * 1024);
}

uint64_t VulkanDevice::GetFreeVramMb() const {
    GpuInfo gpu = GetBestGpu();
    return gpu.memory_free / (1024 * 1024);
}

vk::Instance VulkanDevice::CreateInstance() {
    try {
        vk::ApplicationInfo app_info("InferDeck", VK_MAKE_VERSION(1, 0, 0), "InferDeck", VK_MAKE_VERSION(1, 0, 0), VK_API_VERSION_1_3);
        std::vector<const char*> extensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_VULKAN_EXTENSION_NAME
        };

        // Add platform-specific extensions
#ifdef _WIN32
        extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif __linux__
        extensions.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
#endif

        vk::InstanceCreateInfo create_info({}, &app_info, {}, extensions);
        return vk::createInstance(create_info);
    } catch (const vk::system_error& e) {
        Logger::Get().Error("Failed to create Vulkan instance: " + std::string(e.what()));
        return vk::Instance();
    }
}

std::vector<vk::PhysicalDevice> VulkanDevice::EnumeratePhysicalDevices(vk::Instance instance) {
    try {
        return instance.enumeratePhysicalDevices();
    } catch (const vk::system_error& e) {
        Logger::Get().Error("Failed to enumerate Vulkan devices: " + std::string(e.what()));
        return {};
    }
}

GpuInfo VulkanDevice::PhysicalDeviceToGpuInfo(vk::PhysicalDevice device, uint32_t index) {
    GpuInfo info{};
    info.device_index = index;

    auto props = device.getProperties();
    info.name = std::string(props.deviceName);
    info.memory_total = props.totalMem;
    info.compute_units = props.maxComputeSharedMemSize;
    info.is_discrete = props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;

    // Get memory info
    auto mem_props = device.getMemoryProperties();
    uint64_t free_mem = 0;
    for (uint32_t i = 0; i < mem_props.memoryHeapCount; i++) {
        if (!(mem_props.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal)) {
            continue;
        }
        free_mem += mem_props.memoryHeaps[i].size;
    }
    info.memory_free = free_mem;

    return info;
}

} // namespace inferdeck::core
