/// @file test_vulkan_device.cpp
/// @brief Unit tests for the VulkanDevice module (mocked).

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "llama_cpp/VulkanDevice.hpp"

// Mock VulkanDevice for testing without Vulkan runtime
TEST_CASE("VulkanDeviceInfo has correct defaults", "[vulkan][device]") {
    inferdeck::llama::VulkanDeviceInfo info;
    REQUIRE(info.name.empty());
    REQUIRE(info.device_id == -1);
    REQUIRE(info.vram_total == 0);
    REQUIRE(info.vram_free == 0);
    REQUIRE(info.compute_capability == 0);
}

TEST_CASE("VulkanDeviceInfo assignment works", "[vulkan][device]") {
    inferdeck::llama::VulkanDeviceInfo info;
    info.name = "AMD Radeon RX 6800";
    info.device_id = 0;
    info.vram_total = 16106127360ULL;  // 16 GB
    info.vram_free = 8589934592ULL;     // 8 GB
    info.compute_capability = 100;       // RDNA3

    REQUIRE(info.name == "AMD Radeon RX 6800");
    REQUIRE(info.device_id == 0);
    REQUIRE(info.vram_total == 16106127360ULL);
    REQUIRE(info.vram_free == 8589934592ULL);
    REQUIRE(info.compute_capability == 100);
}

TEST_CASE("GetVRAMUsagePercent calculates correctly", "[vulkan][device]") {
    inferdeck::llama::VulkanDeviceInfo info;
    info.vram_total = 10000000000ULL;
    info.vram_free = 7000000000ULL;

    // Should be 30% usage
    REQUIRE(info.GetVRAMUsagePercent() == 30.0f);

    info.vram_free = 0;
    REQUIRE(info.GetVRAMUsagePercent() == 100.0f);

    info.vram_free = 10000000000ULL;
    REQUIRE(info.GetVRAMUsagePercent() == 0.0f);
}

TEST_CASE("VulkanDeviceCapabilities defaults", "[vulkan][capabilities]") {
    inferdeck::llama::VulkanDeviceCapabilities caps;
    REQUIRE(!caps.supports_fp16);
    REQUIRE(!caps.supports_dp4a);
    REQUIRE(caps.max_work_group_size == 0);
    REQUIRE(caps.max_image_dim_1d == 0);
}
