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
  return load({});
}

Result<void> LlamaCppModel::load(
    const inferdeck::model::LifecycleControl& control) {
  std::lock_guard lk(mtx_);
  if (loaded_.load()) return Result<void>{};
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
