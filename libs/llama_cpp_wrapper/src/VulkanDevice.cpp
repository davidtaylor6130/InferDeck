#include "llama_cpp/VulkanDevice.hpp"
#include "core/Logger.hpp"
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace inferdeck::core {

static bool g_vulkan_tried = false;
static bool g_vulkan_available = false;

VulkanDevice& VulkanDevice::Get() {
    static VulkanDevice instance;
    return instance;
}

bool VulkanDevice::IsVulkanAvailable() {
    if (g_vulkan_tried) {
        return g_vulkan_available;
    }

    g_vulkan_tried = true;

#ifdef _WIN32
    HMODULE vulkan_lib = LoadLibraryA("vulkan-1.dll");
    if (vulkan_lib) {
        FreeLibrary(vulkan_lib);
        g_vulkan_available = true;
        return true;
    }
#endif

    g_vulkan_available = false;
    return false;
}

std::vector<GpuInfo> VulkanDevice::EnumerateGpus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return gpus_;
    }

    gpus_.clear();

#ifdef _WIN32
    HMODULE vulkan_lib = LoadLibraryA("vulkan-1.dll");
    if (vulkan_lib) {
        using PFN_vkEnumerateInstanceVersion = int32_t(*)(uint32_t*);
        using PFN_vkCreateInstance = int32_t(*)(const void*, void*, void*);
        using PFN_vkDestroyInstance = void(*)(void*, void*);
        using PFN_vkEnumeratePhysicalDevices = int32_t(*)(void*, uint32_t*, void*);
        using PFN_vkGetPhysicalDeviceProperties = void(*)(void*, void*);
        using PFN_vkGetPhysicalDeviceMemoryProperties = void(*)(void*, void*);

        auto vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)GetProcAddress(vulkan_lib, "vkEnumerateInstanceVersion");
        auto vkCreateInstance = (PFN_vkCreateInstance)GetProcAddress(vulkan_lib, "vkCreateInstance");
        auto vkDestroyInstance = (PFN_vkDestroyInstance)GetProcAddress(vulkan_lib, "vkDestroyInstance");
        auto vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)GetProcAddress(vulkan_lib, "vkEnumeratePhysicalDevices");
        auto vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)GetProcAddress(vulkan_lib, "vkGetPhysicalDeviceProperties");
        auto vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)GetProcAddress(vulkan_lib, "vkGetPhysicalDeviceMemoryProperties");

        if (vkEnumerateInstanceVersion && vkCreateInstance && vkDestroyInstance &&
            vkEnumeratePhysicalDevices && vkGetPhysicalDeviceProperties && vkGetPhysicalDeviceMemoryProperties) {

            uint32_t version = 0;
            if (vkEnumerateInstanceVersion(&version) == 0) {
                struct VkApplicationInfo {
                    int32_t sType;
                    const char* pApplicationName;
                    uint32_t apiVersion;
                } app_info = {};

                struct VkInstanceCreateInfo {
                    int32_t sType;
                    const VkApplicationInfo* pApplicationInfo;
                } create_info = {};

                app_info.sType = 0;
                app_info.pApplicationName = "InferDeck";
                app_info.apiVersion = 4198400;
                create_info.sType = 1;
                create_info.pApplicationInfo = &app_info;

                void* instance = nullptr;
                if (vkCreateInstance(&create_info, nullptr, &instance) == 0) {
                    uint32_t device_count = 0;
                    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);

                    if (device_count > 0) {
                        std::vector<void*> devices(device_count);
                        vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

                        struct VkPhysicalDeviceProperties {
                            char deviceName[256];
                            int deviceType;
                            int padding1;
                            int64_t padding2;
                            int limits_maxComputeWorkGroupInvocations;
                        };

                        struct VkMemoryHeap {
                            int64_t size;
                            int flags;
                        };

                        struct VkPhysicalDeviceMemoryProperties {
                            uint32_t memoryHeapCount;
                            VkMemoryHeap memoryHeaps[16];
                        };

                        for (uint32_t i = 0; i < device_count; i++) {
                            VkPhysicalDeviceProperties props = {};
                            vkGetPhysicalDeviceProperties(devices[i], &props);

                            VkPhysicalDeviceMemoryProperties mem_props = {};
                            vkGetPhysicalDeviceMemoryProperties(devices[i], &mem_props);

                            GpuInfo gpu;
                            gpu.device_index = i;
                            gpu.name = props.deviceName;

                            int64_t total_memory = 0;
                            for (uint32_t j = 0; j < mem_props.memoryHeapCount && j < 16; j++) {
                                if (mem_props.memoryHeaps[j].flags & 1) {
                                    total_memory = mem_props.memoryHeaps[j].size;
                                    break;
                                }
                            }
                            gpu.memory_total = static_cast<uint64_t>(total_memory);
                            gpu.memory_free = static_cast<uint64_t>(total_memory * 3 / 4);
                            gpu.compute_units = static_cast<uint32_t>(props.limits_maxComputeWorkGroupInvocations);
                            gpu.is_discrete = (props.deviceType == 1);

                            gpus_.push_back(gpu);
                        }
                    }

                    vkDestroyInstance(instance, nullptr);
                }
            }
        }

        FreeLibrary(vulkan_lib);
    }
#endif

    initialized_ = true;
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