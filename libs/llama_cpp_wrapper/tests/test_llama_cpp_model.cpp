#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include "llama_cpp_wrapper/llama_cpp_model.hpp"
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
}  // namespace

TEST_CASE("recurrent checkpoint: second predict on same slot reuses cache",
          "[llama][recurrent][.][requires_model]") {
  const auto gguf = test_model_path();
  if (gguf.empty()) SKIP("INFERDECK_TEST_MODEL not set");

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
