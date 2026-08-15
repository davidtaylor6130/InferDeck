#include "llama_cpp_wrapper/llama_cpp_model.hpp"
#include "llama_cpp_wrapper/streaming_tool_call_state.hpp"
#include "llama_cpp_wrapper/continuous_batch_scheduler.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
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
#include "base64.hpp"
#include "chat.h"
#include "mtmd-helper.h"
#include "sampling.h"
#include "speculative.h"
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
using inferdeck::model::EmbeddingResult;
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

template <typename Json>
std::vector<std::vector<uint8_t>> extract_image_media(Json& messages) {
  std::vector<std::vector<uint8_t>> media;
  for (auto& message : messages) {
    if (!message.is_object() || !message.contains("content") ||
        !message["content"].is_array()) {
      continue;
    }
    for (auto& part : message["content"]) {
      if (!part.is_object()) continue;
      const auto type = part.value("type", std::string{});
      if (type != "image_url" && type != "image" && type != "input_image") {
        continue;
      }
      if (!part.contains("image_url")) {
        throw std::invalid_argument("image input requires image_url");
      }
      const auto& value = part["image_url"];
      std::string url;
      if (value.is_string()) {
        url = value.template get<std::string>();
      } else if (value.is_object() && value.contains("url") && value["url"].is_string()) {
        url = value["url"].template get<std::string>();
      } else {
        throw std::invalid_argument("image_url must be a string or an object with a string url");
      }
      const auto comma = url.find(',');
      if (comma == std::string::npos ||
          !url.starts_with("data:image/") ||
          url.substr(0, comma).find(";base64") == std::string::npos) {
        throw std::invalid_argument("image_url must be a base64 data:image URL");
      }
      const auto encoded = url.substr(comma + 1);
      if (encoded.empty() || encoded.size() > 32U * 1024U * 1024U) {
        throw std::invalid_argument("image payload is empty or exceeds 32 MiB encoded");
      }
      const auto decoded = base64::decode(encoded);
      if (decoded.empty()) {
        throw std::invalid_argument("image payload decoded to empty data");
      }
      media.emplace_back(decoded.begin(), decoded.end());
      part = Json{
          {"type", "media_marker"},
          {"text", mtmd_default_marker()},
      };
    }
  }
  return media;
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
    llama_token token{};
    int n = llama_tokenize(vocab, stop_str.c_str(), static_cast<int>(stop_str.size()),
                           &token, 1, false, true);
    if (n == 1) {
      tokens.push_back(token);
    }
  }
  return tokens;
}

std::string token_to_piece(const llama_vocab* vocab, llama_token token) {
  std::array<char, 256> local{};
  int n = llama_token_to_piece(
      vocab, token, local.data(), static_cast<int>(local.size()), 0, true);
  if (n >= 0) {
    return std::string(local.data(), static_cast<std::size_t>(n));
  }

  std::vector<char> buffer(
      static_cast<std::size_t>(-static_cast<int64_t>(n)));
  while (true) {
    n = llama_token_to_piece(
        vocab, token, buffer.data(), static_cast<int>(buffer.size()), 0, true);
    if (n >= 0) {
      return std::string(buffer.data(), static_cast<std::size_t>(n));
    }
    const auto required = static_cast<std::size_t>(-static_cast<int64_t>(n));
    if (required <= buffer.size()) return {};
    buffer.resize(required);
  }
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
  resolved_mmproj_path_ = normalize_path(info_.mmproj_path);
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
  if (speculative_) {
    common_speculative_free(speculative_);
    speculative_ = nullptr;
  }
  if (draft_ctx_) {
    llama_free(draft_ctx_);
    draft_ctx_ = nullptr;
  }
  if (shared_ctx_) {
    llama_free(shared_ctx_);
    shared_ctx_ = nullptr;
  }
  if (mtmd_) {
    mtmd_free(mtmd_);
    mtmd_ = nullptr;
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
  if (info_.has_vision && resolved_mmproj_path_.empty()) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::NotFound, "vision model has no mmproj_path"));
  }
  if (info_.has_vision && !std::filesystem::exists(resolved_mmproj_path_, ec)) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::NotFound, "mmproj not found: " + resolved_mmproj_path_.string()));
  }

  llama_model_params mparams = llama_model_default_params();
  mparams.load_mode = cfg_.use_mmap
      ? (cfg_.use_mlock ? LLAMA_LOAD_MODE_MMAP_MLOCK : LLAMA_LOAD_MODE_MMAP)
      : (cfg_.use_mlock ? LLAMA_LOAD_MODE_MLOCK : LLAMA_LOAD_MODE_NONE);
  mparams.n_gpu_layers = cfg_.n_gpu_layers.value_or(-1);
  mparams.load_mtp = cfg_.mtp_enabled;

  llama_backend_init();
  const char* sys_info = llama_print_system_info();
  if (sys_info) {
    LOG_INFO("llama_system_info", "{}", sys_info);
  }
  LOG_INFO("llama_model_load_config",
           "model={} path={} use_mmap={} use_mlock={} n_gpu_layers={} n_ctx={} n_slots={} n_batch={} n_ubatch={} flash_attn={} kv_offload={} op_offload={} cache_type_k={} cache_type_v={} mtp_enabled={} mtp_draft_tokens={} mtp_max_active_requests={} swa_full={}",
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
           cfg_.mtp_enabled,
           cfg_.mtp_draft_tokens,
           cfg_.mtp_max_active_requests,
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
  // Empty cfg_.chat_template => use the template embedded in the GGUF; a non-empty
  // value is a literal Jinja override (e.g. the corrected Qwen3.6 template that avoids
  // the "No user query found in messages." crash during multi-step tool calling).
  if (info_.supports("chat_completions")) {
    chat_templates_ = common_chat_templates_init(model_, cfg_.chat_template).release();
    if (chat_templates_ == nullptr) {
      llama_model_free(model_);
      model_ = nullptr;
      return Result<void>(std::unexpect,
          make_error(ErrorCode::ParseError, "common_chat_templates_init returned null"));
    }
  }
  if (info_.has_vision) {
    auto mtmd_params = mtmd_context_params_default();
    mtmd_params.use_gpu = true;
    mtmd_params.n_threads = cfg_.n_threads;
    mtmd_params.flash_attn_type = flash_attn_from_string(cfg_.flash_attn);
    mtmd_ = mtmd_init_from_file(
        resolved_mmproj_path_.string().c_str(), model_, mtmd_params);
    if (mtmd_ == nullptr || !mtmd_support_vision(mtmd_)) {
      if (mtmd_) {
        mtmd_free(mtmd_);
        mtmd_ = nullptr;
      }
      if (chat_templates_) {
        common_chat_templates_free(chat_templates_);
        chat_templates_ = nullptr;
      }
      llama_model_free(model_);
      model_ = nullptr;
      vocab_ = nullptr;
      return Result<void>(std::unexpect,
          make_error(ErrorCode::ParseError,
                     "failed to load vision projector: " + resolved_mmproj_path_.string()));
    }
    LOG_INFO("vision_projector_loaded",
             "model={} path={}", info_.name, resolved_mmproj_path_.string());
  }
  auto ctx_res = init_shared_context_locked();
  if (!ctx_res.has_value()) {
    if (speculative_) {
      common_speculative_free(speculative_);
      speculative_ = nullptr;
    }
    if (draft_ctx_) { llama_free(draft_ctx_); draft_ctx_ = nullptr; }
    if (shared_ctx_) { llama_free(shared_ctx_); shared_ctx_ = nullptr; }
    if (mtmd_) { mtmd_free(mtmd_); mtmd_ = nullptr; }
    slots_.clear();
    if (chat_templates_) {
      common_chat_templates_free(chat_templates_);
      chat_templates_ = nullptr;
    }
    llama_model_free(model_);
    model_ = nullptr;
    vocab_ = nullptr;
    return ctx_res;
  }
  // Populate chat_template_meta_ once from the model's Jinja template.
  if (info_.supports("chat_completions")) {
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
  cparams.embeddings  = info_.supports("embeddings");
  cparams.type_k      = cache_type_from_string(cfg_.cache_type_k);
  cparams.type_v      = cache_type_from_string(cfg_.cache_type_v);
  cparams.n_rs_seq    = cfg_.mtp_enabled
      ? static_cast<std::uint32_t>(std::max(1, cfg_.mtp_draft_tokens))
      : 0;

  LOG_INFO("llama_shared_context_config",
           "model={} n_slots={} ctx_per_slot={} total_ctx={} n_seq_max={} "
           "n_batch={} n_ubatch={} flash_attn={} kv_offload={} op_offload={} "
           "cache_type_k={} cache_type_v={} mtp_enabled={} mtp_draft_tokens={} mtp_max_active_requests={} swa_full={}",
           info_.name, n_slots, ctx_per_slot, total_ctx, n_slots,
           cparams.n_batch, cparams.n_ubatch,
           cfg_.flash_attn, cfg_.kv_offload, cfg_.op_offload,
           cfg_.cache_type_k, cfg_.cache_type_v,
           cfg_.mtp_enabled, cfg_.mtp_draft_tokens,
           cfg_.mtp_max_active_requests, cfg_.swa_full);

  shared_ctx_ = llama_init_from_model(model_, cparams);
  if (shared_ctx_ == nullptr) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::OutOfMemory,
                   "llama_init_from_model returned null (shared context, total_ctx=" +
                   std::to_string(total_ctx) + ")"));
  }

  auto draft_seq_rm_type = COMMON_CONTEXT_SEQ_RM_TYPE_NO;
  if (cfg_.mtp_enabled) {
    auto draft_params = cparams;
    draft_params.ctx_type = LLAMA_CONTEXT_TYPE_MTP;
    draft_params.n_rs_seq = 0;
    draft_params.n_ubatch = std::min<std::uint32_t>(
        draft_params.n_ubatch, 512);
    draft_ctx_ = llama_init_from_model(model_, draft_params);
    if (draft_ctx_ == nullptr) {
      return Result<void>(std::unexpect,
          make_error(ErrorCode::InvalidArgument,
                     "MTP is enabled but the GGUF has no usable MTP head"));
    }

    (void)common_context_can_seq_rm(shared_ctx_);
    draft_seq_rm_type = common_context_can_seq_rm(draft_ctx_);
    if (draft_seq_rm_type == COMMON_CONTEXT_SEQ_RM_TYPE_NO) {
      return Result<void>(std::unexpect,
          make_error(ErrorCode::InvalidArgument,
                     "MTP draft context does not support sequence removal"));
    }

    common_params_speculative params;
    params.types = {COMMON_SPECULATIVE_TYPE_DRAFT_MTP};
    params.draft.n_max = std::clamp(cfg_.mtp_draft_tokens, 1, 4);
    params.draft.n_min = 0;
    params.draft.p_min = std::clamp(cfg_.mtp_p_min, 0.0f, 1.0f);
    params.draft.cache_type_k =
        cache_type_from_string(cfg_.cache_type_k);
    params.draft.cache_type_v =
        cache_type_from_string(cfg_.cache_type_v);
    params.draft.ctx_tgt = shared_ctx_;
    params.draft.ctx_dft = draft_ctx_;
    try {
      speculative_ = common_speculative_init(
          params, static_cast<std::uint32_t>(n_slots));
    } catch (const std::exception& error) {
      return Result<void>(std::unexpect,
          make_error(ErrorCode::Internal,
                     std::string("MTP initialization failed: ") +
                     error.what()));
    }
    if (speculative_ == nullptr) {
      return Result<void>(std::unexpect,
          make_error(ErrorCode::Internal,
                     "MTP initialization returned null"));
    }
    LOG_INFO("llama_mtp_initialized",
             "model={} draft_tokens={} p_min={} max_active_requests={} draft_seq_rm_type={}",
             info_.name,
             params.draft.n_max,
             params.draft.p_min,
             cfg_.mtp_max_active_requests,
             static_cast<int>(draft_seq_rm_type));
  }

  slots_.clear();
  slots_.resize(n_slots);

  // Spawn the scheduler that owns the decode loop for this context.
  if (info_.supports("chat_completions")) {
    scheduler_ = std::make_unique<ContinuousBatchScheduler>(
        shared_ctx_,
        draft_ctx_,
        speculative_,
        mtmd_,
        model_,
        vocab_,
        cfg_.n_batch,
        cfg_.mtp_max_active_requests,
        draft_seq_rm_type);
  }

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
  if (speculative_) {
    common_speculative_free(speculative_);
    speculative_ = nullptr;
  }
  if (draft_ctx_) {
    llama_free(draft_ctx_);
    draft_ctx_ = nullptr;
  }
  if (shared_ctx_) {
    llama_free(shared_ctx_);
    shared_ctx_ = nullptr;
  }
  if (mtmd_) {
    mtmd_free(mtmd_);
    mtmd_ = nullptr;
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
  return estimate_vram_mb(info_.n_slots);
}

bool LlamaCppModel::can_resize_slots() const noexcept {
  return info_.vram_fixed_mb > 0 && info_.vram_per_slot_mb > 0 &&
         info_.n_slots > info_.min_slots;
}

int LlamaCppModel::estimate_vram_mb(int slots) const noexcept {
  if (info_.vram_fixed_mb > 0 && info_.vram_per_slot_mb > 0) {
    return info_.vram_fixed_mb + info_.vram_per_slot_mb * std::max(info_.min_slots, slots);
  }
  return info_.vram_required_mb;
}

Result<void> LlamaCppModel::resize_slots(int slots) {
  if (slots < info_.min_slots || slots > info_.n_slots) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::InvalidArgument, "invalid slot capacity: " + std::to_string(slots)));
  }
  if (slots == info_.n_slots) return Result<void>{};
  {
    std::lock_guard lk(mtx_);
    if (std::any_of(slots_.begin(), slots_.end(), [](const SlotState& slot) { return slot.busy; })) {
      return Result<void>(std::unexpect,
          make_error(ErrorCode::Unavailable, "cannot resize while slots are active"));
    }
  }
  const int previous = info_.n_slots;
  const bool was_loaded = loaded_.load();
  if (was_loaded) {
    auto unloaded = unload();
    if (!unloaded) return unloaded;
  }
  info_.n_slots = slots;
  if (!was_loaded) return Result<void>{};
  auto loaded = load();
  if (loaded) return loaded;
  info_.n_slots = previous;
  (void)load();
  return loaded;
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
  if (draft_ctx_) {
    auto* mem = llama_get_memory(draft_ctx_);
    if (mem) {
      for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        llama_memory_seq_rm(mem, i, 0, -1);
      }
    }
  }
  for (auto& s : slots_) {
    s.busy = false;
    s.last_prompt_tokens.clear();
    s.recurrent_checkpoint.reset();
    s.recurrent_draft_checkpoint.reset();
    s.recurrent_mtp_checkpoint.reset();
    s.checkpoint_pos = 0;
    s.mtp_cache_synced = true;
  }
  return Result<void>{};
}

Result<inferdeck::model::EmbeddingResult> LlamaCppModel::embed(
    int slot_id, const inferdeck::model::EmbeddingRequest& request,
    const std::function<bool()>& cancelled) {
  const auto started = std::chrono::steady_clock::now();
  std::lock_guard lk(mtx_);
  if (!loaded_.load() || !shared_ctx_ || !model_ || !vocab_) {
    return Result<EmbeddingResult>(std::unexpect,
        make_error(ErrorCode::NotLoaded, "embedding model not loaded"));
  }
  if (!info_.supports("embeddings") || scheduler_) {
    return Result<EmbeddingResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument, "dedicated embedding model required"));
  }
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size()) || !slots_[slot_id].busy) {
    return Result<EmbeddingResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument, "embedding slot is not acquired"));
  }

  EmbeddingResult result;
  const int native_dimensions = llama_model_n_embd_out(model_);
  if (request.dimensions && *request.dimensions != native_dimensions) {
    return Result<EmbeddingResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument,
                   "requested dimensions must equal native dimensions: " +
                   std::to_string(native_dimensions)));
  }
  result.embeddings.reserve(request.inputs.size());
  for (const auto& input : request.inputs) {
    if (cancelled && cancelled()) {
      return Result<EmbeddingResult>(std::unexpect,
          make_error(ErrorCode::Cancelled, "embedding request cancelled"));
    }
    auto tokens = common_tokenize(vocab_, input, true, true);
    if (tokens.empty() || tokens.size() > llama_n_batch(shared_ctx_)) {
      return Result<EmbeddingResult>(std::unexpect,
          make_error(ErrorCode::InvalidArgument, "embedding input exceeds model batch limit"));
    }
    llama_batch batch = llama_batch_init(static_cast<int32_t>(tokens.size()), 0, 1);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      common_batch_add(batch, tokens[i], static_cast<llama_pos>(i), {0}, true);
    }
    llama_memory_clear(llama_get_memory(shared_ctx_), true);
    if (llama_decode(shared_ctx_, batch) < 0) {
      llama_batch_free(batch);
      return Result<EmbeddingResult>(std::unexpect,
          make_error(ErrorCode::Internal, "embedding decode failed"));
    }
    const float* raw = llama_pooling_type(shared_ctx_) == LLAMA_POOLING_TYPE_NONE
        ? llama_get_embeddings_ith(shared_ctx_, batch.n_tokens - 1)
        : llama_get_embeddings_seq(shared_ctx_, 0);
    if (!raw) {
      llama_batch_free(batch);
      return Result<EmbeddingResult>(std::unexpect,
          make_error(ErrorCode::Internal, "embedding output unavailable"));
    }
    auto& output = result.embeddings.emplace_back(static_cast<std::size_t>(native_dimensions));
    common_embd_normalize(raw, output.data(), native_dimensions, 2);
    result.prompt_tokens += static_cast<int>(tokens.size());
    llama_batch_free(batch);
  }
  result.duration_ms = std::chrono::duration<float, std::milli>(
      std::chrono::steady_clock::now() - started).count();
  return Result<EmbeddingResult>(std::move(result));
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
  std::vector<std::vector<uint8_t>> media;
  if (body.contains("messages") && body["messages"].is_array()) {
    try {
      media = extract_image_media(body["messages"]);
    } catch (const std::exception& e) {
      return Result<ChatTemplateResult>(std::unexpect,
          make_error(ErrorCode::InvalidArgument, e.what()));
    }
    if (!media.empty() && mtmd_ == nullptr) {
      return Result<ChatTemplateResult>(std::unexpect,
          make_error(ErrorCode::InvalidArgument,
                     "image input requires a loaded vision projector"));
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
    const int max_tok = body.value(
        "max_tokens", inferdeck::model::k_max_tokens_use_context_budget);
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
  if (max_prompt_tokens > 0 && media.empty()) {
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
  meta.thinking_end_tags = chat_params.thinking_end_tags;
  meta.preserved_tokens = chat_params.preserved_tokens;
  meta.supports_thinking = chat_params.supports_thinking;

  ChatTemplateResult result;
  result.prompt = chat_params.prompt;
  result.media = std::move(media);
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
  const bool add_bos = llama_vocab_get_add_bos(vocab_);
  const bool has_media = !tmpl_res->media.empty();
  int n_tokens = 0;
  if (has_media) {
    if (mtmd_ == nullptr) {
      return Result<PredictSetup>(std::unexpect,
          make_error(ErrorCode::InvalidArgument,
                     "image input requires a loaded vision projector"));
    }
    mtmd::bitmaps bitmaps;
    for (const auto& data : tmpl_res->media) {
      auto decoded = mtmd_helper_bitmap_init_from_buf(
          mtmd_, data.data(), data.size(), false);
      mtmd::bitmap bitmap(decoded.bitmap);
      mtmd_helper::video_ptr video(decoded.video_ctx);
      if (!bitmap.ptr) {
        return Result<PredictSetup>(std::unexpect,
            make_error(ErrorCode::InvalidArgument,
                       "image payload could not be decoded"));
      }
      if (video) {
        return Result<PredictSetup>(std::unexpect,
            make_error(ErrorCode::InvalidArgument,
                       "only image media is supported for chat inference"));
      }
      bitmaps.entries.push_back(std::move(bitmap));
    }
    mtmd_input_text input{
        prompt.c_str(),
        add_bos,
        true,
    };
    mtmd::input_chunks chunks(mtmd_input_chunks_init());
    auto bitmap_ptrs = bitmaps.c_ptr();
    const auto vision_tokenize_started = std::chrono::steady_clock::now();
    const int tokenized = mtmd_tokenize(
        mtmd_, chunks.ptr.get(), &input,
        bitmap_ptrs.data(), bitmap_ptrs.size());
    if (tokenized != 0) {
      return Result<PredictSetup>(std::unexpect,
          make_error(ErrorCode::InvalidArgument,
                     "multimodal prompt tokenization failed (rc=" +
                     std::to_string(tokenized) + ")"));
    }
    LOG_INFO("vision_prompt_tokenized",
             "model={} images={} chunks={} tokens={} positions={} duration_ms={:.3f}",
             info_.name,
             tmpl_res->media.size(),
             chunks.size(),
             mtmd_helper_get_n_tokens(chunks.ptr.get()),
             mtmd_helper_get_n_pos(chunks.ptr.get()),
             std::chrono::duration<float, std::milli>(
                 std::chrono::steady_clock::now() - vision_tokenize_started).count());
    s.prompt_position_count = static_cast<int>(
        mtmd_helper_get_n_pos(chunks.ptr.get()));
    for (std::size_t index = 0; index < chunks.size(); ++index) {
      const auto* chunk = chunks[index];
      const auto type = mtmd_input_chunk_get_type(chunk);
      if (type == MTMD_INPUT_CHUNK_TYPE_TEXT) {
        std::size_t count = 0;
        const auto* tokens = mtmd_input_chunk_get_tokens_text(chunk, &count);
        s.prompt_tokens.insert(s.prompt_tokens.end(), tokens, tokens + count);
      } else if (type == MTMD_INPUT_CHUNK_TYPE_IMAGE) {
        const int token_start = static_cast<int>(s.prompt_tokens.size());
        const int token_count = static_cast<int>(
            mtmd_input_chunk_get_n_tokens(chunk));
        auto* copy = mtmd_input_chunk_copy(chunk);
        if (copy == nullptr || token_count <= 0) {
          if (copy) mtmd_input_chunk_free(copy);
          return Result<PredictSetup>(std::unexpect,
              make_error(ErrorCode::Internal,
                         "multimodal prompt produced an invalid image chunk"));
        }
        s.prompt_tokens.insert(
            s.prompt_tokens.end(), token_count, LLAMA_TOKEN_NULL);
        s.media_chunks.push_back({
            token_start,
            token_count,
            static_cast<int>(mtmd_input_chunk_get_n_pos(chunk)),
            std::shared_ptr<mtmd_input_chunk>(
                copy, [](mtmd_input_chunk* value) {
                  mtmd_input_chunk_free(value);
                }),
        });
      } else {
        return Result<PredictSetup>(std::unexpect,
            make_error(ErrorCode::InvalidArgument,
                       "only image media is supported for chat inference"));
      }
    }
    n_tokens = static_cast<int>(s.prompt_tokens.size());
    if (s.prompt_tokens.empty() ||
        s.prompt_tokens.back() == LLAMA_TOKEN_NULL) {
      return Result<PredictSetup>(std::unexpect,
          make_error(ErrorCode::Internal,
                     "multimodal chat template did not produce a text generation suffix"));
    }
    s.checkpoint_capture_pos = 0;
  } else {
    s.prompt_tokens.resize(prompt.size() + 16);
    n_tokens = llama_tokenize(
        vocab_, prompt.data(), static_cast<int>(prompt.size()),
        s.prompt_tokens.data(), static_cast<int>(s.prompt_tokens.size()),
        add_bos, true);
    if (n_tokens < 0) {
      s.prompt_tokens.resize(static_cast<std::size_t>(-n_tokens));
      n_tokens = llama_tokenize(
          vocab_, prompt.data(), static_cast<int>(prompt.size()),
          s.prompt_tokens.data(), static_cast<int>(s.prompt_tokens.size()),
          add_bos, true);
      if (n_tokens < 0) {
        return Result<PredictSetup>(std::unexpect,
            make_error(ErrorCode::Internal, "tokenization failed"));
      }
    }
    s.prompt_tokens.resize(static_cast<std::size_t>(n_tokens));
    s.prompt_position_count = n_tokens;
    s.checkpoint_capture_pos = n_tokens;
  }
  const auto& generation_prompt = s.parser_params.generation_prompt;
  if (!has_media && !generation_prompt.empty() &&
      prompt.size() >= generation_prompt.size() &&
      prompt.compare(
          prompt.size() - generation_prompt.size(),
          generation_prompt.size(),
          generation_prompt) == 0) {
    const std::string stable_prefix =
        prompt.substr(0, prompt.size() - generation_prompt.size());
    std::vector<llama_token> stable_tokens(stable_prefix.size() + 16);
    int stable_count = llama_tokenize(
        vocab_, stable_prefix.data(), static_cast<int>(stable_prefix.size()),
        stable_tokens.data(), static_cast<int>(stable_tokens.size()),
        add_bos, true);
    if (stable_count < 0) {
      stable_tokens.resize(static_cast<std::size_t>(-stable_count));
      stable_count = llama_tokenize(
          vocab_, stable_prefix.data(), static_cast<int>(stable_prefix.size()),
          stable_tokens.data(), static_cast<int>(stable_tokens.size()),
          add_bos, true);
    }
    if (stable_count >= 0) {
      stable_tokens.resize(static_cast<std::size_t>(stable_count));
      const int capture_pos = detail::recurrent_checkpoint_capture_pos(
          s.prompt_tokens, stable_tokens);
      if (capture_pos > 0) {
        s.checkpoint_capture_pos = capture_pos;
      }
    }
  }

  // Per-slot context window = n_ctx_seq (total context / n_slots as set during load)
  s.n_ctx_seq = static_cast<int>(llama_n_ctx_seq(shared_ctx_));

  const int prompt_context = s.prompt_position_count > 0
      ? s.prompt_position_count
      : n_tokens;
  if (prompt_context >= s.n_ctx_seq) {
    if (!cfg_.truncate_prompt || has_media)
      return Result<PredictSetup>(std::unexpect,
          make_error(ErrorCode::ContextLengthExceeded,
                     "This model's maximum context length is " + std::to_string(s.n_ctx_seq) +
                     " tokens. However, your messages resulted in " + std::to_string(prompt_context) +
                     " tokens. Please reduce the length of the messages."));
    maybe_truncate_prompt(s.prompt_tokens, s.n_ctx_seq, req.max_tokens, info_.name);
    n_tokens = static_cast<int>(s.prompt_tokens.size());
    s.prompt_position_count = n_tokens;
  }

  const int ctx_budget = std::max(
      1, s.n_ctx_seq - s.prompt_position_count - 1);
  s.max_tokens = req.max_tokens > 0 ? std::min(req.max_tokens, ctx_budget) : ctx_budget;

  // Snapshot per-slot KV state under the mutex (scheduler may touch these after submit)
  {
    std::lock_guard lk(mtx_);
    s.last_prompt_tokens   = slots_[slot_id].last_prompt_tokens;
    s.recurrent_checkpoint = slots_[slot_id].recurrent_checkpoint;
    s.recurrent_draft_checkpoint = slots_[slot_id].recurrent_draft_checkpoint;
    s.recurrent_mtp_checkpoint = slots_[slot_id].recurrent_mtp_checkpoint;
    s.checkpoint_pos       = slots_[slot_id].checkpoint_pos;
    s.mtp_cache_synced     = slots_[slot_id].mtp_cache_synced;
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
    if (!on_token(ev.id) && !task.caller_stop.load())
      task.caller_cancel.store(true);
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
  task.media_chunks        = std::move(setup.media_chunks);
  task.prompt_position_count = setup.prompt_position_count;
  task.last_prompt_tokens  = setup.last_prompt_tokens;
  task.recurrent_checkpoint = std::move(setup.recurrent_checkpoint);
  task.recurrent_draft_checkpoint = std::move(setup.recurrent_draft_checkpoint);
  task.recurrent_mtp_checkpoint = std::move(setup.recurrent_mtp_checkpoint);
  task.checkpoint_pos       = setup.checkpoint_pos;
  task.checkpoint_capture_pos = setup.checkpoint_capture_pos;
  task.mtp_cache_synced    = setup.mtp_cache_synced;
  task.sampler             = setup.smp;   // scheduler takes ownership
  task.max_tokens          = setup.max_tokens;
  task.stop_tokens         = setup.stop_tokens;

  scheduler_->submit(&task);

  auto drain_res = drain_task(task, [&](llama_token id) -> bool {
    if (string_stopped) return false; // already stopping
    generated.append(token_to_piece(vocab_, id));
    for (const auto& stop : setup.stop_strings) {
      if (!stop.empty() && generated.size() >= stop.size() &&
          generated.compare(generated.size() - stop.size(), stop.size(), stop) == 0) {
        generated.resize(generated.size() - stop.size());
        string_stopped = true;
        task.caller_stop.store(true);
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
    slot.recurrent_draft_checkpoint = std::move(task.out_recurrent_draft_checkpoint);
    slot.recurrent_mtp_checkpoint = std::move(task.out_recurrent_mtp_checkpoint);
    slot.checkpoint_pos       = task.out_checkpoint_pos;
    slot.mtp_cache_synced     = task.out_mtp_cache_synced;
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
  out.generation_duration_ms = task.out_generation_duration_ms > 0.0f
      ? task.out_generation_duration_ms
      : out.duration_ms;
  out.tokens_per_second = detail::generation_tokens_per_second(
      out.completion_tokens, out.generation_duration_ms);
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
  detail::StreamingToolCallState tool_call_stream_state;
  bool callback_aborted = false;
  bool string_stopped = false;

  SlotTask task;
  task.slot_id            = slot_id;
  task.prompt_tokens      = setup.prompt_tokens;
  task.media_chunks       = std::move(setup.media_chunks);
  task.prompt_position_count = setup.prompt_position_count;
  task.last_prompt_tokens = setup.last_prompt_tokens;
  task.recurrent_checkpoint = std::move(setup.recurrent_checkpoint);
  task.recurrent_draft_checkpoint = std::move(setup.recurrent_draft_checkpoint);
  task.recurrent_mtp_checkpoint = std::move(setup.recurrent_mtp_checkpoint);
  task.checkpoint_pos      = setup.checkpoint_pos;
  task.checkpoint_capture_pos = setup.checkpoint_capture_pos;
  task.mtp_cache_synced    = setup.mtp_cache_synced;
  task.sampler            = setup.smp;
  task.max_tokens         = setup.max_tokens;
  task.stop_tokens        = setup.stop_tokens;
  task.ext_cancel         = cancel;

  scheduler_->submit(&task);

  auto drain_res = drain_task(task, [&](llama_token id) -> bool {
    if (callback_aborted || string_stopped) return false;

    std::string piece = token_to_piece(vocab_, id);
    if (!piece.empty()) {
      generated.append(piece);

      for (const auto& stop : setup.stop_strings) {
        if (!stop.empty() && generated.size() >= stop.size() &&
            generated.compare(generated.size() - stop.size(), stop.size(), stop) == 0) {
          generated.resize(generated.size() - stop.size());
          string_stopped = true;
          task.caller_stop.store(true);
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
        tool_call_stream_state.observe(delta);
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
    slot.recurrent_draft_checkpoint = std::move(task.out_recurrent_draft_checkpoint);
    slot.recurrent_mtp_checkpoint = std::move(task.out_recurrent_mtp_checkpoint);
    slot.checkpoint_pos       = task.out_checkpoint_pos;
    slot.mtp_cache_synced     = task.out_mtp_cache_synced;
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
  out.generation_duration_ms = task.out_generation_duration_ms > 0.0f
      ? task.out_generation_duration_ms
      : out.duration_ms;
  out.tokens_per_second = detail::generation_tokens_per_second(
      out.completion_tokens, out.generation_duration_ms);
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
      tool_call_stream_state.observe(delta);
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

  if (fallback_tool_calls_used && tool_call_stream_state.should_emit_fallback()) {
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
