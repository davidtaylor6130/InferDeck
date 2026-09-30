std::string LlamaCppModel::version() {
  const char* info = llama_print_system_info();
  return info ? std::string(info) : std::string("unknown");
}

void LlamaCppModel::init_backend() {
  if (!g_backend_initialized) {
    llama_backend_init();
    g_backend_initialized = true;
    LOG_INFO("llama_backend_init", "llama.cpp backends initialized");
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
  sequence_capacity_.store(0);
  sequence_capacity_limit_.store(0);
  pool_capacity_.store(0);
}

Result<void> LlamaCppModel::load() {
  return load({});
}

Result<void> LlamaCppModel::load(
    const inferdeck::model::LifecycleControl& control) {
  std::lock_guard lk(mtx_);
  if (loaded_.load()) return Result<void>{};
  reclaimed_context_vram_mb_.store(0);
  if (control.is_cancelled()) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::Cancelled, "model load cancelled"));
  }
  if (control.is_expired()) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::Timeout, "model load deadline expired"));
  }
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
  mparams.progress_callback = [](float, void* user_data) {
    const auto* lifecycle = static_cast<
        const inferdeck::model::LifecycleControl*>(user_data);
    return !lifecycle->is_cancelled() && !lifecycle->is_expired();
  };
  mparams.progress_callback_user_data =
      const_cast<inferdeck::model::LifecycleControl*>(&control);

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
    if (control.is_cancelled()) {
      return Result<void>(std::unexpect,
          make_error(ErrorCode::Cancelled, "model load cancelled"));
    }
    if (control.is_expired()) {
      return Result<void>(std::unexpect,
          make_error(ErrorCode::Timeout, "model load deadline expired"));
    }
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
  auto ctx_res = init_shared_context_locked(mparams, control);
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

llama_context_params LlamaCppModel::shared_context_params_locked(int capacity) const {
  llama_context_params cparams = llama_context_default_params();
  cparams.n_ctx      = static_cast<std::uint32_t>(capacity);
  cparams.n_seq_max  = static_cast<std::uint32_t>(std::max(1, n_slots()));
  cparams.kv_unified = cfg_.kv_unified;
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
  return cparams;
}

Result<void> LlamaCppModel::init_shared_context_locked(
    const llama_model_params& model_params,
    const inferdeck::model::LifecycleControl& control,
    std::optional<int> automatic_max_capacity,
    std::optional<int> vram_safety_margin_mb) {
  int n_slots = (info_.concurrency_auto && !automatic_max_capacity)
      ? 1 : std::max(1, this->n_slots());
  if (info_.concurrency_auto && !info_.context_pool_auto) {
    return Result<void>(std::unexpect, make_error(ErrorCode::InvalidArgument,
        "automatic concurrency requires automatic context pooling"));
  }
  const int ctx_per_slot = std::max(512, info_.context_size);
  const int initial_ctx_per_slot = info_.concurrency_auto
      ? std::max(512, std::min(ctx_per_slot, std::max(512, cfg_.n_batch)))
      : ctx_per_slot;
  const int draft_margin = cfg_.mtp_enabled ? std::clamp(cfg_.mtp_draft_tokens, 1, 4) : 0;
  if (info_.context_pool_auto && (!cfg_.kv_unified || info_.context_pool_size != 0)) {
    return Result<void>(std::unexpect, make_error(ErrorCode::InvalidArgument,
        "automatic context pool requires unified KV and no fixed pool size"));
  }
  if (info_.context_pool_size < 0 || (info_.context_pool_size > 0 &&
      (!cfg_.kv_unified || static_cast<std::int64_t>(info_.context_pool_size) <
          static_cast<std::int64_t>(ctx_per_slot) + draft_margin))) {
    return Result<void>(std::unexpect, make_error(ErrorCode::InvalidArgument,
        "shared context pool cannot fit the configured request limit and draft margin"));
  }
  const std::int64_t total_ctx_wide = info_.context_pool_size > 0
      ? info_.context_pool_size
      : static_cast<std::int64_t>(
            info_.concurrency_auto ? initial_ctx_per_slot : ctx_per_slot) * n_slots;
  if (total_ctx_wide > std::numeric_limits<int>::max()) {
    return Result<void>(std::unexpect, make_error(ErrorCode::InvalidArgument, "context capacity exceeds supported range"));
  }
  int total_ctx = static_cast<int>(total_ctx_wide);
  if (info_.concurrency_auto && !automatic_max_capacity.has_value()) {
    total_ctx = std::max(512, initial_ctx_per_slot) + draft_margin;
  }
  if (automatic_max_capacity.has_value()) {
    const int minimum_capacity = info_.concurrency_auto
        ? initial_ctx_per_slot : ctx_per_slot;
    total_ctx = std::max(
        static_cast<int>(static_cast<std::int64_t>(minimum_capacity) + draft_margin),
        *automatic_max_capacity);
  }

  llama_context_params cparams = shared_context_params_locked(total_ctx);

  LOG_INFO("llama_shared_context_config",
           "model={} n_slots={} ctx_per_slot={} total_ctx={} n_seq_max={} kv_unified={} "
           "n_batch={} n_ubatch={} flash_attn={} kv_offload={} op_offload={} "
           "cache_type_k={} cache_type_v={} mtp_enabled={} mtp_draft_tokens={} mtp_max_active_requests={} swa_full={}",
           info_.name, n_slots, ctx_per_slot, total_ctx, n_slots, cfg_.kv_unified,
           cparams.n_batch, cparams.n_ubatch,
           cfg_.flash_attn, cfg_.kv_offload, cfg_.op_offload,
           cfg_.cache_type_k, cfg_.cache_type_v,
           cfg_.mtp_enabled, cfg_.mtp_draft_tokens,
           cfg_.mtp_max_active_requests, cfg_.swa_full);

  const std::int64_t minimum_wide = static_cast<std::int64_t>(
      info_.concurrency_auto ? initial_ctx_per_slot : ctx_per_slot) + draft_margin;
  if (minimum_wide > std::numeric_limits<int>::max()) {
    return Result<void>(std::unexpect, make_error(ErrorCode::InvalidArgument,
        "context pool minimum exceeds supported range"));
  }
  const int minimum_pool = static_cast<int>(minimum_wide);
  if (info_.context_pool_auto) {
    const int effective_margin =
        vram_safety_margin_mb.value_or(cfg_.vram_safety_margin_mb);
    const Result<int> fitted = fit_context_pool(
        resolved_gguf_path_, model_params, cparams, cfg_.mtp_enabled,
        minimum_pool, std::max(minimum_pool, total_ctx), effective_margin, control,
        nullptr, nullptr,
        nullptr);
    if (!fitted) return Result<void>(std::unexpect, fitted.error());
    total_ctx = *fitted;
    cparams.n_ctx = static_cast<std::uint32_t>(total_ctx);
    cparams.n_seq_max = static_cast<std::uint32_t>(n_slots);
  }

  for (int attempt = 0;; ++attempt) {
    if (control.is_cancelled() || control.is_expired()) {
      return Result<void>(std::unexpect, make_error(
          control.is_cancelled() ? ErrorCode::Cancelled : ErrorCode::Timeout,
          "context pool allocation cancelled or expired"));
    }
    std::string allocation_error;
    try {
      shared_ctx_ = llama_init_from_model(model_, cparams);
      if (shared_ctx_ && cfg_.mtp_enabled) {
        llama_context_params draft_params = cparams;
        draft_params.ctx_type = LLAMA_CONTEXT_TYPE_MTP;
        draft_params.n_rs_seq = 0;
        draft_params.n_ubatch = std::min<std::uint32_t>(draft_params.n_ubatch, 512);
        draft_ctx_ = llama_init_from_model(model_, draft_params);
      }
    } catch (const std::exception& error) {
      allocation_error = error.what();
    }
    if (shared_ctx_ && (!cfg_.mtp_enabled || draft_ctx_) && allocation_error.empty()) break;
    const bool missing_draft = shared_ctx_ && cfg_.mtp_enabled && !draft_ctx_;
    if (draft_ctx_) { llama_free(draft_ctx_); draft_ctx_ = nullptr; }
    if (shared_ctx_) { llama_free(shared_ctx_); shared_ctx_ = nullptr; }
    if (info_.concurrency_auto && total_ctx <= minimum_pool && n_slots > 1 && attempt < 16) {
      n_slots = std::max(1, n_slots / 2);
      cparams.n_seq_max = static_cast<std::uint32_t>(n_slots);
      continue;
    }
    if (!info_.context_pool_auto || total_ctx <= minimum_pool || attempt >= 16) {
      return Result<void>(std::unexpect, make_error(
          missing_draft && !info_.context_pool_auto ? ErrorCode::InvalidArgument : ErrorCode::OutOfMemory,
          missing_draft && !info_.context_pool_auto
              ? "MTP is enabled but the GGUF has no usable MTP head or sufficient memory"
              : "shared context allocation failed at capacity " + std::to_string(total_ctx) +
                  (allocation_error.empty() ? std::string{} : ": " + allocation_error)));
    }
    const int next_capacity = attempt >= 7 ? minimum_pool
        : minimum_pool + (total_ctx - minimum_pool) / 2;
    LOG_WARN("context_pool_allocation_retry", "model={} requested={} next={}",
             info_.name, total_ctx, next_capacity);
    total_ctx = next_capacity;
    cparams.n_ctx = static_cast<std::uint32_t>(total_ctx);
    cparams.n_seq_max = static_cast<std::uint32_t>(n_slots);
  }
  sequence_capacity_.store(n_slots);
  if (info_.concurrency_auto) {
    sequence_capacity_limit_.store(static_cast<int>(std::min<std::size_t>(
        llama_max_parallel_sequences(),
        std::max<std::uint32_t>(1, cparams.n_batch))));
  } else {
    sequence_capacity_limit_.store(n_slots);
  }
  pool_capacity_.store(static_cast<int>(llama_n_ctx(shared_ctx_)));
  LOG_INFO("context_pool_allocated", "model={} automatic={} requested={} actual={} request_limit={}",
           info_.name, info_.context_pool_auto, total_ctx, llama_n_ctx(shared_ctx_),
           ctx_per_slot);

  auto draft_seq_rm_type = COMMON_CONTEXT_SEQ_RM_TYPE_NO;
  if (cfg_.mtp_enabled) {
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
  for (int index = 0; index < n_slots; ++index) {
    slots_[index].sequence_id = index;
  }

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
        draft_seq_rm_type,
        info_.context_pool_size > 0 || info_.context_pool_auto);
  }

  return Result<void>{};
}

Result<void> LlamaCppModel::ensure_request_capacity(
    const inferdeck::model::RequestDemand& demand,
    const inferdeck::model::LifecycleControl& control) {
  if (demand.prompt_positions < 0 || demand.output_tokens < 0 ||
      demand.required_context <= 0 || demand.required_sequences <= 0) {
    return Result<void>(std::unexpect, make_error(
        ErrorCode::InvalidArgument, "invalid request capacity demand"));
  }
  if (control.is_cancelled()) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::Cancelled, "request capacity preparation cancelled"));
  }
  if (control.is_expired()) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::Timeout, "request capacity preparation deadline expired"));
  }

  std::unique_lock lk(mtx_);
  if (!loaded_.load() || shared_ctx_ == nullptr) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::NotFound, "model is not loaded"));
  }
  const int current_capacity = static_cast<int>(llama_n_ctx(shared_ctx_));
  const int current_sequences = static_cast<int>(slots_.size());
  const int requested_sequences = std::max(
      demand.required_sequences, demand.aggregate_sequences);
  const int draft_margin = cfg_.mtp_enabled
      ? std::clamp(cfg_.mtp_draft_tokens, 1, 4) : 0;
  const std::int64_t requested_capacity_wide =
      static_cast<std::int64_t>(std::max(
          demand.required_context, demand.aggregate_context)) + draft_margin;
  if (requested_capacity_wide > std::numeric_limits<int>::max()) {
    return Result<void>(std::unexpect, make_error(
        ErrorCode::OutOfMemory, "request context demand exceeds supported range"));
  }
  const int requested_capacity = static_cast<int>(requested_capacity_wide);
  const std::int64_t own_capacity_wide =
      static_cast<std::int64_t>(demand.required_context) + draft_margin;
  if (own_capacity_wide > std::numeric_limits<int>::max()) {
    return Result<void>(std::unexpect, make_error(
        ErrorCode::Unavailable, "request exceeds the supported context capacity"));
  }
  const int own_capacity = static_cast<int>(own_capacity_wide);
  if (requested_capacity <= current_capacity &&
      requested_sequences <= current_sequences) {
    return Result<void>{};
  }
  if (!info_.concurrency_auto || !info_.context_pool_auto || !cfg_.kv_unified) {
    if (own_capacity > current_capacity ||
        demand.required_sequences > current_sequences) {
      return Result<void>(std::unexpect, make_error(
          ErrorCode::Unavailable,
          "request exceeds the fixed context capacity"));
    }
    return Result<void>(std::unexpect, make_error(
        ErrorCode::ResourceBusy,
        "aggregate request demand exceeds the fixed context capacity"));
  }
  const int maximum_sequences = std::max(1, sequence_capacity_limit_.load());
  if (requested_sequences > maximum_sequences) {
    return Result<void>(std::unexpect, make_error(
        ErrorCode::OutOfMemory,
        "request requires more sequence slots than configured"));
  }
  if (std::any_of(slots_.begin(), slots_.end(),
                  [](const SlotState& slot) { return slot.busy; })) {
    return Result<void>(std::unexpect, make_error(
        ErrorCode::ResourceBusy,
        "cannot grow context while requests are active"));
  }
  if (scheduler_ && !scheduler_->healthy()) {
    return Result<void>(std::unexpect, make_error(
        ErrorCode::Unavailable, "cannot grow an unhealthy model context"));
  }

  const int configured_limit = std::max(512, info_.context_size);
  const std::int64_t maximum_wide =
      static_cast<std::int64_t>(configured_limit) *
      std::max(1, maximum_sequences) +
      (cfg_.mtp_enabled ? std::clamp(cfg_.mtp_draft_tokens, 1, 4) : 0);
  if (maximum_wide > std::numeric_limits<int>::max()) {
    return Result<void>(std::unexpect, make_error(
        ErrorCode::InvalidArgument, "configured context capacity exceeds supported range"));
  }
  const int maximum_capacity = static_cast<int>(maximum_wide);
  if (requested_capacity > maximum_capacity) {
    return Result<void>(std::unexpect, make_error(
        ErrorCode::OutOfMemory, "request exceeds the configured context capacity"));
  }

  const int alignment = std::max(1, cfg_.n_batch);
  const auto align_up = [alignment](int value) {
    const std::int64_t rounded =
        (static_cast<std::int64_t>(value) + alignment - 1) / alignment * alignment;
    return static_cast<int>(std::min<std::int64_t>(
        rounded, std::numeric_limits<int>::max()));
  };
  const int geometric = current_capacity > std::numeric_limits<int>::max() / 2
      ? std::numeric_limits<int>::max()
      : current_capacity * 2;
  const int growth_target = requested_capacity > current_capacity
      ? std::max(align_up(requested_capacity), align_up(geometric))
      : current_capacity;
  const int target_capacity = std::min(maximum_capacity, growth_target);

  if (control.is_cancelled() || control.is_expired()) {
    return Result<void>(std::unexpect, make_error(
        control.is_cancelled() ? ErrorCode::Cancelled : ErrorCode::Timeout,
        "request capacity preparation cancelled"));
  }

  llama_model_params model_params = llama_model_default_params();
  model_params.load_mode = cfg_.use_mmap
      ? (cfg_.use_mlock ? LLAMA_LOAD_MODE_MMAP_MLOCK : LLAMA_LOAD_MODE_MMAP)
      : (cfg_.use_mlock ? LLAMA_LOAD_MODE_MLOCK : LLAMA_LOAD_MODE_NONE);
  model_params.n_gpu_layers = cfg_.n_gpu_layers.value_or(-1);
  model_params.load_mtp = cfg_.mtp_enabled;

  if (scheduler_) {
    scheduler_->stop();
    scheduler_.reset();
  }
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
  pool_capacity_.store(0);
  slots_.clear();
  const int target_sequences = std::max(current_sequences,
                                        requested_sequences);
  sequence_capacity_.store(target_sequences);

  const auto initialized = [&]() {
    try {
      return init_shared_context_locked(model_params, control, target_capacity,
                                         cfg_.vram_safety_margin_mb);
    } catch (const std::exception& error) {
      return Result<void>(std::unexpect, make_error(
          ErrorCode::Internal, std::string("context growth failed: ") + error.what()));
    } catch (...) {
      return Result<void>(std::unexpect,
          make_error(ErrorCode::Internal, "context growth failed"));
    }
  }();
  if (!initialized) {
    LOG_ERROR("context_pool_growth_failed", "model={} target_capacity={} error={}",
              info_.name, target_capacity, initialized.error().message);
    if (scheduler_) {
      scheduler_->stop();
      scheduler_.reset();
    }
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
    pool_capacity_.store(0);
    slots_.clear();
    sequence_capacity_.store(current_sequences);
    inferdeck::model::LifecycleControl recovery_control;
    recovery_control.deadline =
        inferdeck::model::LifecycleControl::clock::now() +
        std::chrono::seconds(30);
    const auto recovered = [&]() {
      try {
        return init_shared_context_locked(
            model_params, recovery_control, current_capacity,
            cfg_.vram_safety_margin_mb);
      } catch (const std::exception& error) {
        return Result<void>(std::unexpect, make_error(
            ErrorCode::Internal, std::string("context growth recovery failed: ") +
                                     error.what()));
      } catch (...) {
        return Result<void>(std::unexpect, make_error(
            ErrorCode::Internal, "context growth recovery failed"));
      }
    }();
    if (!recovered) {
      LOG_ERROR("context_pool_growth_recovery_failed",
                "model={} capacity={} error={}",
                info_.name, current_capacity, recovered.error().message);
      return Result<void>(std::unexpect, make_error(
          ErrorCode::Unavailable, "context growth failed and recovery failed"));
    }
    return initialized;
  }

  if (pool_capacity_.load() < requested_capacity ||
      sequence_capacity_.load() < requested_sequences) {
    const int actual_capacity = pool_capacity_.load();
    if (scheduler_) {
      scheduler_->stop();
      scheduler_.reset();
    }
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
    pool_capacity_.store(0);
    slots_.clear();
    sequence_capacity_.store(current_sequences);
    inferdeck::model::LifecycleControl recovery_control;
    recovery_control.deadline =
        inferdeck::model::LifecycleControl::clock::now() +
        std::chrono::seconds(30);
    const auto recovered = [&]() {
      try {
        return init_shared_context_locked(
            model_params, recovery_control, current_capacity,
            cfg_.vram_safety_margin_mb);
      } catch (const std::exception& error) {
        return Result<void>(std::unexpect, make_error(
            ErrorCode::Internal, std::string("context growth recovery failed: ") +
                                     error.what()));
      } catch (...) {
        return Result<void>(std::unexpect, make_error(
            ErrorCode::Internal, "context growth recovery failed"));
      }
    }();
    if (!recovered) {
      return Result<void>(std::unexpect, make_error(
          ErrorCode::Unavailable, "context growth below demand and recovery failed"));
    }
    return Result<void>(std::unexpect, make_error(
        ErrorCode::OutOfMemory,
        "context growth allocated below the requested demand (" +
            std::to_string(actual_capacity) + " < " +
            std::to_string(requested_capacity) + ")"));
  }

  reclaimed_context_vram_mb_.store(0);
  LOG_INFO("context_pool_grown", "model={} old_capacity={} new_capacity={} required_context={} cache_reset=true",
           info_.name, current_capacity, pool_capacity_.load(),
           demand.required_context);
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
  sequence_capacity_.store(0);
  sequence_capacity_limit_.store(0);
  pool_capacity_.store(0);
  reclaimed_context_vram_mb_.store(0);
  log_memory_snapshot("llama_model_unload_memory_after", info_.name);
  return Result<void>{};
}

int LlamaCppModel::vram_usage_mb() const noexcept {
  return estimate_vram_mb(n_slots());
}

bool LlamaCppModel::can_resize_slots() const noexcept {
  if (info_.concurrency_auto) return false;
  return info_.vram_fixed_mb > 0 && info_.vram_per_slot_mb > 0 &&
         info_.n_slots > info_.min_slots;
}

bool LlamaCppModel::can_reclaim_idle_context() const {
  std::lock_guard lk(mtx_);
  if (!loaded_.load() || !info_.context_pool_auto || !cfg_.kv_unified ||
      shared_ctx_ == nullptr ||
      (info_.supports("chat_completions") &&
       (!scheduler_ || !scheduler_->healthy())) ||
      std::any_of(slots_.begin(), slots_.end(),
                  [](const SlotState& slot) { return slot.busy; })) {
    return false;
  }
  const int minimum_ctx = info_.concurrency_auto
      ? std::max(512, std::min(std::max(512, info_.context_size),
                               std::max(512, cfg_.n_batch)))
      : std::max(512, info_.context_size);
  const std::int64_t minimum =
      static_cast<std::int64_t>(minimum_ctx) +
      (cfg_.mtp_enabled ? std::clamp(cfg_.mtp_draft_tokens, 1, 4) : 0);
  return (info_.concurrency_auto && slots_.size() > 1) ||
         static_cast<std::int64_t>(llama_n_ctx(shared_ctx_)) > minimum;
}

Result<bool> LlamaCppModel::reclaim_idle_context(
    int additional_reserve_mb,
    const inferdeck::model::LifecycleControl& control) {
  std::lock_guard lk(mtx_);
  if (additional_reserve_mb < 0) {
    return Result<bool>(std::unexpect, make_error(
        ErrorCode::InvalidArgument,
        "additional context reclamation reserve cannot be negative"));
  }
  if (control.is_cancelled()) {
    return Result<bool>(std::unexpect, make_error(
        ErrorCode::Cancelled, "idle context reclamation cancelled"));
  }
  if (control.is_expired()) {
    return Result<bool>(std::unexpect, make_error(
        ErrorCode::Timeout, "idle context reclamation deadline expired"));
  }
  if (!loaded_.load() || !info_.context_pool_auto || !cfg_.kv_unified ||
      shared_ctx_ == nullptr || additional_reserve_mb == 0) {
    return false;
  }
  if (info_.supports("chat_completions") &&
      (!scheduler_ || !scheduler_->healthy())) {
    return Result<bool>(std::unexpect, make_error(
        ErrorCode::Unavailable,
        "cannot reclaim an unhealthy model context"));
  }
  if (std::any_of(slots_.begin(), slots_.end(),
                  [](const SlotState& slot) { return slot.busy; })) {
    return Result<bool>(std::unexpect, make_error(
        ErrorCode::Unavailable,
        "cannot reclaim context while slots are active"));
  }
  if (additional_reserve_mb >
      std::numeric_limits<int>::max() - cfg_.vram_safety_margin_mb) {
    return Result<bool>(std::unexpect, make_error(
        ErrorCode::InvalidArgument,
        "context reclamation reserve exceeds supported range"));
  }

  const int minimum_ctx = info_.concurrency_auto
      ? std::max(512, std::min(std::max(512, info_.context_size),
                               std::max(512, cfg_.n_batch)))
      : std::max(512, info_.context_size);
  const std::int64_t minimum_pool_wide =
      static_cast<std::int64_t>(minimum_ctx) +
      (cfg_.mtp_enabled ? std::clamp(cfg_.mtp_draft_tokens, 1, 4) : 0);
  if (minimum_pool_wide > std::numeric_limits<int>::max()) {
    return Result<bool>(std::unexpect, make_error(
        ErrorCode::InvalidArgument,
        "context pool minimum exceeds supported range"));
  }
  const int minimum_pool = static_cast<int>(minimum_pool_wide);
  const std::uint32_t old_capacity_raw = llama_n_ctx(shared_ctx_);
  if (old_capacity_raw > static_cast<std::uint32_t>(
          std::numeric_limits<int>::max())) {
    return Result<bool>(std::unexpect, make_error(
        ErrorCode::InvalidArgument,
        "current context capacity exceeds supported range"));
  }
  const int old_capacity = static_cast<int>(old_capacity_raw);
  const int old_sequences = static_cast<int>(slots_.size());
  const int target_sequences = info_.concurrency_auto ? 1 : old_sequences;
  if (old_capacity <= minimum_pool && target_sequences >= old_sequences) {
    return false;
  }

  const Result<std::size_t> before_memory =
      context_pool_device_memory_bytes(shared_ctx_, draft_ctx_);
  if (!before_memory) {
    return Result<bool>(std::unexpect, before_memory.error());
  }
  if (*before_memory == 0) {
    return false;
  }
  if (control.is_cancelled()) {
    return Result<bool>(std::unexpect, make_error(
        ErrorCode::Cancelled, "idle context reclamation cancelled"));
  }
  if (control.is_expired()) {
    return Result<bool>(std::unexpect, make_error(
        ErrorCode::Timeout, "idle context reclamation deadline expired"));
  }

  llama_model_params model_params = llama_model_default_params();
  model_params.load_mode = cfg_.use_mmap
      ? (cfg_.use_mlock ? LLAMA_LOAD_MODE_MMAP_MLOCK : LLAMA_LOAD_MODE_MMAP)
      : (cfg_.use_mlock ? LLAMA_LOAD_MODE_MLOCK : LLAMA_LOAD_MODE_NONE);
  model_params.n_gpu_layers = cfg_.n_gpu_layers.value_or(-1);
  model_params.load_mtp = cfg_.mtp_enabled;

  const int effective_reserve = cfg_.vram_safety_margin_mb + additional_reserve_mb;
  auto preflight_params = shared_context_params_locked(old_capacity);
  preflight_params.n_seq_max = static_cast<std::uint32_t>(target_sequences);
  const Result<int> preflight = fit_context_pool(
      resolved_gguf_path_, model_params, preflight_params,
      cfg_.mtp_enabled, minimum_pool, old_capacity, effective_reserve, control,
      shared_ctx_, draft_ctx_);
  int reclaim_maximum = preflight ? *preflight : old_capacity;
  if (!preflight) {
    if (preflight.error().code == ErrorCode::OutOfMemory) return false;
    if (preflight.error().code != ErrorCode::ResourceBusy) return Result<bool>(std::unexpect, preflight.error());
    if (old_capacity - minimum_pool < 256) return false;
    reclaim_maximum = old_capacity - 256;
    LOG_INFO("context_pool_reclaim_probe_deferred",
        "model={} maximum_capacity={} reason=temporary_state_headroom", info_.name, reclaim_maximum);
  }
  if (control.is_cancelled() || control.is_expired()) {
    return Result<bool>(std::unexpect, make_error(
        control.is_cancelled() ? ErrorCode::Cancelled : ErrorCode::Timeout,
        "idle context reclamation cancelled or expired before recreation"));
  }
  if (reclaim_maximum >= old_capacity && target_sequences >= old_sequences) return false;

  const auto clear_contexts = [&]() {
    pool_capacity_.store(0);
    if (scheduler_) {
      scheduler_->stop();
      scheduler_.reset();
    }
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
    for (int index = 0; index < static_cast<int>(slots_.size()); ++index) {
      slots_[index] = SlotState{};
      slots_[index].sequence_id = index;
    }
  };
  const auto initialize = [&](int maximum_capacity, int reserve_mb,
                              const inferdeck::model::LifecycleControl& operation_control)
      -> Result<void> {
    try {
      return init_shared_context_locked(
          model_params, operation_control, maximum_capacity, reserve_mb);
    } catch (const std::exception& error) {
      return Result<void>(std::unexpect, make_error(
          ErrorCode::Internal,
          std::string("context recreation failed: ") + error.what()));
    } catch (...) {
      return Result<void>(std::unexpect, make_error(
          ErrorCode::Internal, "context recreation failed"));
    }
  };
  const auto recover = [&]() -> Result<void> {
    clear_contexts();
    sequence_capacity_.store(old_sequences);
    inferdeck::model::LifecycleControl recovery_control;
    recovery_control.deadline = inferdeck::model::LifecycleControl::clock::now() +
        std::chrono::seconds(30);
    const Result<void> recovered =
        initialize(old_capacity, cfg_.vram_safety_margin_mb, recovery_control);
    if (!recovered) {
      clear_contexts();
      LOG_ERROR("context_pool_recovery_failed",
                "model={} capacity={} error={}",
                info_.name, old_capacity, recovered.error().message);
    }
    return recovered;
  };

  clear_contexts();
  sequence_capacity_.store(target_sequences);
  const Result<void> resized = initialize(reclaim_maximum, effective_reserve, control);
  if (!resized) {
    LOG_WARN("context_pool_reclaim_fit_failed",
             "model={} capacity={} additional_reserve_mb={} error={}",
             info_.name, old_capacity, additional_reserve_mb,
             resized.error().message);
    const Result<void> recovered = recover();
    if (!recovered) {
      return Result<bool>(std::unexpect, recovered.error());
    }
    if (resized.error().code == ErrorCode::Cancelled ||
        resized.error().code == ErrorCode::Timeout) {
      return Result<bool>(std::unexpect, resized.error());
    }
    return false;
  }

  const int new_capacity = static_cast<int>(llama_n_ctx(shared_ctx_));
  if (new_capacity >= old_capacity && target_sequences >= old_sequences) {
    LOG_INFO("context_pool_reclaim_no_gain",
             "model={} capacity={} additional_reserve_mb={}",
             info_.name, new_capacity, additional_reserve_mb);
    return false;
  }

  const Result<std::size_t> after_memory =
      context_pool_device_memory_bytes(shared_ctx_, draft_ctx_);
  if (!after_memory) {
    const Result<void> recovered = recover();
    if (!recovered) {
      return Result<bool>(std::unexpect, recovered.error());
    }
    return Result<bool>(std::unexpect, after_memory.error());
  }
  constexpr std::size_t mib = 1024ULL * 1024ULL;
  const std::size_t reclaimed_bytes =
      *before_memory > *after_memory ? *before_memory - *after_memory : 0;
  const int reclaimed_mb = static_cast<int>(std::min<std::size_t>(
      reclaimed_bytes / mib,
      static_cast<std::size_t>(std::numeric_limits<int>::max())));
  const int previous_reclaimed = reclaimed_context_vram_mb_.load();
  reclaimed_context_vram_mb_.store(static_cast<int>(
      std::min<std::int64_t>(
          static_cast<std::int64_t>(previous_reclaimed) + reclaimed_mb,
          std::numeric_limits<int>::max())));
  LOG_INFO("context_pool_reclaimed",
           "model={} old_capacity={} new_capacity={} old_sequences={} new_sequences={} reclaimed_device_mb={} additional_reserve_mb={} cache_reset=true",
           info_.name, old_capacity, new_capacity, old_sequences,
           target_sequences, reclaimed_mb, additional_reserve_mb);
  return true;
}

int LlamaCppModel::estimate_vram_mb(int slots) const noexcept {
  const int declared_floor = std::max(0, info_.vram_required_mb);
  int estimate = declared_floor;
  int minimum_estimate = declared_floor;
  if (info_.vram_fixed_mb > 0 && info_.vram_per_slot_mb > 0) {
    const std::int64_t fixed_estimate =
        static_cast<std::int64_t>(info_.vram_fixed_mb) +
        static_cast<std::int64_t>(info_.vram_per_slot_mb) *
            std::max(info_.min_slots, slots);
    estimate = static_cast<int>(std::clamp<std::int64_t>(
        fixed_estimate, 0, std::numeric_limits<int>::max()));
    const std::int64_t minimum_wide =
        static_cast<std::int64_t>(info_.vram_fixed_mb) +
        static_cast<std::int64_t>(info_.vram_per_slot_mb) *
            std::max(0, info_.min_slots);
    minimum_estimate = std::max(
        declared_floor,
        static_cast<int>(std::clamp<std::int64_t>(
            minimum_wide, 0, std::numeric_limits<int>::max())));
  }
  return std::max(
      minimum_estimate, estimate - reclaimed_context_vram_mb_.load());
}

Result<void> LlamaCppModel::resize_slots(int slots) {
  if (info_.concurrency_auto) return Result<void>(std::unexpect,
      make_error(ErrorCode::Unavailable, "automatic sequence capacity is fitted at load time"));
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
  if (shared_ctx_ == nullptr) return 0;
  int free = 0;
  for (const auto& s : slots_) if (!s.busy) ++free;
  return free;
}

bool LlamaCppModel::execution_healthy() const {
  std::lock_guard lock(mtx_);
  return loaded_.load() && shared_ctx_ != nullptr && (!info_.supports("chat_completions") ||
      (scheduler_ && scheduler_->healthy()));
}

Result<int> LlamaCppModel::acquire_slot() {
  std::lock_guard lk(mtx_);
  if (!loaded_.load()) {
    return Result<int>(std::unexpect,
        make_error(ErrorCode::Internal, "model not loaded"));
  }
  if (shared_ctx_ == nullptr || (info_.supports("chat_completions") &&
      (!scheduler_ || !scheduler_->healthy()))) {
    return Result<int>(std::unexpect,
        make_error(ErrorCode::Unavailable, "model execution failed; reload required"));
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
  slot.sequence_bound = false;
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
    s.sequence_bound = false;
    s.last_prompt_tokens.clear();
    s.recurrent_checkpoint.reset();
    s.recurrent_draft_checkpoint.reset();
    s.recurrent_replay_checkpoint.reset();
    s.checkpoint_pos = 0;
    s.mtp_cache_synced = true;
  }
  return Result<void>{};
}
