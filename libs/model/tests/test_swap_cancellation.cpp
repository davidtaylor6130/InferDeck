#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <thread>

#include "model/backend_coordinator.hpp"
#include "model/imodel.hpp"
#include "model/model_registry.hpp"

using namespace inferdeck;
using namespace inferdeck::model;
using inferdeck::foundation::ErrorCode;

namespace {

class StubModel : public IModel {
public:
  StubModel(ModelInfo info, std::atomic<int>* acquire_count)
      : info_(std::move(info)), acquire_count_(acquire_count) {}

  const ModelInfo& info() const noexcept override { return info_; }
  const ChatTemplateMeta& chat_template_meta() const noexcept override { return chat_meta_; }
  foundation::Result<void> load() override { loaded_.store(true); return foundation::Ok(); }
  foundation::Result<void> unload() override { loaded_.store(false); return foundation::Ok(); }
  bool is_loaded() const noexcept override { return loaded_.load(); }
  int vram_usage_mb() const noexcept override { return info_.vram_required_mb; }
  int n_slots() const noexcept override { return info_.n_slots; }
  int n_free_slots() const noexcept override { return info_.n_slots; }
  foundation::Result<int> acquire_slot() override {
    if (acquire_count_) acquire_count_->fetch_add(1);
    return foundation::Result<int>(0);
  }
  foundation::Result<void> release_slot(int) override { return foundation::Ok(); }
  bool slot_busy(int) const noexcept override { return false; }
  foundation::Result<InferenceResult> predict(int, const InferenceRequest&) override {
    return foundation::Result<InferenceResult>(std::unexpect,
        inferdeck::foundation::Error{ErrorCode::Internal, "stub"});
  }
private:
  ModelInfo info_;
  std::atomic<bool> loaded_{false};
  std::atomic<int>* acquire_count_;
  ChatTemplateMeta chat_meta_{};
};

class BlockingLoadModel final : public StubModel {
public:
  BlockingLoadModel(ModelInfo info, std::atomic<int>* acquire_count,
                    std::atomic<bool>* entered)
      : StubModel(std::move(info), acquire_count), entered_(entered) {}

  foundation::Result<void> load(const LifecycleControl& control) override {
    entered_->store(true);
    while (!control.is_cancelled() && !control.is_expired()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (control.is_cancelled()) {
      return foundation::Err<void>(ErrorCode::Cancelled,
                                    "blocking load cancelled");
    }
    return foundation::Err<void>(ErrorCode::Timeout,
                                  "blocking load timed out");
  }

private:
  std::atomic<bool>* entered_;
};

class BlockingUnloadModel final : public StubModel {
public:
  BlockingUnloadModel(ModelInfo info, std::atomic<int>* acquire_count,
                      std::atomic<bool>* entered)
      : StubModel(std::move(info), acquire_count), entered_(entered) {}

  foundation::Result<void> unload(const LifecycleControl& control) override {
    entered_->store(true);
    while (!control.is_cancelled() && !control.is_expired()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return foundation::Err<void>(
        control.is_cancelled() ? ErrorCode::Cancelled : ErrorCode::Timeout,
        "blocking unload stopped");
  }

private:
  std::atomic<bool>* entered_;
};

class BlockingResizeModel final : public StubModel {
public:
  BlockingResizeModel(ModelInfo info, std::atomic<int>* acquire_count,
                      std::atomic<bool>* entered)
      : StubModel(info, acquire_count),
        slots_(info.n_slots),
        min_slots_(info.min_slots),
        fixed_mb_(info.vram_fixed_mb),
        per_slot_mb_(info.vram_per_slot_mb),
        entered_(entered) {}

  int n_slots() const noexcept override { return slots_.load(); }
  int n_free_slots() const noexcept override { return slots_.load(); }
  int min_slots() const noexcept override { return min_slots_; }
  bool can_resize_slots() const noexcept override { return true; }
  int estimate_vram_mb(int slots) const noexcept override {
    return fixed_mb_ + per_slot_mb_ * slots;
  }
  int vram_usage_mb() const noexcept override {
    return estimate_vram_mb(slots_.load());
  }
  foundation::Result<void> resize_slots(
      int, const LifecycleControl& control) override {
    entered_->store(true);
    while (!control.is_cancelled() && !control.is_expired()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return foundation::Err<void>(
        control.is_cancelled() ? ErrorCode::Cancelled : ErrorCode::Timeout,
        "blocking resize stopped");
  }

private:
  std::atomic<int> slots_;
  int min_slots_;
  int fixed_mb_;
  int per_slot_mb_;
  std::atomic<bool>* entered_;
};

}

TEST_CASE("BackendCoordinator: swap cancellation flag", "[coordinator][cancel]") {
  ModelRegistry reg;
  std::atomic<int> acq{0};
  reg.set_factory([&](const ModelInfo& info) {
    return std::make_unique<StubModel>(info, &acq);
  });
  ModelInfo info;
  info.name = "m1";
  info.gguf_path = "C:/fake.gguf";
  info.n_slots = 1;
  reg.register_model(info);
  BackendCoordinator c(reg);
  REQUIRE_FALSE(c.swap_in_progress());
  c.request_swap_cancel();
  REQUIRE(c.swap_cancel_requested());
  c.reset_swap_cancel();
  REQUIRE_FALSE(c.swap_cancel_requested());
}

TEST_CASE("BackendCoordinator: swap_to_cancellable honors cancel before load", "[coordinator][cancel]") {
  ModelRegistry reg;
  std::atomic<int> acq{0};
  reg.set_factory([&](const ModelInfo& info) {
    return std::make_unique<StubModel>(info, &acq);
  });
  ModelInfo info;
  info.name = "m1";
  info.gguf_path = "C:/fake.gguf";
  info.n_slots = 1;
  reg.register_model(info);
  BackendCoordinator c(reg);
  c.request_swap_cancel();
  auto r = c.swap_to_cancellable("m1");
  REQUIRE(!r.has_value());
  REQUIRE(r.error().code == ErrorCode::Cancelled);
  REQUIRE_FALSE(c.swap_in_progress());
}

TEST_CASE("BackendCoordinator: swap_to_cancellable resets cancel flag", "[coordinator][cancel]") {
  ModelRegistry reg;
  std::atomic<int> acq{0};
  reg.set_factory([&](const ModelInfo& info) {
    return std::make_unique<StubModel>(info, &acq);
  });
  ModelInfo info;
  info.name = "m1";
  info.gguf_path = "C:/fake.gguf";
  info.n_slots = 1;
  reg.register_model(info);
  BackendCoordinator c(reg);
  c.request_swap_cancel();
  auto r = c.swap_to_cancellable("m1");
  REQUIRE(!r.has_value());
  REQUIRE_FALSE(c.swap_cancel_requested());
}

TEST_CASE("BackendCoordinator: swap_in_progress true during swap", "[coordinator][cancel]") {
  ModelRegistry reg;
  std::atomic<int> acq{0};
  reg.set_factory([&](const ModelInfo& info) {
    return std::make_unique<StubModel>(info, &acq);
  });
  ModelInfo info;
  info.name = "m1";
  info.gguf_path = "C:/fake.gguf";
  info.n_slots = 1;
  reg.register_model(info);
  BackendCoordinator c(reg);
  REQUIRE(c.load("m1").has_value());
  REQUIRE(c.load("m1").has_value());
  REQUIRE(c.swap_to_cancellable("m1").has_value());
  REQUIRE_FALSE(c.swap_in_progress());
}

TEST_CASE("BackendCoordinator: swap deadline bounds a blocking load",
          "[coordinator][cancel][deadline]") {
  ModelRegistry reg;
  std::atomic<int> acq{0};
  std::atomic<bool> entered{false};
  reg.set_factory([&](const ModelInfo& info) {
    return std::make_unique<BlockingLoadModel>(info, &acq, &entered);
  });
  ModelInfo info;
  info.name = "blocked";
  info.gguf_path = "C:/fake.gguf";
  info.n_slots = 1;
  reg.register_model(info);
  BackendCoordinator c(reg);

  const auto started = std::chrono::steady_clock::now();
  const auto result = c.swap_to_cancellable(
      "blocked", std::chrono::milliseconds{40});
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == ErrorCode::Timeout);
  REQUIRE(entered.load());
  REQUIRE(elapsed < std::chrono::milliseconds{250});
  REQUIRE_FALSE(c.swap_in_progress());
}

TEST_CASE("BackendCoordinator: swap cancellation reaches a blocking load",
          "[coordinator][cancel][deadline]") {
  ModelRegistry reg;
  std::atomic<int> acq{0};
  std::atomic<bool> entered{false};
  reg.set_factory([&](const ModelInfo& info) {
    return std::make_unique<BlockingLoadModel>(info, &acq, &entered);
  });
  ModelInfo info;
  info.name = "blocked";
  info.gguf_path = "C:/fake.gguf";
  info.n_slots = 1;
  reg.register_model(info);
  BackendCoordinator c(reg);

  auto future = std::async(std::launch::async, [&] {
    return c.swap_to_cancellable("blocked", std::chrono::seconds{5});
  });
  const auto entered_deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds{250};
  while (!entered.load() && std::chrono::steady_clock::now() < entered_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(entered.load());
  c.request_swap_cancel();
  REQUIRE(future.wait_for(std::chrono::milliseconds{250}) ==
          std::future_status::ready);
  const auto result = future.get();
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == ErrorCode::Cancelled);
  REQUIRE_FALSE(c.swap_in_progress());
  REQUIRE_FALSE(c.swap_cancel_requested());
}

TEST_CASE("BackendCoordinator: swap deadline bounds a blocking eviction unload",
          "[coordinator][cancel][deadline][residency]") {
  ModelRegistry reg;
  std::atomic<int> acq{0};
  std::atomic<bool> entered{false};
  reg.set_factory([&](const ModelInfo& info) -> std::unique_ptr<IBackend> {
    if (info.name == "resident") {
      return std::make_unique<BlockingUnloadModel>(info, &acq, &entered);
    }
    return std::make_unique<StubModel>(info, &acq);
  });
  ModelInfo resident;
  resident.name = "resident";
  resident.gguf_path = "C:/resident.gguf";
  resident.n_slots = 1;
  resident.vram_required_mb = 5000;
  ModelInfo target = resident;
  target.name = "target";
  target.gguf_path = "C:/target.gguf";
  reg.register_model(resident);
  reg.register_model(target);
  BackendCoordinator c(reg);
  c.set_vram_budget(9000, 0);
  REQUIRE(c.swap_to("resident"));

  const auto started = std::chrono::steady_clock::now();
  const auto result = c.swap_to_cancellable(
      "target", std::chrono::milliseconds{40});
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == ErrorCode::Timeout);
  REQUIRE(entered.load());
  REQUIRE(elapsed < std::chrono::milliseconds{250});
  REQUIRE(c.is_loaded("resident"));
  REQUIRE_FALSE(c.is_loaded("target"));
}

TEST_CASE("BackendCoordinator: swap deadline bounds a blocking capacity resize",
          "[coordinator][cancel][deadline][residency]") {
  ModelRegistry reg;
  std::atomic<int> acq{0};
  std::atomic<bool> entered{false};
  reg.set_factory([&](const ModelInfo& info) -> std::unique_ptr<IBackend> {
    if (info.name == "resident") {
      return std::make_unique<BlockingResizeModel>(info, &acq, &entered);
    }
    return std::make_unique<StubModel>(info, &acq);
  });
  ModelInfo resident;
  resident.name = "resident";
  resident.gguf_path = "C:/resident.gguf";
  resident.n_slots = 2;
  resident.min_slots = 1;
  resident.vram_fixed_mb = 3000;
  resident.vram_per_slot_mb = 1000;
  resident.vram_required_mb = 5000;
  ModelInfo target = resident;
  target.name = "target";
  target.gguf_path = "C:/target.gguf";
  target.n_slots = 1;
  target.min_slots = 1;
  target.vram_fixed_mb = 0;
  target.vram_per_slot_mb = 0;
  reg.register_model(resident);
  reg.register_model(target);
  BackendCoordinator c(reg);
  c.set_vram_budget(9000, 0);
  REQUIRE(c.swap_to("resident"));

  const auto started = std::chrono::steady_clock::now();
  const auto result = c.swap_to_cancellable(
      "target", std::chrono::milliseconds{40});
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == ErrorCode::Timeout);
  REQUIRE(entered.load());
  REQUIRE(elapsed < std::chrono::milliseconds{250});
  REQUIRE(c.is_loaded("resident"));
  REQUIRE(c.get_backend("resident")->n_slots() == 2);
  REQUIRE_FALSE(c.is_loaded("target"));
}
