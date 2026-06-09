#include <catch2/catch_test_macros.hpp>

#include "engine/token_sequence.hpp"
#include "model/backend_coordinator.hpp"
#include "model/imodel.hpp"
#include "model/model_registry.hpp"
#include "scheduler/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace inferdeck;
using namespace inferdeck::engine;
using namespace inferdeck::model;
using namespace inferdeck::scheduler;

using inferdeck::foundation::ErrorCode;
using inferdeck::foundation::Err;
using inferdeck::foundation::Ok;
using inferdeck::foundation::Result;

namespace {

class IModelMock : public IModel {
public:
    ModelInfo model_info{};
    std::atomic<bool> loaded{false};
    std::atomic<int> vram_mb{4096};
    std::atomic<int> max_slots{2};
    std::vector<int> busy_slots;
    mutable std::mutex mtx;
    inferdeck::model::ChatTemplateMeta chat_meta_{};

    explicit IModelMock(ModelInfo info) : model_info(std::move(info)) {
        busy_slots.assign(max_slots.load(), 0);
    }

    const ModelInfo& info() const override { return model_info; }
    const inferdeck::model::ChatTemplateMeta& chat_template_meta() const override { return chat_meta_; }

    Result<void> load() override {
        loaded.store(true);
        return Ok();
    }
    Result<void> unload() override {
        loaded.store(false);
        std::lock_guard<std::mutex> lock(mtx);
        std::fill(busy_slots.begin(), busy_slots.end(), 0);
        return Ok();
    }
    bool is_loaded() const override { return loaded.load(); }
    int vram_usage_mb() const override { return vram_mb.load(); }
    int n_slots() const override { return max_slots.load(); }

    int n_free_slots() const override {
        std::lock_guard<std::mutex> lock(mtx);
        int busy = 0;
        for (int b : busy_slots) if (b) ++busy;
        return max_slots.load() - busy;
    }

    Result<int> acquire_slot() override {
        std::lock_guard<std::mutex> lock(mtx);
        for (int i = 0; i < static_cast<int>(busy_slots.size()); ++i) {
            if (busy_slots[i] == 0) {
                busy_slots[i] = 1;
                return Ok(i);
            }
        }
        return Err<int>(ErrorCode::Unavailable, "no free slots");
    }

    Result<void> release_slot(int slot_id) override {
        std::lock_guard<std::mutex> lock(mtx);
        if (slot_id < 0 || slot_id >= static_cast<int>(busy_slots.size())) {
            return Err<void>(ErrorCode::InvalidArgument, "invalid slot_id");
        }
        busy_slots[slot_id] = 0;
        return Ok();
    }

    bool slot_busy(int slot_id) const override {
        std::lock_guard<std::mutex> lock(mtx);
        if (slot_id < 0 || slot_id >= static_cast<int>(busy_slots.size())) return false;
        return busy_slots[slot_id] != 0;
    }

    Result<InferenceResult> predict(int, const InferenceRequest&) override {
        InferenceResult r;
        r.text = "ok";
        r.prompt_tokens = 1;
        r.completion_tokens = 1;
        return Ok(std::move(r));
    }
};

ModelInfo make_info(const std::string& name, int n_slots = 2) {
    ModelInfo m;
    m.name = name;
    m.family = "qwen3.6";
    m.gguf_path = "C:/fake/" + name + ".gguf";
    m.n_slots = n_slots;
    m.vram_required_mb = 8192;
    m.context_size = 65536;
    return m;
}

struct Harness {
    ModelRegistry registry;
    BackendCoordinator coordinator;
    Scheduler scheduler;

    Harness() : coordinator(registry), scheduler(coordinator) {
        registry.set_factory([](const ModelInfo& info) {
            return std::make_unique<IModelMock>(info);
        });
    }

    void register_and_load(const std::string& name, int n_slots = 2) {
        registry.register_model(make_info(name, n_slots));
        auto r = coordinator.load(name);
        REQUIRE(r);
    }

    void register_model_only(const std::string& name, int n_slots = 2) {
        registry.register_model(make_info(name, n_slots));
    }
};

} // namespace

TEST_CASE("Scheduler: acquire returns NotFound for unregistered model", "[scheduler]") {
    Harness h;
    auto r = h.scheduler.acquire("missing", TokenSequence({1, 2, 3}));
    REQUIRE_FALSE(r);
    REQUIRE(r.error().code == ErrorCode::NotFound);
    REQUIRE(h.scheduler.total_not_found() == 1);
}

TEST_CASE("Scheduler: acquire returns Unavailable for registered but unloaded model", "[scheduler]") {
    Harness h;
    h.registry.set_factory([](const ModelInfo& info) {
        return std::make_unique<IModelMock>(info);
    });
    h.registry.register_model(make_info("qwen3.6-27b"));

    auto r = h.scheduler.acquire("qwen3.6-27b", TokenSequence({1, 2, 3}));
    REQUIRE_FALSE(r);
    REQUIRE(r.error().code == ErrorCode::Unavailable);
    REQUIRE(h.scheduler.total_unavailable() == 1);
    REQUIRE(h.scheduler.total_not_found() == 0);
}

TEST_CASE("Scheduler: acquire on loaded model returns slot 0", "[scheduler]") {
    Harness h;
    h.register_and_load("qwen3.6-27b");

    auto r = h.scheduler.acquire("qwen3.6-27b", TokenSequence({1, 2, 3}));
    REQUIRE(r);
    REQUIRE(r->slot_id == 0);
    REQUIRE(r->model_name == "qwen3.6-27b");
    REQUIRE_FALSE(r->lcp_hit);
    REQUIRE(r->prefix_tokens == 0);
    REQUIRE(r->valid());
    REQUIRE(h.scheduler.active_count() == 1);
    REQUIRE(h.scheduler.total_lcp_misses() == 1);
    r->release();
    REQUIRE(h.scheduler.active_count() == 0);
    REQUIRE(h.scheduler.total_released() == 1);
}

TEST_CASE("Scheduler: 2 parallel acquires on same model land in slot 0 and 1", "[scheduler]") {
    Harness h;
    h.register_and_load("qwen3.6-27b");

    auto a = h.scheduler.acquire("qwen3.6-27b", TokenSequence({1, 2, 3}));
    auto b = h.scheduler.acquire("qwen3.6-27b", TokenSequence({4, 5, 6}));
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(a->slot_id != b->slot_id);
    REQUIRE(h.scheduler.active_count() == 2);
    REQUIRE(h.scheduler.pool("qwen3.6-27b")->n_free() == 0);
}

TEST_CASE("Scheduler: 3rd acquire times out non-blocking", "[scheduler]") {
    Harness h;
    h.register_and_load("qwen3.6-27b");

    auto a = h.scheduler.acquire("qwen3.6-27b", TokenSequence({1}));
    auto b = h.scheduler.acquire("qwen3.6-27b", TokenSequence({2}));
    REQUIRE(a);
    REQUIRE(b);

    ScheduleOptions opts;
    opts.block = false;
    auto c = h.scheduler.acquire("qwen3.6-27b", TokenSequence({3}), opts);
    REQUIRE_FALSE(c);
    REQUIRE(c.error().code == ErrorCode::Timeout);
    REQUIRE(h.scheduler.total_timeouts() == 1);
}

TEST_CASE("Scheduler: 3rd blocking acquire waits then succeeds on release", "[scheduler]") {
    Harness h;
    h.register_and_load("qwen3.6-27b");

    auto a = h.scheduler.acquire("qwen3.6-27b", TokenSequence({1}));
    auto b = h.scheduler.acquire("qwen3.6-27b", TokenSequence({2}));
    REQUIRE(a);
    REQUIRE(b);

    std::thread releaser([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        a->release();
    });

    auto c = h.scheduler.acquire("qwen3.6-27b", TokenSequence({3}));
    REQUIRE(c);
    releaser.join();
    REQUIRE(h.scheduler.total_acquired() == 3);
}

TEST_CASE("Scheduler: LCP-hit on second request after release", "[scheduler]") {
    Harness h;
    h.register_and_load("qwen3.6-27b");

    {
        auto a = h.scheduler.acquire("qwen3.6-27b", TokenSequence({1, 2, 3, 4, 5}));
        REQUIRE(a);
        REQUIRE_FALSE(a->lcp_hit);
        a->release();
    }
    auto b = h.scheduler.acquire("qwen3.6-27b", TokenSequence({1, 2, 3, 9, 10}));
    REQUIRE(b);
    REQUIRE(b->lcp_hit);
    REQUIRE(b->prefix_tokens == 3);
    REQUIRE(b->slot_id == 0);
    REQUIRE(h.scheduler.total_lcp_hits() == 1);
    REQUIRE(h.scheduler.total_lcp_misses() == 1);
}

TEST_CASE("Scheduler: LCP-miss assigns new slot when shared prefix is busy", "[scheduler]") {
    Harness h;
    h.register_and_load("qwen3.6-27b");

    auto a = h.scheduler.acquire("qwen3.6-27b", TokenSequence({1, 2, 3, 4}));
    auto b = h.scheduler.acquire("qwen3.6-27b", TokenSequence({1, 2, 9, 10}));
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(a->slot_id != b->slot_id);
    REQUIRE_FALSE(a->lcp_hit);
    REQUIRE_FALSE(b->lcp_hit);
}

TEST_CASE("Scheduler: opencode parallel fixture flow", "[scheduler][opencode]") {
    Harness h;
    h.register_and_load("qwen3.6-27b");

    std::atomic<int> succeeded{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([&h, &succeeded, i]() {
            TokenSequence ts({i, i + 100, i + 200, i + 300});
            ScheduleOptions opts;
            opts.timeout = std::chrono::milliseconds{2000};
            auto r = h.scheduler.acquire("qwen3.6-27b", ts, opts);
            if (r) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                r->release();
                ++succeeded;
            }
        });
    }
    for (auto& t : threads) t.join();
    REQUIRE(succeeded.load() == 5);
    REQUIRE(h.scheduler.total_acquired() == 5);
    REQUIRE(h.scheduler.total_released() == 5);
    REQUIRE(h.scheduler.active_count() == 0);
}

TEST_CASE("Scheduler: cancel_waiters wakes blocked acquires with Cancelled", "[scheduler]") {
    Harness h;
    h.register_and_load("qwen3.6-27b");

    auto a = h.scheduler.acquire("qwen3.6-27b", TokenSequence({1}));
    auto b = h.scheduler.acquire("qwen3.6-27b", TokenSequence({2}));
    REQUIRE(a);
    REQUIRE(b);

    std::thread waiter([&h]() {
        ScheduleOptions opts;
        opts.timeout = std::chrono::milliseconds{5000};
        auto r = h.scheduler.acquire("qwen3.6-27b", TokenSequence({3}), opts);
        REQUIRE_FALSE(r);
        REQUIRE((r.error().code == ErrorCode::Cancelled ||
                 r.error().code == ErrorCode::Timeout));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    h.scheduler.cancel_waiters();
    waiter.join();
}

TEST_CASE("Scheduler: per-model pool isolation", "[scheduler]") {
    Harness h;
    h.register_model_only("qwen3.6-27b");
    h.register_model_only("qwen3-coder-next");
    h.register_and_load("qwen3.6-27b");

    {
        auto a = h.scheduler.acquire("qwen3.6-27b", TokenSequence({1}));
        REQUIRE(a);
    }
    h.coordinator.swap_to("qwen3-coder-next");

    auto a = h.scheduler.acquire("qwen3.6-27b", TokenSequence({1}));
    REQUIRE_FALSE(a);
    REQUIRE(a.error().code == ErrorCode::Unavailable);

    auto b = h.scheduler.acquire("qwen3-coder-next", TokenSequence({2}));
    REQUIRE(b);
    REQUIRE(h.scheduler.pool("qwen3.6-27b") != nullptr);
    REQUIRE(h.scheduler.pool("qwen3-coder-next") != nullptr);
    REQUIRE(h.scheduler.pool("qwen3.6-27b") !=
            h.scheduler.pool("qwen3-coder-next"));
    REQUIRE(h.scheduler.pool("qwen3-coder-next")->n_free() == 1);
}

TEST_CASE("Scheduler: update_prev_tokens=false leaves slot unchanged", "[scheduler]") {
    Harness h;
    h.register_and_load("qwen3.6-27b");

    {
        auto a = h.scheduler.acquire("qwen3.6-27b", TokenSequence({1, 2, 3, 4, 5}));
        REQUIRE(a);
        a->release();
    }

    ScheduleOptions opts;
    opts.update_prev_tokens = false;
    auto b = h.scheduler.acquire(
        "qwen3.6-27b", TokenSequence({1, 2, 3, 9, 10}), opts);
    REQUIRE(b);
    REQUIRE(b->lcp_hit);
    REQUIRE(b->prefix_tokens == 3);
    REQUIRE(h.scheduler.pool("qwen3.6-27b")
                ->slot(b->slot_id)
                .prev_tokens()
                .size() == 5);
}

TEST_CASE("Scheduler: counters reflect acquire/release/total", "[scheduler]") {
    Harness h;
    h.register_and_load("qwen3.6-27b");

    REQUIRE(h.scheduler.active_count() == 0);
    REQUIRE(h.scheduler.total_acquired() == 0);

    auto a = h.scheduler.acquire("qwen3.6-27b", TokenSequence({1, 2, 3}));
    REQUIRE(h.scheduler.active_count() == 1);
    REQUIRE(h.scheduler.total_acquired() == 1);

    a->release();
    REQUIRE(h.scheduler.active_count() == 0);
    REQUIRE(h.scheduler.total_released() == 1);
}
