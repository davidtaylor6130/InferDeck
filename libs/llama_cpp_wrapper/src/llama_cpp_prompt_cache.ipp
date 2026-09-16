Result<inferdeck::model::RequestDemand>
LlamaCppModel::estimate_request_demand(
    const InferenceRequest& req) const {
  const int loaded_n_ctx_seq = info_.concurrency_auto
      ? std::max(512, info_.context_size)
      : (cfg_.kv_unified
          ? std::min(std::max(512, info_.context_size),
                     static_cast<int>(llama_n_ctx_seq(shared_ctx_)))
          : static_cast<int>(llama_n_ctx_seq(shared_ctx_)));
  const int request_context_limit = req.context_window
      ? std::min(loaded_n_ctx_seq, *req.context_window)
      : loaded_n_ctx_seq;
  auto prompt_res = prepare_prompt(req, request_context_limit);
  if (!prompt_res.has_value()) {
    return Result<inferdeck::model::RequestDemand>(
        std::unexpect, prompt_res.error());
  }

  int prompt_positions = prompt_res->prompt_position_count > 0
      ? prompt_res->prompt_position_count
      : static_cast<int>(prompt_res->prompt_tokens.size());
  if (prompt_positions >= request_context_limit &&
      cfg_.truncate_prompt && prompt_res->media_chunks.empty()) {
    maybe_truncate_prompt(prompt_res->prompt_tokens, request_context_limit,
                          req.max_output_tokens, info_.name);
    prompt_positions = static_cast<int>(prompt_res->prompt_tokens.size());
  }

  const int context_budget = std::max(
      1, request_context_limit - prompt_positions - 1);
  const int output_tokens = req.max_output_tokens > 0
      ? std::min(req.max_output_tokens, context_budget)
      : context_budget;

  inferdeck::model::RequestDemand demand;
  demand.prompt_positions = prompt_positions;
  demand.output_tokens = output_tokens;
  demand.required_context = prompt_positions + output_tokens + 1;
  demand.required_sequences = 1;
  return Result<inferdeck::model::RequestDemand>(std::move(demand));
}

Result<ChatTemplateResult> LlamaCppModel::apply_chat_template(
    const InferenceRequest& req, int max_prompt_tokens) const {
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
  common_chat_params chat_params;
  if (max_prompt_tokens > 0 && media.empty())
  {
    int prompt_tokens = 0;
    const std::size_t dropped = fit_chat_history(inputs,
        [&](const common_chat_templates_inputs& candidate)
        {
          chat_params = common_chat_templates_apply(chat_templates_, candidate);
          const std::string& prompt = chat_params.prompt;
          const int count = llama_tokenize(vocab_, prompt.data(),
              static_cast<int>(prompt.size()), nullptr, 0,
              llama_vocab_get_add_bos(vocab_), true);
          prompt_tokens = count < 0 ? -count : count;
          return prompt_tokens < max_prompt_tokens;
        });
    if (dropped > 0)
    {
      LOG_WARN("chat_history_truncated",
               "model={} dropped_messages={} kept_messages={} prompt_tokens={} budget={}",
               info_.name, dropped, inputs.messages.size(), prompt_tokens, max_prompt_tokens);
    }
  }
  else
  {
    chat_params = common_chat_templates_apply(chat_templates_, inputs);
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
Result<LlamaCppModel::PredictSetup> LlamaCppModel::prepare_prompt(
    const InferenceRequest& req, int request_context_limit) const {
  PredictSetup s;
  const int n_ctx_seq = request_context_limit;
  int budget = 0;
  if (cfg_.truncate_prompt && n_ctx_seq > 0) {
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
          mtmd_, data.data(), data.size(), false,
          mtmd_helper_init_opt_default());
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
  return Result<PredictSetup>(std::move(s));
}

Result<LlamaCppModel::PredictSetup> LlamaCppModel::prepare_inference(
    int slot_id, const InferenceRequest& req) {
  const int loaded_n_ctx_seq = info_.concurrency_auto
      ? std::max(512, info_.context_size)
      : (cfg_.kv_unified
          ? std::min(std::max(512, info_.context_size),
                     static_cast<int>(llama_n_ctx_seq(shared_ctx_)))
          : static_cast<int>(llama_n_ctx_seq(shared_ctx_)));
  const int n_ctx_seq = req.context_window
      ? std::min(loaded_n_ctx_seq, *req.context_window)
      : loaded_n_ctx_seq;
  auto prompt_res = prepare_prompt(req, n_ctx_seq);
  if (!prompt_res.has_value())
    return Result<PredictSetup>(std::unexpect, prompt_res.error());
  PredictSetup s = std::move(*prompt_res);
  const int prompt_context = s.prompt_position_count > 0
      ? s.prompt_position_count
      : static_cast<int>(s.prompt_tokens.size());
  if (prompt_context >= s.n_ctx_seq) {
    if (!cfg_.truncate_prompt || !s.media_chunks.empty())
      return Result<PredictSetup>(std::unexpect,
          make_error(ErrorCode::ContextLengthExceeded,
                     "This model's maximum context length is " + std::to_string(s.n_ctx_seq) +
                     " tokens. However, your messages resulted in " + std::to_string(prompt_context) +
                     " tokens. Please reduce the length of the messages."));
    maybe_truncate_prompt(s.prompt_tokens, s.n_ctx_seq, req.max_output_tokens, info_.name);
    s.prompt_position_count = static_cast<int>(s.prompt_tokens.size());
  }

  const int ctx_budget = std::max(
      1, s.n_ctx_seq - s.prompt_position_count - 1);
  s.max_tokens = req.max_output_tokens > 0
      ? std::min(req.max_output_tokens, ctx_budget) : ctx_budget;

  // Snapshot per-slot KV state under the mutex (scheduler may touch these after submit)
  {
    std::lock_guard lk(mtx_);
    if (s.media_chunks.empty() && !slots_[slot_id].sequence_bound)
    {
      const auto reusable_prefix = [&](const SlotState& slot) -> std::size_t
      {
        const int common = detail::recurrent_checkpoint_capture_pos(
            s.prompt_tokens, slot.last_prompt_tokens);
        if (llama_model_is_recurrent(model_) || llama_model_is_hybrid(model_))
        {
          const auto usable = [&](const std::shared_ptr<const std::vector<uint8_t>>& checkpoint)
          {
            return detail::recurrent_checkpoint_usable(
                checkpoint ? checkpoint->size() : 0, slot.checkpoint_pos,
                common, static_cast<int>(s.prompt_tokens.size()));
          };
          if (!usable(slot.recurrent_checkpoint)) return 0;
          return static_cast<std::size_t>(slot.checkpoint_pos);
        }
        return static_cast<std::size_t>(common);
      };
      int best_slot = slot_id;
      std::size_t best_prefix = reusable_prefix(slots_[slot_id]);
      for (int index = 0; index < static_cast<int>(slots_.size()); ++index)
      {
        if (slots_[index].sequence_bound) continue;
        const std::size_t prefix = reusable_prefix(slots_[index]);
        if (prefix > best_prefix)
        {
          best_slot = index;
          best_prefix = prefix;
        }
      }
      if (best_slot != slot_id)
      {
        const bool caller_busy = slots_[slot_id].busy;
        const bool peer_busy = slots_[best_slot].busy;
        std::swap(slots_[slot_id], slots_[best_slot]);
        slots_[slot_id].busy = caller_busy;
        slots_[best_slot].busy = peer_busy;
        foundation::LOG_DEBUG("slot_cache_affinity", "model={} slot={} sequence={} common_tokens={}",
                  info_.name, slot_id, slots_[slot_id].sequence_id, best_prefix);
      }
    }
    slots_[slot_id].sequence_bound = true;
    s.sequence_id          = slots_[slot_id].sequence_id;
    s.last_prompt_tokens   = slots_[slot_id].last_prompt_tokens;
    s.recurrent_checkpoint = slots_[slot_id].recurrent_checkpoint;
    s.recurrent_draft_checkpoint = slots_[slot_id].recurrent_draft_checkpoint;
    s.recurrent_replay_checkpoint = slots_[slot_id].recurrent_replay_checkpoint;
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
