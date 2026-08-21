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
