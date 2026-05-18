/// @file GpuResourceManager.hpp
/// @brief Single-GPU VRAM arbitration manager.
///
/// Prevents backend collisions by tracking VRAM usage and enforcing
/// priority-based allocation across all running backends.

#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <optional>

namespace inferdeck::backends {

enum class VRamPriority {
    SYSTEM_HEALTH = 10,
    IMAGE_GENERATION = 1,
    FINE_TUNING = 3,
    TEXT_TO_SPEECH = 7,
    SPEECH_TO_TEXT = 7,
    EMBEDDING = 6,
    DOCUMENT_QUERY = 8,
    DOCUMENT_INDEX = 5,
    TEXT_GENERATION = 8
};

struct VRAMSnapshot {
    uint64_t total_vram_bytes;
    uint64_t used_vram_bytes;
    uint64_t free_vram_bytes;
    uint64_t backend_vram_bytes;
    double utilization_percent;
    int backend_count;
};

class GpuResourceManager {
public:
    static GpuResourceManager& Instance();

    GpuResourceManager(const GpuResourceManager&) = delete;
    GpuResourceManager& operator=(const GpuResourceManager&) = delete;

    /// Initialize GPU detection.
    bool Initialize(int device_id = 0);

    /// Check if GPU was detected.
    bool IsAvailable() const;

    /// Get total VRAM in bytes.
    uint64_t GetTotalVRAM() const;

    /// Get used VRAM in bytes.
    uint64_t GetUsedVRAM() const;

    /// Get free VRAM in bytes.
    uint64_t GetFreeVRAM() const;

    /// Acquire VRAM for a backend.
    bool AcquireVRAM(const std::string& backend_name, uint64_t vram_needed,
                     VRamPriority priority);

    /// Release VRAM back to pool.
    void ReleaseVRAM(const std::string& backend_name, uint64_t vram_allocated);

    /// Try to acquire VRAM with timeout.
    bool TryAcquireVRAM(const std::string& backend_name, uint64_t vram_needed,
                        VRamPriority priority, int timeout_ms = 30000);

    /// Pre-allocate exclusive VRAM for a backend.
    bool LockExclusiveVRAM(const std::string& backend_name, uint64_t vram_amount,
                           VRamPriority priority);

    /// Unlock exclusive VRAM.
    void UnlockExclusiveVRAM(const std::string& backend_name);

    /// Check if a backend has exclusive lock.
    bool HasExclusiveLock(const std::string& backend_name) const;

    /// Get current VRAM snapshot.
    VRAMSnapshot GetSnapshot() const;

    /// Get VRAM usage percentage.
    double GetVRAMUsagePercent() const;

    /// Get allocated VRAM per backend.
    uint64_t GetBackendVRAM(const std::string& backend_name) const;

    /// Get count of active backends.
    int GetActiveBackendCount() const;

    /// Get GPU device info.
    nlohmann::json GetDeviceInfo() const;

private:
    GpuResourceManager();
    ~GpuResourceManager() = default;

    bool DetectGPU(int device_id);
    std::string PriorityToString(VRamPriority priority) const;

    mutable std::mutex mutex_;
    bool initialized_{false};
    bool gpu_available_{false};
    int device_id_{0};
    std::string device_name_;
    uint64_t total_vram_{0};
    uint64_t used_vram_{0};
    uint64_t exclusive_reserved_{0};
    std::unordered_map<std::string, uint64_t> backend_vram_;
    std::unordered_map<std::string, VRamPriority> backend_priorities_;
    std::unordered_map<std::string, bool> exclusive_locks_;
    std::atomic<int> active_backends_{0};
};

} // namespace inferdeck::backends
