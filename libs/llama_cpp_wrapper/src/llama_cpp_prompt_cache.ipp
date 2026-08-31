Result<ChatTemplateResult> LlamaCppModel::apply_chat_template(
    const InferenceRequest& req, int max_prompt_tokens) {
  if (!chat_templates_) {
    return Result<ChatTemplateResult>(std::unexpect, make_error(ErrorCode::Internal, "chat templates not initialized"));
  }

  auto caps = common_chat_templates_get_caps(chat_templates_);
  LlamaChatAdapterOptions adapter_options;
  adapter_options.supports_thinking =
      common_chat_templates_support_enable_thinking(chat_templates_);
  adapter_options.supports_parallel_tool_calls =
      caps["supports_parallel_tool_calls"];
  adapter_options.default_reasoning_format = info_.reasoning_format.empty()
      ? cfg_.reasoning_format : info_.reasoning_format;
  auto adapted = adapt_generation_request(req, info_, adapter_options);
  if (!adapted) {
    return Result<ChatTemplateResult>(std::unexpect,
        make_error(adapted.error().code, adapted.error().message));
  }
  auto inputs = std::move(adapted->inputs);
  auto media = std::move(adapted->media);
  if (!media.empty() && mtmd_ == nullptr) {
    return Result<ChatTemplateResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument,
                   "image input requires a loaded vision projector"));
  }

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
    LOG_INFO("request_shape",
             "model={} msgs={} [sys={} sys_chars={} user={} asst={} tool_results={} tool_result_chars={}] "
             "tools_defined={} max_tokens={}",
             info_.name, inputs.messages.size(),
             n_sys, sys_chars, n_user, n_assistant, n_tool, tool_result_chars,
             req.tools.size(), req.max_output_tokens);
  }

  if (req.reasoning_effort) {
    LOG_INFO("reasoning_effort_applied", "model={} effort={}",
             info_.name, *req.reasoning_effort);
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
  result.stop_strings.insert(result.stop_strings.end(), req.stop.begin(),
                             req.stop.end());
  // Sampler params: explicit per-request values win; otherwise
  // fall back to the server-side SamplingConfig defaults (issue #42), which
  // mirror stock llama-server (DRY off, repeat_penalty neutral).
  const auto& sc = cfg_.sampling;
  result.sampling_params.temp          = req.sampling.temperature.value_or(sc.temperature);
  result.sampling_params.top_p         = req.sampling.top_p.value_or(sc.top_p);
  result.sampling_params.top_k         = req.sampling.top_k.value_or(sc.top_k);
  result.sampling_params.min_p         = req.sampling.min_p.value_or(sc.min_p);
  result.sampling_params.penalty_repeat = req.sampling.repeat_penalty.value_or(sc.repeat_penalty);
  result.sampling_params.penalty_last_n = req.sampling.repeat_last_n.value_or(sc.repeat_last_n);
  result.sampling_params.penalty_freq = req.sampling.frequency_penalty.value_or(0.0f);
  result.sampling_params.penalty_present = req.sampling.presence_penalty.value_or(0.0f);
  if (req.sampling.mirostat) result.sampling_params.mirostat = *req.sampling.mirostat;
  if (req.sampling.mirostat_eta) {
    result.sampling_params.mirostat_eta = *req.sampling.mirostat_eta;
  }
  if (req.sampling.mirostat_tau) {
    result.sampling_params.mirostat_tau = *req.sampling.mirostat_tau;
  }
  result.sampling_params.logit_bias.reserve(req.sampling.logit_bias.size());
  for (const auto& [token, bias] : req.sampling.logit_bias) {
    result.sampling_params.logit_bias.push_back({
        static_cast<llama_token>(token), bias});
  }
  result.sampling_params.dry_multiplier     = sc.dry_multiplier;
  result.sampling_params.dry_base           = sc.dry_base;
  result.sampling_params.dry_allowed_length = sc.dry_allowed_length;
  result.sampling_params.dry_penalty_last_n = sc.dry_penalty_last_n;
  result.sampling_params.dry_sequence_breakers = sc.dry_seq_breakers;
  result.sampling_params.seed = req.sampling.seed >= 0
      ? static_cast<std::uint32_t>(req.sampling.seed) : LLAMA_DEFAULT_SEED;

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
           info_.name, opt_f(req.sampling.temperature), opt_f(req.sampling.top_p),
           opt_i(req.sampling.top_k), opt_f(req.sampling.repeat_penalty),
           opt_i(req.sampling.repeat_last_n),
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
  const int loaded_n_ctx_seq = static_cast<int>(llama_n_ctx_seq(shared_ctx_));
  const int n_ctx_seq = req.context_window
      ? std::min(loaded_n_ctx_seq, *req.context_window)
      : loaded_n_ctx_seq;
  int budget = 0;
  if (cfg_.truncate_prompt && n_ctx_seq > 0) {
    // See maybe_truncate_prompt: clamp bounds must satisfy lo <= hi (UB
    // otherwise) when n_ctx_seq < 1024.
    const int reserve_hi = n_ctx_seq / 4;
    const int reserve = std::clamp(req.max_output_tokens > 0 ? req.max_output_tokens : 1024,
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
        prompt.size(),
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
  s.n_ctx_seq = n_ctx_seq;

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
    maybe_truncate_prompt(s.prompt_tokens, s.n_ctx_seq, req.max_output_tokens, info_.name);
    n_tokens = static_cast<int>(s.prompt_tokens.size());
    s.prompt_position_count = n_tokens;
  }

  const int ctx_budget = std::max(
      1, s.n_ctx_seq - s.prompt_position_count - 1);
  s.max_tokens = req.max_output_tokens > 0
      ? std::min(req.max_output_tokens, ctx_budget) : ctx_budget;

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
