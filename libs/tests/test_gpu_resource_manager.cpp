/// @file test_gpu_resource_manager.cpp
/// @brief Tests for GpuResourceManager VRAM arbitration.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include "backends/GpuResourceManager.hpp"
#include <thread>
#include <chrono>

using namespace inferdeck::backends;

TEST_CASE("VRAM: Singleton Instance", "[gpu][singleton]") {
    auto& a = GpuResourceManager::Instance();
    auto& b = GpuResourceManager::Instance();
    REQUIRE(&a == &b);
}

TEST_CASE("VRAM: Initialize sets GPU available", "[gpu][init]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());
    REQUIRE(mgr.IsAvailable());
}

TEST_CASE("VRAM: GetTotalVRAM returns 16GB simulated", "[gpu][vram]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());
    auto total = mgr.GetTotalVRAM();
    REQUIRE(total == 16ULL * 1024 * 1024 * 1024);
}

TEST_CASE("VRAM: GetUsedVRAM starts at zero", "[gpu][vram]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());
    REQUIRE(mgr.GetUsedVRAM() == 0);
}

TEST_CASE("VRAM: GetFreeVRAM equals total when idle", "[gpu][vram]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());
    REQUIRE(mgr.GetFreeVRAM() == 16ULL * 1024 * 1024 * 1024);
}

TEST_CASE("VRAM: AcquireVRAM allocates bytes", "[gpu][allocate]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());

    uint64_t needed = 4ULL * 1024 * 1024 * 1024; // 4GB
    REQUIRE(mgr.AcquireVRAM("test_backend", needed, VRamPriority::TEXT_GENERATION));

    REQUIRE(mgr.GetUsedVRAM() == needed);
    REQUIRE(mgr.GetFreeVRAM() == 12ULL * 1024 * 1024 * 1024);
    REQUIRE(mgr.GetBackendVRAM("test_backend") == needed);
}

TEST_CASE("VRAM: AcquireVRAM rejects when not initialized", "[gpu][allocate]") {
    GpuResourceManager temp;
    REQUIRE(!temp.AcquireVRAM("test", 1024, VRamPriority::TEXT_GENERATION));
}

TEST_CASE("VRAM: ReleaseVRAM frees bytes", "[gpu][allocate]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());

    uint64_t needed = 2ULL * 1024 * 1024 * 1024;
    mgr.AcquireVRAM("release_test", needed, VRamPriority::EMBEDDING);
    REQUIRE(mgr.GetUsedVRAM() == needed);

    mgr.ReleaseVRAM("release_test", needed);
    REQUIRE(mgr.GetUsedVRAM() == 0);
    REQUIRE(mgr.GetBackendVRAM("release_test") == 0);
}

TEST_CASE("VRAM: ReleaseVRAM for unknown backend is safe", "[gpu][allocate]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());
    REQUIRE_NOTHROW(mgr.ReleaseVRAM("nonexistent", 1024));
}

TEST_CASE("VRAM: TryAcquireVRAM retries on timeout", "[gpu][allocate]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());

    // Fill all VRAM
    uint64_t full = 16ULL * 1024 * 1024 * 1024;
    mgr.AcquireVRAM("filler", full, VRamPriority::SYSTEM_HEALTH);

    // Try to acquire more with short timeout
    bool result = mgr.TryAcquireVRAM("overflow", 1ULL << 30, VRamPriority::IMAGE_GENERATION, 100);
    REQUIRE(!result);

    mgr.ReleaseVRAM("filler", full);
}

TEST_CASE("VRAM: LockExclusiveVRAM reserves memory", "[gpu][exclusive]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());

    uint64_t needed = 8ULL * 1024 * 1024 * 1024;
    REQUIRE(mgr.LockExclusiveVRAM("training", needed, VRamPriority::FINE_TUNING));
    REQUIRE(mgr.HasExclusiveLock("training"));
}

TEST_CASE("VRAM: LockExclusiveVRAM fails when already locked", "[gpu][exclusive]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());

    uint64_t needed = 8ULL * 1024 * 1024 * 1024;
    mgr.LockExclusiveVRAM("first_lock", needed, VRamPriority::FINE_TUNING);

    bool result = mgr.LockExclusiveVRAM("second_lock", 2ULL << 30, VRamPriority::IMAGE_GENERATION);
    REQUIRE(!result);

    mgr.UnlockExclusiveVRAM("first_lock");
}

TEST_CASE("VRAM: UnlockExclusiveVRAM releases lock", "[gpu][exclusive]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());

    uint64_t needed = 8ULL * 1024 * 1024 * 1024;
    mgr.LockExclusiveVRAM("unlock_test", needed, VRamPriority::FINE_TUNING);
    REQUIRE(mgr.HasExclusiveLock("unlock_test"));

    mgr.UnlockExclusiveVRAM("unlock_test");
    REQUIRE(!mgr.HasExclusiveLock("unlock_test"));
}

TEST_CASE("VRAM: UnlockExclusiveVRAM for unknown backend is safe", "[gpu][exclusive]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());
    REQUIRE_NOTHROW(mgr.UnlockExclusiveVRAM("nonexistent"));
}

TEST_CASE("VRAM: GetSnapshot returns valid data", "[gpu][snapshot]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());

    auto snap = mgr.GetSnapshot();
    REQUIRE(snap.total_vram_bytes == 16ULL * 1024 * 1024 * 1024);
    REQUIRE(snap.free_vram_bytes == snap.total_vram_bytes);
    REQUIRE(snap.utilization_percent == 0.0);
    REQUIRE(snap.backend_count == 0);
}

TEST_CASE("VRAM: GetSnapshot after allocation", "[gpu][snapshot]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());

    uint64_t needed = 4ULL * 1024 * 1024 * 1024;
    mgr.AcquireVRAM("snapshot_test", needed, VRamPriority::TEXT_GENERATION);

    auto snap = mgr.GetSnapshot();
    REQUIRE(snap.used_vram_bytes == needed);
    REQUIRE(snap.free_vram_bytes == 12ULL * 1024 * 1024 * 1024);
    REQUIRE(snap.backend_count == 1);
    REQUIRE(snap.utilization_percent > 0.0);

    mgr.ReleaseVRAM("snapshot_test", needed);
}

TEST_CASE("VRAM: GetVRAMUsagePercent returns correct value", "[gpu][snapshot]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());

    double usage = mgr.GetVRAMUsagePercent();
    REQUIRE(usage == 0.0);

    uint64_t needed = 8ULL * 1024 * 1024 * 1024;
    mgr.AcquireVRAM("percent_test", needed, VRamPriority::TEXT_GENERATION);

    usage = mgr.GetVRAMUsagePercent();
    REQUIRE(usage == 50.0);

    mgr.ReleaseVRAM("percent_test", needed);
}

TEST_CASE("VRAM: GetActiveBackendCount tracks backends", "[gpu][vram]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());
    REQUIRE(mgr.GetActiveBackendCount() == 0);

    mgr.AcquireVRAM("count_test", 1ULL << 30, VRamPriority::EMBEDDING);
    REQUIRE(mgr.GetActiveBackendCount() == 1);

    mgr.ReleaseVRAM("count_test", 1ULL << 30);
    REQUIRE(mgr.GetActiveBackendCount() == 0);
}

TEST_CASE("VRAM: GetDeviceInfo returns valid JSON", "[gpu][info]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());

    auto info = mgr.GetDeviceInfo();
    REQUIRE(info.is_object());
    REQUIRE(info.contains("device_id"));
    REQUIRE(info.contains("device_name"));
    REQUIRE(info.contains("total_vram"));
    REQUIRE(info.contains("gpu_available"));
    REQUIRE(info["gpu_available"].get<bool>() == true);
}

TEST_CASE("VRAM: AcquireVRAM rejects duplicate allocation", "[gpu][allocate]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());

    uint64_t needed = 4ULL * 1024 * 1024 * 1024;
    REQUIRE(mgr.AcquireVRAM("dup_test", needed, VRamPriority::TEXT_GENERATION));
    // Second acquire for same backend should return true (already allocated)
    REQUIRE(mgr.AcquireVRAM("dup_test", needed, VRamPriority::TEXT_GENERATION));

    mgr.ReleaseVRAM("dup_test", needed);
}

TEST_CASE("VRAM: VRAMPriority enum values", "[gpu][enum]") {
    REQUIRE(static_cast<int>(VRamPriority::SYSTEM_HEALTH) == 10);
    REQUIRE(static_cast<int>(VRamPriority::TEXT_GENERATION) == 8);
    REQUIRE(static_cast<int>(VRamPriority::DOCUMENT_QUERY) == 8);
    REQUIRE(static_cast<int>(VRamPriority::SPEECH_TO_TEXT) == 7);
    REQUIRE(static_cast<int>(VRamPriority::TEXT_TO_SPEECH) == 7);
    REQUIRE(static_cast<int>(VRamPriority::EMBEDDING) == 6);
    REQUIRE(static_cast<int>(VRamPriority::DOCUMENT_INDEX) == 5);
    REQUIRE(static_cast<int>(VRamPriority::FINE_TUNING) == 3);
    REQUIRE(static_cast<int>(VRamPriority::IMAGE_GENERATION) == 1);
}

TEST_CASE("VRAM: PriorityToString conversion", "[gpu][string]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.PriorityToString(VRamPriority::SYSTEM_HEALTH) == "system_health");
    REQUIRE(mgr.PriorityToString(VRamPriority::IMAGE_GENERATION) == "image_generation");
    REQUIRE(mgr.PriorityToString(VRamPriority::FINE_TUNING) == "fine_tuning");
    REQUIRE(mgr.PriorityToString(VRamPriority::TEXT_TO_SPEECH) == "text_to_speech");
    REQUIRE(mgr.PriorityToString(VRamPriority::SPEECH_TO_TEXT) == "speech_to_text");
    REQUIRE(mgr.PriorityToString(VRamPriority::EMBEDDING) == "embedding");
    REQUIRE(mgr.PriorityToString(VRamPriority::DOCUMENT_QUERY) == "document_query");
    REQUIRE(mgr.PriorityToString(VRamPriority::DOCUMENT_INDEX) == "document_index");
    REQUIRE(mgr.PriorityToString(VRamPriority::TEXT_GENERATION) == "text_generation");
}

TEST_CASE("VRAM: VRAMSnapshot struct fields", "[gpu][struct]") {
    VRAMSnapshot snap;
    snap.total_vram_bytes = 16ULL << 30;
    snap.used_vram_bytes = 8ULL << 30;
    snap.free_vram_bytes = 8ULL << 30;
    snap.backend_vram_bytes = 8ULL << 30;
    snap.utilization_percent = 50.0;
    snap.backend_count = 2;

    REQUIRE(snap.total_vram_bytes == (16ULL << 30));
    REQUIRE(snap.used_vram_bytes == (8ULL << 30));
    REQUIRE(snap.free_vram_bytes == (8ULL << 30));
    REQUIRE(snap.utilization_percent == 50.0);
    REQUIRE(snap.backend_count == 2);
}

TEST_CASE("VRAM: Multiple backends can coexist", "[gpu][concurrent]") {
    auto& mgr = GpuResourceManager::Instance();
    REQUIRE(mgr.Initialize());

    uint64_t needed = 2ULL * 1024 * 1024 * 1024;
    REQUIRE(mgr.AcquireVRAM("backend_a", needed, VRamPriority::TEXT_GENERATION));
    REQUIRE(mgr.AcquireVRAM("backend_b", needed, VRamPriority::EMBEDDING));
    REQUIRE(mgr.AcquireVRAM("backend_c", needed, VRamPriority::SPEECH_TO_TEXT));

    auto snap = mgr.GetSnapshot();
    REQUIRE(snap.backend_count == 3);
    REQUIRE(snap.used_vram_bytes == needed * 3);

    mgr.ReleaseVRAM("backend_a", needed);
    mgr.ReleaseVRAM("backend_b", needed);
    mgr.ReleaseVRAM("backend_c", needed);
}
