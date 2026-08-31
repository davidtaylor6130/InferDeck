
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
    if (!on_token(ev) && !task.caller_stop.load())
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
  task.capture_probabilities = req.logprobs;
  task.top_probabilities   = req.top_logprobs;

  scheduler_->submit(&task);

  std::vector<inference::TokenLogprob> generated_logprobs;
  auto drain_res = drain_task(task, [&](const TokenEvent& event) -> bool {
    const llama_token id = event.id;
    if (string_stopped) return false; // already stopping
    const std::string piece = token_to_piece(vocab_, id);
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
    if (req.logprobs) {
      inference::TokenLogprob token;
      token.token = piece;
      token.bytes.assign(piece.begin(), piece.end());
      token.logprob = std::log(std::max(
          event.probability, std::numeric_limits<float>::min()));
      for (const auto& [top_id, probability] : event.top_probabilities) {
        inference::TopTokenLogprob top;
        top.token = token_to_piece(vocab_, top_id);
        top.bytes.assign(top.token.begin(), top.token.end());
        top.logprob = std::log(std::max(
            probability, std::numeric_limits<float>::min()));
        token.top_logprobs.push_back(std::move(top));
      }
      generated_logprobs.push_back(std::move(token));
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
  out.prompt_tokens         = static_cast<int>(setup.prompt_tokens.size());
  out.cached_prompt_tokens  = task.out_cached_prompt_tokens;
  out.completion_tokens     = static_cast<int>(decoded_ids.size());
  out.duration_ms = std::chrono::duration<float, std::milli>(end - start).count();
  out.prompt_duration_ms = task.out_prompt_duration_ms;
  out.first_token_duration_ms = task.out_first_token_duration_ms;
  out.generation_duration_ms = task.out_generation_duration_ms > 0.0f
      ? task.out_generation_duration_ms
      : out.duration_ms;
  out.tokens_per_second = detail::generation_tokens_per_second(
      out.completion_tokens, out.generation_duration_ms);
  out.mtp_drafted_tokens = task.n_drafted;
  out.mtp_accepted_tokens = task.n_draft_accepted;
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
  out.reasoning_tokens = count_output_tokens(vocab_, out.reasoning_text);
  if (req.logprobs) {
    out.logprobs = std::move(generated_logprobs);
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
  task.capture_probabilities = req.logprobs;
  task.top_probabilities  = req.top_logprobs;

  scheduler_->submit(&task);

  std::vector<inference::TokenLogprob> generated_logprobs;
  auto drain_res = drain_task(task, [&](const TokenEvent& event) -> bool {
    const llama_token id = event.id;
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
      std::optional<inference::TokenLogprob> token_logprob;
      if (req.logprobs) {
        inference::TokenLogprob token;
        token.token = piece;
        token.bytes.assign(piece.begin(), piece.end());
        token.logprob = std::log(std::max(
            event.probability, std::numeric_limits<float>::min()));
        for (const auto& [top_id, probability] : event.top_probabilities) {
          inference::TopTokenLogprob top;
          top.token = token_to_piece(vocab_, top_id);
          top.bytes.assign(top.token.begin(), top.token.end());
          top.logprob = std::log(std::max(
              probability, std::numeric_limits<float>::min()));
          token.top_logprobs.push_back(std::move(top));
        }
        generated_logprobs.push_back(token);
        token_logprob = std::move(token);
      }
      std::vector<common_chat_msg_diff> diffs;
      try {
        diffs = parser_state.update(piece, /*is_partial=*/true,
                                    setup.parser_params.parse_tool_calls);
      } catch (...) {}

      for (const auto& diff : diffs) {
        auto delta = to_delta(diff);
        if (token_logprob && delta.content == piece) {
          delta.logprobs.push_back(*token_logprob);
        }
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
  out.prompt_duration_ms = task.out_prompt_duration_ms;
  out.first_token_duration_ms = task.out_first_token_duration_ms;
  out.generation_duration_ms = task.out_generation_duration_ms > 0.0f
      ? task.out_generation_duration_ms
      : out.duration_ms;
  out.tokens_per_second = detail::generation_tokens_per_second(
      out.completion_tokens, out.generation_duration_ms);
  out.mtp_drafted_tokens = task.n_drafted;
  out.mtp_accepted_tokens = task.n_draft_accepted;
  if (req.logprobs) {
    out.logprobs = std::move(generated_logprobs);
  }
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
  out.reasoning_tokens = count_output_tokens(vocab_, out.reasoning_text);
  return Result<InferenceResult>(std::move(out));
}
