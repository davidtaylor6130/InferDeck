#include <catch2/catch_test_macros.hpp>

#include "model/backend_coordinator.hpp"
#include "model/imodel.hpp"
#include "model/model_registry.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace inferdeck::model;
using inferdeck::foundation::ErrorCode;
using inferdeck::foundation::Ok;
using inferdeck::foundation::Err;
using inferdeck::foundation::Result;

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
    std::atomic<bool> loaded{false};
    std::atomic<int> vram_mb{4096};
    std::atomic<int> max_slots{2};
    std::vector<int> busy_slots;
    std::vector<std::pair<int, InferenceRequest>> predictions;
    std::atomic<int> next_slot_to_assign{0};
    std::vector<CallRecord> calls;
    std::string text_to_return{"hello world"};
    std::mutex calls_mtx;
    ChatTemplateMeta chat_meta_{};

    explicit IModelMock(ModelInfo info) : model_info(std::move(info)) {
        busy_slots.assign(max_slots.load(), 0);
    }

    void record(const std::string& method, const std::string& detail = {}) {
        std::lock_guard<std::mutex> lock(calls_mtx);
        calls.push_back({method, detail});
    }

    const ModelInfo& info() const override { return model_info; }
    const ChatTemplateMeta& chat_template_meta() const override { return chat_meta_; }

    Result<void> load() override {
        record("load");
        if (load_should_fail.load()) {
            return Err<void>(ErrorCode::Internal, "mock load failure");
        }
        int delay = load_delay_ms.load();
        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        loaded.store(true);
        return Ok();
    }

    Result<void> unload() override {
        record("unload");
        if (unload_should_fail.load()) {
            return Err<void>(ErrorCode::Internal, "mock unload failure");
        }
        loaded.store(false);
        busy_slots.assign(max_slots.load(), 0);
        return Ok();
    }

    bool is_loaded() const override { return loaded.load(); }
    int vram_usage_mb() const override { return vram_mb.load(); }
    int n_slots() const override { return max_slots.load(); }

    int n_free_slots() const override {
        int busy = 0;
        for (int b : busy_slots) if (b) ++busy;
        return max_slots.load() - busy;
    }

    Result<int> acquire_slot() override {
        record("acquire_slot");
        for (int i = 0; i < static_cast<int>(busy_slots.size()); ++i) {
            if (busy_slots[i] == 0) {
                busy_slots[i] = 1;
                return Ok(i);
            }
        }
        return Err<int>(ErrorCode::Unavailable, "no free slots");
    }

    Result<void> release_slot(int slot_id) override {
        record("release_slot:" + std::to_string(slot_id));
        if (slot_id < 0 || slot_id >= static_cast<int>(busy_slots.size())) {
            return Err<void>(ErrorCode::InvalidArgument, "invalid slot_id");
        }
        busy_slots[slot_id] = 0;
        return Ok();
    }

    bool slot_busy(int slot_id) const override {
        if (slot_id < 0 || slot_id >= static_cast<int>(busy_slots.size())) return false;
        return busy_slots[slot_id] != 0;
    }

    Result<InferenceResult> predict(int slot_id, const InferenceRequest& req) override {
        record("predict:" + std::to_string(slot_id));
        std::lock_guard<std::mutex> lock(calls_mtx);
        predictions.emplace_back(slot_id, req);
        InferenceResult r;
        r.text = text_to_return;
        r.prompt_tokens = 10;
        r.completion_tokens = 5;
        r.duration_ms = 100.0f;
        r.tokens_per_second = 50.0f;
        return Ok(r);
    }
};

IModelMock* as_mock(IModel* m) { return static_cast<IModelMock*>(m); }

ModelInfo make_info(const std::string& name, const std::string& family = "qwen3.6") {
    ModelInfo i;
    i.name = name;
    i.family = family;
    i.gguf_path = "C:/models/" + name + ".gguf";
    i.n_slots = 2;
    i.vram_required_mb = 18000;
    i.context_size = 65536;
    i.has_vision = (name.find("27b") != std::string::npos);
    return i;
}

} // namespace

TEST_CASE("ModelRegistry: register, has, get_info, list", "[model][registry]") {
    ModelRegistry reg;
    reg.register_model(make_info("qwen3.6-27b"));
    reg.register_model(make_info("qwen3-coder-next", "qwen3-coder"));

    REQUIRE(reg.has("qwen3.6-27b"));
    REQUIRE(reg.has("qwen3-coder-next"));
    REQUIRE_FALSE(reg.has("missing"));

    REQUIRE(reg.size() == 2);
    auto listing = reg.list();
    REQUIRE(listing.size() == 2);
    REQUIRE(listing[0] == "qwen3-coder-next");
    REQUIRE(listing[1] == "qwen3.6-27b");

    const auto& i = reg.get_info("qwen3.6-27b");
    REQUIRE(i.name == "qwen3.6-27b");
    REQUIRE(i.family == "qwen3.6");
    REQUIRE(i.n_slots == 2);
    REQUIRE(i.has_vision);
}

TEST_CASE("ModelRegistry: unregister removes entry", "[model][registry]") {
    ModelRegistry reg;
    reg.register_model(make_info("a"));
    reg.register_model(make_info("b"));
    REQUIRE(reg.size() == 2);
    reg.unregister_model("a");
    REQUIRE(reg.size() == 1);
    REQUIRE_FALSE(reg.has("a"));
    REQUIRE(reg.has("b"));
}

TEST_CASE("ModelRegistry: create uses factory", "[model][registry]") {
    ModelRegistry reg;
    reg.register_model(make_info("foo"));
    reg.set_factory([](const ModelInfo& info) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(info);
    });
    auto m = reg.create("foo");
    REQUIRE(m != nullptr);
    REQUIRE(m->info().name == "foo");
    auto missing = reg.create("missing");
    REQUIRE(missing == nullptr);
    auto no_factory = ModelRegistry{};
    no_factory.register_model(make_info("bar"));
    auto none = no_factory.create("bar");
    REQUIRE(none == nullptr);
}

TEST_CASE("ModelRegistry: register rejects empty name", "[model][registry]") {
    ModelRegistry reg;
    REQUIRE_THROWS_AS(reg.register_model(ModelInfo{}), std::invalid_argument);
}

TEST_CASE("BackendCoordinator: load marks current_loaded_", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("a"));
    BackendCoordinator c(reg);
    REQUIRE(c.load("a").has_value());
    REQUIRE(c.is_loaded("a"));
    REQUIRE(c.get_loaded_model().has_value());
    REQUIRE(c.get_loaded_model().value() == "a");
    REQUIRE(c.get_vram_usage() == 4096);
}

TEST_CASE("BackendCoordinator: load of unregistered model fails", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    BackendCoordinator c(reg);
    auto r = c.load("nonexistent");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::NotFound);
}

TEST_CASE("BackendCoordinator: load failure surfaces error", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        auto m = std::make_unique<IModelMock>(i);
        m->load_should_fail.store(true);
        return m;
    });
    reg.register_model(make_info("a"));
    BackendCoordinator c(reg);
    auto r = c.load("a");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::Internal);
    REQUIRE_FALSE(c.is_loaded("a"));
    REQUIRE_FALSE(c.get_loaded_model().has_value());
}

TEST_CASE("BackendCoordinator: load is idempotent", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("a"));
    BackendCoordinator c(reg);
    REQUIRE(c.load("a").has_value());
    auto* m = c.get_model("a");
    REQUIRE(m != nullptr);
    auto* mock = as_mock(const_cast<IModel*>(m));
    REQUIRE(mock != nullptr);
    std::size_t before = mock->calls.size();
    REQUIRE(c.load("a").has_value());
    REQUIRE(mock->calls.size() == before);
}

TEST_CASE("BackendCoordinator: ensure_loaded is no-op when loaded", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("a"));
    BackendCoordinator c(reg);
    REQUIRE(c.ensure_loaded("a").has_value());
    auto* m = c.get_model("a");
    auto* mock = as_mock(const_cast<IModel*>(m));
    std::size_t before = mock->calls.size();
    REQUIRE(c.ensure_loaded("a").has_value());
    REQUIRE(mock->calls.size() == before);
}

TEST_CASE("BackendCoordinator: ensure_loaded triggers load", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("a"));
    BackendCoordinator c(reg);
    REQUIRE(c.ensure_loaded("a").has_value());
    REQUIRE(c.is_loaded("a"));
}

TEST_CASE("BackendCoordinator: unload_current clears current_loaded_", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("a"));
    BackendCoordinator c(reg);
    REQUIRE(c.load("a").has_value());
    REQUIRE(c.unload_current().has_value());
    REQUIRE_FALSE(c.get_loaded_model().has_value());
    REQUIRE_FALSE(c.is_loaded("a"));
    REQUIRE(c.get_vram_usage() == 0);
}

TEST_CASE("BackendCoordinator: unload with no model loaded is ok", "[model][coordinator]") {
    ModelRegistry reg;
    BackendCoordinator c(reg);
    REQUIRE(c.unload_current().has_value());
}

TEST_CASE("BackendCoordinator: swap_to unloads previous and loads new", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("a"));
    reg.register_model(make_info("b"));
    BackendCoordinator c(reg);
    REQUIRE(c.load("a").has_value());
    REQUIRE(c.swap_to("b").has_value());
    REQUIRE(c.get_loaded_model().value() == "b");
    REQUIRE_FALSE(c.is_loaded("a"));
    REQUIRE(c.is_loaded("b"));
}

TEST_CASE("BackendCoordinator: swap_to same model is no-op", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("a"));
    BackendCoordinator c(reg);
    REQUIRE(c.load("a").has_value());
    auto* m = c.get_model("a");
    auto* mock = as_mock(const_cast<IModel*>(m));
    std::size_t before = mock->calls.size();
    REQUIRE(c.swap_to("a").has_value());
    REQUIRE(mock->calls.size() == before);
}

TEST_CASE("BackendCoordinator: concurrent swaps serialize model loads", "[model][coordinator]") {
    std::atomic<int> active_loads{0};
    std::atomic<int> max_active_loads{0};

    class SerialLoadMock : public IModelMock {
    public:
        std::atomic<int>& active_loads;
        std::atomic<int>& max_active_loads;

        SerialLoadMock(ModelInfo info, std::atomic<int>& active, std::atomic<int>& max_active)
            : IModelMock(std::move(info)), active_loads(active), max_active_loads(max_active) {
            load_delay_ms.store(100);
        }

        Result<void> load() override {
            int active = active_loads.fetch_add(1) + 1;
            int observed = max_active_loads.load();
            while (active > observed &&
                   !max_active_loads.compare_exchange_weak(observed, active)) {
            }
            auto r = IModelMock::load();
            active_loads.fetch_sub(1);
            return r;
        }
    };

    ModelRegistry reg;
    reg.set_factory([&](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<SerialLoadMock>(i, active_loads, max_active_loads);
    });
    reg.register_model(make_info("a"));
    reg.register_model(make_info("b"));
    BackendCoordinator c(reg);

    std::thread t1([&] { REQUIRE(c.swap_to("a").has_value()); });
    std::thread t2([&] { REQUIRE(c.swap_to("b").has_value()); });
    t1.join();
    t2.join();

    REQUIRE(max_active_loads.load() == 1);
}

TEST_CASE("BackendCoordinator: acquire_slot returns slot id", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("a"));
    BackendCoordinator c(reg);
    REQUIRE(c.load("a").has_value());
    auto s1 = c.acquire_slot("a");
    REQUIRE(s1.has_value());
    auto s2 = c.acquire_slot("a");
    REQUIRE(s2.has_value());
    REQUIRE(s1.value() != s2.value());
    REQUIRE(c.active_request_count() == 2);
    REQUIRE(c.release_slot("a", s1.value()).has_value());
    REQUIRE(c.release_slot("a", s2.value()).has_value());
    REQUIRE(c.active_request_count() == 0);
}

TEST_CASE("BackendCoordinator: acquire_slot fails when no model loaded", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("a"));
    BackendCoordinator c(reg);
    auto r = c.acquire_slot("a");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::NotFound);
}

TEST_CASE("BackendCoordinator: acquire_slot times out when all busy (non-blocking)", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("a"));
    BackendCoordinator c(reg);
    REQUIRE(c.load("a").has_value());
    auto s1 = c.acquire_slot("a");
    REQUIRE(s1.has_value());
    auto s2 = c.acquire_slot("a");
    REQUIRE(s2.has_value());
    AcquireSlotOptions opts;
    opts.block = false;
    auto r = c.acquire_slot("a", opts);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::Unavailable);
    REQUIRE(c.release_slot("a", s1.value()).has_value());
    REQUIRE(c.release_slot("a", s2.value()).has_value());
}

TEST_CASE("BackendCoordinator: acquire_slot times out when all busy (blocking, short timeout)", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("a"));
    BackendCoordinator c(reg);
    REQUIRE(c.load("a").has_value());
    auto s1 = c.acquire_slot("a");
    REQUIRE(s1.has_value());
    auto s2 = c.acquire_slot("a");
    REQUIRE(s2.has_value());
    AcquireSlotOptions opts;
    opts.timeout = std::chrono::milliseconds{100};
    auto r = c.acquire_slot("a", opts);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::Timeout);
    REQUIRE(c.release_slot("a", s1.value()).has_value());
    REQUIRE(c.release_slot("a", s2.value()).has_value());
}

TEST_CASE("BackendCoordinator: acquire_slot succeeds after release", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("a"));
    BackendCoordinator c(reg);
    REQUIRE(c.load("a").has_value());
    auto s1 = c.acquire_slot("a");
    REQUIRE(s1.has_value());
    auto s2 = c.acquire_slot("a");
    REQUIRE(s2.has_value());
    AcquireSlotOptions opts;
    opts.timeout = std::chrono::milliseconds{50};
    auto blocked = c.acquire_slot("a", opts);
    REQUIRE_FALSE(blocked.has_value());
    REQUIRE(c.release_slot("a", s1.value()).has_value());
    auto s3 = c.acquire_slot("a");
    REQUIRE(s3.has_value());
    REQUIRE(c.release_slot("a", s3.value()).has_value());
    REQUIRE(c.release_slot("a", s2.value()).has_value());
}

TEST_CASE("BackendCoordinator: predict routes to model", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("a"));
    BackendCoordinator c(reg);
    REQUIRE(c.load("a").has_value());
    auto s = c.acquire_slot("a");
    REQUIRE(s.has_value());
    InferenceRequest req;
    req.prompt = "hi";
    req.max_tokens = 8;
    auto r = c.predict("a", s.value(), req);
    REQUIRE(r.has_value());
    REQUIRE(r.value().text == "hello world");
    REQUIRE(r.value().completion_tokens == 5);
    REQUIRE(c.release_slot("a", s.value()).has_value());
}

TEST_CASE("BackendCoordinator: vram_usage sums loaded models", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        auto m = std::make_unique<IModelMock>(i);
        m->vram_mb.store(8000);
        return m;
    });
    reg.register_model(make_info("a"));
    reg.register_model(make_info("b"));
    BackendCoordinator c(reg);
    REQUIRE(c.load("a").has_value());
    REQUIRE(c.get_vram_usage() == 8000);
    REQUIRE(c.swap_to("b").has_value());
    REQUIRE(c.get_vram_usage() == 8000);
}

TEST_CASE("BackendCoordinator: unregister refuses loaded model", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    reg.register_model(make_info("a"));
    BackendCoordinator c(reg);
    REQUIRE(c.load("a").has_value());
    auto r = c.unregister("a");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::AlreadyExists);
    REQUIRE(c.is_loaded("a"));
}

TEST_CASE("BackendCoordinator: register_existing adds to registry", "[model][coordinator]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<IModelMock>(i);
    });
    BackendCoordinator c(reg);
    REQUIRE(c.register_existing(make_info("a")).has_value());
    REQUIRE(c.load("a").has_value());
}
