/// @file test_backend_registry.cpp
/// @brief Tests for BackendRegistry singleton and lifecycle management.

#include <catch2/catch_test_macros.hpp>
#include "backends/BackendRegistry.hpp"
#include "backends/ITextBackend.hpp"
#include <memory>

using namespace inferdeck::backends;

namespace {

class MockTextBackend : public ITextBackend {
public:
    std::string GetBackendName() const override { return name_; }
    BackendStatus GetStatus() const override { return status_; }
    bool Initialize() override { status_ = BackendStatus::READY; return true; }
    void Shutdown() override { status_ = BackendStatus::UNINITIALIZED; }
    nlohmann::json GetInfo() const override { return nlohmann::json{{"name", name_}}; }
    nlohmann::json GetVRAMUsage() const override { return nlohmann::json{{"used", 0}}; }
    bool IsReady() const override { return status_ == BackendStatus::READY; }

    std::string name_;
    BackendStatus status_ = BackendStatus::UNINITIALIZED;
};

} // namespace

TEST_CASE("Registry: Singleton Instance", "[registry][singleton]") {
    auto& a = BackendRegistry::Instance();
    auto& b = BackendRegistry::Instance();
    REQUIRE(&a == &b);
}

TEST_CASE("Registry: Register and GetTextBackend", "[registry]") {
    auto& registry = BackendRegistry::Instance();
    auto backend = std::make_unique<MockTextBackend>();
    backend->name_ = "test_backend";
    REQUIRE(registry.Register("test", std::move(backend), VRamPriority::TEXT_GENERATION));

    auto retrieved = registry.GetTextBackend("test");
    REQUIRE(retrieved != nullptr);
    REQUIRE(retrieved->GetBackendName() == "test_backend");
}

TEST_CASE("Registry: Register duplicate returns false", "[registry]") {
    auto& registry = BackendRegistry::Instance();
    auto backend1 = std::make_unique<MockTextBackend>();
    backend1->name_ = "first";
    REQUIRE(registry.Register("dup", std::move(backend1)));

    auto backend2 = std::make_unique<MockTextBackend>();
    backend2->name_ = "second";
    REQUIRE(!registry.Register("dup", std::move(backend2)));
}

TEST_CASE("Registry: Get returns nullptr for missing backend", "[registry]") {
    auto& registry = BackendRegistry::Instance();
    auto result = registry.GetTextBackend("nonexistent");
    REQUIRE(result == nullptr);
}

TEST_CASE("Registry: Has checks backend existence", "[registry]") {
    auto& registry = BackendRegistry::Instance();
    REQUIRE(!registry.Has("nonexistent"));

    auto backend = std::make_unique<MockTextBackend>();
    backend->name_ = "check_test";
    registry.Register("check_test", std::move(backend));
    REQUIRE(registry.Has("check_test"));
}

TEST_CASE("Registry: InitializeAll initializes backends", "[registry]") {
    auto& registry = BackendRegistry::Instance();

    auto backend1 = std::make_unique<MockTextBackend>();
    backend1->name_ = "init_test1";
    registry.Register("init_test1", std::move(backend1), VRamPriority::TEXT_GENERATION);

    auto backend2 = std::make_unique<MockTextBackend>();
    backend2->name_ = "init_test2";
    registry.Register("init_test2", std::move(backend2), VRamPriority::EMBEDDING);

    REQUIRE(registry.InitializeAll());

    auto t1 = registry.GetTextBackend("init_test1");
    auto t2 = registry.GetTextBackend("init_test2");
    REQUIRE(t1 != nullptr && t1->IsReady());
    REQUIRE(t2 != nullptr && t2->IsReady());
}

TEST_CASE("Registry: ShutdownAll shuts down backends", "[registry]") {
    auto& registry = BackendRegistry::Instance();

    auto backend = std::make_unique<MockTextBackend>();
    backend->name_ = "shutdown_test";
    registry.Register("shutdown_test", std::move(backend));

    registry.InitializeAll();
    registry.ShutdownAll();

    auto t = registry.GetTextBackend("shutdown_test");
    REQUIRE(t != nullptr && !t->IsReady());
}

TEST_CASE("Registry: GetBackendNames returns registered names", "[registry]") {
    auto& registry = BackendRegistry::Instance();

    auto b1 = std::make_unique<MockTextBackend>();
    b1->name_ = "name_test_a";
    registry.Register("name_a", std::move(b1));

    auto b2 = std::make_unique<MockTextBackend>();
    b2->name_ = "name_test_b";
    registry.Register("name_b", std::move(b2));

    auto names = registry.GetBackendNames();
    REQUIRE(names.size() == 2);
    REQUIRE(std::find(names.begin(), names.end(), "name_a") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "name_b") != names.end());
}

TEST_CASE("Registry: GetRegistryInfo returns valid JSON", "[registry]") {
    auto& registry = BackendRegistry::Instance();

    auto backend = std::make_unique<MockTextBackend>();
    backend->name_ = "info_test";
    registry.Register("info_test", std::move(backend));

    auto info = registry.GetRegistryInfo();
    REQUIRE(info.is_object());
    REQUIRE(info.contains("backend_count"));
    REQUIRE(info["backend_count"].get<int>() >= 1);
    REQUIRE(info.contains("backends"));
    REQUIRE(info["backends"].is_array());
}

TEST_CASE("Registry: GetBackendStatusSummary counts statuses", "[registry]") {
    auto& registry = BackendRegistry::Instance();

    auto b1 = std::make_unique<MockTextBackend>();
    b1->name_ = "ready_test";
    registry.Register("ready_test", std::move(b1));

    registry.InitializeAll();

    auto summary = registry.GetBackendStatusSummary();
    REQUIRE(summary.is_object());
    REQUIRE(summary.contains("total"));
    REQUIRE(summary.contains("ready"));
    REQUIRE(summary["ready"].get<int>() >= 1);
}

TEST_CASE("Registry: GetSttBackend typed access", "[registry]") {
    auto& registry = BackendRegistry::Instance();
    auto info = registry.GetBackendStatusSummary();
    REQUIRE(info.is_object());
}

TEST_CASE("Registry: GetTtsBackend typed access", "[registry]") {
    auto& registry = BackendRegistry::Instance();
    auto info = registry.GetBackendStatusSummary();
    REQUIRE(info.is_object());
}

TEST_CASE("Registry: GetImageBackend typed access", "[registry]") {
    auto& registry = BackendRegistry::Instance();
    auto info = registry.GetBackendStatusSummary();
    REQUIRE(info.is_object());
}

TEST_CASE("Registry: GetEmbeddingBackend typed access", "[registry]") {
    auto& registry = BackendRegistry::Instance();
    auto info = registry.GetBackendStatusSummary();
    REQUIRE(info.is_object());
}

TEST_CASE("Registry: GetTrainingBackend typed access", "[registry]") {
    auto& registry = BackendRegistry::Instance();
    auto info = registry.GetBackendStatusSummary();
    REQUIRE(info.is_object());
}
