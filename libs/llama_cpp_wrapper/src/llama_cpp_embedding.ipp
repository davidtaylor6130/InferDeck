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
    std::vector<llama_token> tokens;
    if (const auto* text = std::get_if<inferdeck::model::EmbeddingTextInput>(&input)) {
      tokens = common_tokenize(vocab_, text->text, true, true);
    } else {
      const auto& token_input = std::get<inferdeck::model::EmbeddingTokenInput>(input);
      const int32_t vocabulary_size = llama_vocab_n_tokens(vocab_);
      tokens.reserve(token_input.tokens.size());
      for (const std::int32_t token : token_input.tokens) {
        if (token < 0 || token >= vocabulary_size) {
          return Result<EmbeddingResult>(std::unexpect,
              make_error(ErrorCode::InvalidArgument,
                         "embedding input contains an invalid token ID"));
        }
        tokens.push_back(static_cast<llama_token>(token));
      }
    }
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
