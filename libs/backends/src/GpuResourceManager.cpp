/// @file GpuResourceManager.cpp
/// @brief Single-GPU VRAM arbitration implementation.

#include "backends/GpuResourceManager.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>

namespace inferdeck::backends {

GpuResourceManager& GpuResourceManager::Instance() {
    static GpuResourceManager instance;
    return instance;
}

GpuResourceManager::GpuResourceManager() : device_id_(0), device_name_("unknown") {
    spdlog::info("GpuResourceManager: created (Vulkan not yet loaded)");
}

bool GpuResourceManager::Initialize(int device_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    device_id_ = device_id;
    if (DetectGPU(device_id)) {
        initialized_ = true;
        spdlog::info("GpuResourceManager: GPU detected - {}: {} total VRAM",
                     device_name_, total_vram_ / (1024 * 1024));
    } else {
        spdlog::warn("GpuResourceManager: Vulkan GPU detection failed, using simulated mode");
        total_vram_ = 8ULL * 1024 * 1024 * 1024;
        used_vram_ = 0;
        initialized_ = true;
        gpu_available_ = true;
    }
    return initialized_;
}

bool GpuResourceManager::DetectGPU(int device_id) {
    // Attempt Vulkan detection via vulkan.h
    // Falls back gracefully when Vulkan headers/ABI are unavailable
    // (e.g. during macOS development for Windows target)

    // Simulated detection for cross-platform development
    // When built on target (Windows + Vulkan), this would use:
    // VkInstance instance; vkCreateInstance(...);
    // VkPhysicalDevice devices; vkEnumeratePhysicalDevices(...);
    // VkPhysicalDeviceMemoryProperties mem_props;
    // vkGetPhysicalDeviceMemoryProperties(devices, &mem_props);
    // for each heap: if (mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
    //     total_vram_ += mem_props.memoryHeaps[i].size;

    device_name_ = "gpu_device_" + std::to_string(device_id);
    total_vram_ = 16ULL * 1024 * 1024 * 1024; // 16GB simulated default
    used_vram_ = 0;
    gpu_available_ = true;
    return true;
}

bool GpuResourceManager::IsAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gpu_available_ && initialized_;
}

uint64_t GpuResourceManager::GetTotalVRAM() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_vram_;
}

uint64_t GpuResourceManager::GetUsedVRAM() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return used_vram_;
}

uint64_t GpuResourceManager::GetFreeVRAM() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_vram_ - used_vram_;
}

bool GpuResourceManager::AcquireVRAM(const std::string& backend_name, uint64_t vram_needed,
                                      VRamPriority priority) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_ || !gpu_available_) {
        spdlog::error("GpuResourceManager: GPU not initialized");
        return false;
    }

    if (backend_vram_.find(backend_name) != backend_vram_.end()) {
        spdlog::warn("GpuResourceManager: backend '{}' already has VRAM allocated ({})",
                     backend_name, backend_vram_[backend_name]);
        return true;
    }

    uint64_t free_vram = total_vram_ - used_vram_ - exclusive_reserved_;
    if (vram_needed > free_vram) {
        spdlog::error("GpuResourceManager: insufficient VRAM for '{}': need {} bytes, {} free",
                      backend_name, vram_needed, free_vram);
        return false;
    }

    backend_vram_[backend_name] = vram_needed;
    backend_priorities_[backend_name] = priority;
    used_vram_ += vram_needed;
    active_backends_++;
    spdlog::info("GpuResourceManager: allocated {} bytes to '{}' [priority: {}]",
                 vram_needed, backend_name, static_cast<int>(priority));
    return true;
}

void GpuResourceManager::ReleaseVRAM(const std::string& backend_name, uint64_t vram_allocated) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = backend_vram_.find(backend_name);
    if (it != backend_vram_.end()) {
        used_vram_ -= it->second;
        backend_vram_.erase(it);
        backend_priorities_.erase(backend_name);
        active_backends_--;
        spdlog::info("GpuResourceManager: released {} bytes from '{}'",
                     vram_allocated, backend_name);
    }
}

bool GpuResourceManager::TryAcquireVRAM(const std::string& backend_name, uint64_t vram_needed,
                                         VRamPriority priority, int timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < timeout_ms) {
        if (AcquireVRAM(backend_name, vram_needed, priority)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    spdlog::error("GpuResourceManager: timeout waiting for VRAM for '{}' ({} ms)",
                  backend_name, timeout_ms);
    return false;
}

bool GpuResourceManager::LockExclusiveVRAM(const std::string& backend_name,
                                            uint64_t vram_amount, VRamPriority priority) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (exclusive_reserved_ > 0) {
        spdlog::error("GpuResourceManager: exclusive VRAM already locked by another backend");
        return false;
    }

    uint64_t free_vram = total_vram_ - used_vram_;
    if (vram_amount > free_vram) {
        spdlog::error("GpuResourceManager: not enough free VRAM for exclusive lock: need {} bytes",
                      vram_amount);
        return false;
    }

    // Acquire the VRAM normally first
    backend_vram_[backend_name] = vram_amount;
    backend_priorities_[backend_name] = priority;
    exclusive_reserved_ = vram_amount;
    used_vram_ += vram_amount;
    active_backends_++;

    exclusive_locks_[backend_name] = true;
    spdlog::info("GpuResourceManager: exclusive lock on {} bytes for '{}'",
                 vram_amount, backend_name);
    return true;
}

void GpuResourceManager::UnlockExclusiveVRAM(const std::string& backend_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = exclusive_locks_.find(backend_name);
    if (it != exclusive_locks_.end()) {
        exclusive_reserved_ = 0;
        exclusive_locks_.erase(it);
        spdlog::info("GpuResourceManager: released exclusive lock from '{}'", backend_name);
    }
}

bool GpuResourceManager::HasExclusiveLock(const std::string& backend_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = exclusive_locks_.find(backend_name);
    return it != exclusive_locks_.end() && it->second;
}

VRAMSnapshot GpuResourceManager::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    VRAMSnapshot snapshot{};
    snapshot.total_vram_bytes = total_vram_;
    snapshot.used_vram_bytes = used_vram_;
    snapshot.free_vram_bytes = total_vram_ - used_vram_;
    snapshot.backend_vram_bytes = used_vram_;
    snapshot.utilization_percent = total_vram_ > 0
                                       ? (static_cast<double>(used_vram_) / total_vram_) * 100.0
                                       : 0.0;
    snapshot.backend_count = static_cast<int>(backend_vram_.size());
    return snapshot;
}

double GpuResourceManager::GetVRAMUsagePercent() const {
    auto snap = GetSnapshot();
    return snap.utilization_percent;
}

uint64_t GpuResourceManager::GetBackendVRAM(const std::string& backend_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backend_vram_.find(backend_name);
    if (it != backend_vram_.end()) {
        return it->second;
    }
    return 0;
}

int GpuResourceManager::GetActiveBackendCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_backends_;
}

nlohmann::json GpuResourceManager::GetDeviceInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);

    nlohmann::json info;
    info["device_id"] = device_id_;
    info["device_name"] = device_name_;
    info["total_vram"] = total_vram_;
    info["used_vram"] = used_vram_;
    info["free_vram"] = total_vram_ - used_vram_;
    info["utilization_percent"] = total_vram_ > 0
                                      ? (static_cast<double>(used_vram_) / total_vram_) * 100.0
                                      : 0.0;
    info["gpu_available"] = gpu_available_;
    info["active_backends"] = active_backends_.load();

    nlohmann::json backends_json = nlohmann::json::array();
    for (const auto& [name, vram] : backend_vram_) {
        nlohmann::json b;
        b["name"] = name;
        b["vram_bytes"] = vram;
        b["priority"] = PriorityToString(backend_priorities_.count(name) ?
                                         backend_priorities_.at(name) : VRamPriority::TEXT_GENERATION);
        backends_json.push_back(b);
    }
    info["backends"] = backends_json;

    return info;
}

std::string GpuResourceManager::PriorityToString(VRamPriority priority) const {
    switch (priority) {
        case VRamPriority::SYSTEM_HEALTH: return "system_health";
        case VRamPriority::IMAGE_GENERATION: return "image_generation";
        case VRamPriority::FINE_TUNING: return "fine_tuning";
        case VRamPriority::TEXT_TO_SPEECH: return "text_to_speech";
        case VRamPriority::SPEECH_TO_TEXT: return "speech_to_text";
        case VRamPriority::EMBEDDING: return "embedding";
        case VRamPriority::DOCUMENT_QUERY: return "document_query";
        case VRamPriority::DOCUMENT_INDEX: return "document_index";
        case VRamPriority::TEXT_GENERATION: return "text_generation";
        default: return "unknown";
    }
}

} // namespace inferdeck::backends
