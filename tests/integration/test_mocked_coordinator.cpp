#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "model/backend_coordinator.hpp"
#include "model/imodel.hpp"
#include "model/model_registry.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "fixture_loader.hpp"

using inferdeck::model::AcquireSlotOptions;
using inferdeck::model::BackendCoordinator;
using inferdeck::model::IModel;
using inferdeck::model::InferenceRequest;
using inferdeck::model::InferenceResult;
using inferdeck::model::ModelInfo;
using inferdeck::model::ModelRegistry;
using inferdeck::foundation::ErrorCode;
using nlohmann::json;

namespace {

struct CallRecord {
    std::string method;
    std::string detail;
};

class IModelMock : public IModel {
public:
    ModelInfo model_info{};
    std::atomic<bool> load_should_fail{false};
    std::atomic<bool> unload_should_fail{false};
    std::atomic<int> load_delay_ms{0};
    std::atomic<int> unload_delay_ms{0};
    std::atomic<bool> loaded{false};
    std::atomic<int> vram_mb{4096};
    std::atomic<int> max_slots{2};
    std::vector<int> busy_slots;
    std::vector<std::pair<int, InferenceRequest>> predictions;
    std::vector<CallRecord> calls;
    std::string text_to_return{"hello world"};
    std::vector<json> scripted_responses{};
    std::mutex calls_mtx;

    explicit IModelMock(ModelInfo info) : model_info(std::move(info)) {
        busy_slots.assign(max_slots.load(), 0);
    }

    void record(const std::string& method, const std::string& detail = {}) {
        std::lock_guard<std::mutex> lock(calls_mtx);
        calls.push_back({method, detail});
    }

    const ModelInfo& info() const override { return model_info; }

    inferdeck::foundation::Result<void> load() override {
        record("load");
        if (load_should_fail.load()) {
            return inferdeck::foundation::Err<void>(ErrorCode::Internal, "mock load failure");
        }
        int delay = load_delay_ms.load();
        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        loaded.store(true);
        return inferdeck::foundation::Ok();
    }

    inferdeck::foundation::Result<void> unload() override {
        record("unload");
        if (unload_should_fail.load()) {
            return inferdeck::foundation::Err<void>(ErrorCode::Internal, "mock unload failure");
        }
        int delay = unload_delay_ms.load();
        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        loaded.store(false);
        std::lock_guard<std::mutex> lock(calls_mtx);
        std::fill(busy_slots.begin(), busy_slots.end(), 0);
        return inferdeck::foundation::Ok();
    }

    bool is_loaded() const override { return loaded.load(); }
    int vram_usage_mb() const override { return vram_mb.load(); }
    int n_slots() const override { return max_slots.load(); }

    int n_free_slots() const override {
        int busy = 0;
        for (int b : busy_slots) if (b) ++busy;
        return max_slots.load() - busy;
    }

    inferdeck::foundation::Result<int> acquire_slot() override {
        record("acquire_slot");
        for (int i = 0; i < static_cast<int>(busy_slots.size()); ++i) {
            if (busy_slots[i] == 0) {
                busy_slots[i] = 1;
                return inferdeck::foundation::Ok(i);
            }
        }
        return inferdeck::foundation::Err<int>(ErrorCode::Unavailable, "no free slots");
    }

    inferdeck::foundation::Result<void> release_slot(int slot_id) override {
        record("release_slot:" + std::to_string(slot_id));
        if (slot_id < 0 || slot_id >= static_cast<int>(busy_slots.size())) {
            return inferdeck::foundation::Err<void>(ErrorCode::InvalidArgument, "invalid slot_id");
        }
        busy_slots[slot_id] = 0;
        return inferdeck::foundation::Ok();
    }

    bool slot_busy(int slot_id) const override {
        if (slot_id < 0 || slot_id >= static_cast<int>(busy_slots.size())) return false;
        return busy_slots[slot_id] != 0;
    }

    inferdeck::foundation::Result<InferenceResult> predict(
        int slot_id, const InferenceRequest& req) override {
        record("predict:" + std::to_string(slot_id));
        std::lock_guard<std::mutex> lock(calls_mtx);
        predictions.emplace_back(slot_id, req);
        InferenceResult r;
        r.text = text_to_return;
        r.prompt_tokens = 10;
        r.completion_tokens = 5;
        r.duration_ms = 100.0f;
        r.tokens_per_second = 50.0f;
        return inferdeck::foundation::Ok(r);
    }
};

ModelInfo make_info(const std::string& name, const std::string& family = "qwen3.6",
                    int n_slots = 2, int vram = 18000, bool vision = false) {
    ModelInfo i;
    i.name = name;
    i.family = family;
    i.gguf_path = "C:/models/" + name + ".gguf";
    i.n_slots = n_slots;
    i.vram_required_mb = vram;
    i.context_size = 65536;
    i.has_vision = vision;
    return i;
}

ModelRegistry build_registry() {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("qwen3.6-27b", "qwen3.6", 2, 22000, true));
    reg.register_model(make_info("qwen3-coder-next", "qwen3-coder", 2, 24000, false));
    return reg;
}
static_assert(true);

std::unique_ptr<ModelRegistry> build_registry_ptr() {
    return std::make_unique<ModelRegistry>(build_registry());
}

} // namespace

TEST_CASE("Mocked Tier B: load model exercises IModel::load", "[integration][mocked]") {
    auto reg_ptr = build_registry_ptr();
    BackendCoordinator c(*reg_ptr);
    REQUIRE(c.load("qwen3.6-27b").has_value());
    REQUIRE(c.is_loaded("qwen3.6-27b"));
    REQUIRE(c.get_loaded_model().value() == "qwen3.6-27b");
    auto* m = dynamic_cast<IModelMock*>(const_cast<IModel*>(c.get_model("qwen3.6-27b")));
    REQUIRE(m != nullptr);
    bool saw_load = false;
    for (const auto& rec : m->calls) {
        if (rec.method == "load") saw_load = true;
    }
    REQUIRE(saw_load);
}

TEST_CASE("Mocked Tier B: load then unload clears state", "[integration][mocked]") {
    auto reg_ptr = build_registry_ptr();
    BackendCoordinator c(*reg_ptr);
    REQUIRE(c.load("qwen3.6-27b").has_value());
    REQUIRE(c.unload_current().has_value());
    REQUIRE_FALSE(c.get_loaded_model().has_value());
    REQUIRE_FALSE(c.is_loaded("qwen3.6-27b"));
    REQUIRE(c.get_vram_usage() == 0);
}

TEST_CASE("Mocked Tier B: 503 path when no model loaded for requested name", "[integration][mocked]") {
    auto reg_ptr = build_registry_ptr();
    BackendCoordinator c(*reg_ptr);
    auto slot = c.acquire_slot("qwen3.6-27b");
    REQUIRE_FALSE(slot.has_value());
    REQUIRE(slot.error().code == ErrorCode::NotFound);
}

TEST_CASE("Mocked Tier B: swap 27B -> Coder-Next -> 27B end-to-end", "[integration][mocked]") {
    auto reg_ptr = build_registry_ptr();
    BackendCoordinator c(*reg_ptr);

    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(c.load("qwen3.6-27b").has_value());
    REQUIRE(c.swap_to("qwen3-coder-next").has_value());
    REQUIRE(c.swap_to("qwen3.6-27b").has_value());
    auto t1 = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    REQUIRE(elapsed_ms < 5000);

    REQUIRE(c.get_loaded_model().value() == "qwen3.6-27b");
    REQUIRE(c.is_loaded("qwen3.6-27b"));
    REQUIRE_FALSE(c.is_loaded("qwen3-coder-next"));
}

TEST_CASE("Mocked Tier B: acquire 2 concurrent slots, third times out", "[integration][mocked]") {
    auto reg_ptr = build_registry_ptr();
    BackendCoordinator c(*reg_ptr);
    REQUIRE(c.load("qwen3.6-27b").has_value());
    auto s1 = c.acquire_slot("qwen3.6-27b");
    auto s2 = c.acquire_slot("qwen3.6-27b");
    REQUIRE(s1.has_value());
    REQUIRE(s2.has_value());

    AcquireSlotOptions opts;
    opts.timeout = std::chrono::milliseconds{200};
    auto s3 = c.acquire_slot("qwen3.6-27b", opts);
    REQUIRE_FALSE(s3.has_value());
    REQUIRE(s3.error().code == ErrorCode::Timeout);

    REQUIRE(c.release_slot("qwen3.6-27b", s1.value()).has_value());
    REQUIRE(c.release_slot("qwen3.6-27b", s2.value()).has_value());
    REQUIRE(c.active_request_count() == 0);
}

TEST_CASE("Mocked Tier B: predict routes to correct model and returns text", "[integration][mocked]") {
    auto reg_ptr = build_registry_ptr();
    BackendCoordinator c(*reg_ptr);
    REQUIRE(c.load("qwen3.6-27b").has_value());
    auto s = c.acquire_slot("qwen3.6-27b");
    REQUIRE(s.has_value());

    InferenceRequest req;
    req.prompt = "What is 2+2?";
    req.max_tokens = 16;
    req.temperature = 0.0f;
    auto r = c.predict("qwen3.6-27b", s.value(), req);
    REQUIRE(r.has_value());
    REQUIRE(r.value().text == "hello world");
    REQUIRE(r.value().completion_tokens == 5);
    REQUIRE(r.value().tokens_per_second > 0.0f);

    auto* m = dynamic_cast<IModelMock*>(const_cast<IModel*>(c.get_model("qwen3.6-27b")));
    REQUIRE(m != nullptr);
    REQUIRE(m->predictions.size() == 1);
    REQUIRE(m->predictions[0].first == s.value());
    REQUIRE(m->predictions[0].second.prompt == "What is 2+2?");

    REQUIRE(c.release_slot("qwen3.6-27b", s.value()).has_value());
}

TEST_CASE("Mocked Tier B: load failure surfaces error and clears state", "[integration][mocked]") {
    auto reg_ptr = std::make_unique<ModelRegistry>();
    reg_ptr->set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        auto m = std::make_unique<IModelMock>(i);
        if (i.name == "qwen3.6-27b") m->load_should_fail.store(true);
        return m;
    });
    reg_ptr->register_model(make_info("qwen3.6-27b"));
    BackendCoordinator c(*reg_ptr);
    auto r = c.load("qwen3.6-27b");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::Internal);
    REQUIRE_FALSE(c.get_loaded_model().has_value());
}

TEST_CASE("Mocked Tier B: vram math updates across load/unload", "[integration][mocked]") {
    auto reg_ptr = build_registry_ptr();
    BackendCoordinator c(*reg_ptr);
    REQUIRE(c.get_vram_usage() == 0);
    REQUIRE(c.load("qwen3.6-27b").has_value());
    REQUIRE(c.get_vram_usage() == 4096);
    REQUIRE(c.swap_to("qwen3-coder-next").has_value());
    REQUIRE(c.get_vram_usage() == 4096);
    REQUIRE(c.unload_current().has_value());
    REQUIRE(c.get_vram_usage() == 0);
}

TEST_CASE("Mocked Tier B: opencode coding fixture flows through coordinator", "[integration][mocked]") {
    auto raw = test_helpers::load_fixture_text("opencode_coding_tool_call.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());

    auto reg_ptr = build_registry_ptr();
    BackendCoordinator c(*reg_ptr);
    REQUIRE(c.load("qwen3.6-27b").has_value());

    auto slot = c.acquire_slot("qwen3.6-27b");
    REQUIRE(slot.has_value());

    InferenceRequest req;
    req.prompt = j.value("model", std::string{"qwen3.6-27b"});
    if (j.contains("max_tokens")) req.max_tokens = j["max_tokens"].get<int>();
    if (j.contains("temperature")) req.temperature = j["temperature"].get<float>();
    if (j.contains("top_p")) req.top_p = j["top_p"].get<float>();

    auto r = c.predict("qwen3.6-27b", slot.value(), req);
    REQUIRE(r.has_value());
    REQUIRE(r.value().completion_tokens > 0);

    REQUIRE(c.release_slot("qwen3.6-27b", slot.value()).has_value());
    REQUIRE(c.active_request_count() == 0);
}

TEST_CASE("Mocked Tier B: swap latency is bounded with delayed load/unload", "[integration][mocked]") {
    auto reg_ptr = std::make_unique<ModelRegistry>();
    reg_ptr->set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        auto m = std::make_unique<IModelMock>(i);
        m->load_delay_ms.store(50);
        m->unload_delay_ms.store(50);
        return m;
    });
    reg_ptr->register_model(make_info("qwen3.6-27b"));
    reg_ptr->register_model(make_info("qwen3-coder-next"));

    BackendCoordinator c(*reg_ptr);
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(c.load("qwen3.6-27b").has_value());
    REQUIRE(c.swap_to("qwen3-coder-next").has_value());
    REQUIRE(c.swap_to("qwen3.6-27b").has_value());
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    REQUIRE(elapsed_ms < 30000);
}

TEST_CASE("Mocked Tier B: parallel acquire/release does not deadlock", "[integration][mocked]") {
    auto reg_ptr = build_registry_ptr();
    BackendCoordinator c(*reg_ptr);
    REQUIRE(c.load("qwen3.6-27b").has_value());

    std::vector<std::thread> threads;
    std::atomic<int> success{0};
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&c, &success]() {
            auto s = c.acquire_slot("qwen3.6-27b");
            if (s.has_value()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                c.release_slot("qwen3.6-27b", s.value());
                ++success;
            }
        });
    }
    for (auto& t : threads) t.join();
    REQUIRE(success.load() == 8);
    REQUIRE(c.active_request_count() == 0);
}
