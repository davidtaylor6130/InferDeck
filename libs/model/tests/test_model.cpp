#include <catch2/catch_test_macros.hpp>

#include "model/backend_coordinator.hpp"
#include "model/imodel.hpp"
#include "model/model_registry.hpp"

#include <atomic>
#include <chrono>
#include <future>
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
    std::atomic<bool> load_started{false};
    std::function<void()> load_gate{};
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
        load_started.store(true);
        if (load_should_fail.load()) {
            return Err<void>(ErrorCode::Internal, "mock load failure");
        }
        if (load_gate) load_gate();
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
    int vram_usage_mb() const override { return estimate_vram_mb(max_slots.load()); }
    int n_slots() const override { return max_slots.load(); }
    int min_slots() const override { return model_info.min_slots; }
    bool can_resize_slots() const override {
        return model_info.vram_fixed_mb > 0 && model_info.vram_per_slot_mb > 0 &&
               max_slots.load() > model_info.min_slots;
    }
    int estimate_vram_mb(int slots) const override {
        if (model_info.vram_fixed_mb > 0 && model_info.vram_per_slot_mb > 0) {
            return model_info.vram_fixed_mb + model_info.vram_per_slot_mb * slots;
        }
        return vram_mb.load();
    }
    Result<void> resize_slots(int slots) override {
        if (slots < model_info.min_slots || slots > model_info.n_slots) {
            return Err<void>(ErrorCode::InvalidArgument, "invalid capacity");
        }
        if (n_free_slots() != max_slots.load()) {
            return Err<void>(ErrorCode::Unavailable, "active slots");
        }
        max_slots.store(slots);
        busy_slots.assign(slots, 0);
        return Ok();
    }

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

class IBackendMock : public IBackend {
public:
    explicit IBackendMock(ModelInfo info) : info_(std::move(info)) {}

    const ModelInfo& info() const override { return info_; }
    Result<void> load() override { loaded_ = true; return Ok(); }
    Result<void> unload() override { loaded_ = false; busy_ = false; return Ok(); }
    bool is_loaded() const override { return loaded_; }
    int vram_usage_mb() const override { return loaded_ ? info_.vram_required_mb : 0; }
    int n_slots() const override { return 1; }
    int n_free_slots() const override { return busy_ ? 0 : 1; }
    Result<int> acquire_slot() override {
        if (busy_) return Err<int>(ErrorCode::Unavailable, "busy");
        busy_ = true;
        return Ok(0);
    }
    Result<void> release_slot(int slot_id) override {
        if (slot_id != 0) return Err<void>(ErrorCode::InvalidArgument, "invalid slot");
        busy_ = false;
        return Ok();
    }
    bool slot_busy(int slot_id) const override { return slot_id == 0 && busy_; }

private:
    ModelInfo info_;
    bool loaded_{false};
    bool busy_{false};
};

class EmbeddingBackendMock : public IBackendMock, public IEmbeddingBackend {
public:
    explicit EmbeddingBackendMock(ModelInfo info) : IBackendMock(std::move(info)) {}

    Result<EmbeddingResult> embed(
        int, const EmbeddingRequest& request,
        const std::function<bool()>& cancelled = {}) override {
        if (cancelled && cancelled()) {
            return Err<EmbeddingResult>(ErrorCode::Cancelled, "cancelled");
        }
        EmbeddingResult result;
        result.embeddings.resize(request.inputs.size(), std::vector<float>{1.0f, 2.0f});
        result.prompt_tokens = static_cast<int>(request.inputs.size());
        return Ok(std::move(result));
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

TEST_CASE("ModelRegistry: selects runtime factory", "[model][registry]") {
    ModelRegistry reg;
    reg.register_factory("llama_cpp", [](const ModelInfo& info) {
        auto backend = std::make_unique<IModelMock>(info);
        backend->text_to_return = "llama";
        return backend;
    });
    reg.register_factory("image_cpp", [](const ModelInfo& info) {
        return std::make_unique<IBackendMock>(info);
    });
    auto text = make_info("text");
    auto image = make_info("image");
    image.runtime = "image_cpp";
    image.modality = "image";
    image.capabilities = {"image_generation"};
    reg.register_model(text);
    reg.register_model(image);

    REQUIRE(reg.has_factory("llama_cpp"));
    REQUIRE(reg.has_factory("image_cpp"));
    REQUIRE(dynamic_cast<IModel*>(reg.create("text").get()) != nullptr);
    REQUIRE(dynamic_cast<IModel*>(reg.create("image").get()) == nullptr);
}

TEST_CASE("ModelRegistry: reports missing runtime", "[model][registry]") {
    ModelRegistry reg;
    auto info = make_info("missing-runtime");
    info.runtime = "unknown";
    reg.register_model(info);
    auto result = reg.create_result(info.name);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == ErrorCode::Unavailable);
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

TEST_CASE("BackendCoordinator: rejects text execution on non-text backend", "[model][coordinator]") {
    ModelRegistry reg;
    reg.register_factory("image_cpp", [](const ModelInfo& info) {
        return std::make_unique<IBackendMock>(info);
    });
    auto info = make_info("image");
    info.runtime = "image_cpp";
    info.modality = "image";
    info.capabilities = {"image_generation"};
    reg.register_model(info);
    BackendCoordinator coordinator(reg);
    REQUIRE(coordinator.load(info.name).has_value());
    auto slot = coordinator.acquire_slot(info.name);
    REQUIRE(slot.has_value());
    auto result = coordinator.predict(info.name, slot.value(), InferenceRequest{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == ErrorCode::InvalidArgument);
    REQUIRE(coordinator.release_slot(info.name, slot.value()).has_value());
}

TEST_CASE("BackendCoordinator: routes embeddings by capability", "[model][coordinator]") {
    ModelRegistry reg;
    reg.register_factory("embedding_cpp", [](const ModelInfo& info) {
        return std::make_unique<EmbeddingBackendMock>(info);
    });
    auto info = make_info("embedding");
    info.runtime = "embedding_cpp";
    info.modality = "embedding";
    info.capabilities = {"embeddings"};
    reg.register_model(info);
    BackendCoordinator coordinator(reg);
    REQUIRE(coordinator.load(info.name).has_value());
    auto slot = coordinator.acquire_slot(info.name);
    REQUIRE(slot.has_value());
    EmbeddingRequest request;
    request.inputs = {EmbeddingTextInput{"one"},
                      EmbeddingTokenInput{{1, 2, 3}}};
    auto result = coordinator.embed(info.name, *slot, request);
    REQUIRE(result.has_value());
    REQUIRE(result->embeddings.size() == 2);
    REQUIRE(result->prompt_tokens == 2);
    REQUIRE(coordinator.release_slot(info.name, *slot).has_value());
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

TEST_CASE("BackendCoordinator: duplicate release cannot affect a newer lease",
          "[model][coordinator][lease]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        auto model = std::make_unique<IModelMock>(i);
        model->max_slots.store(1);
        model->busy_slots.assign(1, 0);
        return model;
    });
    reg.register_model(make_info("a"));
    BackendCoordinator coordinator(reg);
    REQUIRE(coordinator.load("a").has_value());

    auto first = coordinator.acquire_slot("a");
    REQUIRE(first.has_value());
    REQUIRE(coordinator.release_slot("a", *first).has_value());

    auto second = coordinator.acquire_slot("a");
    REQUIRE(second.has_value());
    REQUIRE(*second != *first);
    REQUIRE(coordinator.release_slot("a", *first).has_value());
    REQUIRE(coordinator.active_request_count("a") == 1);

    auto stale = coordinator.predict("a", *first, InferenceRequest{});
    REQUIRE_FALSE(stale.has_value());
    REQUIRE(stale.error().code == ErrorCode::InvalidArgument);
    REQUIRE(coordinator.predict("a", *second, InferenceRequest{}).has_value());

    AcquireSlotOptions non_blocking;
    non_blocking.block = false;
    auto third = coordinator.acquire_slot("a", non_blocking);
    REQUIRE_FALSE(third.has_value());
    REQUIRE(third.error().code == ErrorCode::Unavailable);
    REQUIRE(coordinator.release_slot("a", *second).has_value());
    REQUIRE(coordinator.active_request_count() == 0);
}

TEST_CASE("BackendCoordinator: unload blocks admission without holding coordinator mutex",
          "[model][coordinator][drain]") {
    std::atomic<bool> unload_started{false};
    std::atomic<bool> allow_unload{false};

    class BlockingUnloadMock : public IModelMock {
    public:
        BlockingUnloadMock(ModelInfo info, std::atomic<bool>& started,
                           std::atomic<bool>& allow)
            : IModelMock(std::move(info)), started_(started), allow_(allow) {
            max_slots.store(2);
            busy_slots.assign(2, 0);
        }

        Result<void> unload() override {
            started_.store(true);
            while (!allow_.load()) std::this_thread::yield();
            return IModelMock::unload();
        }

    private:
        std::atomic<bool>& started_;
        std::atomic<bool>& allow_;
    };

    ModelRegistry reg;
    reg.set_factory([&](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<BlockingUnloadMock>(i, unload_started, allow_unload);
    });
    reg.register_model(make_info("a"));
    BackendCoordinator coordinator(reg);
    REQUIRE(coordinator.load("a").has_value());
    auto held = coordinator.acquire_slot("a");
    REQUIRE(held.has_value());

    auto unloading = std::async(std::launch::async, [&] {
        return coordinator.unload("a");
    });

    bool drain_observed = false;
    bool probe_release_failed = false;
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (std::chrono::steady_clock::now() < drain_deadline) {
        AcquireSlotOptions options;
        options.block = false;
        auto probe = coordinator.acquire_slot("a", options);
        if (!probe && probe.error().message == "model is draining: a") {
            drain_observed = true;
            break;
        }
        if (probe && !coordinator.release_slot("a", *probe)) {
            probe_release_failed = true;
            break;
        }
        std::this_thread::yield();
    }

    REQUIRE(coordinator.release_slot("a", *held).has_value());
    const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (!unload_started.load() && std::chrono::steady_clock::now() < start_deadline) {
        std::this_thread::yield();
    }

    auto query = std::async(std::launch::async, [&] {
        return coordinator.active_request_count();
    });
    const bool query_completed =
        query.wait_for(std::chrono::milliseconds{100}) == std::future_status::ready;
    AcquireSlotOptions options;
    options.block = false;
    auto during_unload = coordinator.acquire_slot("a", options);

    allow_unload.store(true);
    const auto unloaded = unloading.get();
    const auto active = query.get();

    REQUIRE(drain_observed);
    REQUIRE_FALSE(probe_release_failed);
    REQUIRE(unload_started.load());
    REQUIRE(query_completed);
    REQUIRE(active == 0);
    REQUIRE_FALSE(during_unload.has_value());
    REQUIRE(during_unload.error().code == ErrorCode::Unavailable);
    REQUIRE(unloaded.has_value());
    REQUIRE_FALSE(coordinator.is_loaded("a"));
}

TEST_CASE("BackendCoordinator: request queue is FIFO at equal priority", "[model][coordinator][queue]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        auto model = std::make_unique<IModelMock>(i);
        model->max_slots.store(1);
        model->busy_slots.assign(1, 0);
        return model;
    });
    reg.register_model(make_info("a"));
    BackendCoordinator coordinator(reg);
    REQUIRE(coordinator.load("a").has_value());
    auto held = coordinator.acquire_slot("a");
    REQUIRE(held.has_value());

    std::vector<int> order;
    std::mutex order_mutex;
    auto run = [&](int id) {
        auto slot = coordinator.acquire_slot("a");
        if (!slot) return;
        {
            std::lock_guard lock(order_mutex);
            order.push_back(id);
        }
        (void)coordinator.release_slot("a", *slot);
    };
    std::thread first(run, 1);
    while (coordinator.queued_request_count() != 1) std::this_thread::yield();
    std::thread second(run, 2);
    while (coordinator.queued_request_count() != 2) std::this_thread::yield();
    REQUIRE(coordinator.release_slot("a", *held).has_value());
    first.join();
    second.join();
    REQUIRE(order == std::vector<int>{1, 2});
}

TEST_CASE("BackendCoordinator: higher priority request advances first", "[model][coordinator][queue]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        auto model = std::make_unique<IModelMock>(i);
        model->max_slots.store(1);
        model->busy_slots.assign(1, 0);
        return model;
    });
    reg.register_model(make_info("a"));
    BackendCoordinator coordinator(reg);
    REQUIRE(coordinator.load("a").has_value());
    auto held = coordinator.acquire_slot("a");
    REQUIRE(held.has_value());

    std::vector<int> order;
    std::mutex order_mutex;
    auto run = [&](int id, int priority) {
        AcquireSlotOptions options;
        options.priority = priority;
        auto slot = coordinator.acquire_slot("a", options);
        if (!slot) return;
        {
            std::lock_guard lock(order_mutex);
            order.push_back(id);
        }
        (void)coordinator.release_slot("a", *slot);
    };
    std::thread low(run, 1, 0);
    while (coordinator.queued_request_count() != 1) std::this_thread::yield();
    std::thread high(run, 2, 10);
    while (coordinator.queued_request_count() != 2) std::this_thread::yield();
    REQUIRE(coordinator.release_slot("a", *held).has_value());
    low.join();
    high.join();
    REQUIRE(order == std::vector<int>{2, 1});
}

TEST_CASE("BackendCoordinator: queued request can be cancelled", "[model][coordinator][queue]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        auto model = std::make_unique<IModelMock>(i);
        model->max_slots.store(1);
        model->busy_slots.assign(1, 0);
        return model;
    });
    reg.register_model(make_info("a"));
    BackendCoordinator coordinator(reg);
    REQUIRE(coordinator.load("a").has_value());
    auto held = coordinator.acquire_slot("a");
    REQUIRE(held.has_value());

    std::atomic<bool> cancel{false};
    Result<int> result = Err<int>(ErrorCode::Internal, "not run");
    std::thread waiter([&] {
        AcquireSlotOptions options;
        options.cancelled = [&] { return cancel.load(); };
        result = coordinator.acquire_slot("a", options);
    });
    while (coordinator.queued_request_count() != 1) std::this_thread::yield();
    cancel.store(true);
    waiter.join();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == ErrorCode::Cancelled);
    REQUIRE(coordinator.queued_request_count() == 0);
    REQUIRE(coordinator.release_slot("a", *held).has_value());
}

TEST_CASE("BackendCoordinator: bounded queue rejects overflow", "[model][coordinator][queue]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        auto model = std::make_unique<IModelMock>(i);
        model->max_slots.store(1);
        model->busy_slots.assign(1, 0);
        return model;
    });
    reg.register_model(make_info("a"));
    BackendCoordinator coordinator(reg);
    coordinator.set_max_queue_size(1);
    REQUIRE(coordinator.load("a").has_value());
    auto held = coordinator.acquire_slot("a");
    REQUIRE(held.has_value());

    std::atomic<bool> cancel{false};
    std::thread waiter([&] {
        AcquireSlotOptions options;
        options.cancelled = [&] { return cancel.load(); };
        (void)coordinator.acquire_slot("a", options);
    });
    while (coordinator.queued_request_count() != 1) std::this_thread::yield();
    auto overflow = coordinator.acquire_slot("a");
    REQUIRE_FALSE(overflow.has_value());
    REQUIRE(overflow.error().code == ErrorCode::Unavailable);
    cancel.store(true);
    waiter.join();
    REQUIRE(coordinator.release_slot("a", *held).has_value());
}

TEST_CASE("BackendCoordinator: queued request remains admitted across model swap", "[model][coordinator][queue]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        auto model = std::make_unique<IModelMock>(i);
        model->max_slots.store(1);
        model->busy_slots.assign(1, 0);
        return model;
    });
    reg.register_model(make_info("a"));
    reg.register_model(make_info("b"));
    BackendCoordinator coordinator(reg);
    REQUIRE(coordinator.load("a").has_value());
    auto held = coordinator.acquire_slot("a");
    REQUIRE(held.has_value());

    Result<int> result = Err<int>(ErrorCode::Internal, "not run");
    std::thread waiter([&] {
        AcquireSlotOptions options;
        options.prepare = [&] { return coordinator.swap_to("b"); };
        result = coordinator.acquire_slot("b", options);
    });
    while (coordinator.queued_request_count() != 1) std::this_thread::yield();
    REQUIRE(coordinator.queue().front().model == "b");
    REQUIRE(coordinator.release_slot("a", *held).has_value());
    waiter.join();
    REQUIRE(result.has_value());
    REQUIRE(coordinator.is_loaded("b"));
    REQUIRE(coordinator.release_slot("b", *result).has_value());
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
    req.max_output_tokens = 8;
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

TEST_CASE("BackendCoordinator: keeps two models resident when budget fits", "[model][coordinator][residency]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        return backend;
    });
    auto a = make_info("a");
    auto b = make_info("b");
    a.vram_required_mb = 4000;
    b.vram_required_mb = 4000;
    reg.register_model(a);
    reg.register_model(b);
    BackendCoordinator coordinator(reg);
    coordinator.set_vram_budget(9000, 1000);

    REQUIRE(coordinator.swap_to("a").has_value());
    REQUIRE(coordinator.swap_to("b").has_value());
    REQUIRE(coordinator.is_loaded("a"));
    REQUIRE(coordinator.is_loaded("b"));
    REQUIRE(coordinator.get_loaded_model() == "b");
    REQUIRE(coordinator.get_loaded_models() == std::vector<std::string>{"a", "b"});
    REQUIRE(coordinator.vram_available_mb() == 0);
}

TEST_CASE("BackendCoordinator: shrinks idle capacity before loading", "[model][coordinator][residency]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        return backend;
    });
    auto a = make_info("a");
    a.vram_fixed_mb = 3000;
    a.vram_per_slot_mb = 1000;
    a.vram_required_mb = 5000;
    auto b = make_info("b");
    b.vram_required_mb = 5000;
    reg.register_model(a);
    reg.register_model(b);
    BackendCoordinator coordinator(reg);
    coordinator.set_vram_budget(9000, 0);

    REQUIRE(coordinator.swap_to("a").has_value());
    REQUIRE(coordinator.swap_to("b").has_value());
    REQUIRE(coordinator.is_loaded("a"));
    REQUIRE(coordinator.is_loaded("b"));
    REQUIRE(coordinator.get_backend("a")->n_slots() == 1);
    REQUIRE(coordinator.get_vram_usage() == 9000);
}

TEST_CASE("BackendCoordinator: active model defers eviction", "[model][coordinator][residency]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        return backend;
    });
    auto a = make_info("a");
    a.vram_fixed_mb = 3000;
    a.vram_per_slot_mb = 1000;
    a.vram_required_mb = 5000;
    auto b = make_info("b");
    b.vram_required_mb = 5000;
    reg.register_model(a);
    reg.register_model(b);
    BackendCoordinator coordinator(reg);
    coordinator.set_vram_budget(9000, 0);

    REQUIRE(coordinator.swap_to("a").has_value());
    auto slot = coordinator.acquire_slot("a");
    REQUIRE(slot.has_value());
    auto result = coordinator.swap_to("b");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == ErrorCode::ResourceBusy);
    REQUIRE(coordinator.is_loaded("a"));
    REQUIRE_FALSE(coordinator.is_loaded("b"));
    REQUIRE(coordinator.release_slot("a", *slot).has_value());
}

TEST_CASE("BackendCoordinator: impossible residency remains a hard VRAM error",
          "[model][coordinator][residency]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        return backend;
    });
    auto model = make_info("too-large");
    model.vram_required_mb = 10000;
    reg.register_model(model);
    BackendCoordinator coordinator(reg);
    coordinator.set_vram_budget(9000, 0);

    auto result = coordinator.swap_to("too-large");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::OutOfMemory);
}

TEST_CASE("BackendCoordinator: capacity-blocked request stays queued until active release",
          "[model][coordinator][queue][residency]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        backend->max_slots.store(1);
        backend->busy_slots.assign(1, 0);
        return backend;
    });
    auto active = make_info("gemma");
    active.vram_required_mb = 5000;
    active.n_slots = 1;
    active.min_slots = 1;
    auto waiting = make_info("qwen");
    waiting.vram_required_mb = 5000;
    waiting.n_slots = 1;
    waiting.min_slots = 1;
    reg.register_model(active);
    reg.register_model(waiting);
    BackendCoordinator coordinator(reg);
    coordinator.set_vram_budget(9000, 0);

    REQUIRE(coordinator.swap_to("gemma").has_value());
    auto held = coordinator.acquire_slot("gemma");
    REQUIRE(held.has_value());

    Result<int> result = Err<int>(ErrorCode::Internal, "not run");
    std::jthread waiter([&] {
        AcquireSlotOptions options;
        options.timeout = std::chrono::seconds{2};
        options.prepare = [&] { return coordinator.swap_to("qwen"); };
        result = coordinator.acquire_slot("qwen", options);
    });
    for (int attempt = 0; attempt < 500 &&
         coordinator.last_resource_decision().find("waiting for active residency") ==
             std::string::npos;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK(coordinator.last_resource_decision().find("waiting for active residency") !=
          std::string::npos);
    CHECK_FALSE(coordinator.is_loaded("qwen"));
    REQUIRE(coordinator.release_slot("gemma", *held).has_value());
    waiter.join();

    REQUIRE(result.has_value());
    CHECK_FALSE(coordinator.is_loaded("gemma"));
    CHECK(coordinator.is_loaded("qwen"));
    REQUIRE(coordinator.release_slot("qwen", *result).has_value());
}

TEST_CASE("BackendCoordinator: VRAM pressure preserves zero-VRAM voice residency",
          "[model][coordinator][residency][voice]") {
    ModelRegistry reg;
    const auto factory = [](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        backend->max_slots.store(1);
        backend->busy_slots.assign(1, 0);
        return backend;
    };
    reg.set_factory(factory);
    reg.register_factory("sherpa_onnx", factory);
    auto voice = make_info("voice");
    voice.runtime = "sherpa_onnx";
    voice.modality = "audio_speech";
    voice.vram_required_mb = 0;
    voice.n_slots = 1;
    voice.min_slots = 1;
    auto active = make_info("gemma");
    active.vram_required_mb = 5000;
    active.n_slots = 1;
    active.min_slots = 1;
    auto target = make_info("qwen");
    target.vram_required_mb = 5000;
    target.n_slots = 1;
    target.min_slots = 1;
    reg.register_model(voice);
    reg.register_model(active);
    reg.register_model(target);
    BackendCoordinator coordinator(reg);
    coordinator.set_vram_budget(9000, 0);

    REQUIRE(coordinator.load("gemma").has_value());
    REQUIRE(coordinator.load("voice").has_value());
    CHECK(coordinator.get_loaded_model() == "gemma");
    auto held = coordinator.acquire_slot("gemma");
    REQUIRE(held.has_value());

    auto blocked = coordinator.swap_to("qwen");
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error().code == ErrorCode::ResourceBusy);
    CHECK(coordinator.is_loaded("voice"));
    CHECK(coordinator.get_loaded_model() == "gemma");

    REQUIRE(coordinator.release_slot("gemma", *held).has_value());
    REQUIRE(coordinator.swap_to("qwen").has_value());
    CHECK(coordinator.is_loaded("voice"));
    CHECK_FALSE(coordinator.is_loaded("gemma"));
    CHECK(coordinator.is_loaded("qwen"));
    CHECK(coordinator.get_loaded_model() == "qwen");
}

TEST_CASE("BackendCoordinator: resident voice bypasses a capacity-blocked LLM waiter",
          "[model][coordinator][queue][residency][voice]") {
    ModelRegistry reg;
    const auto factory = [](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        backend->max_slots.store(1);
        backend->busy_slots.assign(1, 0);
        return backend;
    };
    reg.set_factory(factory);
    reg.register_factory("sherpa_onnx", factory);
    auto voice = make_info("voice");
    voice.runtime = "sherpa_onnx";
    voice.modality = "audio_speech";
    voice.vram_required_mb = 0;
    voice.n_slots = 1;
    voice.min_slots = 1;
    auto active = make_info("gemma");
    active.vram_required_mb = 5000;
    active.n_slots = 1;
    active.min_slots = 1;
    auto target = make_info("qwen");
    target.vram_required_mb = 5000;
    target.n_slots = 1;
    target.min_slots = 1;
    reg.register_model(voice);
    reg.register_model(active);
    reg.register_model(target);
    BackendCoordinator coordinator(reg);
    coordinator.set_vram_budget(9000, 0);
    REQUIRE(coordinator.load("gemma"));
    REQUIRE(coordinator.load("voice"));
    auto held = coordinator.acquire_slot("gemma");
    REQUIRE(held);

    Result<int> qwen_slot = Err<int>(ErrorCode::Internal, "not completed");
    std::jthread waiter([&] {
        AcquireSlotOptions options;
        options.timeout = std::chrono::seconds{2};
        options.prepare = [&] { return coordinator.swap_to("qwen"); };
        qwen_slot = coordinator.acquire_slot("qwen", options);
    });
    for (int attempt = 0; attempt < 100 &&
         coordinator.last_resource_decision().find("waiting") == std::string::npos;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    REQUIRE(coordinator.last_resource_decision().find("waiting") != std::string::npos);

    AcquireSlotOptions voice_options;
    voice_options.timeout = std::chrono::milliseconds{100};
    auto voice_slot = coordinator.acquire_slot("voice", voice_options);
    REQUIRE(voice_slot);
    REQUIRE(coordinator.release_slot("voice", *voice_slot));
    REQUIRE(coordinator.release_slot("gemma", *held));
    waiter.join();
    REQUIRE(qwen_slot);
    REQUIRE(coordinator.release_slot("qwen", *qwen_slot));
}

TEST_CASE("BackendCoordinator: resident voice bypasses a preparing LLM waiter",
          "[model][coordinator][queue][residency][voice]") {
    ModelRegistry reg;
    const auto factory = [](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        backend->max_slots.store(1);
        backend->busy_slots.assign(1, 0);
        if (i.name == "qwen") backend->load_delay_ms.store(300);
        return backend;
    };
    reg.set_factory(factory);
    reg.register_factory("sherpa_onnx", factory);
    auto voice = make_info("voice");
    voice.runtime = "sherpa_onnx";
    voice.modality = "audio_speech";
    voice.vram_required_mb = 0;
    voice.n_slots = 1;
    voice.min_slots = 1;
    auto active = make_info("gemma");
    active.vram_required_mb = 5000;
    active.n_slots = 1;
    active.min_slots = 1;
    auto target = make_info("qwen");
    target.vram_required_mb = 5000;
    target.n_slots = 1;
    target.min_slots = 1;
    reg.register_model(voice);
    reg.register_model(active);
    reg.register_model(target);
    BackendCoordinator coordinator(reg);
    coordinator.set_vram_budget(9000, 0);
    REQUIRE(coordinator.load("gemma"));
    REQUIRE(coordinator.load("voice"));
    auto held = coordinator.acquire_slot("gemma");
    REQUIRE(held);

    Result<int> qwen_slot = Err<int>(ErrorCode::Internal, "not completed");
    std::atomic<bool> qwen_acquired{false};
    std::jthread waiter([&] {
        AcquireSlotOptions options;
        options.timeout = std::chrono::seconds{2};
        options.prepare = [&] { return coordinator.swap_to("qwen"); };
        qwen_slot = coordinator.acquire_slot("qwen", options);
        qwen_acquired.store(qwen_slot.has_value());
    });
    for (int attempt = 0; attempt < 500 &&
         coordinator.last_resource_decision().find("waiting") == std::string::npos;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    REQUIRE(coordinator.last_resource_decision().find("waiting") != std::string::npos);
    REQUIRE(coordinator.release_slot("gemma", *held));

    IModelMock* qwen = nullptr;
    for (int attempt = 0; attempt < 500; ++attempt) {
        qwen = const_cast<IModelMock*>(
            dynamic_cast<const IModelMock*>(coordinator.get_backend("qwen")));
        if (qwen && qwen->load_started.load() && !qwen->loaded.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(qwen);
    REQUIRE(qwen->load_started.load());
    REQUIRE_FALSE(qwen->loaded.load());

    AcquireSlotOptions voice_options;
    voice_options.timeout = std::chrono::milliseconds{100};
    auto voice_slot = coordinator.acquire_slot("voice", voice_options);
    REQUIRE(voice_slot);
    std::this_thread::sleep_for(std::chrono::milliseconds{350});
    CHECK_FALSE(qwen_acquired.load());
    REQUIRE(coordinator.release_slot("voice", *voice_slot));
    waiter.join();
    REQUIRE(qwen_slot);
    REQUIRE(coordinator.release_slot("qwen", *qwen_slot));
}

TEST_CASE("BackendCoordinator: runnable voice bypasses a busy loaded waiter",
          "[model][coordinator][queue][residency][voice]") {
    ModelRegistry reg;
    const auto factory = [](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        backend->max_slots.store(1);
        backend->busy_slots.assign(1, 0);
        return backend;
    };
    reg.set_factory(factory);
    reg.register_factory("sherpa_onnx", factory);
    auto chat = make_info("chat");
    chat.n_slots = 1;
    chat.min_slots = 1;
    auto voice = make_info("voice");
    voice.runtime = "sherpa_onnx";
    voice.modality = "audio_speech";
    voice.vram_required_mb = 0;
    voice.n_slots = 1;
    voice.min_slots = 1;
    reg.register_model(chat);
    reg.register_model(voice);
    BackendCoordinator coordinator(reg);
    REQUIRE(coordinator.load("chat"));
    REQUIRE(coordinator.load("voice"));
    auto held = coordinator.acquire_slot("chat");
    REQUIRE(held);

    Result<int> queued = Err<int>(ErrorCode::Internal, "not completed");
    std::jthread waiter([&] {
        AcquireSlotOptions options;
        options.timeout = std::chrono::seconds{2};
        queued = coordinator.acquire_slot("chat", options);
    });
    for (int attempt = 0; attempt < 500 && coordinator.queued_request_count() == 0;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(coordinator.queued_request_count() == 1);

    AcquireSlotOptions voice_options;
    voice_options.timeout = std::chrono::milliseconds{100};
    auto voice_slot = coordinator.acquire_slot("voice", voice_options);
    REQUIRE(voice_slot);
    REQUIRE(coordinator.release_slot("voice", *voice_slot));
    REQUIRE(coordinator.release_slot("chat", *held));
    waiter.join();
    REQUIRE(queued);
    REQUIRE(coordinator.release_slot("chat", *queued));
}

TEST_CASE("BackendCoordinator: one cold prepare completes before another can swap",
          "[model][coordinator][queue][residency]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        backend->max_slots.store(1);
        backend->busy_slots.assign(1, 0);
        if (i.name == "a") backend->load_delay_ms.store(150);
        return backend;
    });
    auto a = make_info("a");
    a.vram_required_mb = 5000;
    a.n_slots = 1;
    a.min_slots = 1;
    auto b = make_info("b");
    b.vram_required_mb = 5000;
    b.n_slots = 1;
    b.min_slots = 1;
    reg.register_model(a);
    reg.register_model(b);
    BackendCoordinator coordinator(reg);
    coordinator.set_vram_budget(5000, 0);

    Result<int> a_slot = Err<int>(ErrorCode::Internal, "not completed");
    std::jthread first([&] {
        AcquireSlotOptions options;
        options.timeout = std::chrono::seconds{2};
        options.prepare = [&] { return coordinator.swap_to("a"); };
        a_slot = coordinator.acquire_slot("a", options);
    });
    IModelMock* a_backend = nullptr;
    for (int attempt = 0; attempt < 500; ++attempt) {
        a_backend = const_cast<IModelMock*>(
            dynamic_cast<const IModelMock*>(coordinator.get_backend("a")));
        if (a_backend && a_backend->load_started.load() &&
            !a_backend->loaded.load()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(a_backend);
    REQUIRE(a_backend->load_started.load());
    REQUIRE_FALSE(a_backend->loaded.load());

    Result<int> b_slot = Err<int>(ErrorCode::Internal, "not completed");
    std::atomic<bool> b_acquired{false};
    std::jthread second([&] {
        AcquireSlotOptions options;
        options.timeout = std::chrono::seconds{2};
        options.priority = 100;
        options.prepare = [&] { return coordinator.swap_to("b"); };
        b_slot = coordinator.acquire_slot("b", options);
        b_acquired.store(b_slot.has_value());
    });
    first.join();

    REQUIRE(a_slot);
    CHECK(coordinator.is_loaded("a"));
    CHECK_FALSE(coordinator.is_loaded("b"));
    CHECK_FALSE(b_acquired.load());
    const auto* b_backend = dynamic_cast<const IModelMock*>(
        coordinator.get_backend("b"));
    CHECK((!b_backend || !b_backend->load_started.load()));

    REQUIRE(coordinator.release_slot("a", *a_slot));
    second.join();
    REQUIRE(b_slot);
    CHECK_FALSE(coordinator.is_loaded("a"));
    CHECK(coordinator.is_loaded("b"));
    REQUIRE(coordinator.release_slot("b", *b_slot));
}

TEST_CASE("BackendCoordinator: primary selection uses modality rather than VRAM",
          "[model][coordinator][residency]") {
    ModelRegistry reg;
    const auto factory = [](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        return backend;
    };
    reg.set_factory(factory);
    reg.register_factory("sherpa_onnx", factory);
    auto gpu_text = make_info("gpu-text");
    gpu_text.vram_required_mb = 4096;
    auto cpu_text = make_info("cpu-text");
    cpu_text.vram_required_mb = 0;
    auto voice = make_info("voice");
    voice.runtime = "sherpa_onnx";
    voice.modality = "audio_speech";
    voice.vram_required_mb = 0;
    reg.register_model(gpu_text);
    reg.register_model(cpu_text);
    reg.register_model(voice);
    BackendCoordinator coordinator(reg);

    REQUIRE(coordinator.load("gpu-text"));
    REQUIRE(coordinator.load("cpu-text"));
    REQUIRE(coordinator.load("voice"));
    CHECK(coordinator.get_loaded_model() == "cpu-text");
    REQUIRE(coordinator.unload_current());
    CHECK_FALSE(coordinator.is_loaded("cpu-text"));
    CHECK(coordinator.is_loaded("gpu-text"));
    CHECK(coordinator.is_loaded("voice"));
    CHECK(coordinator.get_loaded_model() == "gpu-text");
}

TEST_CASE("BackendCoordinator: no-budget sidecar swap preserves the primary model",
          "[model][coordinator][residency][voice]") {
    ModelRegistry reg;
    const auto factory = [](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        return backend;
    };
    reg.set_factory(factory);
    reg.register_factory("sherpa_onnx", factory);
    auto text = make_info("text");
    text.vram_required_mb = 4096;
    auto voice = make_info("voice");
    voice.runtime = "sherpa_onnx";
    voice.modality = "audio_speech";
    voice.vram_required_mb = 0;
    reg.register_model(text);
    reg.register_model(voice);
    BackendCoordinator coordinator(reg);

    REQUIRE(coordinator.load("text"));
    REQUIRE(coordinator.swap_to("voice"));
    CHECK(coordinator.is_loaded("text"));
    CHECK(coordinator.is_loaded("voice"));
    CHECK(coordinator.get_loaded_model() == "text");
}

TEST_CASE("BackendCoordinator: cold sidecar acquires during a GPU prepare",
          "[model][coordinator][residency][voice]") {
    ModelRegistry reg;
    auto allow_qwen = std::make_shared<std::atomic<bool>>(false);
    auto allow_voice = std::make_shared<std::atomic<bool>>(false);
    const auto factory = [allow_qwen, allow_voice](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        if (i.name == "qwen") {
            backend->load_gate = [allow_qwen] {
                while (!allow_qwen->load()) std::this_thread::yield();
            };
        }
        if (i.name == "voice") {
            backend->load_gate = [allow_voice] {
                while (!allow_voice->load()) std::this_thread::yield();
            };
        }
        return backend;
    };
    reg.set_factory(factory);
    reg.register_factory("sherpa_onnx", factory);
    auto qwen = make_info("qwen");
    qwen.vram_required_mb = 5000;
    auto voice = make_info("voice");
    voice.runtime = "sherpa_onnx";
    voice.modality = "audio_speech";
    voice.vram_required_mb = 0;
    reg.register_model(qwen);
    reg.register_model(voice);
    BackendCoordinator coordinator(reg);
    coordinator.set_vram_budget(5000, 0);

    AcquireSlotOptions qwen_options;
    qwen_options.timeout = std::chrono::seconds{1};
    qwen_options.prepare = [&] { return coordinator.swap_to("qwen"); };
    Result<int> qwen_slot = Err<int>(ErrorCode::Internal, "not completed");
    std::atomic<bool> qwen_acquired{false};
    std::jthread loader([&] {
        qwen_slot = coordinator.acquire_slot("qwen", qwen_options);
        qwen_acquired.store(qwen_slot.has_value());
    });
    IModelMock* qwen_backend = nullptr;
    for (int attempt = 0; attempt < 500; ++attempt) {
        qwen_backend = const_cast<IModelMock*>(
            dynamic_cast<const IModelMock*>(coordinator.get_backend("qwen")));
        if (qwen_backend && qwen_backend->load_started.load() &&
            !qwen_backend->loaded.load()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(qwen_backend);
    REQUIRE(qwen_backend->load_started.load());
    REQUIRE_FALSE(qwen_backend->loaded.load());
    AcquireSlotOptions voice_options;
    voice_options.timeout = std::chrono::seconds{1};
    voice_options.priority = 100;
    voice_options.prepare = [&] { return coordinator.swap_to("voice"); };
    Result<int> voice_slot = Err<int>(ErrorCode::Internal, "not completed");
    std::jthread voice_loader([&] {
        voice_slot = coordinator.acquire_slot("voice", voice_options);
    });
    IModelMock* voice_backend = nullptr;
    for (int attempt = 0; attempt < 500; ++attempt) {
        voice_backend = const_cast<IModelMock*>(
            dynamic_cast<const IModelMock*>(coordinator.get_backend("voice")));
        if (voice_backend && voice_backend->load_started.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const bool voice_started_during_qwen_prepare =
        voice_backend && voice_backend->load_started.load();
    allow_qwen->store(true);
    for (int attempt = 0; attempt < 500 && !qwen_backend->loaded.load(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    CHECK(voice_started_during_qwen_prepare);
    CHECK(qwen_backend->loaded.load());
    CHECK_FALSE(qwen_acquired.load());
    allow_voice->store(true);
    voice_loader.join();
    REQUIRE(voice_slot);
    CHECK(coordinator.is_loaded("voice"));
    REQUIRE(coordinator.release_slot("voice", *voice_slot));
    loader.join();
    REQUIRE(qwen_slot);
    REQUIRE(coordinator.release_slot("qwen", *qwen_slot));
    CHECK(coordinator.is_loaded("qwen"));
}

TEST_CASE("BackendCoordinator: cold sidecar lock wait respects deadline",
          "[model][coordinator][voice][deadline]") {
    ModelRegistry reg;
    auto release_first = std::make_shared<std::atomic<bool>>(false);
    const auto factory = [release_first](const ModelInfo& info) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(info);
        backend->vram_mb.store(0);
        if (info.name == "voice-a") {
            backend->load_gate = [release_first] {
                while (!release_first->load()) std::this_thread::yield();
            };
        }
        return backend;
    };
    reg.set_factory(factory);
    reg.register_factory("sherpa_onnx", factory);
    auto voice_a = make_info("voice-a");
    voice_a.runtime = "sherpa_onnx";
    voice_a.modality = "audio_speech";
    voice_a.vram_required_mb = 0;
    auto voice_b = voice_a;
    voice_b.name = "voice-b";
    reg.register_model(voice_a);
    reg.register_model(voice_b);
    BackendCoordinator coordinator(reg);

    Result<void> first_load = Err<void>(ErrorCode::Internal, "not completed");
    std::jthread first([&] { first_load = coordinator.load("voice-a"); });
    IModelMock* first_backend = nullptr;
    for (int attempt = 0; attempt < 500; ++attempt) {
        first_backend = const_cast<IModelMock*>(
            dynamic_cast<const IModelMock*>(coordinator.get_backend("voice-a")));
        if (first_backend && first_backend->load_started.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto second_load = coordinator.load_with_lock_deadline(
        "voice-b", std::chrono::steady_clock::now() +
            std::chrono::milliseconds{30}, {});
    release_first->store(true);
    first.join();

    REQUIRE(first_backend);
    REQUIRE(first_backend->load_started.load());
    REQUIRE(first_load);
    REQUIRE_FALSE(second_load);
    CHECK(second_load.error().code == ErrorCode::Timeout);
    CHECK_FALSE(coordinator.is_loaded("voice-b"));
}

TEST_CASE("BackendCoordinator: voice session reservation bridges media gaps",
          "[model][coordinator][queue][voice]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& info) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(info);
        backend->max_slots.store(1);
        backend->busy_slots.assign(1, 0);
        return backend;
    });
    auto qwen = make_info("qwen");
    qwen.n_slots = 1;
    auto gemma = make_info("gemma");
    gemma.n_slots = 1;
    reg.register_model(qwen);
    reg.register_model(gemma);
    BackendCoordinator coordinator(reg);
    REQUIRE(coordinator.load("qwen"));
    REQUIRE(coordinator.load("gemma"));
    auto held_qwen = coordinator.acquire_slot("qwen");
    REQUIRE(held_qwen);

    Result<int> queued_qwen = Err<int>(ErrorCode::Internal, "not completed");
    std::atomic<bool> qwen_acquired{false};
    std::jthread qwen_waiter([&] {
        AcquireSlotOptions options;
        options.timeout = std::chrono::seconds{1};
        queued_qwen = coordinator.acquire_slot("qwen", options);
        qwen_acquired.store(queued_qwen.has_value());
    });
    for (int attempt = 0; attempt < 500 && coordinator.queued_request_count() == 0;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(coordinator.queued_request_count() == 1);
    const auto session_token = coordinator.reserve_priority_session(
        "openwebui", "gemma", std::chrono::milliseconds{20});
    const auto held_session =
        coordinator.hold_priority_session("openwebui", "gemma");
    REQUIRE(held_session == session_token);
    REQUIRE(coordinator.release_slot("qwen", *held_qwen));
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    CHECK_FALSE(qwen_acquired.load());

    AcquireSlotOptions voice_chat;
    voice_chat.priority = 100;
    voice_chat.reservation_key = "openwebui";
    auto gemma_slot = coordinator.acquire_slot("gemma", voice_chat);
    REQUIRE(gemma_slot);
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    REQUIRE(coordinator.release_slot("gemma", *gemma_slot));
    coordinator.complete_priority_session_hold(
        "openwebui", session_token, std::chrono::milliseconds{100});
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    CHECK_FALSE(qwen_acquired.load());

    coordinator.release_priority_session("openwebui", session_token);
    qwen_waiter.join();
    REQUIRE(queued_qwen);
    REQUIRE(coordinator.release_slot("qwen", *queued_qwen));
}

TEST_CASE("BackendCoordinator: stale voice token cannot clear a newer session",
          "[model][coordinator][voice]") {
    ModelRegistry reg;
    reg.register_model(make_info("gemma"));
    BackendCoordinator coordinator(reg);
    const auto old_token = coordinator.reserve_priority_session(
        "openwebui", "gemma", std::chrono::seconds{1});
    const auto new_token = coordinator.reserve_priority_session(
        "openwebui", "gemma", std::chrono::seconds{1});

    coordinator.release_priority_session("openwebui", old_token);
    CHECK(coordinator.priority_session_matches("openwebui", "gemma"));
    coordinator.release_priority_session("openwebui", new_token);
    CHECK_FALSE(coordinator.priority_session_matches("openwebui", "gemma"));
}

TEST_CASE("BackendCoordinator: voice reservation stops a destructive GPU swap",
          "[model][coordinator][voice][residency]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& info) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(info);
        backend->vram_mb.store(info.vram_required_mb);
        return backend;
    });
    auto gemma = make_info("gemma");
    gemma.vram_required_mb = 5000;
    auto qwen = make_info("qwen");
    qwen.vram_required_mb = 5000;
    reg.register_model(gemma);
    reg.register_model(qwen);
    BackendCoordinator coordinator(reg);
    coordinator.set_vram_budget(5000, 0);
    REQUIRE(coordinator.load("gemma"));
    const auto token = coordinator.reserve_priority_session(
        "openwebui", "gemma", std::chrono::seconds{1});

    const auto swapped = coordinator.swap_to("qwen");

    REQUIRE_FALSE(swapped);
    CHECK(swapped.error().code == ErrorCode::ResourceBusy);
    CHECK(coordinator.is_loaded("gemma"));
    CHECK_FALSE(coordinator.is_loaded("qwen"));
    coordinator.release_priority_session("openwebui", token);
}

TEST_CASE("BackendCoordinator: evicts idle model only after resize options", "[model][coordinator][residency]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        return backend;
    });
    auto a = make_info("a");
    auto b = make_info("b");
    a.vram_required_mb = 5000;
    b.vram_required_mb = 5000;
    reg.register_model(a);
    reg.register_model(b);
    BackendCoordinator coordinator(reg);
    coordinator.set_vram_budget(9000, 0);

    REQUIRE(coordinator.swap_to("a").has_value());
    REQUIRE(coordinator.swap_to("b").has_value());
    REQUIRE_FALSE(coordinator.is_loaded("a"));
    REQUIRE(coordinator.is_loaded("b"));
}

TEST_CASE("BackendCoordinator: restores residency after target load failure", "[model][coordinator][residency]") {
    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IBackend> {
        auto backend = std::make_unique<IModelMock>(i);
        backend->vram_mb.store(i.vram_required_mb);
        backend->load_should_fail.store(i.name == "b");
        return backend;
    });
    auto a = make_info("a");
    auto b = make_info("b");
    a.vram_required_mb = 5000;
    b.vram_required_mb = 5000;
    reg.register_model(a);
    reg.register_model(b);
    BackendCoordinator coordinator(reg);
    coordinator.set_vram_budget(9000, 0);

    REQUIRE(coordinator.swap_to("a").has_value());
    auto result = coordinator.swap_to("b");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(coordinator.is_loaded("a"));
    REQUIRE_FALSE(coordinator.is_loaded("b"));
    REQUIRE(coordinator.get_loaded_model() == "a");
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

TEST_CASE("ModelRegistry: stable aliases enforce their compatibility contract", "[model][alias]") {
    ModelRegistry registry;
    auto primary = make_info("primary");
    primary.context_size = 32768;
    primary.capabilities = {"chat_completions", "responses"};
    auto compatible = make_info("compatible");
    compatible.context_size = 65536;
    compatible.capabilities = primary.capabilities;
    auto incompatible = make_info("incompatible");
    incompatible.context_size = 8192;
    incompatible.capabilities = {"chat_completions"};
    registry.register_model(primary);
    registry.register_model(compatible);
    registry.register_model(incompatible);

    auto created = registry.set_alias({"production", "primary"});
    REQUIRE(created);
    CHECK(created->required_context_size == 32768);
    REQUIRE(registry.resolve("production"));
    CHECK(*registry.resolve("production") == "primary");

    auto retargeted = registry.set_alias({"production", "compatible"});
    REQUIRE(retargeted);
    REQUIRE(registry.resolve("production"));
    CHECK(*registry.resolve("production") == "compatible");

    const auto rejected = registry.set_alias({"production", "incompatible"});
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message.find("context_size") != std::string::npos);
    REQUIRE(registry.resolve("production"));
    CHECK(*registry.resolve("production") == "compatible");
    REQUIRE_FALSE(registry.set_alias({"nested", "production"}));
    REQUIRE_FALSE(registry.set_alias({"primary", "compatible"}));
}

TEST_CASE("BackendCoordinator: explicit CPU helper never becomes selected",
          "[model][coordinator][resources][helper]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        return std::make_unique<IModelMock>(info);
    });
    auto conversation = make_info("conversation");
    conversation.role = ModelRole::Conversation;
    conversation.compute = ModelCompute::VulkanGpu;
    conversation.residency = ResidencyPolicy::Managed;
    conversation.admission_pool = "conversation";
    conversation.concurrency_limit = conversation.n_slots;
    conversation.eviction_eligible = true;
    conversation.resource_metadata_explicit = true;
    auto helper = make_info("helper");
    helper.role = ModelRole::Helper;
    helper.compute = ModelCompute::Cpu;
    helper.residency = ResidencyPolicy::Always;
    helper.admission_pool = "helper";
    helper.concurrency_limit = 1;
    helper.n_slots = 1;
    helper.min_slots = 1;
    helper.memory_required_mb = 64;
    helper.eviction_eligible = false;
    helper.resource_metadata_explicit = true;
    registry.register_model(conversation);
    registry.register_model(helper);
    BackendCoordinator coordinator(registry);

    REQUIRE(coordinator.load("conversation"));
    REQUIRE(coordinator.load("helper"));
    CHECK(coordinator.selected_model() == "conversation");
    REQUIRE(coordinator.swap_to("helper"));
    CHECK(coordinator.selected_model() == "conversation");
    REQUIRE(coordinator.is_loaded("helper"));
    REQUIRE(coordinator.is_loaded("conversation"));

    const auto residency = coordinator.residency();
    const auto found = std::find_if(
        residency.begin(), residency.end(),
        [](const auto& item) { return item.name == "helper"; });
    REQUIRE(found != residency.end());
    CHECK(found->role == "helper");
    CHECK(found->compute == "cpu");
    CHECK(found->residency == "always");
    CHECK_FALSE(found->primary);
    CHECK_FALSE(found->eviction_eligible);

    const auto slot = coordinator.acquire_slot("helper");
    REQUIRE(slot);
    const auto identities = coordinator.identity_snapshot("helper-alias", "helper");
    CHECK(identities.requested == "helper-alias");
    CHECK(identities.resolved == "helper");
    CHECK(identities.selected == "conversation");
    CHECK(identities.resident == std::vector<std::string>{"conversation", "helper"});
    CHECK(identities.executing == std::vector<std::string>{"helper"});
    REQUIRE(coordinator.release_slot("helper", *slot));
}

TEST_CASE("ModelRegistry: impossible explicit resources are rejected",
          "[model][resources]") {
    ModelRegistry registry;
    auto helper = make_info("bad-helper");
    helper.role = ModelRole::Helper;
    helper.compute = ModelCompute::VulkanGpu;
    helper.residency = ResidencyPolicy::Always;
    helper.admission_pool = "helper";
    helper.concurrency_limit = 1;
    helper.eviction_eligible = false;
    helper.resource_metadata_explicit = true;
    CHECK_THROWS_AS(registry.register_model(helper), std::invalid_argument);
}

TEST_CASE("BackendCoordinator: admission pool limits span models",
          "[model][coordinator][resources][admission]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        return std::make_unique<IModelMock>(info);
    });
    const auto make_helper = [](const std::string& name) {
        auto info = make_info(name);
        info.role = ModelRole::Helper;
        info.compute = ModelCompute::Cpu;
        info.residency = ResidencyPolicy::Always;
        info.admission_pool = "helper";
        info.n_slots = 1;
        info.min_slots = 1;
        info.concurrency_limit = 1;
        info.eviction_eligible = false;
        info.resource_metadata_explicit = true;
        return info;
    };
    registry.register_model(make_helper("helper-a"));
    registry.register_model(make_helper("helper-b"));
    BackendCoordinator coordinator(registry);
    REQUIRE(coordinator.load("helper-a"));
    REQUIRE(coordinator.load("helper-b"));

    const auto first = coordinator.acquire_slot("helper-a");
    REQUIRE(first);
    AcquireSlotOptions immediate;
    immediate.block = false;
    const auto rejected = coordinator.acquire_slot("helper-b", immediate);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == ErrorCode::Unavailable);
    REQUIRE(coordinator.release_slot("helper-a", *first));
    const auto second = coordinator.acquire_slot("helper-b", immediate);
    REQUIRE(second);
    REQUIRE(coordinator.release_slot("helper-b", *second));
}
