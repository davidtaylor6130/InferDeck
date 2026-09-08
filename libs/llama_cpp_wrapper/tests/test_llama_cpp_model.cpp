#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "llama_cpp_wrapper/llama_cpp_model.hpp"
#include "llama_cpp_wrapper/llama_chat_adapter.hpp"
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

TEST_CASE("LlamaCppModel: reasoning effort reaches chat template inputs",
          "[llama][reasoning]") {
  ModelInfo info;
  info.name = "reasoning-model";
  info.reasoning.supported = true;
  info.reasoning.efforts = {"low", "medium", "xhigh"};
  info.reasoning.default_effort = "xhigh";
  info.reasoning.none_disables = true;
  info.reasoning.aliases = {{"high", "xhigh"}};

  common_chat_templates_inputs inputs;
  auto effort = apply_reasoning_effort(inputs, std::string{"low"}, info);
  REQUIRE(effort);
  CHECK(*effort == "low");
  CHECK(inputs.chat_template_kwargs.at("reasoning_effort") == "\"low\"");

  common_chat_templates_inputs alias_inputs;
  effort = apply_reasoning_effort(
      alias_inputs, std::string{"high"}, info);
  REQUIRE(effort);
  CHECK(*effort == "xhigh");
  CHECK(alias_inputs.chat_template_kwargs.at("reasoning_effort") == "\"xhigh\"");

  common_chat_templates_inputs disabled_inputs;
  disabled_inputs.enable_thinking = true;
  effort = apply_reasoning_effort(
      disabled_inputs, std::string{"none"}, info);
  REQUIRE(effort);
  CHECK(*effort == "none");
  CHECK_FALSE(disabled_inputs.enable_thinking);
  CHECK_FALSE(disabled_inputs.chat_template_kwargs.contains("reasoning_effort"));
}

TEST_CASE("LlamaCppModel: unsupported reasoning effort is rejected",
          "[llama][reasoning]") {
  ModelInfo info;
  info.name = "plain-model";
  common_chat_templates_inputs inputs;
  auto effort = apply_reasoning_effort(
      inputs, std::string{"medium"}, info);
  REQUIRE_FALSE(effort);
  CHECK(effort.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Llama chat adapter preserves typed roles and tool defaults",
          "[llama][adapter]") {
  inference::GenerationRequest request;
  request.messages.emplace_back(inference::MessageRole::Developer, "policy");
  request.messages.emplace_back(inference::MessageRole::System, "context");
  request.messages.emplace_back(inference::MessageRole::User, "question");
  request.tools.push_back(inference::FunctionTool{.name = "lookup"});
  request.enable_reasoning = false;

  ModelInfo info;
  info.name = "typed-model";
  LlamaChatAdapterOptions options;
  options.supports_thinking = true;
  const auto adapted = adapt_generation_request(request, info, options);
  REQUIRE(adapted);
  REQUIRE(adapted->inputs.messages.size() == 3);
  CHECK(adapted->inputs.messages[0].role == "developer");
  CHECK(adapted->inputs.messages[1].role == "system");
  CHECK(adapted->inputs.messages[2].role == "user");
  REQUIRE(adapted->inputs.tools.size() == 1);
  CHECK(adapted->inputs.tools[0].parameters == "{}");
  CHECK_FALSE(adapted->inputs.enable_thinking);
}

TEST_CASE("Llama chat adapter rejects unsupported or empty media",
          "[llama][adapter][validation]") {
  ModelInfo info;
  info.name = "typed-model";
  LlamaChatAdapterOptions options;

  inference::GenerationRequest audio_request;
  inference::Message audio_message;
  audio_message.role = inference::MessageRole::User;
  audio_message.content.emplace_back(inference::AudioContent{
      {std::byte{0x01}}, "audio/wav"});
  audio_request.messages.push_back(std::move(audio_message));
  const auto audio = adapt_generation_request(audio_request, info, options);
  REQUIRE_FALSE(audio);
  CHECK(audio.error().code == ErrorCode::InvalidArgument);

  inference::GenerationRequest image_request;
  inference::Message image_message;
  image_message.role = inference::MessageRole::User;
  image_message.content.emplace_back(inference::ImageContent{});
  image_request.messages.push_back(std::move(image_message));
  const auto image = adapt_generation_request(image_request, info, options);
  REQUIRE_FALSE(image);
  CHECK(image.error().code == ErrorCode::InvalidArgument);
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
  CHECK_FALSE(scheduler.healthy());

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
  task.recurrent_replay_checkpoint = mtp_checkpoint;
  checkpoint.reset();
  draft_checkpoint.reset();
  mtp_checkpoint.reset();
  REQUIRE(task.recurrent_checkpoint);
  REQUIRE(task.recurrent_checkpoint->size() == 32);
  REQUIRE(task.recurrent_checkpoint->front() == 7);
  REQUIRE(task.recurrent_draft_checkpoint);
  REQUIRE(task.recurrent_draft_checkpoint->size() == 16);
  REQUIRE(task.recurrent_draft_checkpoint->front() == 5);
  REQUIRE(task.recurrent_replay_checkpoint);
  REQUIRE(task.recurrent_replay_checkpoint->size() == 8);
  REQUIRE(task.recurrent_replay_checkpoint->front() == 3);
}

// ---------------------------------------------------------------------------
// Recurrent-checkpoint tests â€” require a real model.
// Run with: ctest -L unit -R "recurrent" --tests-regex . -V
// Or explicitly: ./llama_cpp_model_tests "[requires_model]"
// ---------------------------------------------------------------------------

namespace {
// Returns the gguf path from INFERDECK_TEST_MODEL env var, or empty string.
std::string test_model_path() {
  const char* p = std::getenv("INFERDECK_TEST_MODEL");
  return p ? std::string(p) : std::string{};
}

LlamaCppConfig test_runtime_config()
{
  LlamaCppConfig config;
  config.kv_unified = std::getenv("INFERDECK_TEST_KV_UNIFIED") != nullptr;
  if (std::getenv("INFERDECK_TEST_CPU_ONLY") != nullptr)
  {
    config.n_gpu_layers = 0;
    config.n_threads = 4;
    config.kv_offload = false;
    config.op_offload = false;
    config.use_mmap = true;
  }
  return config;
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
  req.max_output_tokens = 4;
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
  req1.max_output_tokens = 8;
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
  req2.max_output_tokens = 8;
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
  LlamaCppConfig config = test_runtime_config();
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
  req.max_output_tokens = 4;
  auto first = lm.predict(slot_id, req);
  REQUIRE(first.has_value());
  auto second = lm.predict(slot_id, req);
  REQUIRE(second.has_value());
  REQUIRE(second->cached_prompt_tokens > 0);

  (void)lm.release_slot(slot_id);
  (void)lm.unload();
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("MTP multi-turn checkpoint preserves facts and matches cold inference",
          "[llama][recurrent][mtp][mtp-fact-parity][.][requires_mtp_model]")
{
  const std::string gguf = test_mtp_model_path();
  if (gguf.empty()) SKIP("INFERDECK_TEST_MTP_MODEL not set");
  ScopedTestLogger logger;
  LlamaCppModel::init_backend();
  ModelInfo info;
  info.name = "mtp-fact-parity";
  info.gguf_path = gguf;
  info.n_slots = 1;
  info.context_size = 8192;
  info.reasoning.supported = true;
  info.reasoning.efforts = {"none"};
  info.reasoning.none_disables = true;
  LlamaCppConfig config = test_runtime_config();
  config.n_batch = 2048;
  config.n_ubatch = 2048;
  config.cache_type_k = "q4_0";
  config.cache_type_v = "q4_0";
  config.mtp_enabled = true;
  config.mtp_draft_tokens = 2;
  config.mtp_max_active_requests = 1;
  LlamaCppModel model(info, config);
  REQUIRE(model.load());

  const std::string first_fact = "COPPER_FINCH_742";
  const std::string changed_fact = "SILVER_OTTER_913";
  std::string archive = "The project codename is " + first_fact + ".\nArchive notes: ";
  for (int index = 0; index < 2048; ++index) archive += "entry ";
  archive += "\nRemember the project codename. Reply with READY exactly 32 times, separated by spaces, and nothing else.";
  InferenceRequest opening;
  opening.messages = {
      ChatMessage{"system", "Answer exactly as requested without explanations."},
      ChatMessage{"user", archive}};
  opening.max_output_tokens = 128;
  opening.reasoning_effort = "none";
  opening.enable_reasoning = false;
  opening.sampling.temperature = 0.0f;
  opening.sampling.seed = 742;
  const foundation::Result<int> lease = model.acquire_slot();
  REQUIRE(lease);
  const foundation::Result<InferenceResult> warm = model.predict(*lease, opening);
  REQUIRE(warm);
  REQUIRE(warm->prompt_tokens > 2000);
  REQUIRE(warm->text.find(first_fact) == std::string::npos);
  REQUIRE(warm->completion_tokens >= 16);
  REQUIRE(warm->completion_tokens > config.mtp_draft_tokens + 2);

  const foundation::Result<InferenceResult> repeated = model.predict(*lease, opening);
  REQUIRE(repeated);
  INFO("repeat_prompt=" << repeated->prompt_tokens << " repeat_cached="
       << repeated->cached_prompt_tokens << " warm_completion=" << warm->completion_tokens);
  REQUIRE(repeated->cached_prompt_tokens >= warm->prompt_tokens - 32);
  REQUIRE(repeated->cached_prompt_tokens < repeated->prompt_tokens - 1);
  CHECK(repeated->text == warm->text);

  InferenceRequest followup = opening;
  followup.messages.push_back(ChatMessage{"assistant", warm->text});
  followup.messages.push_back(ChatMessage{
      "user", "What is the project codename? Reply with only the exact codename."});
  const foundation::Result<InferenceResult> cached = model.predict(*lease, followup);
  REQUIRE(cached);
  INFO("warm_prompt=" << warm->prompt_tokens << " followup_prompt=" << cached->prompt_tokens
       << " cached=" << cached->cached_prompt_tokens << " output=" << cached->text);
  CHECK(cached->cached_prompt_tokens >= warm->prompt_tokens - 32);
  CHECK(cached->mtp_drafted_tokens > 0);
  const auto trimmed = [](const std::string& text)
  {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    return begin == std::string::npos ? std::string{} :
        text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
  };
  CHECK(trimmed(cached->text) == first_fact);
  REQUIRE(model.release_slot(*lease));
  REQUIRE(model.reset_all_slots());

  const foundation::Result<int> cold_lease = model.acquire_slot();
  REQUIRE(cold_lease);
  const foundation::Result<InferenceResult> cold = model.predict(*cold_lease, followup);
  REQUIRE(cold);
  CHECK(cold->cached_prompt_tokens == 0);
  CHECK(trimmed(cold->text) == first_fact);
  CHECK(cached->text == cold->text);

  InferenceRequest changed = followup;
  const std::size_t fact_position = archive.find(first_fact);
  REQUIRE(fact_position != std::string::npos);
  archive.replace(fact_position, first_fact.size(), changed_fact);
  changed.messages[1] = ChatMessage{"user", archive};
  const foundation::Result<InferenceResult> changed_result = model.predict(*cold_lease, changed);
  REQUIRE(changed_result);
  INFO("changed_cached=" << changed_result->cached_prompt_tokens
       << " changed_output=" << changed_result->text);
  CHECK(changed_result->cached_prompt_tokens == 0);
  CHECK(trimmed(changed_result->text) == changed_fact);
  CHECK(changed_result->text.find(first_fact) == std::string::npos);
  REQUIRE(model.release_slot(*cold_lease));
  REQUIRE(model.unload());
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("Concurrent turns preserve target checkpoints before solitary MTP resumes",
          "[llama][recurrent][mtp][concurrent-cache][.][requires_mtp_model]")
{
  const std::string gguf = test_mtp_model_path();
  if (gguf.empty()) SKIP("INFERDECK_TEST_MTP_MODEL not set");
  ScopedTestLogger logger;
  LlamaCppModel::init_backend();
  ModelInfo info;
  info.name = "concurrent-target-cache";
  info.gguf_path = gguf;
  info.n_slots = 2;
  info.context_size = 8192;
  info.reasoning.supported = true;
  info.reasoning.efforts = {"none"};
  info.reasoning.none_disables = true;
  LlamaCppConfig config = test_runtime_config();
  config.n_batch = 512;
  config.n_ubatch = 512;
  config.cache_type_k = "q4_0";
  config.cache_type_v = "q4_0";
  config.mtp_enabled = true;
  config.mtp_draft_tokens = 2;
  config.mtp_max_active_requests = 1;
  LlamaCppModel model(info, config);
  REQUIRE(model.load());
  const foundation::Result<int> keeper_slot = model.acquire_slot();
  const foundation::Result<int> probe_slot = model.acquire_slot();
  REQUIRE(keeper_slot);
  REQUIRE(probe_slot);

  InferenceRequest keeper_request;
  keeper_request.messages = {ChatMessage{
      "user", "Count every integer from 1 to 100000, one number per line. Do not abbreviate or explain. Continue until the output limit."}};
  keeper_request.max_output_tokens = 4096;
  keeper_request.reasoning_effort = "none";
  keeper_request.enable_reasoning = false;
  keeper_request.sampling.temperature = 0.0f;
  keeper_request.sampling.seed = 913;
  std::atomic<bool> cancel_keeper{false};
  std::atomic<bool> keeper_started{false};
  std::atomic<bool> keeper_finished{false};
  std::future<foundation::Result<InferenceResult>> keeper = std::async(std::launch::async, [&]
  {
    foundation::Result<InferenceResult> result = model.predict_stream(
        *keeper_slot, keeper_request, [&](const InferenceDelta& delta)
        {
          if (!delta.content.empty()) keeper_started.store(true);
          return true;
        }, &cancel_keeper);
    keeper_finished.store(true);
    return result;
  });
  struct CancelKeeperOnExit
  {
    std::atomic<bool>& flag;
    ~CancelKeeperOnExit() { flag.store(true); }
  } cancel_on_exit{cancel_keeper};
  const std::chrono::seconds timeout{
      std::getenv("INFERDECK_TEST_CPU_ONLY") ? 180 : 60};
  const std::chrono::steady_clock::time_point start_deadline =
      std::chrono::steady_clock::now() + timeout;
  while (!keeper_started.load() && !keeper_finished.load() &&
         std::chrono::steady_clock::now() < start_deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(keeper_started.load());
  REQUIRE_FALSE(keeper_finished.load());

  const std::string first_fact = "COPPER_FINCH_742";
  const std::string changed_fact = "SILVER_OTTER_913";
  std::string archive = "The project codename is " + first_fact + ".\nArchive notes: ";
  for (int index = 0; index < 2048; ++index) archive += "entry ";
  archive += "\nRemember the codename. Reply with READY exactly 16 times, separated by spaces, and nothing else.";
  InferenceRequest opening = keeper_request;
  opening.messages = {
      ChatMessage{"system", "Answer exactly as requested without explanations."},
      ChatMessage{"user", archive}};
  opening.max_output_tokens = 64;
  const foundation::Result<InferenceResult> warm = model.predict(*probe_slot, opening);
  REQUIRE(warm);
  REQUIRE_FALSE(keeper_finished.load());
  REQUIRE(warm->prompt_tokens > 2000);
  REQUIRE(warm->completion_tokens >= 8);
  REQUIRE(warm->text.find(first_fact) == std::string::npos);
  CHECK(warm->mtp_drafted_tokens == 0);
  const foundation::Result<InferenceResult> repeated = model.predict(*probe_slot, opening);
  REQUIRE(repeated);
  REQUIRE_FALSE(keeper_finished.load());
  INFO("repeat_prompt=" << repeated->prompt_tokens << " repeat_cached="
       << repeated->cached_prompt_tokens);
  CHECK(repeated->cached_prompt_tokens >= warm->prompt_tokens - 32);
  CHECK(repeated->cached_prompt_tokens < repeated->prompt_tokens - 1);
  CHECK(repeated->mtp_drafted_tokens == 0);
  CHECK(repeated->text == warm->text);

  InferenceRequest followup = opening;
  followup.messages.push_back(ChatMessage{"assistant", warm->text});
  followup.messages.push_back(ChatMessage{
      "user", "What is the project codename? Reply with only the exact codename."});
  const foundation::Result<InferenceResult> cached = model.predict(*probe_slot, followup);
  REQUIRE(cached);
  REQUIRE_FALSE(keeper_finished.load());
  CHECK(cached->cached_prompt_tokens >= warm->prompt_tokens - 32);
  CHECK(cached->mtp_drafted_tokens == 0);
  const auto trimmed = [](const std::string& text)
  {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    return begin == std::string::npos ? std::string{} :
        text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
  };
  CHECK(trimmed(cached->text) == first_fact);
  InferenceRequest changed = followup;
  const std::size_t fact_position = archive.find(first_fact);
  REQUIRE(fact_position != std::string::npos);
  archive.replace(fact_position, first_fact.size(), changed_fact);
  changed.messages[1] = ChatMessage{"user", archive};
  const foundation::Result<InferenceResult> changed_result = model.predict(*probe_slot, changed);
  REQUIRE(changed_result);
  REQUIRE_FALSE(keeper_finished.load());
  CHECK(changed_result->cached_prompt_tokens == 0);
  CHECK(trimmed(changed_result->text) == changed_fact);

  const foundation::Result<InferenceResult> survivor = model.predict_stream(
      *probe_slot, changed, [&](const InferenceDelta& delta)
      {
        if (!delta.content.empty()) cancel_keeper.store(true);
        return true;
      });
  REQUIRE(survivor);
  REQUIRE(cancel_keeper.load());
  CHECK(survivor->cached_prompt_tokens >= changed_result->prompt_tokens - 32);
  CHECK(survivor->mtp_drafted_tokens == 0);
  CHECK(trimmed(survivor->text) == changed_fact);
  REQUIRE(keeper.wait_for(timeout) == std::future_status::ready);
  REQUIRE(keeper.get());
  REQUIRE(model.release_slot(*keeper_slot));
  CHECK(model.execution_healthy());

  InferenceRequest tiny_followup = changed;
  tiny_followup.max_output_tokens = config.mtp_draft_tokens + 2;
  const foundation::Result<InferenceResult> tiny_cached =
      model.predict(*probe_slot, tiny_followup);
  REQUIRE(tiny_cached);
  CHECK(tiny_cached->cached_prompt_tokens >= changed_result->prompt_tokens - 32);
  CHECK(tiny_cached->mtp_drafted_tokens == 0);
  CHECK(tiny_cached->completion_tokens > 0);

  const foundation::Result<InferenceResult> solitary = model.predict(*probe_slot, changed);
  REQUIRE(solitary);
  CHECK(solitary->cached_prompt_tokens == 0);
  CHECK(solitary->mtp_drafted_tokens > 0);
  CHECK(trimmed(solitary->text) == changed_fact);
  REQUIRE(model.release_slot(*probe_slot));
  REQUIRE(model.reset_all_slots());
  const foundation::Result<int> cold_slot = model.acquire_slot();
  REQUIRE(cold_slot);
  const foundation::Result<InferenceResult> cold = model.predict(*cold_slot, followup);
  REQUIRE(cold);
  CHECK(cold->cached_prompt_tokens == 0);
  CHECK(trimmed(cold->text) == first_fact);
  CHECK(cold->text == cached->text);
  REQUIRE(model.release_slot(*cold_slot));
  REQUIRE(model.reset_all_slots());
  const foundation::Result<int> tiny_cold_slot = model.acquire_slot();
  REQUIRE(tiny_cold_slot);
  const foundation::Result<InferenceResult> tiny_cold =
      model.predict(*tiny_cold_slot, tiny_followup);
  REQUIRE(tiny_cold);
  CHECK(tiny_cold->cached_prompt_tokens == 0);
  CHECK(tiny_cold->text == tiny_cached->text);
  REQUIRE(model.release_slot(*tiny_cold_slot));
  REQUIRE(model.unload());
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
  LlamaCppConfig config = test_runtime_config();
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

  const std::chrono::seconds timeout{
      std::getenv("INFERDECK_TEST_CPU_ONLY") ? 180 : 60};
  std::atomic<bool> cancel_first{false};
  std::atomic<bool> first_started{false};
  std::atomic<bool> second_started{false};
  InferenceRequest first_request;
  first_request.messages = {
      ChatMessage{"user", "Write a long numbered list without stopping."}};
  first_request.max_output_tokens = 1024;
  InferenceRequest second_request;
  second_request.messages = {
      ChatMessage{"user", "Count from one to twenty, one number per line."}};
  second_request.max_output_tokens = 64;

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
      timeout;
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

  REQUIRE(first.wait_for(timeout) ==
          std::future_status::ready);
  REQUIRE(second.wait_for(timeout) ==
          std::future_status::ready);
  CHECK(first.get().has_value());
  CHECK(second.get().has_value());
  CHECK(second_started.load());

  (void)lm.release_slot(*first_slot);
  (void)lm.release_slot(*second_slot);
  (void)lm.unload();
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("Cache affinity reuses an idle sequence without moving busy work",
          "[llama][affinity][.][requires_model]") {
  const std::string gguf = test_model_path();
  if (gguf.empty()) SKIP("INFERDECK_TEST_MODEL not set");
  ScopedTestLogger logger;
  LlamaCppModel::init_backend();
  ModelInfo info;
  info.name = "cache-affinity";
  info.gguf_path = gguf;
  info.n_slots = 2;
  info.context_size = 2048;
  info.reasoning.supported = true;
  info.reasoning.efforts = {"none"};
  info.reasoning.none_disables = true;
  LlamaCppConfig config = test_runtime_config();
  config.mtp_enabled = std::getenv("INFERDECK_TEST_AFFINITY_MTP") != nullptr;
  config.cache_type_k = "q4_0";
  config.cache_type_v = "q4_0";
  LlamaCppModel model(info, config);
  REQUIRE(model.load());
  InferenceRequest first;
  first.messages = {ChatMessage{"user", std::string(600, 'A') + " Reply with exactly ALPHA."}};
  first.max_output_tokens = 8;
  first.reasoning_effort = "none";
  first.enable_reasoning = false;
  first.sampling.temperature = 0.0f;
  InferenceRequest second = first;
  second.messages = {ChatMessage{"user", std::string(600, 'B') + " Reply with exactly BRAVO."}};
  const foundation::Result<int> first_slot = model.acquire_slot();
  const foundation::Result<int> second_slot = model.acquire_slot();
  REQUIRE(first_slot);
  REQUIRE(second_slot);
  const foundation::Result<InferenceResult> first_warm = model.predict(*first_slot, first);
  REQUIRE(first_warm);
  if (config.mtp_enabled) REQUIRE(first_warm->text.find("ALPHA") != std::string::npos);
  const foundation::Result<InferenceResult> warm = model.predict(*second_slot, second);
  REQUIRE(warm);
  if (config.mtp_enabled) REQUIRE(warm->text.find("BRAVO") != std::string::npos);
  REQUIRE(model.release_slot(*first_slot));
  REQUIRE(model.release_slot(*second_slot));
  for (const InferenceRequest* request : {&second, &first, &second}) {
    const foundation::Result<int> lease = model.acquire_slot();
    REQUIRE(lease);
    const foundation::Result<InferenceResult> reused = model.predict(*lease, *request);
    REQUIRE(reused);
    INFO("prompt=" << reused->prompt_tokens << " cached=" << reused->cached_prompt_tokens);
    CHECK(reused->cached_prompt_tokens >= reused->prompt_tokens - 16);
    CHECK(reused->text == (request == &first ? first_warm->text : warm->text));
    if (config.mtp_enabled)
    {
      CHECK(reused->mtp_drafted_tokens > 0);
    }
    REQUIRE(model.release_slot(*lease));
  }
  const foundation::Result<int> reserved_first = model.acquire_slot();
  const foundation::Result<int> reserved_second = model.acquire_slot();
  REQUIRE(reserved_first);
  REQUIRE(reserved_second);
  const foundation::Result<InferenceResult> reversed_first = model.predict(*reserved_first, first);
  REQUIRE(reversed_first);
  CHECK(reversed_first->cached_prompt_tokens >= reversed_first->prompt_tokens - 16);
  CHECK(reversed_first->text == first_warm->text);
  CHECK(model.slot_busy(*reserved_first));
  CHECK(model.slot_busy(*reserved_second));
  CHECK_FALSE(model.acquire_slot());
  const foundation::Result<InferenceResult> reversed_second = model.predict(*reserved_second, second);
  REQUIRE(reversed_second);
  CHECK(reversed_second->cached_prompt_tokens >= reversed_second->prompt_tokens - 16);
  CHECK(reversed_second->text == warm->text);
  CHECK(model.slot_busy(*reserved_first));
  CHECK(model.slot_busy(*reserved_second));
  const foundation::Result<InferenceResult> bound_repeat = model.predict(*reserved_first, first);
  REQUIRE(bound_repeat);
  CHECK(bound_repeat->cached_prompt_tokens >= bound_repeat->prompt_tokens - 16);
  CHECK(bound_repeat->text == first_warm->text);
  CHECK(model.slot_busy(*reserved_second));
  REQUIRE(model.release_slot(*reserved_first));
  REQUIRE(model.release_slot(*reserved_second));
  const foundation::Result<int> busy = model.acquire_slot();
  const foundation::Result<int> other = model.acquire_slot();
  REQUIRE(busy);
  REQUIRE(other);
  CHECK(model.slot_busy(*busy));
  CHECK(model.slot_busy(*other));
  REQUIRE(model.predict(*other, second));
  CHECK(model.slot_busy(*busy));
  REQUIRE(model.release_slot(*busy));
  REQUIRE(model.release_slot(*other));
  REQUIRE(model.reset_all_slots());
  const foundation::Result<int> reset = model.acquire_slot();
  REQUIRE(reset);
  const foundation::Result<InferenceResult> cold = model.predict(*reset, second);
  REQUIRE(cold);
  CHECK(cold->cached_prompt_tokens == 0);
  REQUIRE(model.release_slot(*reset));
  REQUIRE(model.unload());
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("History fitting bounds renders and preserves complete recent tool turns",
          "[llama][history]")
{
  common_chat_templates_inputs inputs;
  const auto append = [&](const std::string& role, const std::string& content)
  {
    common_chat_msg message;
    message.role = role;
    message.content = content;
    inputs.messages.push_back(std::move(message));
  };
  append("system", "policy");
  append("developer", "instructions");
  for (int index = 0; index < 256; ++index)
  {
    append("user", "old question " + std::to_string(index));
    append("assistant", "old answer");
  }
  append("user", "latest question");
  append("assistant", "latest tool call");
  append("tool", "latest result");
  int renders = 0;
  const std::size_t dropped = fit_chat_history(inputs,
      [&](const common_chat_templates_inputs& candidate)
      {
        ++renders;
        return candidate.messages.size() <= 5;
      });
  CHECK(dropped == 512);
  CHECK(renders <= 11);
  REQUIRE(inputs.messages.size() == 5);
  CHECK(inputs.messages[0].content == "policy");
  CHECK(inputs.messages[1].content == "instructions");
  CHECK(inputs.messages[2].content == "latest question");
  CHECK(inputs.messages.back().content == "latest result");
  CHECK(fit_chat_history(inputs, [](const common_chat_templates_inputs&) { return false; }) == 0);
  CHECK(inputs.messages.size() == 5);
  CHECK(fit_chat_history(inputs, [](const common_chat_templates_inputs&) { return true; }) == 0);
}

TEST_CASE("Batch scheduling reserves decode capacity and rotates prompt service",
          "[llama][scheduler][fairness]")
{
  SlotTask first;
  SlotTask second;
  SlotTask decoder;
  decoder.prompt_done = true;
  decoder.spec_draft = {4, 5};
  std::vector<SlotTask*> tasks{&first, &second, &decoder};
  CHECK(detail::prepare_batch_order(tasks, 16, 0) == 7);
  CHECK(tasks.front() == &decoder);
  CHECK(tasks[1] == &first);
  tasks = {&first, &second, &decoder};
  CHECK(detail::prepare_batch_order(tasks, 16, 1) == 7);
  CHECK(tasks.front() == &decoder);
  CHECK(tasks[1] == &second);
  decoder.caller_cancel.store(true);
  CHECK(detail::prepare_batch_order(tasks, 16, 0) == 8);
  second.caller_stop.store(true);
  CHECK(detail::prepare_batch_order(tasks, 16, 0) == 16);
  tasks = {&first};
  CHECK(detail::prepare_batch_order(tasks, 512, 0) == 512);
  tasks = {&first, &second};
  second.caller_stop.store(false);
  CHECK(detail::prepare_batch_order(tasks, 1, 0) == 1);
  CHECK(tasks.front() == &first);
  tasks = {&first, &second};
  CHECK(detail::prepare_batch_order(tasks, 1, 1) == 1);
  CHECK(tasks.front() == &second);
}

TEST_CASE("Mixed prefill bounds the delay before active decoders run again",
          "[llama][scheduler][fairness][mixed-prefill-budget]")
{
  SlotTask decoder;
  decoder.slot_id = 0;
  decoder.prompt_done = true;
  SlotTask first;
  SlotTask second;
  SlotTask third;
  SlotTask fourth;
  first.slot_id = 1;
  second.slot_id = 2;
  third.slot_id = 3;
  fourth.slot_id = 4;
  std::vector<SlotTask*> tasks{&first, &decoder};
  CHECK(detail::prepare_batch_order(tasks, 2048, 0) == 1024);
  tasks = {&fourth, &third, &decoder, &second, &first};
  CHECK(detail::prepare_batch_order(tasks, 2048, 2) == 256);
  for (std::size_t index = 0; index < tasks.size(); ++index)
  {
    CHECK(tasks[index]->slot_id == static_cast<int>(index));
  }
  tasks = {&third, &decoder, &second, &first};
  CHECK(detail::prepare_batch_order(tasks, 2048, 1) == 341);
  tasks = {&third, &second, &first};
  CHECK(detail::prepare_batch_order(tasks, 2048, 0) == 683);
  tasks = {&decoder, &first};
  CHECK(detail::prepare_batch_order(tasks, 64, 0) == 63);
}

TEST_CASE("Equal-share prefill keeps sequence order while constrained quotas rotate",
          "[llama][scheduler][fairness][prefill-order]")
{
  SlotTask first;
  SlotTask second;
  SlotTask third;
  SlotTask fourth;
  first.slot_id = 0;
  second.slot_id = 1;
  third.slot_id = 2;
  fourth.slot_id = 3;
  const std::vector<SlotTask*> original{&third, &first, &fourth, &second};
  for (std::size_t turn = 0; turn < original.size(); ++turn)
  {
    std::vector<SlotTask*> tasks = original;
    CHECK(detail::prepare_batch_order(tasks, 2048, turn) == 512);
    for (std::size_t index = 0; index < tasks.size(); ++index)
    {
      CHECK(tasks[index]->slot_id == static_cast<int>(index));
    }
    tasks = original;
    CHECK(detail::prepare_batch_order(tasks, 2047, turn) == 512);
    CHECK(tasks.front() == original[turn]);
    tasks = original;
    CHECK(detail::prepare_batch_order(tasks, 3, turn) == 1);
    CHECK(tasks.front() == original[turn]);
  }
  SlotTask decoder;
  decoder.slot_id = 4;
  decoder.prompt_done = true;
  std::vector<SlotTask*> mixed = original;
  mixed.push_back(&decoder);
  CHECK(detail::prepare_batch_order(mixed, 2049, 2) == 256);
  CHECK(mixed.front() == &decoder);
  for (std::size_t index = 1; index < mixed.size(); ++index)
  {
    CHECK(mixed[index]->slot_id == static_cast<int>(index - 1));
  }
  fourth.caller_cancel.store(true);
  mixed = original;
  CHECK(detail::prepare_batch_order(mixed, 2046, 1) == 682);
  CHECK(mixed.front() == &first);
}

TEST_CASE("Decoder batches keep sequence order without starving smaller batches",
          "[llama][scheduler][fairness][decoder-order]")
{
  SlotTask first;
  SlotTask second;
  SlotTask third;
  first.slot_id = 0;
  second.slot_id = 1;
  third.slot_id = 2;
  first.prompt_done = second.prompt_done = third.prompt_done = true;
  SlotTask prompt_a;
  SlotTask prompt_b;
  bool served_a_first = false;
  bool served_b_first = false;
  for (std::size_t turn = 0; turn < 10; ++turn)
  {
    INFO("turn=" << turn);
    std::vector<SlotTask*> decoding{&third, &first, &second};
    CHECK(detail::prepare_batch_order(decoding, 3, turn) == 0);
    CHECK(decoding == std::vector<SlotTask*>{&first, &second, &third});
    std::vector<SlotTask*> mixed{&prompt_a, &third, &first, &prompt_b, &second};
    CHECK(detail::prepare_batch_order(mixed, 7, turn) == 2);
    CHECK(std::vector<SlotTask*>(mixed.begin(), mixed.begin() + 3) ==
          std::vector<SlotTask*>{&first, &second, &third});
    CHECK(((mixed[3] == &prompt_a && mixed[4] == &prompt_b) ||
           (mixed[3] == &prompt_b && mixed[4] == &prompt_a)));
    served_a_first = served_a_first || mixed[3] == &prompt_a;
    served_b_first = served_b_first || mixed[3] == &prompt_b;
  }
  CHECK(served_a_first);
  CHECK(served_b_first);
  const std::vector<SlotTask*> original{&third, &first, &second};
  for (std::size_t turn = 0; turn < 6; ++turn)
  {
    std::vector<SlotTask*> small = original;
    CHECK(detail::prepare_batch_order(small, 1, turn) == 0);
    CHECK(small.front() == original[turn % original.size()]);
  }
  first.spec_draft = {4, 5};
  std::vector<SlotTask*> speculative = original;
  CHECK(detail::prepare_batch_order(speculative, 4, 2) == 0);
  CHECK(speculative.front() == &second);
  speculative = original;
  CHECK(detail::prepare_batch_order(speculative, 5, 2) == 0);
  CHECK(speculative == std::vector<SlotTask*>{&first, &second, &third});
}

TEST_CASE("Mixed requests serve a short prompt before a long prefill finishes",
          "[llama][mixed][.][requires_model]")
{
  const std::string gguf = test_model_path();
  if (gguf.empty()) SKIP("INFERDECK_TEST_MODEL not set");
  ScopedTestLogger logger;
  LlamaCppModel::init_backend();
  ModelInfo info;
  info.name = "mixed-prefill";
  info.gguf_path = gguf;
  info.n_slots = 2;
  info.context_size = 8192;
  info.reasoning.supported = true;
  info.reasoning.efforts = {"none"};
  info.reasoning.none_disables = true;
  LlamaCppModel model(info, test_runtime_config());
  REQUIRE(model.load());
  const foundation::Result<int> long_slot = model.acquire_slot();
  const foundation::Result<int> short_slot = model.acquire_slot();
  REQUIRE(long_slot);
  REQUIRE(short_slot);
  InferenceRequest long_request;
  std::string content;
  for (int index = 0; index < 6000; ++index) content += "word ";
  long_request.messages = {ChatMessage{"user", content + " Reply with OK."}};
  long_request.max_output_tokens = 4;
  long_request.reasoning_effort = "none";
  long_request.enable_reasoning = false;
  long_request.sampling.temperature = 0.0f;
  InferenceRequest short_request;
  short_request.messages = {ChatMessage{"user", "Reply with only SHORT_OK."}};
  short_request.max_output_tokens = 8;
  short_request.reasoning_effort = "none";
  short_request.enable_reasoning = false;
  short_request.sampling.temperature = 0.0f;
  std::atomic<int> sequence{0};
  std::atomic<int> long_first{0};
  std::atomic<int> short_first{0};
  std::future<foundation::Result<InferenceResult>> long_run = std::async(std::launch::async, [&]
  {
    return model.predict_stream(*long_slot, long_request, [&](const InferenceDelta& delta)
    {
      if (!delta.content.empty() && long_first.load() == 0) long_first.store(++sequence);
      return true;
    });
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const foundation::Result<InferenceResult> short_result = model.predict_stream(
      *short_slot, short_request, [&](const InferenceDelta& delta)
      {
        if (!delta.content.empty() && short_first.load() == 0) short_first.store(++sequence);
        return true;
      });
  const foundation::Result<InferenceResult> long_result = long_run.get();
  REQUIRE(short_result);
  REQUIRE(long_result);
  CHECK(short_first.load() > 0);
  CHECK(short_first.load() < long_first.load());
  REQUIRE(model.release_slot(*long_slot));
  REQUIRE(model.release_slot(*short_slot));
  REQUIRE(model.reset_all_slots());
  const foundation::Result<int> serial_slot = model.acquire_slot();
  REQUIRE(serial_slot);
  const foundation::Result<InferenceResult> serial = model.predict(*serial_slot, short_request);
  REQUIRE(serial);
  INFO("concurrent=" << short_result->text << " serial=" << serial->text);
  CHECK_FALSE(short_result->text.empty());
  CHECK(short_result->text == serial->text);
  foundation::LOG_INFO("mixed_prefill_verification",
      "long_prompt_ms={} short_first_token_ms={} short_first={} long_first={}",
      long_result->prompt_duration_ms, short_result->first_token_duration_ms,
      short_first.load(), long_first.load());
  REQUIRE(model.release_slot(*serial_slot));
  REQUIRE(model.unload());
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("Non-stream cancellation during prompt processing preserves the peer",
          "[llama][cancel-prefill][.][requires_model]") {
  const std::string gguf = test_model_path();
  if (gguf.empty()) SKIP("INFERDECK_TEST_MODEL not set");
  ScopedTestLogger logger;
  LlamaCppModel::init_backend();
  ModelInfo info;
  info.name = "cancel-prefill";
  info.gguf_path = gguf;
  info.n_slots = 2;
  info.context_size = 4096;
  LlamaCppConfig config = test_runtime_config();
  config.n_threads = 4;
  config.n_batch = 64;
  config.n_ubatch = 64;
  LlamaCppModel model(info, config);
  REQUIRE(model.load());
  const auto first = model.acquire_slot();
  const auto peer = model.acquire_slot();
  REQUIRE(first);
  REQUIRE(peer);
  InferenceRequest short_request;
  short_request.messages = {ChatMessage{"user", "Reply with only OK."}};
  short_request.max_output_tokens = 4;
  short_request.sampling.temperature = 0.0f;
  const auto baseline = model.predict(*peer, short_request);
  REQUIRE(baseline);
  InferenceRequest long_request = short_request;
  std::string words;
  for (int index = 0; index < 3000; ++index) words += "word ";
  long_request.messages = {ChatMessage{"user", words + " Reply with OK."}};
  long_request.max_output_tokens = 512;
  std::atomic<bool> cancelled{false};
  std::future<foundation::Result<InferenceResult>> pending =
      std::async(std::launch::async, [&] {
        return model.predict_cancellable(*first, long_request, &cancelled);
      });
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  const auto concurrent = model.predict(*peer, short_request);
  cancelled.store(true);
  const auto partial = pending.get();
  REQUIRE(concurrent);
  REQUIRE(partial);
  CHECK(concurrent->text == baseline->text);
  CHECK(partial->prompt_tokens < 3000);
  CHECK(partial->prompt_duration_ms > 0);
  CHECK(partial->generation_duration_ms == 0);
  CHECK(partial->completion_tokens == 0);
  REQUIRE(model.release_slot(*first));
  REQUIRE(model.release_slot(*peer));
  const auto reused = model.acquire_slot();
  REQUIRE(reused);
  const auto after = model.predict(*reused, short_request);
  REQUIRE(after);
  CHECK(after->text == baseline->text);
  CHECK(model.execution_healthy());
  REQUIRE(model.release_slot(*reused));
  REQUIRE(model.unload());
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("Unified pool preserves the configured request context limit", "[llama][pool][.][requires_model]") {
  const std::string path = test_model_path();
  if (path.empty()) SKIP("INFERDECK_TEST_MODEL not set");
  ScopedTestLogger logger;
  LlamaCppModel::init_backend();
  ModelInfo info;
  info.name = "pool-context-limit";
  info.context_pool_auto = std::getenv("INFERDECK_TEST_POOL_AUTO") != nullptr;
  info.gguf_path = path;
  info.n_slots = 2;
  info.context_size = 512;
  LlamaCppConfig config = test_runtime_config();
  config.kv_unified = true;
  config.truncate_prompt = false;
  LlamaCppModel model(info, config);
  REQUIRE(model.load());
  const foundation::Result<int> slot = model.acquire_slot();
  REQUIRE(slot);
  InferenceRequest oversized;
  std::string text;
  for (int index = 0; index < 600; ++index) text += " hello";
  oversized.messages = {ChatMessage{"user", text}};
  oversized.max_output_tokens = 4;
  const foundation::Result<InferenceResult> rejected = model.predict(*slot, oversized);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == ErrorCode::ContextLengthExceeded);
  CHECK(rejected.error().message.find("512") != std::string::npos);
  InferenceRequest small;
  small.messages = {ChatMessage{"user", "Say hello."}};
  small.max_output_tokens = 4;
  REQUIRE(model.predict(*slot, small));
  REQUIRE(model.release_slot(*slot));
  REQUIRE(model.unload());
}

TEST_CASE("Idle automatic pool reclamation preserves a CPU model",
          "[llama][pool-reclaim][.][requires_model]") {
  const std::string path = test_model_path();
  if (path.empty()) SKIP("INFERDECK_TEST_MODEL not set");
  ScopedTestLogger logger;
  LlamaCppModel::init_backend();
  ModelInfo info;
  info.name = "pool-reclaim-cpu";
  info.gguf_path = path;
  info.n_slots = 2;
  info.context_size = 512;
  info.context_pool_auto = true;
  LlamaCppConfig config = test_runtime_config();
  config.kv_unified = true;
  config.n_gpu_layers = 0;
  config.kv_offload = false;
  config.op_offload = false;
  LlamaCppModel model(info, config);
  REQUIRE(model.load());
  CHECK(model.can_reclaim_idle_context());

  InferenceRequest request;
  request.messages = {ChatMessage{"user", "Reply with OK only. /no_think"}};
  request.max_output_tokens = 8;
  request.enable_reasoning = false;
  request.sampling.temperature = 0.0f;
  const foundation::Result<int> before_slot = model.acquire_slot();
  REQUIRE(before_slot);
  REQUIRE(model.predict(*before_slot, request));

  CHECK_FALSE(model.can_reclaim_idle_context());
  const foundation::Result<bool> held =
      model.reclaim_idle_context(1024, {});
  REQUIRE_FALSE(held);
  CHECK(held.error().code == ErrorCode::Unavailable);
  REQUIRE(model.release_slot(*before_slot));

  LifecycleControl cancelled;
  cancelled.cancelled = [] { return true; };
  const foundation::Result<bool> pre_cancelled =
      model.reclaim_idle_context(1024, cancelled);
  REQUIRE_FALSE(pre_cancelled);
  CHECK(pre_cancelled.error().code == ErrorCode::Cancelled);
  CHECK(model.execution_healthy());

  const foundation::Result<bool> no_device_gain =
      model.reclaim_idle_context(1024, {});
  REQUIRE(no_device_gain);
  CHECK_FALSE(*no_device_gain);
  CHECK(model.execution_healthy());

  const foundation::Result<int> after_slot = model.acquire_slot();
  REQUIRE(after_slot);
  REQUIRE(model.predict(*after_slot, request));
  REQUIRE(model.release_slot(*after_slot));
  REQUIRE(model.unload());
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("No-gain GPU reclamation preserves the idle prompt cache",
          "[llama][pool-reclaim-no-gain][.][requires_model]") {
  const std::string path = test_model_path();
  if (path.empty()) SKIP("INFERDECK_TEST_MODEL not set");
  if (std::getenv("INFERDECK_TEST_CPU_ONLY")) SKIP("GPU context required");
  ScopedTestLogger logger;
  LlamaCppModel::init_backend();
  ModelInfo info;
  info.name = "pool-reclaim-cache";
  info.gguf_path = path;
  info.n_slots = 2;
  info.context_size = 2048;
  info.context_pool_auto = true;
  LlamaCppConfig config = test_runtime_config();
  config.kv_unified = true;
  config.n_gpu_layers = 99;
  config.kv_offload = true;
  config.op_offload = true;
  config.vram_safety_margin_mb = 0;
  LlamaCppModel model(info, config);
  REQUIRE(model.load());
  InferenceRequest request;
  std::string prompt;
  for (int index = 0; index < 128; ++index) prompt += " hello";
  prompt += " Reply OK.";
  request.messages = {ChatMessage{"user", prompt}};
  request.max_output_tokens = 4;
  request.sampling.temperature = 0.0f;
  const foundation::Result<int> first_slot = model.acquire_slot();
  REQUIRE(first_slot);
  const foundation::Result<InferenceResult> first = model.predict(*first_slot, request);
  REQUIRE(first);
  REQUIRE(model.release_slot(*first_slot));
  int cancellation_checks = 0;
  LifecycleControl during_preflight;
  during_preflight.cancelled = [&] { return ++cancellation_checks >= 4; };
  const foundation::Result<bool> cancelled_reclaim =
      model.reclaim_idle_context(1, during_preflight);
  REQUIRE_FALSE(cancelled_reclaim);
  CHECK(cancelled_reclaim.error().code == ErrorCode::Cancelled);
  const foundation::Result<bool> impossible_reclaim =
      model.reclaim_idle_context(std::numeric_limits<int>::max(), {});
  REQUIRE(impossible_reclaim);
  CHECK_FALSE(*impossible_reclaim);
  const foundation::Result<bool> reclaimed = model.reclaim_idle_context(1, {});
  REQUIRE(reclaimed);
  CHECK_FALSE(*reclaimed);
  CHECK(model.execution_healthy());
  const foundation::Result<int> second_slot = model.acquire_slot();
  REQUIRE(second_slot);
  const foundation::Result<InferenceResult> repeated = model.predict(*second_slot, request);
  REQUIRE(repeated);
  CHECK(repeated->cached_prompt_tokens >= first->prompt_tokens - 2);
  CHECK(repeated->text == first->text);
  REQUIRE(model.release_slot(*second_slot));
  REQUIRE(model.unload());
  LlamaCppModel::shutdown_backend();
}

TEST_CASE("Bounded unified pool serializes large requests and preserves outputs", "[llama][pool-admission][.][requires_model]") {
  const std::string path = test_model_path();
  if (path.empty()) SKIP("INFERDECK_TEST_MODEL not set");
  ScopedTestLogger logger;
  LlamaCppModel::init_backend();
  ModelInfo info;
  info.name = "bounded-pool";
  info.gguf_path = path;
  info.n_slots = 4;
  info.context_size = 2048;
  info.context_pool_auto = std::getenv("INFERDECK_TEST_POOL_AUTO") != nullptr;
  const char* pool_override = std::getenv("INFERDECK_TEST_POOL_CAPACITY");
  info.context_pool_size = info.context_pool_auto ? 0 : (pool_override ? std::atoi(pool_override) : 2052);
  LlamaCppConfig config = test_runtime_config();
  config.kv_unified = true;
  config.mtp_enabled = std::getenv("INFERDECK_TEST_AFFINITY_MTP") != nullptr;
  config.mtp_draft_tokens = 2;
  config.n_threads = 4;
  config.n_batch = 256;
  config.n_ubatch = 256;
  LlamaCppModel model(info, config);
  REQUIRE(model.load());
  std::vector<InferenceRequest> requests;
  const std::array<std::string, 4> expected{"2", "4", "6", "8"};
  for (int index = 0; index < 4; ++index) {
    InferenceRequest request;
    std::string text;
    for (int word = 0; word < 1200; ++word) text += " hello";
    const int operand = index + 1;
    text += "\nFinal task: calculate " + std::to_string(operand) +
        " + " + std::to_string(operand) + ". Reply with the single digit " +
        expected[index] + " only. The answer is " + expected[index] + ". /no_think";
    request.messages = {ChatMessage{"user", std::move(text)}};
    request.max_output_tokens = 16;
    request.enable_reasoning = false;
    request.sampling.temperature = 0.0f;
    const foundation::Result<int> slot = model.acquire_slot();
    REQUIRE(slot);
    const foundation::Result<InferenceResult> result = model.predict(*slot, request);
    REQUIRE(result);
    REQUIRE(result->prompt_tokens > 1024);
    INFO("serial index=" << index << " expected=" << expected[index]
         << " actual=" << result->text);
    CHECK(result->text == expected[index]);
    requests.push_back(std::move(request));
    REQUIRE(model.release_slot(*slot));
  }
  std::vector<int> slots;
  std::array<std::atomic<bool>, 4> cancelled{};
  std::vector<std::future<foundation::Result<InferenceResult>>> pending;
  for (int index = 0; index < 4; ++index) {
    const foundation::Result<int> slot = model.acquire_slot();
    REQUIRE(slot);
    slots.push_back(*slot);
  }
  for (int index = 0; index < 4; ++index) {
    pending.push_back(std::async(std::launch::async, [&, index] {
      return model.predict_cancellable(slots[index], requests[index], &cancelled[index]);
    }));
  }
  const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds{120};
  bool completed = true;
  for (std::future<foundation::Result<InferenceResult>>& future : pending) {
    if (future.wait_until(deadline) != std::future_status::ready) {
      completed = false;
      for (std::atomic<bool>& flag : cancelled) flag.store(true);
      break;
    }
  }
  CHECK(completed);
  for (int index = 0; index < 4; ++index) {
    const foundation::Result<InferenceResult> result = pending[index].get();
    REQUIRE(result);
    foundation::LOG_INFO(
        "pool_admission_parity",
        "index={} cached_tokens={} expected='{}' actual='{}'",
        index, result->cached_prompt_tokens, expected[index], result->text);
    INFO("concurrent index=" << index << " expected=" << expected[index]
         << " actual=" << result->text
         << " cached_tokens=" << result->cached_prompt_tokens);
    CHECK(result->text == expected[index]);
    REQUIRE(model.release_slot(slots[index]));
  }
  CHECK(model.execution_healthy());
  REQUIRE(model.unload());
}

TEST_CASE("JSON object requests provide an explicit object constraint", "[llama][adapter][json-object]") {
  inference::GenerationRequest request;
  request.messages.emplace_back(inference::MessageRole::User, "Return JSON.");
  request.output.kind = inference::StructuredOutputKind::JsonObject;
  request.output.schema = "{}";
  ModelInfo info;
  info.name = "json-object";
  const auto adapted = adapt_generation_request(request, info, LlamaChatAdapterOptions{});
  REQUIRE(adapted);
  CHECK(adapted->inputs.json_schema == R"({"type":"object"})");
  request.output.kind = inference::StructuredOutputKind::JsonSchema;
  request.output.schema = "{  }";
  const auto unconstrained = adapt_generation_request(request, info, LlamaChatAdapterOptions{});
  REQUIRE(unconstrained);
  CHECK(unconstrained->inputs.json_schema == R"({"$comment":"Unconstrained JSON output"})");
}
