#include <catch2/catch_test_macros.hpp>

#include "model/backend_coordinator.hpp"
#include "model/imodel.hpp"
#include "model/model_registry.hpp"

using namespace inferdeck;
using namespace inferdeck::model;
using inferdeck::foundation::ErrorCode;

namespace {

class StubModel final : public IModel {
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
