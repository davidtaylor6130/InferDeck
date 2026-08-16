#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "llama_cpp_wrapper/llama_cpp_model.hpp"
#include "llama_cpp_wrapper/streaming_tool_call_state.hpp"
#include "foundation/logging.hpp"
#include "model/imodel.hpp"

using namespace inferdeck;
using namespace inferdeck::llama_wrapper;
using namespace inferdeck::model;
using inferdeck::foundation::ErrorCode;

namespace {

std::filesystem::path write_fake_gguf(const std::filesystem::path& dir) {
  const auto path = dir / "fake.gguf";
  std::ofstream f(path, std::ios::binary);
  f << "GGUF";
  return path;
}

}

TEST_CASE("LlamaCppModel: version string non-empty", "[llama][meta]") {
  REQUIRE_FALSE(LlamaCppModel::version().empty());
}

TEST_CASE("LlamaCppModel: backend init/shutdown do not throw", "[llama][backend]") {
  REQUIRE_NOTHROW(LlamaCppModel::init_backend());
  REQUIRE_NOTHROW(LlamaCppModel::shutdown_backend());
}

TEST_CASE("LlamaCppModel: missing gguf returns NotFound error", "[llama][load]") {
  LlamaCppModel::init_backend();
  ModelInfo info;
  info.name = "fake";
  info.gguf_path = "C:/no/such/file/anywhere/zzz.gguf";
  info.n_slots = 1;
  info.context_size = 512;
  info.vram_required_mb = 100;
  LlamaCppModel m(std::move(info));
  auto r = m.load();
  REQUIRE(!r.has_value());
  REQUIRE(r.error().code == ErrorCode::NotFound);
  REQUIRE_FALSE(m.is_loaded());
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("LlamaCppModel: invalid gguf content returns Internal", "[llama][load]") {
  LlamaCppModel::init_backend();
  const auto dir = std::filesystem::temp_directory_path() / "inferdeck_llama_test_xxx";
  std::filesystem::create_directories(dir);
  const auto path = write_fake_gguf(dir);
  ModelInfo info;
  info.name = "fake";
  info.gguf_path = path.string();
  info.n_slots = 1;
  info.context_size = 512;
  info.vram_required_mb = 100;
  LlamaCppModel m(std::move(info));
  auto r = m.load();
  REQUIRE(!r.has_value());
  REQUIRE_FALSE(m.is_loaded());
  std::filesystem::remove_all(dir);
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("LlamaCppModel: empty gguf_path returns NotFound", "[llama][load]") {
  LlamaCppModel::init_backend();
  ModelInfo info;
  info.name = "x";
  info.gguf_path = "";
  LlamaCppModel m(std::move(info));
  auto r = m.load();
  REQUIRE(!r.has_value());
  REQUIRE(r.error().code == ErrorCode::NotFound);
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("LlamaCppModel: vision requires an existing projector", "[llama][vision][load]") {
  const auto dir = std::filesystem::temp_directory_path() /
      "inferdeck_llama_vision_test";
  std::filesystem::create_directories(dir);
  const auto gguf = write_fake_gguf(dir);

  ModelInfo missing_path;
  missing_path.name = "vision-missing-path";
  missing_path.gguf_path = gguf.string();
  missing_path.has_vision = true;
  LlamaCppModel without_path(missing_path);
  const auto no_path = without_path.load();
  REQUIRE_FALSE(no_path.has_value());
  CHECK(no_path.error().code == ErrorCode::NotFound);
  CHECK(no_path.error().message == "vision model has no mmproj_path");

  ModelInfo missing_file = missing_path;
  missing_file.name = "vision-missing-file";
  missing_file.mmproj_path = (dir / "missing-mmproj.gguf").string();
  LlamaCppModel without_file(missing_file);
  const auto no_file = without_file.load();
  REQUIRE_FALSE(no_file.has_value());
  CHECK(no_file.error().code == ErrorCode::NotFound);
  CHECK(no_file.error().message.starts_with("mmproj not found:"));

  std::filesystem::remove_all(dir);
}

TEST_CASE("LlamaCppModel: info() returns registered info", "[llama][info]") {
  ModelInfo info;
  info.name = "test-model";
  info.family = "qwen3";
  info.n_slots = 4;
  info.context_size = 1024;
  info.vram_required_mb = 8192;
  LlamaCppModel m(info);
  const auto& out = m.info();
  REQUIRE(out.name == "test-model");
  REQUIRE(out.family == "qwen3");
  REQUIRE(out.n_slots == 4);
  REQUIRE(out.context_size == 1024);
  REQUIRE(m.n_slots() == 4);
  REQUIRE(m.vram_usage_mb() == 8192);
}

TEST_CASE("LlamaCppModel: predict before load returns error", "[llama][predict]") {
  ModelInfo info;
  info.name = "test";
  info.gguf_path = "C:/no/such/path.gguf";
  info.n_slots = 1;
  LlamaCppModel m(info);
  InferenceRequest req;
  req.prompt = "hello";
  auto r = m.predict(0, req);
  REQUIRE(!r.has_value());
}

TEST_CASE("LlamaCppModel: predict with invalid slot_id returns error", "[llama][predict]") {
  ModelInfo info;
  info.name = "test";
  info.gguf_path = "C:/no/such/path.gguf";
  info.n_slots = 1;
  LlamaCppModel m(info);
  InferenceRequest req;
  req.prompt = "hello";
  auto r1 = m.predict(-1, req);
  REQUIRE(!r1.has_value());
  REQUIRE(r1.error().code == ErrorCode::InvalidArgument);
  auto r2 = m.predict(99, req);
  REQUIRE(!r2.has_value());
  REQUIRE(r2.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("LlamaCppModel: slot management when not loaded", "[llama][slots]") {
  ModelInfo info;
  info.name = "x";
  info.gguf_path = "C:/no/path.gguf";
  info.n_slots = 2;
  LlamaCppModel m(info);
  REQUIRE(m.n_free_slots() == 0);
  auto r = m.acquire_slot();
  REQUIRE(!r.has_value());
  REQUIRE(r.error().code == ErrorCode::Internal);
}

TEST_CASE("LlamaCppModel: release_slot with invalid id returns error", "[llama][slots]") {
  ModelInfo info;
  info.name = "x";
  info.gguf_path = "C:/no/path.gguf";
  info.n_slots = 2;
  LlamaCppModel m(info);
  auto r1 = m.release_slot(-1);
  REQUIRE(!r1.has_value());
  REQUIRE(r1.error().code == ErrorCode::InvalidArgument);
  auto r2 = m.release_slot(5);
  REQUIRE(!r2.has_value());
}

TEST_CASE("LlamaCppModel: double load is a no-op", "[llama][load]") {
  LlamaCppModel::init_backend();
  ModelInfo info;
  info.name = "test";
  info.gguf_path = "C:/no/such/file.gguf";
  info.n_slots = 1;
  LlamaCppModel m(info);
  auto r1 = m.load();
  REQUIRE(!r1.has_value());
  auto r2 = m.load();
  REQUIRE(!r2.has_value());
  REQUIRE(r2.error().code == ErrorCode::NotFound);
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("LlamaCppModel: unload when not loaded is no-op", "[llama][unload]") {
  ModelInfo info;
  info.name = "x";
  info.gguf_path = "C:/no/path.gguf";
  LlamaCppModel m(info);
  auto r = m.unload();
  REQUIRE(r.has_value());
  REQUIRE_FALSE(m.is_loaded());
}

TEST_CASE("LlamaCppModel: slot_busy out-of-range returns false", "[llama][slots]") {
  ModelInfo info;
  info.name = "x";
  info.gguf_path = "C:/no/path.gguf";
  info.n_slots = 2;
  LlamaCppModel m(info);
  REQUIRE_FALSE(m.slot_busy(-1));
  REQUIRE_FALSE(m.slot_busy(99));
  REQUIRE_FALSE(m.slot_busy(0));
}

TEST_CASE("StreamingToolCallState suppresses fallback after a tool delta", "[llama][streaming]") {
  llama_wrapper::detail::StreamingToolCallState state;
  InferenceDelta content;
  content.content = "answer";
  state.observe(content);
  REQUIRE(state.should_emit_fallback());

  InferenceDelta reasoning;
  reasoning.reasoning_text = "thinking";
  state.observe(reasoning);
  REQUIRE(state.should_emit_fallback());

  InferenceDelta tool;
  tool.tool_calls.push_back(ToolCallDelta{0, "call_1", "function", "lookup", "{"});
  state.observe(tool);
  REQUIRE_FALSE(state.should_emit_fallback());
}

TEST_CASE("ContinuousBatchScheduler rejects work after stopping", "[llama][scheduler]") {
  ContinuousBatchScheduler scheduler(nullptr, nullptr, nullptr, 1);
  scheduler.stop();

  SlotTask task;
  scheduler.submit(&task);

  std::unique_lock lock(task.out_mtx);
  REQUIRE(task.out_cv.wait_for(
      lock, std::chrono::seconds(1), [&task] { return !task.out_queue.empty(); }));
  const auto event = std::move(task.out_queue.front());
  REQUIRE(event.is_done);
  REQUIRE(event.is_error);
  REQUIRE(event.error_msg == "scheduler stopped");
}

TEST_CASE("Adaptive MTP is limited to the configured low-concurrency window",
          "[llama][scheduler][mtp]") {
  using inferdeck::llama_wrapper::detail::adaptive_mtp_enabled;
  using inferdeck::llama_wrapper::detail::adaptive_mtp_request_eligible;

  CHECK_FALSE(adaptive_mtp_enabled(false, 1, 1));
  CHECK_FALSE(adaptive_mtp_enabled(true, 0, 1));
  CHECK(adaptive_mtp_enabled(true, 1, 1));
  CHECK_FALSE(adaptive_mtp_enabled(true, 2, 1));
  CHECK(adaptive_mtp_enabled(true, 2, 2));
  CHECK(adaptive_mtp_request_eligible(true, true, 1, 1));
  CHECK_FALSE(adaptive_mtp_request_eligible(true, true, 2, 1));
  CHECK_FALSE(adaptive_mtp_request_eligible(false, true, 1, 1));
}

TEST_CASE("ContinuousBatchScheduler generation limit includes the current token",
          "[llama][scheduler]") {
  REQUIRE(detail::generation_limit_reached(0, 1));
  REQUIRE_FALSE(detail::generation_limit_reached(0, 2));
  REQUIRE(detail::generation_limit_reached(1, 2));
  REQUIRE(detail::generation_limit_reached(0, 0));
}

TEST_CASE("Generation TPS excludes prompt prefill duration",
          "[llama][scheduler][metrics]") {
  using inferdeck::llama_wrapper::detail::generation_tokens_per_second;

  CHECK(generation_tokens_per_second(200, 4000.0f) == 50.0f);
  CHECK(generation_tokens_per_second(0, 4000.0f) == 0.0f);
  CHECK(generation_tokens_per_second(200, 0.0f) == 0.0f);
}

TEST_CASE("Recurrent checkpoint eligibility requires a reusable prefix",
          "[llama][scheduler][recurrent]") {
  REQUIRE(detail::recurrent_checkpoint_usable(4096, 128, 256, 512));
  REQUIRE(detail::recurrent_checkpoint_usable(4096, 128, 128, 128));
  REQUIRE_FALSE(detail::recurrent_checkpoint_usable(0, 128, 256, 512));
  REQUIRE_FALSE(detail::recurrent_checkpoint_usable(4096, 0, 256, 512));
  REQUIRE_FALSE(detail::recurrent_checkpoint_usable(4096, 257, 256, 512));
  REQUIRE_FALSE(detail::recurrent_checkpoint_usable(4096, 513, 600, 512));
}

TEST_CASE("Recurrent checkpoint capture stops before the generation prompt",
          "[llama][scheduler][recurrent]") {
  const std::vector<llama_token> prompt{1, 2, 3, 4, 5};
  const std::vector<llama_token> stable_prefix{1, 2, 3};
  REQUIRE(detail::recurrent_checkpoint_capture_pos(prompt, stable_prefix) == 3);

  const std::vector<llama_token> divergent_prefix{1, 2, 9, 4};
  REQUIRE(detail::recurrent_checkpoint_capture_pos(prompt, divergent_prefix) == 2);
}

TEST_CASE("SlotTask keeps checkpoint storage alive",
          "[llama][scheduler][recurrent]") {
  auto checkpoint = std::make_shared<std::vector<uint8_t>>(32, 7);
  auto draft_checkpoint = std::make_shared<std::vector<uint8_t>>(16, 5);
  auto mtp_checkpoint = std::make_shared<std::vector<uint8_t>>(8, 3);
  SlotTask task;
  task.recurrent_checkpoint = checkpoint;
  task.recurrent_draft_checkpoint = draft_checkpoint;
  task.recurrent_mtp_checkpoint = mtp_checkpoint;
  checkpoint.reset();
  draft_checkpoint.reset();
  mtp_checkpoint.reset();
  REQUIRE(task.recurrent_checkpoint);
  REQUIRE(task.recurrent_checkpoint->size() == 32);
  REQUIRE(task.recurrent_checkpoint->front() == 7);
  REQUIRE(task.recurrent_draft_checkpoint);
  REQUIRE(task.recurrent_draft_checkpoint->size() == 16);
  REQUIRE(task.recurrent_draft_checkpoint->front() == 5);
  REQUIRE(task.recurrent_mtp_checkpoint);
  REQUIRE(task.recurrent_mtp_checkpoint->size() == 8);
  REQUIRE(task.recurrent_mtp_checkpoint->front() == 3);
}

// ---------------------------------------------------------------------------
// Recurrent-checkpoint tests — require a real model.
// Run with: ctest -L unit -R "recurrent" --tests-regex . -V
// Or explicitly: ./llama_cpp_model_tests "[requires_model]"
// ---------------------------------------------------------------------------

namespace {
// Returns the gguf path from INFERDECK_TEST_MODEL env var, or empty string.
std::string test_model_path() {
  const char* p = std::getenv("INFERDECK_TEST_MODEL");
  return p ? std::string(p) : std::string{};
}

std::string test_mtp_model_path() {
  const char* p = std::getenv("INFERDECK_TEST_MTP_MODEL");
  return p ? std::string(p) : std::string{};
}

struct ScopedTestLogger {
  ScopedTestLogger() {
    foundation::LogConfig config;
    config.level = foundation::LogLevel::Debug;
    foundation::Logger::instance().initialize(config);
  }
  ~ScopedTestLogger() {
    foundation::Logger::instance().shutdown();
  }
};
}  // namespace

TEST_CASE("recurrent checkpoint: second predict on same slot reuses cache",
          "[llama][recurrent][.][requires_model]") {
  const auto gguf = test_model_path();
  if (gguf.empty()) SKIP("INFERDECK_TEST_MODEL not set");
  ScopedTestLogger logger;

  LlamaCppModel::init_backend();
  ModelInfo minfo;
  minfo.name            = "test-checkpoint-noop";
  minfo.gguf_path       = gguf;
  minfo.n_slots         = 1;
  minfo.context_size    = 512;
  minfo.vram_required_mb = 0;
  LlamaCppModel lm(minfo);
  REQUIRE(lm.load().has_value());

  auto slot_r = lm.acquire_slot();
  REQUIRE(slot_r.has_value());
  const int slot_id = slot_r.value();

  InferenceRequest req;
  req.messages = {ChatMessage{"user", "Say hello."}};
  req.max_tokens = 4;
  auto r1 = lm.predict(slot_id, req);
  REQUIRE(r1.has_value());

  // Second identical request: for full-attention models this exercises KV reuse;
  // for recurrent models it exercises the checkpoint no-op path (size == 0).
  // Either way the result must succeed and not crash.
  auto r2 = lm.predict(slot_id, req);
  REQUIRE(r2.has_value());
  REQUIRE(r2->cached_prompt_tokens > 0);

  (void)lm.release_slot(slot_id);
  (void)lm.unload();
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("recurrent checkpoint: multi-turn conversation reuses cache on second turn",
          "[llama][recurrent][.][requires_model]") {
  const auto gguf = test_model_path();
  if (gguf.empty()) SKIP("INFERDECK_TEST_MODEL not set");
  ScopedTestLogger logger;

  LlamaCppModel::init_backend();
  ModelInfo minfo;
  minfo.name            = "test-checkpoint-restore";
  minfo.gguf_path       = gguf;
  minfo.n_slots         = 1;
  minfo.context_size    = 512;
  minfo.vram_required_mb = 0;
  LlamaCppModel lm(minfo);
  REQUIRE(lm.load().has_value());

  auto slot_r = lm.acquire_slot();
  REQUIRE(slot_r.has_value());
  const int slot_id = slot_r.value();

  InferenceRequest req1;
  req1.messages = {ChatMessage{"user", "Count to three."}};
  req1.max_tokens = 8;
  auto r1 = lm.predict(slot_id, req1);
  REQUIRE(r1.has_value());

  // Second turn extends the conversation. For recurrent models this hits the
  // checkpoint restore path; for full-attention models it hits normal seq_rm.
  // cached_prompt_tokens > 0 confirms neither path did a full re-prefill from 0.
  InferenceRequest req2;
  req2.messages = {
      ChatMessage{"user",      "Count to three."},
      ChatMessage{"assistant", r1->text},
      ChatMessage{"user",      "Now count to five."},
  };
  req2.max_tokens = 8;
  auto r2 = lm.predict(slot_id, req2);
  REQUIRE(r2.has_value());
  REQUIRE(r2->cached_prompt_tokens > 0);

  (void)lm.release_slot(slot_id);
  (void)lm.unload();
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("MTP recurrent checkpoint reuses an identical prompt",
          "[llama][recurrent][mtp][.][requires_mtp_model]") {
  const auto gguf = test_mtp_model_path();
  if (gguf.empty()) SKIP("INFERDECK_TEST_MTP_MODEL not set");
  ScopedTestLogger logger;

  LlamaCppModel::init_backend();
  ModelInfo minfo;
  minfo.name             = "test-mtp-checkpoint";
  minfo.gguf_path        = gguf;
  minfo.n_slots          = 1;
  minfo.context_size     = 512;
  minfo.vram_required_mb = 0;
  LlamaCppConfig config;
  config.n_batch = 512;
  config.n_ubatch = 512;
  config.cache_type_k = "q4_0";
  config.cache_type_v = "q4_0";
  config.mtp_enabled = true;
  config.mtp_draft_tokens = 2;
  config.mtp_max_active_requests = 1;
  LlamaCppModel lm(minfo, config);
  REQUIRE(lm.load().has_value());

  auto slot_r = lm.acquire_slot();
  REQUIRE(slot_r.has_value());
  const int slot_id = slot_r.value();

  InferenceRequest req;
  req.messages = {ChatMessage{"user", "Reply with only CACHE_OK."}};
  req.max_tokens = 4;
  auto first = lm.predict(slot_id, req);
  REQUIRE(first.has_value());
  auto second = lm.predict(slot_id, req);
  REQUIRE(second.has_value());
  REQUIRE(second->cached_prompt_tokens > 0);

  (void)lm.release_slot(slot_id);
  (void)lm.unload();
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("MTP cancellation is safe while another slot decodes",
          "[llama][scheduler][mtp][.][requires_mtp_model]") {
  const auto gguf = test_mtp_model_path();
  if (gguf.empty()) SKIP("INFERDECK_TEST_MTP_MODEL not set");
  ScopedTestLogger logger;

  LlamaCppModel::init_backend();
  ModelInfo minfo;
  minfo.name = "test-mtp-cancel-overlap";
  minfo.gguf_path = gguf;
  minfo.n_slots = 2;
  minfo.context_size = 2048;
  minfo.vram_required_mb = 0;
  LlamaCppConfig config;
  config.n_batch = 512;
  config.n_ubatch = 512;
  config.cache_type_k = "q4_0";
  config.cache_type_v = "q4_0";
  config.mtp_enabled = true;
  config.mtp_draft_tokens = 2;
  config.mtp_max_active_requests = 1;
  LlamaCppModel lm(minfo, config);
  REQUIRE(lm.load().has_value());

  const auto first_slot = lm.acquire_slot();
  const auto second_slot = lm.acquire_slot();
  REQUIRE(first_slot.has_value());
  REQUIRE(second_slot.has_value());

  std::atomic<bool> cancel_first{false};
  std::atomic<bool> first_started{false};
  std::atomic<bool> second_started{false};
  InferenceRequest first_request;
  first_request.messages = {
      ChatMessage{"user", "Write a long numbered list without stopping."}};
  first_request.max_tokens = 1024;
  InferenceRequest second_request;
  second_request.messages = {
      ChatMessage{"user", "Count from one to twenty, one number per line."}};
  second_request.max_tokens = 64;

  auto first = std::async(std::launch::async, [&] {
    return lm.predict_stream(
        *first_slot,
        first_request,
        [&](const InferenceDelta&) {
          first_started.store(true);
          return true;
        },
        &cancel_first);
  });

  const auto first_deadline = std::chrono::steady_clock::now() +
      std::chrono::seconds(60);
  while (!first_started.load() &&
         std::chrono::steady_clock::now() < first_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(first_started.load());

  auto second = std::async(std::launch::async, [&] {
    return lm.predict_stream(
        *second_slot,
        second_request,
        [&](const InferenceDelta&) {
          if (!second_started.exchange(true)) {
            cancel_first.store(true);
          }
          return true;
        },
        nullptr);
  });

  REQUIRE(first.wait_for(std::chrono::seconds(60)) ==
          std::future_status::ready);
  REQUIRE(second.wait_for(std::chrono::seconds(60)) ==
          std::future_status::ready);
  CHECK(first.get().has_value());
  CHECK(second.get().has_value());
  CHECK(second_started.load());

  (void)lm.release_slot(*first_slot);
  (void)lm.release_slot(*second_slot);
  (void)lm.unload();
  LlamaCppModel::shutdown_backend();
}
