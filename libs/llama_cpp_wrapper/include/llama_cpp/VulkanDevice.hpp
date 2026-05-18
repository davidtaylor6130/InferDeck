/// @file VulkanDevice.hpp
/// @brief Vulkan device abstraction for InferDeck GPU backend.
///
/// Provides device enumeration, adapter selection, and GPU info
/// for Vulkan-based GPU acceleration on AMD GPUs.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <vulkan/vulkan.hpp>

namespace inferdeck::core {

/// GPU adapter information.
struct GpuInfo {
    uint32_t device_index = 0;
    std::string name;
    uint64_t memory_total = 0;
    uint64_t memory_free = 0;
    uint32_t compute_units = 0;
    bool is_discrete = true;
};

/// VulkanDevice manages Vulkan GPU device selection and initialization.
///
/// Enumerates available Vulkan devices, selects the best GPU based on
/// criteria (discrete > integrated, most memory first), and provides
/// a handle for downstream use by the llama.cpp backend.
class VulkanDevice {
public:
    /// Get the singleton VulkanDevice instance.
    /// @return Reference to the singleton VulkanDevice instance.
    static VulkanDevice& Get();

    /// Enumerate all available Vulkan GPUs.
    /// @return Vector of GPU info for all detected devices.
    std::vector<GpuInfo> EnumerateGpus();

    /// Get the best available GPU device.
    /// @return The selected GpuInfo, or empty if no GPU found.
    GpuInfo GetBestGpu() const;

    /// Get a specific GPU by index.
    /// @param index The device index.
    /// @return The GpuInfo, or empty if not found.
    GpuInfo GetGpuByIndex(uint32_t index) const;

    /// Get the Vulkan physical device handle.
    /// @param index The device index (0-based).
    /// @return The vk::PhysicalDevice handle, or empty if not found.
    vk::PhysicalDevice GetPhysicalDevice(uint32_t index) const;

    /// Get the total VRAM of the best GPU in MB.
    /// @return Total VRAM in megabytes.
    uint64_t GetTotalVramMb() const;

    /// Get the free VRAM of the best GPU in MB.
    /// @return Free VRAM in megabytes.
    uint64_t GetFreeVramMb() const;

    /// Check if Vulkan is available on this system.
    /// @return True if Vulkan instance can be created.
    static bool IsVulkanAvailable();

private:
    VulkanDevice();
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    vk::Instance CreateInstance();
    std::vector<vk::PhysicalDevice> EnumeratePhysicalDevices(vk::Instance instance);
    GpuInfo PhysicalDeviceToGpuInfo(vk::PhysicalDevice device, uint32_t index);

    mutable vk::Instance instance_;
    std::vector<vk::PhysicalDevice> physical_devices_;
    mutable bool initialized_ = false;
    mutable std::mutex mutex_;
};

} // namespace inferdeck::core
