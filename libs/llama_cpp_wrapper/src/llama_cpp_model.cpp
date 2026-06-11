#include "llama_cpp_wrapper/llama_cpp_model.hpp"

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

static int prepare_prompt_cache(
    llama_context* ctx,
    const std::vector<int>& previous_prompt_tokens,
    const std::vector<llama_token>& prompt_tokens,
    int seq_id,
    const std::string& model_name) {
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
  if (loaded_.load()) {
    (void)unload();
  }
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
  auto ctx_res = init_contexts_locked();
  if (!ctx_res.has_value()) {
    llama_model_free(model_);
    model_ = nullptr;
    return ctx_res;
  }
  loaded_.store(true);
  log_memory_snapshot("llama_contexts_initialized_memory_after", info_.name);
  LOG_INFO("chat_template_loaded", "model={} kind=jinja", info_.name);
  return Result<void>{};
}

Result<void> LlamaCppModel::init_contexts_locked() {
  slots_.clear();
  slots_.resize(info_.n_slots);
  for (int i = 0; i < info_.n_slots; ++i) {
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = static_cast<std::uint32_t>(std::max(512, info_.context_size));
    cparams.n_threads = cfg_.n_threads;
    cparams.n_batch = static_cast<std::uint32_t>(std::max(1, cfg_.n_batch));
    cparams.n_ubatch = static_cast<std::uint32_t>(std::max(1, cfg_.n_ubatch));
    cparams.n_seq_max = 1;
    cparams.flash_attn_type = flash_attn_from_string(cfg_.flash_attn);
    cparams.offload_kqv = cfg_.kv_offload;
    cparams.op_offload = cfg_.op_offload;
    cparams.swa_full = cfg_.swa_full;
    cparams.type_k = cache_type_from_string(cfg_.cache_type_k);
    cparams.type_v = cache_type_from_string(cfg_.cache_type_v);
    LOG_INFO("llama_context_config",
             "model={} slot={} n_ctx={} n_batch={} n_ubatch={} n_seq_max={} flash_attn={} kv_offload={} op_offload={} cache_type_k={} cache_type_v={} swa_full={}",
             info_.name,
             i,
             cparams.n_ctx,
             cparams.n_batch,
             cparams.n_ubatch,
             cparams.n_seq_max,
             cfg_.flash_attn,
             cfg_.kv_offload,
             cfg_.op_offload,
             cfg_.cache_type_k,
             cfg_.cache_type_v,
             cfg_.swa_full);
    auto* ctx = llama_init_from_model(model_, cparams);
    if (ctx == nullptr) {
      return Result<void>(std::unexpect,
          make_error(ErrorCode::OutOfMemory,
                     "llama_init_from_model returned null for slot " + std::to_string(i)));
    }
    slots_[i].ctx = std::unique_ptr<llama_context, void(*)(llama_context*)>(
        ctx, [](llama_context* c) { if (c) llama_free(c); });
    slots_[i].busy = false;
  }
  return Result<void>{};
}

Result<void> LlamaCppModel::unload() {
  std::lock_guard lk(mtx_);
  if (!loaded_.load()) return Result<void>{};
  log_memory_snapshot("llama_model_unload_memory_before", info_.name);
  for (auto& s : slots_) {
    s.ctx.reset();
    s.busy = false;
  }
  slots_.clear();
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
  for (auto& s : slots_) {
    if (s.ctx) {
      llama_memory_t mem = llama_get_memory(s.ctx.get());
      llama_memory_clear(mem, false);
    }
    s.busy = false;
    s.last_prompt_tokens.clear();
  }
  return Result<void>{};
}

Result<ChatTemplateResult> LlamaCppModel::apply_chat_template(
    const InferenceRequest& req) {
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
  result.sampling_params.temp = req.temperature;
  result.sampling_params.top_p = req.top_p;
  result.sampling_params.top_k = req.top_k;
  result.sampling_params.penalty_repeat = req.repeat_penalty;
  result.sampling_params.penalty_last_n = req.repeat_last_n;
  result.sampling_params.seed = req.seed >= 0 ? static_cast<std::uint32_t>(req.seed) : LLAMA_DEFAULT_SEED;
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

Result<void> LlamaCppModel::build_sampler_locked(
    llama_sampler** out, const InferenceRequest& req) {
  const std::uint32_t seed = req.seed >= 0 ? static_cast<std::uint32_t>(req.seed)
                                           : static_cast<std::uint32_t>(0xFFFFFFFFu);
  llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
  llama_sampler* chain = llama_sampler_chain_init(sparams);
  if (chain == nullptr) {
    return Result<void>(std::unexpect, make_error(ErrorCode::Internal, "llama_sampler_chain_init returned null"));
  }
  auto append = [&](llama_sampler* s) {
    if (s) llama_sampler_chain_add(chain, s);
  };

  const char* dry_seq_breakers[] = {"\n", ":", "\"", "*"};
  append(llama_sampler_init_dry(
      vocab_, llama_model_n_ctx_train(model_),
      0.8f,    // dry_multiplier
      1.75f,   // dry_base
      2,       // dry_allowed_length
      1024,    // dry_penalty_last_n
      dry_seq_breakers, 4));

  append(llama_sampler_init_penalties(
      req.repeat_last_n, req.repeat_penalty, 0.0f, 0.0f));

  append(llama_sampler_init_top_k(static_cast<int32_t>(req.top_k)));
  append(llama_sampler_init_top_p(req.top_p, 1));
  append(llama_sampler_init_min_p(0.05f, 1));
  append(llama_sampler_init_temp(req.temperature));
  append(llama_sampler_init_dist(seed));
  *out = chain;
  return Result<void>{};
}

Result<InferenceResult> LlamaCppModel::predict(int slot_id, const InferenceRequest& req) {
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument, "slot_id out of range"));
  }
  std::lock_guard lk(mtx_);
  if (!loaded_.load()) {
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::Internal, "model not loaded"));
  }
  auto& slot = slots_[slot_id];
  if (!slot.busy || !slot.ctx) {
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument, "slot not acquired or no context"));
  }

  auto tmpl_res = apply_chat_template(req);
  if (!tmpl_res.has_value()) {
    return Result<InferenceResult>(std::unexpect, tmpl_res.error());
  }
  std::string prompt = std::move(tmpl_res->prompt);
  auto parser_params = std::move(tmpl_res->parser_params);
  auto sampling_params = std::move(tmpl_res->sampling_params);
  auto stop_strings = std::move(tmpl_res->stop_strings);
  auto stop_tokens = tokenize_stop_strings(vocab_, stop_strings);
  chat_template_meta_ = std::move(tmpl_res->meta);

  std::vector<llama_token> prompt_tokens(prompt.size() + 16);
  const bool add_bos = llama_vocab_get_add_bos(vocab_);
  int n_tokens = llama_tokenize(vocab_,
                                prompt.data(),
                                static_cast<int>(prompt.size()),
                                prompt_tokens.data(),
                                static_cast<int>(prompt_tokens.size()),
                                add_bos, true);
  if (n_tokens < 0) {
    prompt_tokens.resize(static_cast<std::size_t>(-n_tokens));
    n_tokens = llama_tokenize(vocab_,
                              prompt.data(),
                              static_cast<int>(prompt.size()),
                              prompt_tokens.data(),
                              static_cast<int>(prompt_tokens.size()),
                              add_bos, true);
    if (n_tokens < 0) {
      return Result<InferenceResult>(std::unexpect,
          make_error(ErrorCode::Internal, "tokenization failed"));
    }
  }
  prompt_tokens.resize(static_cast<std::size_t>(n_tokens));
  const int seq_id = 0;
  const auto n_ctx = llama_n_ctx_seq(slot.ctx.get());
  if (n_tokens >= static_cast<int>(n_ctx)) {
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument,
                   "This model's maximum context length is " + std::to_string(n_ctx) +
                   " tokens. However, your messages resulted in " + std::to_string(n_tokens) +
                   " tokens. Please reduce the length of the messages."));
  }
  const int cached_prompt_tokens = prepare_prompt_cache(
      slot.ctx.get(), slot.last_prompt_tokens, prompt_tokens, seq_id, info_.name);

  if (!process_prompt_chunks(slot.ctx.get(), prompt_tokens, cached_prompt_tokens, seq_id, static_cast<int>(cfg_.n_batch), info_.name)) {
    slot.last_prompt_tokens.clear();
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::Internal, "llama_decode (prompt) failed"));
  }

  common_sampler* smp = common_sampler_init(model_, sampling_params);
  if (smp == nullptr) {
    slot.last_prompt_tokens.clear();
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::Internal, "common_sampler_init returned null"));
  }
  InferenceResult out;
  out.prompt_tokens = n_tokens;
  out.cached_prompt_tokens = cached_prompt_tokens;
  const int ctx_budget = std::max(1, static_cast<int>(n_ctx) - n_tokens - 1);
  const int max_tokens = req.max_tokens > 0 ? std::min(req.max_tokens, ctx_budget) : ctx_budget;
  const auto start = std::chrono::steady_clock::now();
  std::string generated;
  generated.reserve(4096);
  std::vector<llama_token> decoded_ids;
  int n_cur = n_tokens;
  int n_decoded = 0;
  bool stopped = false;
  bool truncated = false;
  for (int i = 0; i < max_tokens; ++i) {
    const llama_token id = common_sampler_sample(smp, slot.ctx.get(), -1);
    bool is_stop = llama_vocab_is_eog(vocab_, id);
    if (!is_stop) {
      for (auto t : stop_tokens) {
        if (t == id) { is_stop = true; break; }
      }
    }
    if (is_stop) {
      stopped = true;
      break;
    }

    char buf[256];
    const int n = llama_token_to_piece(vocab_, id, buf, sizeof(buf), 0, true);
    if (n > 0) generated.append(buf, static_cast<std::size_t>(n));
    bool string_stop = false;
    for (const auto& stop : stop_strings) {
      if (!stop.empty() && generated.size() >= stop.size() &&
          generated.compare(generated.size() - stop.size(), stop.size(), stop) == 0) {
        generated.resize(generated.size() - stop.size());
        string_stop = true;
        break;
      }
    }
    out.completion_tokens += 1;
    n_decoded += 1;
    common_sampler_accept(smp, id, true);
    if (string_stop) {
      stopped = true;
      break;
    }
    if (n_cur + 1 >= static_cast<int>(n_ctx)) {
      truncated = true;
      stopped = true;
      out.finish_reason = "length";
      LOG_INFO("llama_context_limit",
               "model={} n_tokens={} truncated={} n_decoded={} n_ctx={}",
               info_.name,
               n_cur,
               truncated,
               n_decoded,
               n_ctx);
      break;
    }
    llama_batch one = llama_batch_init(1, 0, 1);
    one.token[0] = id;
    one.pos[0] = n_cur;
    one.n_seq_id[0] = 1;
    one.seq_id[0][0] = seq_id;
    one.logits[0] = 1;
    one.n_tokens = 1;
    const int rc = llama_decode(slot.ctx.get(), one);
    if (rc != 0) {
      log_token_decode_failed(info_.name, rc, i, id, one.pos[0], n_cur, static_cast<int>(n_ctx), n_decoded);
      llama_batch_free(one);
      common_sampler_free(smp);
      slot.last_prompt_tokens.clear();
      return Result<InferenceResult>(std::unexpect,
          make_error(ErrorCode::Internal, "llama_decode (token) failed"));
    }
    n_cur += 1;
    decoded_ids.push_back(id);
    llama_batch_free(one);
  }
  log_slot_release(info_.name, n_cur, truncated, n_decoded, static_cast<int>(n_ctx));
  const auto end = std::chrono::steady_clock::now();
  out.duration_ms = std::chrono::duration<float, std::milli>(end - start).count();
  if (out.completion_tokens > 0 && out.duration_ms > 0.0f) {
    out.tokens_per_second = (out.completion_tokens * 1000.0f) / out.duration_ms;
  }
  if (!stopped && out.completion_tokens >= max_tokens) {
    out.finish_reason = "length";
  }
  try {
    auto msg = parse_final_message_with_ids(generated, parser_params);
    apply_parsed_message(out, msg);
    if (parser_params.parse_tool_calls && out.tool_calls.empty()) {
      apply_fallback_tool_calls(out, generated);
    }
  } catch (const std::exception& e) {
    LOG_ERROR("chat_parse_failed", "model={} error={}", info_.name, e.what());
    out.text = std::move(generated);
    if (parser_params.parse_tool_calls) {
      apply_fallback_tool_calls(out, out.text);
    }
  }
  slot.last_prompt_tokens.assign(prompt_tokens.begin(), prompt_tokens.end());
  slot.last_prompt_tokens.insert(slot.last_prompt_tokens.end(),
                                 decoded_ids.begin(), decoded_ids.end());

  common_sampler_free(smp);
  return Result<InferenceResult>(std::move(out));
}

Result<InferenceResult> LlamaCppModel::predict_stream(
    int slot_id, const InferenceRequest& req, const TokenCallback& callback) {
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument, "slot_id out of range"));
  }
  std::lock_guard lk(mtx_);
  if (!loaded_.load()) {
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::Internal, "model not loaded"));
  }
  auto& slot = slots_[slot_id];
  if (!slot.busy || !slot.ctx) {
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument, "slot not acquired or no context"));
  }

  auto tmpl_res = apply_chat_template(req);
  if (!tmpl_res.has_value()) {
    return Result<InferenceResult>(std::unexpect, tmpl_res.error());
  }
  std::string prompt = std::move(tmpl_res->prompt);
  auto parser_params = std::move(tmpl_res->parser_params);
  auto sampling_params = std::move(tmpl_res->sampling_params);
  auto stop_strings = std::move(tmpl_res->stop_strings);
  auto stop_tokens = tokenize_stop_strings(vocab_, stop_strings);
  chat_template_meta_ = std::move(tmpl_res->meta);

  std::vector<llama_token> prompt_tokens(prompt.size() + 16);
  const bool add_bos = llama_vocab_get_add_bos(vocab_);
  int n_tokens = llama_tokenize(vocab_,
                                prompt.data(),
                                static_cast<int>(prompt.size()),
                                prompt_tokens.data(),
                                static_cast<int>(prompt_tokens.size()),
                                add_bos, true);
  if (n_tokens < 0) {
    prompt_tokens.resize(static_cast<std::size_t>(-n_tokens));
    n_tokens = llama_tokenize(vocab_,
                              prompt.data(),
                              static_cast<int>(prompt.size()),
                              prompt_tokens.data(),
                              static_cast<int>(prompt_tokens.size()),
                              add_bos, true);
    if (n_tokens < 0) {
      return Result<InferenceResult>(std::unexpect,
          make_error(ErrorCode::Internal, "tokenization failed"));
    }
  }
  prompt_tokens.resize(static_cast<std::size_t>(n_tokens));
  const int seq_id = 0;
  const auto n_ctx = llama_n_ctx_seq(slot.ctx.get());
  if (n_tokens >= static_cast<int>(n_ctx)) {
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument,
                   "This model's maximum context length is " + std::to_string(n_ctx) +
                   " tokens. However, your messages resulted in " + std::to_string(n_tokens) +
                   " tokens. Please reduce the length of the messages."));
  }
  const int cached_prompt_tokens = prepare_prompt_cache(
      slot.ctx.get(), slot.last_prompt_tokens, prompt_tokens, seq_id, info_.name);

  if (!process_prompt_chunks(slot.ctx.get(), prompt_tokens, cached_prompt_tokens, seq_id, static_cast<int>(cfg_.n_batch), info_.name)) {
    slot.last_prompt_tokens.clear();
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::Internal, "llama_decode (prompt) failed"));
  }

  common_sampler* smp = common_sampler_init(model_, sampling_params);
  if (smp == nullptr) {
    slot.last_prompt_tokens.clear();
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::Internal, "common_sampler_init returned null"));
  }
  InferenceResult out;
  out.prompt_tokens = n_tokens;
  out.cached_prompt_tokens = cached_prompt_tokens;
  const int ctx_budget = std::max(1, static_cast<int>(n_ctx) - n_tokens - 1);
  const int max_tokens = req.max_tokens > 0 ? std::min(req.max_tokens, ctx_budget) : ctx_budget;
  const auto start = std::chrono::steady_clock::now();
  std::string generated;
  generated.reserve(4096);
  std::vector<llama_token> decoded_ids;
  StreamingChatParserState parser_state(parser_params);
  int n_cur = n_tokens;
  int n_decoded = 0;
  bool stopped = false;
  bool truncated = false;
  for (int i = 0; i < max_tokens; ++i) {
    const llama_token id = common_sampler_sample(smp, slot.ctx.get(), -1);
    bool is_stop = llama_vocab_is_eog(vocab_, id);
    if (!is_stop) {
      for (auto t : stop_tokens) {
        if (t == id) { is_stop = true; break; }
      }
    }
    if (is_stop) {
      stopped = true;
      break;
    }

    char buf[256];
    const int n = llama_token_to_piece(vocab_, id, buf, sizeof(buf), 0, true);
    bool string_stop = false;
    if (n > 0) {
      std::string token(buf, static_cast<std::size_t>(n));
      generated.append(token);
      for (const auto& stop : stop_strings) {
        if (!stop.empty() && generated.size() >= stop.size() &&
            generated.compare(generated.size() - stop.size(), stop.size(), stop) == 0) {
          generated.resize(generated.size() - stop.size());
          string_stop = true;
          break;
        }
      }
      std::vector<common_chat_msg_diff> diffs;
      try {
        diffs = parser_state.update(token, true, parser_params.parse_tool_calls);
      } catch (...) {
      }
      for (const auto& diff : diffs) {
        auto delta = to_delta(diff);
        if (delta.content.empty() && delta.reasoning_text.empty() && delta.tool_calls.empty()) continue;
        if (!callback(delta)) {
          common_sampler_free(smp);
          slot.last_prompt_tokens.clear();
          return Result<InferenceResult>(std::move(out));
        }
      }
    }
    common_sampler_accept(smp, id, true);
    out.completion_tokens += 1;
    n_decoded += 1;
    if (string_stop) {
      stopped = true;
      break;
    }
    if (n_cur + 1 >= static_cast<int>(n_ctx)) {
      truncated = true;
      stopped = true;
      out.finish_reason = "length";
      LOG_INFO("llama_context_limit",
               "model={} n_tokens={} truncated={} n_decoded={} n_ctx={}",
               info_.name,
               n_cur,
               truncated,
               n_decoded,
               n_ctx);
      break;
    }
    llama_batch one = llama_batch_init(1, 0, 1);
    one.token[0] = id;
    one.pos[0] = n_cur;
    one.n_seq_id[0] = 1;
    one.seq_id[0][0] = seq_id;
    one.logits[0] = 1;
    one.n_tokens = 1;
    const int rc = llama_decode(slot.ctx.get(), one);
    if (rc != 0) {
      log_token_decode_failed(info_.name, rc, i, id, one.pos[0], n_cur, static_cast<int>(n_ctx), n_decoded);
      llama_batch_free(one);
      common_sampler_free(smp);
      slot.last_prompt_tokens.clear();
      return Result<InferenceResult>(std::unexpect,
          make_error(ErrorCode::Internal, "llama_decode (token) failed"));
    }
    n_cur += 1;
    decoded_ids.push_back(id);
    llama_batch_free(one);
  }
  log_slot_release(info_.name, n_cur, truncated, n_decoded, static_cast<int>(n_ctx));
  const auto end = std::chrono::steady_clock::now();
  out.duration_ms = std::chrono::duration<float, std::milli>(end - start).count();
  if (out.completion_tokens > 0 && out.duration_ms > 0.0f) {
    out.tokens_per_second = (out.completion_tokens * 1000.0f) / out.duration_ms;
  }
  if (!stopped && out.completion_tokens >= max_tokens) {
    out.finish_reason = "length";
  }
  bool fallback_tool_calls_used = false;
  try {
    std::vector<common_chat_msg_diff> final_diffs;
    final_diffs = parser_state.update("", false, parser_params.parse_tool_calls);
    for (const auto& diff : final_diffs) {
      auto delta = to_delta(diff);
      if (delta.content.empty() && delta.reasoning_text.empty() && delta.tool_calls.empty()) continue;
      if (!callback(delta)) {
        common_sampler_free(smp);
        slot.last_prompt_tokens.clear();
        return Result<InferenceResult>(std::move(out));
      }
    }
    auto msg = parse_final_message_with_ids(generated, parser_params);
    apply_parsed_message(out, msg);
    if (parser_params.parse_tool_calls && out.tool_calls.empty()) {
      apply_fallback_tool_calls(out, generated);
      fallback_tool_calls_used = !out.tool_calls.empty();
    }
  } catch (const std::exception& e) {
    LOG_ERROR("chat_parse_failed", "model={} error={}", info_.name, e.what());
    out.text = std::move(generated);
    if (parser_params.parse_tool_calls) {
      apply_fallback_tool_calls(out, out.text);
      fallback_tool_calls_used = !out.tool_calls.empty();
    }
  }
  if (fallback_tool_calls_used) {
    for (std::size_t i = 0; i < out.tool_calls.size(); ++i) {
      const auto& tc = out.tool_calls[i];
      InferenceDelta delta;
      ToolCallDelta tcd;
      tcd.index = i;
      tcd.id = tc.id;
      tcd.type = "function";
      tcd.function_name = tc.function_name;
      tcd.function_arguments = tc.function_arguments;
      delta.tool_calls.push_back(std::move(tcd));
      if (!callback(delta)) break;
    }
  }
  slot.last_prompt_tokens.assign(prompt_tokens.begin(), prompt_tokens.end());
  slot.last_prompt_tokens.insert(slot.last_prompt_tokens.end(),
                                 decoded_ids.begin(), decoded_ids.end());

  common_sampler_free(smp);
  return Result<InferenceResult>(std::move(out));
}

}
