#include "llama_cpp_wrapper/llama_cpp_model.hpp"
#include "llama_cpp_wrapper/continuous_batch_scheduler.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <excpt.h>
#include <psapi.h>
#include <wrl/client.h>
#endif

#include "llama.h"
#include "ggml.h"
#include "foundation/logging.hpp"
#include "chat.h"
#include "sampling.h"
#include <nlohmann/json.hpp>

namespace inferdeck::llama_wrapper {

namespace {

using inferdeck::foundation::Error;
using inferdeck::foundation::ErrorCode;
using inferdeck::foundation::Result;
using inferdeck::foundation::LOG_INFO;
using inferdeck::foundation::LOG_WARN;
using inferdeck::foundation::LOG_ERROR;
using inferdeck::model::InferenceRequest;
using inferdeck::model::InferenceResult;
using inferdeck::model::ChatMessage;
using inferdeck::model::InferenceDelta;
using inferdeck::model::ToolCall;
using inferdeck::model::ToolCallDelta;

#ifdef _WIN32
static void log_stack_overflow(int iteration, int prompt_tokens, const char* model_name) {
    std::ofstream log("logs/inference.log", std::ios::app);
    if (log.is_open()) {
        log << "[STACK_OVERFLOW] predict_stream iteration=" << iteration
            << " prompt_tokens=" << prompt_tokens
            << " model=" << (model_name ? model_name : "unknown")
            << " timestamp=" << std::time(nullptr) << "\n";
    }
    std::cerr << "[STACK_OVERFLOW] predict_stream iteration=" << iteration
              << " prompt_tokens=" << prompt_tokens
              << " model=" << (model_name ? model_name : "unknown") << std::endl;
}
#endif

inline Error make_error(ErrorCode code, std::string msg) {
  return Error{code, std::move(msg)};
}

std::string random_string(std::size_t n = 32) {
  static constexpr char chars[] =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  static thread_local std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<std::size_t> dist(0, sizeof(chars) - 2);
  std::string out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(chars[dist(rng)]);
  }
  return out;
}

std::string gen_tool_call_id() {
  return random_string();
}

static int common_prefix_length(
    const std::vector<int>& previous,
    const std::vector<llama_token>& current) {
  const auto n = std::min(previous.size(), current.size());
  std::size_t i = 0;
  while (i < n && previous[i] == current[i]) {
    ++i;
  }
  return static_cast<int>(i);
}

static void take_recurrent_checkpoint(
    llama_context* ctx, int seq_id, int pos,
    std::vector<uint8_t>& out_buf, int& out_pos) {
  const size_t sz = llama_state_seq_get_size_ext(
      ctx, seq_id, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
  if (sz == 0) { out_buf.clear(); return; }
  out_buf.resize(sz);
  const size_t written = llama_state_seq_get_data_ext(
      ctx, out_buf.data(), sz, seq_id, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
  if (written != sz) { out_buf.clear(); return; }
  out_pos = pos;
}

static int prepare_prompt_cache(
    llama_context* ctx,
    const std::vector<int>& previous_prompt_tokens,
    const std::vector<llama_token>& prompt_tokens,
    int seq_id,
    const std::string& model_name,
    const std::vector<uint8_t>& checkpoint_buf,
    int checkpoint_pos) {
  int n_past = common_prefix_length(previous_prompt_tokens, prompt_tokens);
  if (n_past >= static_cast<int>(prompt_tokens.size()) && n_past > 0) {
    --n_past;
  }
  auto* mem = llama_get_memory(ctx);
  if (n_past <= 0) {
    llama_memory_clear(mem, true);
    return 0;
  }
  const auto pos_max = llama_memory_seq_pos_max(mem, seq_id);
  if (n_past > pos_max) {
    LOG_INFO("llama_prompt_cache_extend",
             "model={} cached_prompt_tokens={} prompt_tokens={}",
             model_name,
             n_past,
             prompt_tokens.size());
    return n_past;
  }
  if (!llama_memory_seq_rm(mem, seq_id, n_past, -1)) {
    if (!checkpoint_buf.empty() && checkpoint_pos <= n_past) {
      llama_memory_clear(mem, true);
      const size_t restored = llama_state_seq_set_data_ext(
          ctx, checkpoint_buf.data(), checkpoint_buf.size(),
          seq_id, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
      if (restored > 0) {
        LOG_INFO("llama_prompt_cache_checkpoint_restore",
                 "model={} checkpoint_pos={} n_past={} prompt_tokens={}",
                 model_name,
                 checkpoint_pos,
                 n_past,
                 prompt_tokens.size());
        return checkpoint_pos;
      }
    }
    LOG_WARN("llama_prompt_cache_fallback",
             "model={} cached_prompt_tokens={} reason=seq_rm_failed pos_min={} pos_max={}",
             model_name,
             n_past,
             llama_memory_seq_pos_min(mem, seq_id),
             llama_memory_seq_pos_max(mem, seq_id));
    llama_memory_clear(mem, true);
    return 0;
  }
  LOG_INFO("llama_prompt_cache_reuse",
           "model={} cached_prompt_tokens={} prompt_tokens={}",
           model_name,
           n_past,
           prompt_tokens.size());
  return n_past;
}

static bool maybe_truncate_prompt(
    std::vector<llama_token>& prompt_tokens,
    int n_ctx,
    int req_max_tokens,
    const std::string& model_name) {
  const int n_tokens = static_cast<int>(prompt_tokens.size());
  if (n_tokens < n_ctx) return false;
  // Guard the clamp bounds: when n_ctx < 1024 the upper bound (n_ctx/4) falls
  // below 256, so std::clamp(x, 256, hi) would have lo > hi, which is UB.
  const int reserve_hi = n_ctx / 4;
  const int reserve = std::clamp(req_max_tokens > 0 ? req_max_tokens : 1024,
                                 std::min(256, reserve_hi), reserve_hi);
  const int target = n_ctx - reserve - 1;
  const int keep_head = std::min(1024, target / 4);
  const int keep_tail = target - keep_head;
  std::vector<llama_token> kept;
  kept.reserve(static_cast<std::size_t>(target));
  kept.insert(kept.end(), prompt_tokens.begin(), prompt_tokens.begin() + keep_head);
  kept.insert(kept.end(), prompt_tokens.end() - keep_tail, prompt_tokens.end());
  LOG_WARN("llama_prompt_truncated",
           "model={} original_tokens={} kept_tokens={} keep_head={} keep_tail={} n_ctx={}",
           model_name,
           n_tokens,
           kept.size(),
           keep_head,
           keep_tail,
           n_ctx);
  prompt_tokens = std::move(kept);
  return true;
}

static bool process_prompt_chunks(
    llama_context* ctx,
    const std::vector<llama_token>& prompt_tokens,
    int start_token_index,
    int seq_id,
    int n_batch,
    const std::string& model_name) {
  const int n_tokens = static_cast<int>(prompt_tokens.size());
  int n_prompt_processed = std::max(0, std::min(start_token_index, n_tokens));
  while (n_prompt_processed < n_tokens) {
    int n_chunk = std::min(n_batch, n_tokens - n_prompt_processed);
    llama_batch batch = llama_batch_init(n_chunk, 0, 1);
    for (int i = 0; i < n_chunk; ++i) {
      batch.token[i] = prompt_tokens[n_prompt_processed + i];
      batch.pos[i] = n_prompt_processed + i;
      batch.n_seq_id[i] = 1;
      batch.seq_id[i][0] = seq_id;
      batch.logits[i] = (i == n_chunk - 1) ? 1 : 0;
    }
    batch.n_tokens = n_chunk;
    const int rc = llama_decode(ctx, batch);
    if (rc != 0) {
      LOG_ERROR("llama_prompt_decode_failed",
                "model={} rc={} chunk_start={} chunk_tokens={} prompt_tokens={} n_batch={} n_ctx={} n_ctx_seq={}",
                model_name,
                rc,
                n_prompt_processed,
                n_chunk,
                n_tokens,
                n_batch,
                llama_n_ctx(ctx),
                llama_n_ctx_seq(ctx));
      llama_batch_free(batch);
      return false;
    }
    llama_batch_free(batch);
    n_prompt_processed += n_chunk;
  }
  return true;
}

static bool g_backend_initialized = false;

std::string lower_copy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

ggml_type cache_type_from_string(const std::string& type) {
  const auto t = lower_copy(type);
  if (t == "f32") return GGML_TYPE_F32;
  if (t == "f16") return GGML_TYPE_F16;
  if (t == "bf16") return GGML_TYPE_BF16;
  if (t == "q8_0") return GGML_TYPE_Q8_0;
  if (t == "q4_0") return GGML_TYPE_Q4_0;
  if (t == "q4_1") return GGML_TYPE_Q4_1;
  if (t == "q5_0") return GGML_TYPE_Q5_0;
  if (t == "q5_1") return GGML_TYPE_Q5_1;
  if (t == "iq4_nl") return GGML_TYPE_IQ4_NL;
  return GGML_TYPE_F16;
}

llama_flash_attn_type flash_attn_from_string(const std::string& value) {
  const auto v = lower_copy(value);
  if (v == "on" || v == "enabled" || v == "true" || v == "1") {
    return LLAMA_FLASH_ATTN_TYPE_ENABLED;
  }
  if (v == "off" || v == "disabled" || v == "false" || v == "0") {
    return LLAMA_FLASH_ATTN_TYPE_DISABLED;
  }
  return LLAMA_FLASH_ATTN_TYPE_AUTO;
}

struct ProcessMemorySnapshot {
  std::uint64_t working_set_mb{0};
  std::uint64_t private_mb{0};
  std::uint64_t system_commit_mb{0};
  std::uint64_t gpu_local_mb{0};
  std::uint64_t gpu_nonlocal_mb{0};
  bool gpu_memory_available{false};
};

std::optional<ProcessMemorySnapshot> process_memory_snapshot() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                            sizeof(pmc))) {
    return std::nullopt;
  }
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  ProcessMemorySnapshot out;
  out.working_set_mb = static_cast<std::uint64_t>(pmc.WorkingSetSize / (1024 * 1024));
  out.private_mb = static_cast<std::uint64_t>(pmc.PrivateUsage / (1024 * 1024));
  if (GlobalMemoryStatusEx(&mem)) {
    out.system_commit_mb =
        static_cast<std::uint64_t>((mem.ullTotalPageFile - mem.ullAvailPageFile) / (1024 * 1024));
  }
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf())))) {
    for (UINT i = 0;; ++i) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapters1(i, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
      if (FAILED(adapter.As(&adapter3))) {
        continue;
      }
      DXGI_QUERY_VIDEO_MEMORY_INFO local{};
      DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal{};
      if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
        out.gpu_local_mb += static_cast<std::uint64_t>(local.CurrentUsage / (1024 * 1024));
        out.gpu_memory_available = true;
      }
      if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal))) {
        out.gpu_nonlocal_mb += static_cast<std::uint64_t>(nonlocal.CurrentUsage / (1024 * 1024));
        out.gpu_memory_available = true;
      }
    }
  }
  return out;
#else
  return std::nullopt;
#endif
}

void log_memory_snapshot(const char* event, const std::string& model_name) {
  const auto snap = process_memory_snapshot();
  if (!snap) {
    LOG_INFO(event, "model={} process_memory=unavailable", model_name);
    return;
  }
  if (snap->gpu_memory_available) {
    LOG_INFO(event,
             "model={} working_set_mb={} private_mb={} system_commit_mb={} gpu_local_mb={} gpu_nonlocal_mb={}",
             model_name,
             snap->working_set_mb,
             snap->private_mb,
             snap->system_commit_mb,
             snap->gpu_local_mb,
             snap->gpu_nonlocal_mb);
    return;
  }
  LOG_INFO(event,
           "model={} working_set_mb={} private_mb={} system_commit_mb={} gpu_memory=unavailable",
           model_name,
           snap->working_set_mb,
           snap->private_mb,
           snap->system_commit_mb);
}

void log_slot_release(
    const std::string& model_name,
    int n_tokens,
    bool truncated,
    int n_decoded,
    int n_ctx) {
  LOG_INFO("llama_slot_release",
           "model={} n_tokens={} truncated={} n_decoded={} n_ctx={}",
           model_name,
           n_tokens,
           truncated,
           n_decoded,
           n_ctx);
}

void log_token_decode_failed(
    const std::string& model_name,
    int rc,
    int token_index,
    llama_token token_id,
    int pos,
    int n_cur,
    int n_ctx,
    int n_decoded) {
  LOG_ERROR("llama_token_decode_failed",
            "model={} rc={} token_index={} token_id={} pos={} n_cur={} n_ctx={} n_decoded={}",
            model_name,
            rc,
            token_index,
            token_id,
            pos,
            n_cur,
            n_ctx,
            n_decoded);
  log_memory_snapshot("llama_token_decode_failed_memory", model_name);
}

std::string normalize_path(const std::string& p) {
  std::filesystem::path path(p);
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) return std::filesystem::absolute(path, ec).string();
  return p;
}

std::string role_to_template_role(const std::string& role) {
  if (role == "system") return "system";
  if (role == "assistant") return "assistant";
  if (role == "tool") return "tool";
  return "user";
}

common_chat_msg to_common_chat_msg(const ChatMessage& msg) {
  common_chat_msg cmsg;
  cmsg.role = role_to_template_role(msg.role).c_str();
  cmsg.content = msg.content.c_str();
  cmsg.tool_call_id = msg.tool_call_id.c_str();

  if (msg.role == "assistant" && !msg.content.empty()) {
    try {
      auto calls = nlohmann::json::parse(msg.content);
      if (calls.is_array()) {
        for (const auto& call : calls) {
          common_chat_tool_call tc;
          if (call.contains("function")) {
            const auto& fn = call["function"];
            tc.name = fn.value("name", "").c_str();
            if (fn.contains("arguments")) {
              const auto& a = fn["arguments"];
              tc.arguments = a.is_string() ? a.get<std::string>().c_str() : a.dump().c_str();
            }
          } else if (call.contains("name")) {
            tc.name = call.value("name", "").c_str();
            if (call.contains("arguments")) {
              const auto& a = call["arguments"];
              tc.arguments = a.is_string() ? a.get<std::string>().c_str() : a.dump().c_str();
            }
          }
          tc.id = call.value("id", "").c_str();
          if (!tc.name.empty()) cmsg.tool_calls.push_back(std::move(tc));
        }
      }
    } catch (...) {}
  }

  if (msg.role == "tool") {
    cmsg.content = msg.content.c_str();
    cmsg.tool_call_id = msg.tool_call_id.c_str();
  }

  return cmsg;
}

std::vector<common_chat_tool> parse_tools_json(const std::string& tools_json) {
  std::vector<common_chat_tool> tools;
  if (tools_json.empty()) return tools;
  try {
    auto j = nlohmann::json::parse(tools_json);
    if (!j.is_array()) return tools;
    for (const auto& item : j) {
      common_chat_tool tool;
      if (item.contains("function")) {
        const auto& fn = item["function"];
        tool.name = fn.value("name", "").c_str();
        tool.description = fn.value("description", "").c_str();
        if (fn.contains("parameters")) {
          tool.parameters = fn["parameters"].dump().c_str();
        }
      } else {
        tool.name = item.value("name", "").c_str();
        tool.description = item.value("description", "").c_str();
        if (item.contains("parameters")) {
          tool.parameters = item["parameters"].dump().c_str();
        }
      }
      if (!tool.name.empty()) tools.push_back(std::move(tool));
    }
  } catch (...) {}
  return tools;
}

common_reasoning_format parse_reasoning_format(const std::string& value) {
  if (value.empty()) return COMMON_REASONING_FORMAT_AUTO;
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  std::replace(lower.begin(), lower.end(), '-', '_');
  if (lower == "none") return COMMON_REASONING_FORMAT_NONE;
  if (lower == "deepseek") return COMMON_REASONING_FORMAT_DEEPSEEK;
  if (lower == "deepseek_legacy") return COMMON_REASONING_FORMAT_DEEPSEEK_LEGACY;
  if (lower == "auto") return COMMON_REASONING_FORMAT_AUTO;
  return COMMON_REASONING_FORMAT_AUTO;
}

common_chat_tool_choice parse_tool_choice(const nlohmann::ordered_json& body) {
  if (!body.contains("tool_choice") || body["tool_choice"].is_null()) {
    return COMMON_CHAT_TOOL_CHOICE_AUTO;
  }
  const auto& tc = body["tool_choice"];
  if (tc.is_string()) {
    return common_chat_tool_choice_parse_oaicompat(tc.get<std::string>());
  }
  if (tc.is_object()) {
    const auto type = tc.value("type", std::string{"auto"});
    if (type == "none") return COMMON_CHAT_TOOL_CHOICE_NONE;
    if (type == "required" || type == "any") return COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    return COMMON_CHAT_TOOL_CHOICE_AUTO;
  }
  return COMMON_CHAT_TOOL_CHOICE_AUTO;
}

std::vector<llama_token> tokenize_stop_strings(
    const llama_vocab* vocab, const std::vector<std::string>& stops) {
  std::vector<llama_token> tokens;
  for (const auto& stop_str : stops) {
    if (stop_str.empty()) continue;
    int n = llama_tokenize(vocab, stop_str.c_str(), static_cast<int>(stop_str.size()),
                           nullptr, 0, false, true);
    if (n == 1) {
      std::vector<llama_token> tmp(1);
      llama_tokenize(vocab, stop_str.c_str(), static_cast<int>(stop_str.size()),
                     tmp.data(), n, false, true);
      tokens.push_back(tmp[0]);
    }
  }
  return tokens;
}

std::string tool_call_json(const common_chat_tool_call& tc) {
  nlohmann::json j = {
      {"type", "function"},
      {"function", {
          {"name", tc.name},
          {"arguments", tc.arguments},
      }},
  };
  if (!tc.id.empty()) j["id"] = tc.id;
  return j.dump();
}

ToolCall to_tool_call(const common_chat_tool_call& tc) {
  ToolCall out;
  out.id = tc.id;
  out.type = "function";
  out.function_name = tc.name;
  out.function_arguments = tc.arguments;
  return out;
}

std::string json_arguments(const nlohmann::json& args) {
  return args.is_string() ? args.get<std::string>() : args.dump();
}

std::optional<ToolCall> parse_tool_call_object(const nlohmann::json& obj, std::size_t index) {
  if (!obj.is_object()) return std::nullopt;

  ToolCall call;
  call.id = obj.value("id", "call_" + std::to_string(index));
  call.type = "function";

  if (obj.contains("function") && obj["function"].is_object()) {
    const auto& fn = obj["function"];
    call.function_name = fn.value("name", "");
    if (fn.contains("arguments")) call.function_arguments = json_arguments(fn["arguments"]);
  } else {
    call.function_name = obj.value("name", "");
    if (obj.contains("arguments")) call.function_arguments = json_arguments(obj["arguments"]);
  }

  if (call.function_name.empty()) return std::nullopt;
  if (call.function_arguments.empty()) call.function_arguments = "{}";
  return call;
}

std::optional<std::vector<ToolCall>> parse_fallback_tool_calls(const std::string& text) {
  std::vector<std::string> candidates;
  candidates.push_back(text);

  const auto fence = text.find("```");
  if (fence != std::string::npos) {
    auto body_start = text.find('\n', fence);
    if (body_start != std::string::npos) {
      ++body_start;
      const auto body_end = text.find("```", body_start);
      if (body_end != std::string::npos && body_end > body_start) {
        candidates.push_back(text.substr(body_start, body_end - body_start));
      }
    }
  }

  const auto first_brace = text.find('{');
  const auto last_brace = text.rfind('}');
  if (first_brace != std::string::npos && last_brace != std::string::npos && last_brace > first_brace) {
    candidates.push_back(text.substr(first_brace, last_brace - first_brace + 1));
  }

  for (const auto& candidate : candidates) {
    try {
      auto json = nlohmann::json::parse(candidate);
      std::vector<ToolCall> calls;
      if (json.is_object() && json.contains("tool_calls") && json["tool_calls"].is_array()) {
        std::size_t i = 0;
        for (const auto& item : json["tool_calls"]) {
          if (auto call = parse_tool_call_object(item, i++)) calls.push_back(std::move(*call));
        }
      } else if (json.is_array()) {
        std::size_t i = 0;
        for (const auto& item : json) {
          if (auto call = parse_tool_call_object(item, i++)) calls.push_back(std::move(*call));
        }
      } else if (auto call = parse_tool_call_object(json, 0)) {
        calls.push_back(std::move(*call));
      }
      if (!calls.empty()) return calls;
    } catch (...) {}
  }

  return std::nullopt;
}

InferenceDelta to_delta(const common_chat_msg_diff& diff) {
  InferenceDelta out;
  out.content = diff.content_delta;
  out.reasoning_text = diff.reasoning_content_delta;
  if (diff.tool_call_index != std::string::npos) {
    ToolCallDelta tc;
    tc.index = diff.tool_call_index;
    tc.id = diff.tool_call_delta.id;
    if (!tc.id.empty()) tc.type = "function";
    tc.function_name = diff.tool_call_delta.name;
    tc.function_arguments = diff.tool_call_delta.arguments;
    out.tool_calls.push_back(std::move(tc));
  }
  return out;
}

class StreamingChatParserState {
public:
  explicit StreamingChatParserState(common_chat_parser_params params)
      : parser_params_(std::move(params)) {
    if (!parser_params_.echo) {
      try {
        chat_msg_ = common_chat_parse("", true, parser_params_);
      } catch (...) {
        chat_msg_ = common_chat_msg{};
      }
    }
  }

  std::vector<common_chat_msg_diff> update(
      const std::string& text_added,
      bool is_partial,
      bool filter_tool_calls) {
    std::vector<common_chat_msg_diff> diffs;
    generated_text_ += text_added;
    auto previous = chat_msg_;
    auto next = common_chat_parse(generated_text_, is_partial, parser_params_);
    if (next.empty()) {
      return diffs;
    }

    next.set_tool_call_ids(generated_tool_call_ids_, gen_tool_call_id);
    chat_msg_ = std::move(next);
    auto all_diffs = common_chat_msg_diff::compute_diffs(previous, chat_msg_);
    if (!filter_tool_calls) {
      return all_diffs;
    }

    for (auto& d : all_diffs) {
      for (std::size_t i = 0; i < chat_msg_.tool_calls.size(); ++i) {
        if (sent_tool_call_names_.count(i) || chat_msg_.tool_calls[i].name.empty()) {
          continue;
        }
        if (d.tool_call_index != i || !d.tool_call_delta.arguments.empty()) {
          common_chat_msg_diff header;
          header.tool_call_index = i;
          header.tool_call_delta.id = chat_msg_.tool_calls[i].id;
          header.tool_call_delta.name = chat_msg_.tool_calls[i].name;
          diffs.push_back(std::move(header));
          sent_tool_call_names_.insert(i);
        }
      }

      if (d.tool_call_index == std::string::npos) {
        diffs.push_back(std::move(d));
      } else {
        const std::size_t i = d.tool_call_index;
        if (sent_tool_call_names_.count(i)) {
          if (!d.tool_call_delta.arguments.empty()) {
            d.tool_call_delta.name.clear();
            d.tool_call_delta.id.clear();
            diffs.push_back(std::move(d));
          }
        } else {
          if (!d.tool_call_delta.arguments.empty() || !is_partial) {
            d.tool_call_delta.name = chat_msg_.tool_calls[i].name;
            d.tool_call_delta.id = chat_msg_.tool_calls[i].id;
            diffs.push_back(std::move(d));
            sent_tool_call_names_.insert(i);
          }
        }
      }
    }

    if (!is_partial) {
      for (std::size_t i = 0; i < chat_msg_.tool_calls.size(); ++i) {
        if (!sent_tool_call_names_.count(i) && !chat_msg_.tool_calls[i].name.empty()) {
          common_chat_msg_diff header;
          header.tool_call_index = i;
          header.tool_call_delta.id = chat_msg_.tool_calls[i].id;
          header.tool_call_delta.name = chat_msg_.tool_calls[i].name;
          diffs.push_back(std::move(header));
          sent_tool_call_names_.insert(i);
        }
      }
    }

    return diffs;
  }

private:
  common_chat_parser_params parser_params_;
  common_chat_msg chat_msg_;
  std::string generated_text_;
  std::vector<std::string> generated_tool_call_ids_;
  std::unordered_set<std::size_t> sent_tool_call_names_;
};

void apply_parsed_message(InferenceResult& out, const common_chat_msg& msg) {
  out.text = msg.content;
  out.reasoning_text = msg.reasoning_content;
  out.tool_calls.clear();
  out.tool_calls_json.clear();
  for (const auto& tc : msg.tool_calls) {
    out.tool_calls.push_back(to_tool_call(tc));
    out.tool_calls_json.push_back(tool_call_json(tc));
  }
}

common_chat_msg parse_final_message_with_ids(
    const std::string& generated,
    const common_chat_parser_params& parser_params) {
  auto msg = common_chat_parse(generated, false, parser_params);
  std::vector<std::string> ids;
  msg.set_tool_call_ids(ids, gen_tool_call_id);
  return msg;
}

void apply_fallback_tool_calls(InferenceResult& out, const std::string& generated) {
  auto calls = parse_fallback_tool_calls(generated);
  if (!calls || calls->empty()) return;
  out.text.clear();
  out.tool_calls = std::move(*calls);
  out.tool_calls_json.clear();
  for (const auto& tc : out.tool_calls) {
    nlohmann::json j = {
        {"type", "function"},
        {"function", {
            {"name", tc.function_name},
            {"arguments", tc.function_arguments},
        }},
    };
    if (!tc.id.empty()) j["id"] = tc.id;
    out.tool_calls_json.push_back(j.dump());
  }
}

bool fallback_tool_call_complete(const std::string& generated) {
  return generated.find("```") != std::string::npos && parse_fallback_tool_calls(generated).has_value();
}

} // namespace

std::string LlamaCppModel::version() {
  const char* info = llama_print_system_info();
  return info ? std::string(info) : std::string("unknown");
}

void LlamaCppModel::init_backend() {
  if (!g_backend_initialized) {
    llama_backend_init();
    g_backend_initialized = true;
    LOG_INFO("llama_backend_init", "Vulkan backend initialized");
  }
}
void LlamaCppModel::shutdown_backend() {
  if (g_backend_initialized) {
    llama_backend_free();
    g_backend_initialized = false;
  }
}

LlamaCppModel::LlamaCppModel(inferdeck::model::ModelInfo info, LlamaCppConfig cfg)
    : info_(std::move(info)), cfg_(std::move(cfg)) {
  resolved_gguf_path_ = normalize_path(info_.gguf_path);
}

LlamaCppModel::~LlamaCppModel() {
  // Stop scheduler before taking the mutex so no decode races with cleanup.
  if (scheduler_) {
    scheduler_->stop();
    scheduler_.reset();
  }
  std::lock_guard lk(mtx_);
  for (auto& s : slots_) s.busy = false;
  slots_.clear();
  if (shared_ctx_) {
    llama_free(shared_ctx_);
    shared_ctx_ = nullptr;
  }
  if (chat_templates_) {
    common_chat_templates_free(chat_templates_);
    chat_templates_ = nullptr;
  }
  if (model_) {
    llama_model_free(model_);
    model_ = nullptr;
  }
  vocab_ = nullptr;
  loaded_.store(false);
}

Result<void> LlamaCppModel::load() {
  std::lock_guard lk(mtx_);
  if (loaded_.load()) return Result<void>{};
  if (resolved_gguf_path_.empty()) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::NotFound, "empty gguf_path"));
  }
  std::error_code ec;
  if (!std::filesystem::exists(resolved_gguf_path_, ec)) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::NotFound, "gguf not found: " + resolved_gguf_path_.string()));
  }

  llama_model_params mparams = llama_model_default_params();
  mparams.use_mmap = cfg_.use_mmap;
  mparams.use_mlock = cfg_.use_mlock;
  mparams.n_gpu_layers = cfg_.n_gpu_layers.value_or(-1);

  llama_backend_init();
  const char* sys_info = llama_print_system_info();
  if (sys_info) {
    LOG_INFO("llama_system_info", "{}", sys_info);
  }
  LOG_INFO("llama_model_load_config",
           "model={} path={} use_mmap={} use_mlock={} n_gpu_layers={} n_ctx={} n_slots={} n_batch={} n_ubatch={} flash_attn={} kv_offload={} op_offload={} cache_type_k={} cache_type_v={} swa_full={}",
           info_.name,
           resolved_gguf_path_.string(),
           cfg_.use_mmap,
           cfg_.use_mlock,
           mparams.n_gpu_layers,
           info_.context_size,
           info_.n_slots,
           cfg_.n_batch,
           cfg_.n_ubatch,
           cfg_.flash_attn,
           cfg_.kv_offload,
           cfg_.op_offload,
           cfg_.cache_type_k,
           cfg_.cache_type_v,
           cfg_.swa_full);
  log_memory_snapshot("llama_model_load_memory_before", info_.name);

  model_ = llama_model_load_from_file(resolved_gguf_path_.string().c_str(), mparams);
  if (model_ == nullptr) {
    const char* err = llama_print_system_info();
    LOG_ERROR("model_load_failed", "llama_model_load_from_file returned null for {}", resolved_gguf_path_.string());
    if (err) LOG_ERROR("model_load_failed", "system_info: {}", err);
    return Result<void>(std::unexpect,
        make_error(ErrorCode::Internal,
                   "llama_model_load_from_file returned null for " + resolved_gguf_path_.string()));
  }
  log_memory_snapshot("llama_model_loaded_memory_after", info_.name);
  vocab_ = llama_model_get_vocab(model_);
  if (vocab_ == nullptr) {
    llama_model_free(model_);
    model_ = nullptr;
    return Result<void>(std::unexpect,
        make_error(ErrorCode::ParseError, "llama_model_get_vocab returned null"));
  }
  chat_templates_ = common_chat_templates_init(model_, "").release();
  if (chat_templates_ == nullptr) {
    llama_model_free(model_);
    model_ = nullptr;
    return Result<void>(std::unexpect,
        make_error(ErrorCode::ParseError, "common_chat_templates_init returned null"));
  }
  auto ctx_res = init_shared_context_locked();
  if (!ctx_res.has_value()) {
    if (shared_ctx_) { llama_free(shared_ctx_); shared_ctx_ = nullptr; }
    slots_.clear();
    llama_model_free(model_);
    model_ = nullptr;
    return ctx_res;
  }
  // Populate chat_template_meta_ once from the model's Jinja template.
  {
    InferenceRequest dummy;
    dummy.messages.push_back({"user", "hello"});
    auto meta_res = apply_chat_template(dummy);
    if (meta_res.has_value()) chat_template_meta_ = std::move(meta_res->meta);
  }
  loaded_.store(true);
  log_memory_snapshot("llama_contexts_initialized_memory_after", info_.name);
  LOG_INFO("chat_template_loaded", "model={} kind=jinja", info_.name);
  return Result<void>{};
}

Result<void> LlamaCppModel::init_shared_context_locked() {
  // One shared context for all slots.
  // n_ctx = context_size * n_slots so each slot gets its own context window via sequence IDs.
  // n_seq_max = n_slots so the KV cache can track each slot's sequence independently.
  const int n_slots = std::max(1, info_.n_slots);
  const int ctx_per_slot = std::max(512, info_.context_size);
  const int total_ctx = ctx_per_slot * n_slots;

  llama_context_params cparams = llama_context_default_params();
  cparams.n_ctx      = static_cast<std::uint32_t>(total_ctx);
  cparams.n_seq_max  = static_cast<std::uint32_t>(n_slots);
  cparams.n_threads  = cfg_.n_threads;
  cparams.n_batch    = static_cast<std::uint32_t>(std::max(1, cfg_.n_batch));
  cparams.n_ubatch   = static_cast<std::uint32_t>(std::max(1, cfg_.n_ubatch));
  cparams.flash_attn_type = flash_attn_from_string(cfg_.flash_attn);
  cparams.offload_kqv = cfg_.kv_offload;
  cparams.op_offload  = cfg_.op_offload;
  cparams.swa_full    = cfg_.swa_full;
  cparams.type_k      = cache_type_from_string(cfg_.cache_type_k);
  cparams.type_v      = cache_type_from_string(cfg_.cache_type_v);

  LOG_INFO("llama_shared_context_config",
           "model={} n_slots={} ctx_per_slot={} total_ctx={} n_seq_max={} "
           "n_batch={} n_ubatch={} flash_attn={} kv_offload={} op_offload={} "
           "cache_type_k={} cache_type_v={} swa_full={}",
           info_.name, n_slots, ctx_per_slot, total_ctx, n_slots,
           cparams.n_batch, cparams.n_ubatch,
           cfg_.flash_attn, cfg_.kv_offload, cfg_.op_offload,
           cfg_.cache_type_k, cfg_.cache_type_v, cfg_.swa_full);

  shared_ctx_ = llama_init_from_model(model_, cparams);
  if (shared_ctx_ == nullptr) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::OutOfMemory,
                   "llama_init_from_model returned null (shared context, total_ctx=" +
                   std::to_string(total_ctx) + ")"));
  }

  slots_.clear();
  slots_.resize(n_slots);

  // Spawn the scheduler that owns the decode loop for this context.
  scheduler_ = std::make_unique<ContinuousBatchScheduler>(
      shared_ctx_, model_, vocab_, cfg_.n_batch);

  return Result<void>{};
}

Result<void> LlamaCppModel::unload() {
  // Stop the scheduler first (joins its thread) so no decode can race with teardown.
  if (scheduler_) {
    scheduler_->stop();
    scheduler_.reset();
  }
  std::lock_guard lk(mtx_);
  if (!loaded_.load()) return Result<void>{};
  log_memory_snapshot("llama_model_unload_memory_before", info_.name);
  for (auto& s : slots_) s.busy = false;
  slots_.clear();
  if (shared_ctx_) {
    llama_free(shared_ctx_);
    shared_ctx_ = nullptr;
  }
  if (chat_templates_) {
    common_chat_templates_free(chat_templates_);
    chat_templates_ = nullptr;
  }
  if (model_) {
    llama_model_free(model_);
    model_ = nullptr;
  }
  vocab_ = nullptr;
  loaded_.store(false);
  log_memory_snapshot("llama_model_unload_memory_after", info_.name);
  return Result<void>{};
}

int LlamaCppModel::vram_usage_mb() const noexcept {
  return info_.vram_required_mb;
}

int LlamaCppModel::n_free_slots() const noexcept {
  std::lock_guard lk(mtx_);
  int free = 0;
  for (const auto& s : slots_) if (!s.busy) ++free;
  return free;
}

Result<int> LlamaCppModel::acquire_slot() {
  std::lock_guard lk(mtx_);
  if (!loaded_.load()) {
    return Result<int>(std::unexpect,
        make_error(ErrorCode::Internal, "model not loaded"));
  }
  for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
    if (!slots_[i].busy) {
      slots_[i].busy = true;
      return Result<int>(i);
    }
  }
  return Result<int>(std::unexpect, make_error(ErrorCode::Unavailable, "no free slots"));
}

Result<void> LlamaCppModel::release_slot(int slot_id) {
  std::lock_guard lk(mtx_);
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::InvalidArgument,
                   "slot_id out of range: " + std::to_string(slot_id)));
  }
  auto& slot = slots_[slot_id];
  slot.busy = false;
  return Result<void>{};
}

bool LlamaCppModel::slot_busy(int slot_id) const noexcept {
  std::lock_guard lk(mtx_);
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) return false;
  return slots_[slot_id].busy;
}

Result<void> LlamaCppModel::reset_all_slots() noexcept {
  std::lock_guard lk(mtx_);
  if (shared_ctx_) {
    // Clear KV entries for every slot's sequence individually.
    // This avoids llama_memory_clear which would also clear non-slot sequences.
    auto* mem = llama_get_memory(shared_ctx_);
    if (mem) {
      for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        llama_memory_seq_rm(mem, i, 0, -1);
      }
    }
  }
  for (auto& s : slots_) {
    s.busy = false;
    s.last_prompt_tokens.clear();
    s.recurrent_checkpoint.clear();
    s.checkpoint_pos = 0;
  }
  return Result<void>{};
}

Result<ChatTemplateResult> LlamaCppModel::apply_chat_template(
    const InferenceRequest& req, int max_prompt_tokens) {
  if (!chat_templates_) {
    return Result<ChatTemplateResult>(std::unexpect, make_error(ErrorCode::Internal, "chat templates not initialized"));
  }

  common_chat_templates_inputs inputs;
  nlohmann::ordered_json body = nlohmann::ordered_json::object();
  if (!req.openai_body_json.empty()) {
    try {
      body = nlohmann::ordered_json::parse(req.openai_body_json);
    } catch (const std::exception& e) {
      return Result<ChatTemplateResult>(std::unexpect,
          make_error(ErrorCode::ParseError, std::string("invalid OpenAI request JSON: ") + e.what()));
    }
  }

  inputs.reasoning_format = parse_reasoning_format(info_.reasoning_format.empty() ? cfg_.reasoning_format : info_.reasoning_format);
  if (body.contains("reasoning_format") && body["reasoning_format"].is_string()) {
    inputs.reasoning_format = parse_reasoning_format(body["reasoning_format"].get<std::string>());
  }
  inputs.add_generation_prompt = true;
  if (body.contains("add_generation_prompt") && body["add_generation_prompt"].is_boolean()) {
    inputs.add_generation_prompt = body["add_generation_prompt"].get<bool>();
  }
  inputs.use_jinja = true;
  inputs.enable_thinking = common_chat_templates_support_enable_thinking(chat_templates_);
  auto caps = common_chat_templates_get_caps(chat_templates_);
  inputs.parallel_tool_calls = caps["supports_parallel_tool_calls"];

  if (body.contains("messages") && body["messages"].is_array()) {
    for (auto& m : body["messages"]) {
      if (m.is_object() && !m.contains("content") && !m.contains("tool_calls")) {
        m["content"] = "";
      }
    }
    try {
      inputs.messages = common_chat_msgs_parse_oaicompat(body["messages"]);
    } catch (const std::exception& e) {
      return Result<ChatTemplateResult>(std::unexpect,
          make_error(ErrorCode::ParseError, std::string("invalid OpenAI messages: ") + e.what()));
    }
  } else {
    for (const auto& m : req.messages) {
      inputs.messages.push_back(to_common_chat_msg(m));
    }
    if (inputs.messages.empty()) {
      common_chat_msg msg;
      msg.role = "user";
      msg.content = req.prompt;
      inputs.messages.push_back(std::move(msg));
    }
  }

  // DEBUG: log incoming request shape so we can compare OpenCode vs direct Ollama.
  {
    std::size_t sys_chars = 0, user_chars = 0, tool_result_chars = 0;
    int n_sys = 0, n_user = 0, n_assistant = 0, n_tool = 0;
    for (const auto& m : inputs.messages) {
      const std::size_t len = m.content.size();
      if (m.role == "system")    { ++n_sys; sys_chars += len; }
      else if (m.role == "user") { ++n_user; user_chars += len; }
      else if (m.role == "assistant") { ++n_assistant; }
      else if (m.role == "tool") { ++n_tool; tool_result_chars += len; }
    }
    const int n_tools_defined = body.contains("tools") && body["tools"].is_array()
                                  ? static_cast<int>(body["tools"].size()) : 0;
    const int max_tok = body.value("max_tokens", -1);
    LOG_INFO("request_shape",
             "model={} msgs={} [sys={} sys_chars={} user={} asst={} tool_results={} tool_result_chars={}] "
             "tools_defined={} max_tokens={}",
             info_.name, inputs.messages.size(),
             n_sys, sys_chars, n_user, n_assistant, n_tool, tool_result_chars,
             n_tools_defined, max_tok);
  }

  if (body.contains("tools") && body["tools"].is_array()) {
    try {
      inputs.tools = common_chat_tools_parse_oaicompat(body["tools"]);
    } catch (const std::exception& e) {
      return Result<ChatTemplateResult>(std::unexpect,
          make_error(ErrorCode::ParseError, std::string("invalid OpenAI tools: ") + e.what()));
    }
    inputs.tool_choice = parse_tool_choice(body);
  } else if (!req.tools_json.empty()) {
    inputs.tools = parse_tools_json(req.tools_json);
    inputs.tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;
  }
  if (!inputs.tools.empty() && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE &&
      body.contains("grammar")) {
    return Result<ChatTemplateResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument, "Cannot use custom grammar constraints with tools."));
  }
  if (body.contains("parallel_tool_calls") && body["parallel_tool_calls"].is_boolean()) {
    inputs.parallel_tool_calls = body["parallel_tool_calls"].get<bool>();
  }
  if (body.contains("chat_template_kwargs") && body["chat_template_kwargs"].is_object()) {
    for (const auto& item : body["chat_template_kwargs"].items()) {
      inputs.chat_template_kwargs[item.key()] = item.value().dump();
    }
    const auto it = inputs.chat_template_kwargs.find("enable_thinking");
    if (it != inputs.chat_template_kwargs.end()) {
      if (it->second == "true") inputs.enable_thinking = true;
      if (it->second == "false") inputs.enable_thinking = false;
    }
  }
  if (body.contains("grammar") && body["grammar"].is_string()) {
    inputs.grammar = body["grammar"].get<std::string>();
  }
  if (body.contains("json_schema")) {
    inputs.json_schema = body["json_schema"].is_null() ? "" : body["json_schema"].dump();
  }
  if (body.contains("response_format") && body["response_format"].is_object()) {
    const auto& rf = body["response_format"];
    const auto type = rf.value("type", std::string{});
    if (type == "json_object") {
      if (rf.contains("schema")) inputs.json_schema = rf["schema"].dump();
      else if (inputs.json_schema.empty()) inputs.json_schema = nlohmann::ordered_json::object().dump();
    } else if (type == "json_schema" && rf.contains("json_schema")) {
      const auto& wrapper = rf["json_schema"];
      if (wrapper.is_object() && wrapper.contains("schema")) {
        inputs.json_schema = wrapper["schema"].dump();
      }
    }
  }

  try {
  auto chat_params = common_chat_templates_apply(chat_templates_, inputs);

  // History-aware truncation (issue #38): rather than middle-dropping the raw
  // token stream (which severs conversation history and defeats KV prefix
  // reuse), drop the oldest *whole* non-system messages and re-template until
  // the prompt fits the budget. The leading system block and the most recent
  // turn are always preserved.
  if (max_prompt_tokens > 0) {
    auto count_prompt_tokens = [&](const std::string& p) -> int {
      if (p.empty()) return 0;
      const int n = llama_tokenize(vocab_, p.data(), static_cast<int>(p.size()),
                                   nullptr, 0, llama_vocab_get_add_bos(vocab_), true);
      return n < 0 ? -n : n;
    };
    std::size_t sys_end = 0;
    while (sys_end < inputs.messages.size() && inputs.messages[sys_end].role == "system")
      ++sys_end;
    int dropped = 0;
    while (count_prompt_tokens(chat_params.prompt) >= max_prompt_tokens &&
           inputs.messages.size() - sys_end > 1) {
      inputs.messages.erase(inputs.messages.begin() + static_cast<std::ptrdiff_t>(sys_end));
      ++dropped;
      // Drop any now-orphaned tool results whose assistant tool_call was removed.
      while (inputs.messages.size() - sys_end > 1 &&
             inputs.messages[sys_end].role == "tool") {
        inputs.messages.erase(inputs.messages.begin() + static_cast<std::ptrdiff_t>(sys_end));
        ++dropped;
      }
      chat_params = common_chat_templates_apply(chat_templates_, inputs);
    }
    if (dropped > 0) {
      LOG_WARN("chat_history_truncated",
               "model={} dropped_messages={} kept_messages={} prompt_tokens={} budget={}",
               info_.name, dropped, inputs.messages.size(),
               count_prompt_tokens(chat_params.prompt), max_prompt_tokens);
    }
  }

  ChatTemplateMeta meta;
  meta.thinking_start_tag = chat_params.thinking_start_tag;
  meta.thinking_end_tag = chat_params.thinking_end_tag;
  meta.preserved_tokens = chat_params.preserved_tokens;
  meta.supports_thinking = chat_params.supports_thinking;

  ChatTemplateResult result;
  result.prompt = chat_params.prompt;
  result.stop_strings = chat_params.additional_stops;
  result.parser_params = common_chat_parser_params(chat_params);
  result.parser_params.reasoning_format = inputs.reasoning_format;
  result.parser_params.reasoning_in_content = false;
  result.parser_params.parse_tool_calls = !inputs.tools.empty() && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE;
  if (!chat_params.parser.empty()) {
    result.parser_params.parser.load(chat_params.parser);
  }
  if (body.contains("stop")) {
    if (body["stop"].is_string()) {
      result.stop_strings.push_back(body["stop"].get<std::string>());
    } else if (body["stop"].is_array()) {
      for (const auto& stop : body["stop"]) {
        if (stop.is_string()) result.stop_strings.push_back(stop.get<std::string>());
      }
    }
  }
  // Sampler params: explicit per-request (OpenAI body) values win; otherwise
  // fall back to the server-side SamplingConfig defaults (issue #42), which
  // mirror stock llama-server (DRY off, repeat_penalty neutral).
  const auto& sc = cfg_.sampling;
  result.sampling_params.temp          = req.temperature.value_or(sc.temperature);
  result.sampling_params.top_p         = req.top_p.value_or(sc.top_p);
  result.sampling_params.top_k         = req.top_k.value_or(sc.top_k);
  result.sampling_params.min_p         = sc.min_p;
  result.sampling_params.penalty_repeat = req.repeat_penalty.value_or(sc.repeat_penalty);
  result.sampling_params.penalty_last_n = req.repeat_last_n.value_or(sc.repeat_last_n);
  result.sampling_params.dry_multiplier     = sc.dry_multiplier;
  result.sampling_params.dry_base           = sc.dry_base;
  result.sampling_params.dry_allowed_length = sc.dry_allowed_length;
  result.sampling_params.dry_penalty_last_n = sc.dry_penalty_last_n;
  result.sampling_params.dry_sequence_breakers = sc.dry_seq_breakers;
  result.sampling_params.seed = req.seed >= 0 ? static_cast<std::uint32_t>(req.seed) : LLAMA_DEFAULT_SEED;

  // DEBUG (issue #42 diagnosis): log what the client sent vs what was resolved,
  // so we can see whether OpenCode/Claude Code override the server-side config.
  auto opt_f = [](const std::optional<float>& v) {
    return v.has_value() ? std::to_string(*v) : std::string("unset");
  };
  auto opt_i = [](const std::optional<int>& v) {
    return v.has_value() ? std::to_string(*v) : std::string("unset");
  };
  LOG_INFO("sampling_resolved",
           "model={} client[temp={} top_p={} top_k={} repeat_penalty={} repeat_last_n={}] "
           "resolved[temp={:.3f} top_p={:.3f} top_k={} min_p={:.3f} repeat_penalty={:.3f} "
           "repeat_last_n={} dry_mult={:.3f}]",
           info_.name, opt_f(req.temperature), opt_f(req.top_p), opt_i(req.top_k),
           opt_f(req.repeat_penalty), opt_i(req.repeat_last_n),
           result.sampling_params.temp, result.sampling_params.top_p,
           result.sampling_params.top_k, result.sampling_params.min_p,
           result.sampling_params.penalty_repeat, result.sampling_params.penalty_last_n,
           result.sampling_params.dry_multiplier);

  if (!chat_params.grammar.empty()) {
    if (!inputs.tools.empty() && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
      result.sampling_params.grammar = {COMMON_GRAMMAR_TYPE_TOOL_CALLS, chat_params.grammar};
    } else if (!inputs.json_schema.empty()) {
      result.sampling_params.grammar = {COMMON_GRAMMAR_TYPE_OUTPUT_FORMAT, chat_params.grammar};
    } else {
      result.sampling_params.grammar = {COMMON_GRAMMAR_TYPE_USER, chat_params.grammar};
    }
  }
  result.sampling_params.grammar_lazy = chat_params.grammar_lazy;
  result.sampling_params.grammar_triggers = chat_params.grammar_triggers;
  result.sampling_params.generation_prompt = chat_params.generation_prompt;
  for (const auto& token_str : chat_params.preserved_tokens) {
    auto toks = tokenize_stop_strings(vocab_, {token_str});
    for (auto t : toks) result.sampling_params.preserved_tokens.insert(t);
  }
  result.meta = std::move(meta);

  return Result<ChatTemplateResult>(std::move(result));
  } catch (const std::exception& e) {
    LOG_WARN("chat_template_failed", "model={} error={}", info_.name, e.what());
    return Result<ChatTemplateResult>(std::unexpect,
        make_error(ErrorCode::ParseError, std::string("chat template failed: ") + e.what()));
  }
}

// Tokenizes the request, checks context limits, snapshots per-slot KV state,
// and initialises a sampler. All of this runs on the HTTP handler thread
// before the task is handed off to the scheduler.
Result<LlamaCppModel::PredictSetup> LlamaCppModel::prepare_inference(
    int slot_id, const InferenceRequest& req) {
  PredictSetup s;
  // Compute the prompt-token budget so apply_chat_template can drop whole
  // oldest messages (history-aware truncation, issue #38) before tokenizing.
  // Mirrors the reserve/target maths in maybe_truncate_prompt, which remains as
  // a hard safety net for the pathological single-oversized-message case.
  const int n_ctx_seq = static_cast<int>(llama_n_ctx_seq(shared_ctx_));
  int budget = 0;
  if (cfg_.truncate_prompt && n_ctx_seq > 0) {
    // See maybe_truncate_prompt: clamp bounds must satisfy lo <= hi (UB
    // otherwise) when n_ctx_seq < 1024.
    const int reserve_hi = n_ctx_seq / 4;
    const int reserve = std::clamp(req.max_tokens > 0 ? req.max_tokens : 1024,
                                   std::min(256, reserve_hi), reserve_hi);
    budget = n_ctx_seq - reserve - 1;
  }
  auto tmpl_res = apply_chat_template(req, budget);
  if (!tmpl_res.has_value())
    return Result<PredictSetup>(std::unexpect, tmpl_res.error());

  s.parser_params   = std::move(tmpl_res->parser_params);
  s.sampling_params = std::move(tmpl_res->sampling_params);
  s.stop_strings    = std::move(tmpl_res->stop_strings);
  s.stop_tokens     = tokenize_stop_strings(vocab_, s.stop_strings);

  const std::string& prompt = tmpl_res->prompt;
  s.prompt_tokens.resize(prompt.size() + 16);
  const bool add_bos = llama_vocab_get_add_bos(vocab_);
  int n_tokens = llama_tokenize(vocab_, prompt.data(), static_cast<int>(prompt.size()),
                                s.prompt_tokens.data(), static_cast<int>(s.prompt_tokens.size()),
                                add_bos, true);
  if (n_tokens < 0) {
    s.prompt_tokens.resize(static_cast<std::size_t>(-n_tokens));
    n_tokens = llama_tokenize(vocab_, prompt.data(), static_cast<int>(prompt.size()),
                              s.prompt_tokens.data(), static_cast<int>(s.prompt_tokens.size()),
                              add_bos, true);
    if (n_tokens < 0)
      return Result<PredictSetup>(std::unexpect, make_error(ErrorCode::Internal, "tokenization failed"));
  }
  s.prompt_tokens.resize(static_cast<std::size_t>(n_tokens));

  // Per-slot context window = n_ctx_seq (total context / n_slots as set during load)
  s.n_ctx_seq = static_cast<int>(llama_n_ctx_seq(shared_ctx_));

  if (n_tokens >= s.n_ctx_seq) {
    if (!cfg_.truncate_prompt)
      return Result<PredictSetup>(std::unexpect,
          make_error(ErrorCode::InvalidArgument,
                     "This model's maximum context length is " + std::to_string(s.n_ctx_seq) +
                     " tokens. However, your messages resulted in " + std::to_string(n_tokens) +
                     " tokens. Please reduce the length of the messages."));
    maybe_truncate_prompt(s.prompt_tokens, s.n_ctx_seq, req.max_tokens, info_.name);
    n_tokens = static_cast<int>(s.prompt_tokens.size());
  }

  const int ctx_budget = std::max(1, s.n_ctx_seq - n_tokens - 1);
  s.max_tokens = req.max_tokens > 0 ? std::min(req.max_tokens, ctx_budget) : ctx_budget;

  // Snapshot per-slot KV state under the mutex (scheduler may touch these after submit)
  {
    std::lock_guard lk(mtx_);
    s.last_prompt_tokens   = slots_[slot_id].last_prompt_tokens;
    s.recurrent_checkpoint = slots_[slot_id].recurrent_checkpoint;
    s.checkpoint_pos       = slots_[slot_id].checkpoint_pos;
  }

  common_sampler* smp = common_sampler_init(model_, s.sampling_params);
  if (smp == nullptr)
    return Result<PredictSetup>(std::unexpect,
        make_error(ErrorCode::Internal, "common_sampler_init returned null"));
  s.smp = smp;

  return Result<PredictSetup>(std::move(s));
}

// Drain task.out_queue until the done event, calling on_token for each token.
// Returns error if the scheduler reported one.
Result<void> LlamaCppModel::drain_task(SlotTask& task, const OnToken& on_token) {
  while (true) {
    TokenEvent ev;
    {
      std::unique_lock lk(task.out_mtx);
      task.out_cv.wait(lk, [&task] { return !task.out_queue.empty(); });
      ev = std::move(task.out_queue.front());
      task.out_queue.pop();
    }
    if (ev.is_error)
      return Result<void>(std::unexpect, make_error(ErrorCode::Internal, ev.error_msg));
    if (ev.is_done)
      return Result<void>{};
    if (!on_token(ev.id)) {
      // Caller wants to stop early (stop string hit, client disconnect, etc.)
      task.caller_cancel.store(true);
      // Continue draining until the scheduler acknowledges with a done event
    }
  }
}

Result<InferenceResult> LlamaCppModel::predict(int slot_id, const InferenceRequest& req) {
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument, "slot_id out of range"));
  }
  {
    std::lock_guard lk(mtx_);
    if (!loaded_.load())
      return Result<InferenceResult>(std::unexpect, make_error(ErrorCode::Internal, "model not loaded"));
    if (!slots_[slot_id].busy)
      return Result<InferenceResult>(std::unexpect, make_error(ErrorCode::InvalidArgument, "slot not acquired"));
  }

  auto setup_res = prepare_inference(slot_id, req);
  if (!setup_res.has_value())
    return Result<InferenceResult>(std::unexpect, setup_res.error());
  auto& setup = *setup_res;

  const auto start = std::chrono::steady_clock::now();
  std::string generated;
  generated.reserve(4096);
  std::vector<llama_token> decoded_ids;
  bool string_stopped = false;

  SlotTask task;
  task.slot_id             = slot_id;
  task.prompt_tokens       = setup.prompt_tokens;
  task.last_prompt_tokens  = setup.last_prompt_tokens;
  task.sampler             = setup.smp;   // scheduler takes ownership
  task.max_tokens          = setup.max_tokens;
  task.stop_tokens         = setup.stop_tokens;

  scheduler_->submit(&task);

  auto drain_res = drain_task(task, [&](llama_token id) -> bool {
    if (string_stopped) return false; // already stopping
    char buf[256];
    const int n = llama_token_to_piece(vocab_, id, buf, sizeof(buf), 0, true);
    if (n > 0) generated.append(buf, static_cast<std::size_t>(n));
    for (const auto& stop : setup.stop_strings) {
      if (!stop.empty() && generated.size() >= stop.size() &&
          generated.compare(generated.size() - stop.size(), stop.size(), stop) == 0) {
        generated.resize(generated.size() - stop.size());
        string_stopped = true;
        return false; // do NOT push this token — it's part of the stop string
      }
    }
    decoded_ids.push_back(id);
    return true;
  });
  if (!drain_res.has_value())
    return Result<InferenceResult>(std::unexpect, drain_res.error());

  const auto end = std::chrono::steady_clock::now();

  // Update per-slot KV state
  {
    std::lock_guard lk(mtx_);
    auto& slot = slots_[slot_id];
    slot.last_prompt_tokens.assign(setup.prompt_tokens.begin(), setup.prompt_tokens.end());
    slot.last_prompt_tokens.insert(slot.last_prompt_tokens.end(),
                                   decoded_ids.begin(), decoded_ids.end());
    slot.recurrent_checkpoint = std::move(task.out_recurrent_checkpoint);
    slot.checkpoint_pos       = task.out_checkpoint_pos;
  }

  log_slot_release(info_.name,
                   static_cast<int>(setup.prompt_tokens.size()),
                   false,
                   static_cast<int>(decoded_ids.size()),
                   setup.n_ctx_seq);

  InferenceResult out;
  out.prompt_tokens         = static_cast<int>(setup.prompt_tokens.size());
  out.cached_prompt_tokens  = task.out_cached_prompt_tokens;
  out.completion_tokens     = static_cast<int>(decoded_ids.size());
  out.duration_ms = std::chrono::duration<float, std::milli>(end - start).count();
  if (out.completion_tokens > 0 && out.duration_ms > 0.0f)
    out.tokens_per_second = (out.completion_tokens * 1000.0f) / out.duration_ms;
  if (out.completion_tokens >= setup.max_tokens)
    out.finish_reason = "length";

  try {
    auto msg = parse_final_message_with_ids(generated, setup.parser_params);
    apply_parsed_message(out, msg);
    if (setup.parser_params.parse_tool_calls && out.tool_calls.empty())
      apply_fallback_tool_calls(out, generated);
  } catch (const std::exception& e) {
    LOG_ERROR("chat_parse_failed", "model={} error={}", info_.name, e.what());
    out.text = std::move(generated);
    if (setup.parser_params.parse_tool_calls)
      apply_fallback_tool_calls(out, out.text);
  }
  return Result<InferenceResult>(std::move(out));
}

Result<InferenceResult> LlamaCppModel::predict_stream(
    int slot_id, const InferenceRequest& req, const TokenCallback& callback,
    const std::atomic<bool>* cancel) {
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument, "slot_id out of range"));
  }
  {
    std::lock_guard lk(mtx_);
    if (!loaded_.load())
      return Result<InferenceResult>(std::unexpect, make_error(ErrorCode::Internal, "model not loaded"));
    if (!slots_[slot_id].busy)
      return Result<InferenceResult>(std::unexpect, make_error(ErrorCode::InvalidArgument, "slot not acquired"));
  }

  auto setup_res = prepare_inference(slot_id, req);
  if (!setup_res.has_value())
    return Result<InferenceResult>(std::unexpect, setup_res.error());
  auto& setup = *setup_res;

  const auto start = std::chrono::steady_clock::now();
  std::string generated;
  generated.reserve(4096);
  std::vector<llama_token> decoded_ids;
  StreamingChatParserState parser_state(setup.parser_params);
  bool callback_aborted = false;
  bool string_stopped = false;

  SlotTask task;
  task.slot_id            = slot_id;
  task.prompt_tokens      = setup.prompt_tokens;
  task.last_prompt_tokens = setup.last_prompt_tokens;
  task.sampler            = setup.smp;
  task.max_tokens         = setup.max_tokens;
  task.stop_tokens        = setup.stop_tokens;
  task.ext_cancel         = cancel;

  scheduler_->submit(&task);

  auto drain_res = drain_task(task, [&](llama_token id) -> bool {
    if (callback_aborted || string_stopped) return false;

    char buf[256];
    const int n = llama_token_to_piece(vocab_, id, buf, sizeof(buf), 0, true);
    if (n > 0) {
      std::string piece(buf, static_cast<std::size_t>(n));
      generated.append(piece);

      for (const auto& stop : setup.stop_strings) {
        if (!stop.empty() && generated.size() >= stop.size() &&
            generated.compare(generated.size() - stop.size(), stop.size(), stop) == 0) {
          generated.resize(generated.size() - stop.size());
          string_stopped = true;
          return false; // do NOT push this token — it's part of the stop string
        }
      }

      decoded_ids.push_back(id);
      std::vector<common_chat_msg_diff> diffs;
      try {
        diffs = parser_state.update(piece, /*is_partial=*/true,
                                    setup.parser_params.parse_tool_calls);
      } catch (...) {}

      for (const auto& diff : diffs) {
        auto delta = to_delta(diff);
        if (delta.content.empty() && delta.reasoning_text.empty() && delta.tool_calls.empty())
          continue;
        if (!callback(delta)) {
          callback_aborted = true;
          return false;
        }
      }
    }
    return true;
  });
  if (!drain_res.has_value())
    return Result<InferenceResult>(std::unexpect, drain_res.error());

  const auto end = std::chrono::steady_clock::now();

  // Update per-slot KV state
  {
    std::lock_guard lk(mtx_);
    auto& slot = slots_[slot_id];
    slot.last_prompt_tokens.assign(setup.prompt_tokens.begin(), setup.prompt_tokens.end());
    slot.last_prompt_tokens.insert(slot.last_prompt_tokens.end(),
                                   decoded_ids.begin(), decoded_ids.end());
    slot.recurrent_checkpoint = std::move(task.out_recurrent_checkpoint);
    slot.checkpoint_pos       = task.out_checkpoint_pos;
  }

  log_slot_release(info_.name,
                   static_cast<int>(setup.prompt_tokens.size()),
                   false,
                   static_cast<int>(decoded_ids.size()),
                   setup.n_ctx_seq);

  InferenceResult out;
  out.prompt_tokens        = static_cast<int>(setup.prompt_tokens.size());
  out.cached_prompt_tokens = task.out_cached_prompt_tokens;
  out.completion_tokens    = static_cast<int>(decoded_ids.size());
  out.duration_ms = std::chrono::duration<float, std::milli>(end - start).count();
  if (out.completion_tokens > 0 && out.duration_ms > 0.0f)
    out.tokens_per_second = (out.completion_tokens * 1000.0f) / out.duration_ms;
  if (out.completion_tokens >= setup.max_tokens)
    out.finish_reason = "length";

  if (callback_aborted) return Result<InferenceResult>(std::move(out));

  bool fallback_tool_calls_used = false;
  try {
    auto final_diffs = parser_state.update("", /*is_partial=*/false,
                                            setup.parser_params.parse_tool_calls);
    for (const auto& diff : final_diffs) {
      auto delta = to_delta(diff);
      if (delta.content.empty() && delta.reasoning_text.empty() && delta.tool_calls.empty())
        continue;
      if (!callback(delta)) return Result<InferenceResult>(std::move(out));
    }
    auto msg = parse_final_message_with_ids(generated, setup.parser_params);
    apply_parsed_message(out, msg);
    if (setup.parser_params.parse_tool_calls && out.tool_calls.empty()) {
      apply_fallback_tool_calls(out, generated);
      fallback_tool_calls_used = !out.tool_calls.empty();
    }
  } catch (const std::exception& e) {
    LOG_ERROR("chat_parse_failed", "model={} error={}", info_.name, e.what());
    out.text = std::move(generated);
    if (setup.parser_params.parse_tool_calls) {
      apply_fallback_tool_calls(out, out.text);
      fallback_tool_calls_used = !out.tool_calls.empty();
    }
  }

  if (fallback_tool_calls_used) {
    for (std::size_t i = 0; i < out.tool_calls.size(); ++i) {
      const auto& tc = out.tool_calls[i];
      InferenceDelta delta;
      ToolCallDelta tcd;
      tcd.index = i; tcd.id = tc.id; tcd.type = "function";
      tcd.function_name = tc.function_name;
      tcd.function_arguments = tc.function_arguments;
      delta.tool_calls.push_back(std::move(tcd));
      if (!callback(delta)) break;
    }
  }
  return Result<InferenceResult>(std::move(out));
}

}
