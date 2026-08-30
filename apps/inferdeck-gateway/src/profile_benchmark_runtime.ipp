inferdeck::llama_wrapper::LlamaCppConfig make_llama_config(
    const inferdeck::gateway::GatewayConfig& cfg,
    const model::ModelInfo& info,
    const inferdeck::optimize::ProfileCandidate* candidate = nullptr) {
    inferdeck::llama_wrapper::LlamaCppConfig result;
    result.n_batch = candidate
        ? candidate->n_batch
        : info.n_batch.value_or(cfg.n_batch);
    result.n_ubatch = candidate
        ? candidate->n_ubatch
        : info.n_ubatch.value_or(cfg.n_ubatch);
    result.use_mmap = cfg.use_mmap;
    result.use_mlock = cfg.use_mlock;
    result.n_gpu_layers =
        info.n_gpu_layers.has_value() ? info.n_gpu_layers : cfg.n_gpu_layers;
    result.flash_attn =
        candidate ? candidate->flash_attention : cfg.flash_attn;
    result.kv_offload = cfg.kv_offload;
    result.op_offload = cfg.op_offload;
    result.cache_type_k =
        candidate
            ? candidate->cache_type_k
            : (info.cache_type_k.empty() ? cfg.cache_type_k : info.cache_type_k);
    result.cache_type_v =
        candidate
            ? candidate->cache_type_v
            : (info.cache_type_v.empty() ? cfg.cache_type_v : info.cache_type_v);
    result.mtp_enabled = info.mtp_enabled;
    result.mtp_draft_tokens = info.mtp_draft_tokens;
    result.mtp_p_min = info.mtp_p_min;
    result.mtp_max_active_requests = candidate
        ? candidate->mtp_max_active_requests
        : info.mtp_max_active_requests;
    result.swa_full = cfg.swa_full;
    result.truncate_prompt = cfg.truncate_prompt;
    result.reasoning_format =
        info.reasoning_format.empty() ? "auto" : info.reasoning_format;
    result.sampling = info.sampling;
    if (!info.chat_template_path.empty()) {
        result.chat_template =
            inferdeck::gateway::read_text_file(info.chat_template_path);
    }
    return result;
}

std::string normalized_answer(std::string text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (const unsigned char character : text) {
        if (std::isalnum(character)) {
            normalized.push_back(
                static_cast<char>(std::tolower(character)));
        }
    }
    return normalized;
}

foundation::Result<inferdeck::gateway::ProfileBenchmarkTrialMetrics>
run_profile_benchmark_trial(
    const inferdeck::gateway::GatewayConfig& cfg,
    observability::GpuTelemetry& gpu,
    const model::ModelInfo& registered,
    const inferdeck::optimize::ProfileCandidate& candidate,
    const std::vector<inferdeck::gateway::ProfileBenchmarkPrompt>& prompts,
    const std::atomic<bool>& cancel,
    const inferdeck::gateway::ProfileBenchmarkProgress& progress) {
    using Metrics = inferdeck::gateway::ProfileBenchmarkTrialMetrics;
    model::ModelInfo info = registered;
    info.context_size = candidate.context_per_slot;
    info.n_slots = candidate.slots;
    info.mtp_max_active_requests = candidate.mtp_max_active_requests;
    auto runtime = std::make_unique<inferdeck::llama_wrapper::LlamaCppModel>(
        info, make_llama_config(cfg, info, &candidate));
    const double baseline_vram = gpu.latest().vram_mb;
    std::atomic<double> peak_vram{baseline_vram};
    const auto sample_vram = [&] {
        const double sample = gpu.latest().vram_mb;
        double current = peak_vram.load();
        while (sample > current &&
               !peak_vram.compare_exchange_weak(current, sample)) {}
    };
    progress("loading", "Loading candidate profile into the selected model");
    const auto load_started = std::chrono::steady_clock::now();
    auto loaded = runtime->load();
    const auto load_finished = std::chrono::steady_clock::now();
    if (!loaded) {
        return foundation::Err<Metrics>(
            loaded.error().code, loaded.error().message);
    }
    const auto unload = [&] {
        (void)runtime->unload();
        for (int sample = 0; sample < 20; ++sample) {
            if (gpu.latest().vram_mb <= baseline_vram + 512.0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    };
    for (int sample = 0; sample < 10 && !cancel.load(); ++sample) {
        sample_vram();
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    if (cancel.load()) {
        unload();
        return foundation::Err<Metrics>(
            foundation::ErrorCode::Cancelled,
            "benchmark cancelled after loading the candidate");
    }

    Metrics measured;
    measured.load_ms =
        std::chrono::duration<double, std::milli>(
            load_finished - load_started).count();
    double total_ttft_ms = 0.0;
    progress("quality", "Running fixed-seed quality and latency probes");
    for (std::size_t index = 0; index < prompts.size(); ++index) {
        if (cancel.load()) {
            unload();
            return foundation::Err<Metrics>(
                foundation::ErrorCode::Cancelled,
                "benchmark cancelled during quality probes");
        }
        auto slot = runtime->acquire_slot();
        if (!slot) {
            unload();
            return foundation::Err<Metrics>(
                slot.error().code, slot.error().message);
        }
        model::InferenceRequest request;
        request.messages = {
            {"system",
             "Answer directly and concisely. <|think_off|>"},
            {"user", prompts[index].prompt},
        };
        request.max_output_tokens = prompts[index].max_tokens;
        request.sampling.temperature = 0.0f;
        request.sampling.top_p = 1.0f;
        request.sampling.top_k = 0;
        request.sampling.repeat_penalty = 1.0f;
        request.sampling.seed = 4242 + static_cast<int>(index);
        const auto started = std::chrono::steady_clock::now();
        std::atomic<bool> first_token{false};
        std::chrono::steady_clock::time_point first_token_at{};
        auto result = runtime->predict_stream(
            *slot, request,
            [&](const model::InferenceDelta& delta) {
                sample_vram();
                if ((!delta.content.empty() ||
                     !delta.reasoning_text.empty()) &&
                    !first_token.exchange(true)) {
                    first_token_at = std::chrono::steady_clock::now();
                }
                return !cancel.load();
            },
            &cancel);
        (void)runtime->release_slot(*slot);
        if (!result) {
            unload();
            return foundation::Err<Metrics>(
                result.error().code, result.error().message);
        }
        const auto finished = std::chrono::steady_clock::now();
        const double ttft = first_token.load()
            ? std::chrono::duration<double, std::milli>(
                  first_token_at - started).count()
            : std::chrono::duration<double, std::milli>(
                  finished - started).count();
        total_ttft_ms += ttft;
        measured.prompt_tokens += result->prompt_tokens;
        measured.completion_tokens += result->completion_tokens;
        ++measured.quality_total;
        const auto& answer = result->text.empty()
            ? result->reasoning_text
            : result->text;
        const auto output = normalized_answer(answer);
        const auto reference = normalized_answer(prompts[index].reference);
        double score = 0.0;
        if (output == reference) {
            score = 1.0;
            ++measured.quality_passes;
        } else if (!reference.empty() &&
                   output.find(reference) != std::string::npos) {
            score = 0.75;
            ++measured.quality_passes;
        }
        measured.quality_score += score;
        measured.output_samples.push_back(
            prompts[index].id + ": " +
            answer.substr(0, std::min<std::size_t>(
                answer.size(), 160)));
    }
    if (measured.quality_total > 0) {
        measured.quality_score /=
            static_cast<double>(measured.quality_total);
        measured.average_time_to_first_token_ms =
            total_ttft_ms / static_cast<double>(measured.quality_total);
    }

    progress("speed", "Measuring sustained single-slot output throughput");
    auto speed_slot = runtime->acquire_slot();
    if (!speed_slot) {
        unload();
        return foundation::Err<Metrics>(
            speed_slot.error().code, speed_slot.error().message);
    }
    model::InferenceRequest speed_request;
    speed_request.messages = {
        {"system", "Answer directly and concisely. <|think_off|>"},
        {"user",
         "Output the lowercase word benchmark exactly 128 times, "
         "separated by single spaces. Do not add punctuation or any "
         "other text."},
    };
    speed_request.max_output_tokens = 192;
    speed_request.sampling.temperature = 0.0f;
    speed_request.sampling.top_p = 1.0f;
    speed_request.sampling.top_k = 0;
    speed_request.sampling.repeat_penalty = 1.0f;
    speed_request.sampling.seed = 7001;
    auto speed_result = runtime->predict_stream(
        *speed_slot, speed_request,
        [&](const model::InferenceDelta&) {
            sample_vram();
            return !cancel.load();
        },
        &cancel);
    (void)runtime->release_slot(*speed_slot);
    if (!speed_result) {
        unload();
        return foundation::Err<Metrics>(
            speed_result.error().code, speed_result.error().message);
    }
    measured.prompt_tokens += speed_result->prompt_tokens;
    measured.completion_tokens += speed_result->completion_tokens;
    measured.prompt_tokens_per_second = speed_result->prompt_duration_ms > 0.0f
        ? static_cast<double>(speed_result->prompt_tokens) * 1000.0 /
            static_cast<double>(speed_result->prompt_duration_ms)
        : 0.0;
    measured.average_tokens_per_second =
        speed_result->tokens_per_second;

    std::vector<int> concurrency_levels;
    if (candidate.slots >= 2) concurrency_levels.push_back(2);
    if (candidate.slots >= 4) {
        concurrency_levels.push_back(4);
    } else if (candidate.slots > 2) {
        concurrency_levels.push_back(candidate.slots);
    }
    if (concurrency_levels.empty()) concurrency_levels.push_back(1);
    for (const int parallel_slots : concurrency_levels) {
        progress(
            "parallelism",
            "Measuring " + std::to_string(parallel_slots) +
                " concurrent requests and MTP drafting");
        const auto parallel_started = std::chrono::steady_clock::now();
        std::vector<std::future<foundation::Result<model::InferenceResult>>> futures;
        futures.reserve(static_cast<std::size_t>(parallel_slots));
        for (int index = 0; index < parallel_slots; ++index) {
            futures.push_back(std::async(
                std::launch::async,
                [&, index, parallel_slots] {
                    auto slot = runtime->acquire_slot();
                    if (!slot) {
                        return foundation::Err<model::InferenceResult>(
                            slot.error().code, slot.error().message);
                    }
                    model::InferenceRequest request;
                    request.messages = {
                        {"system",
                         "Answer directly and concisely. <|think_off|>"},
                        {"user",
                         "Output the lowercase word benchmark exactly 96 times, "
                         "separated by single spaces. Do not add punctuation or "
                         "any other text. Concurrency " +
                             std::to_string(parallel_slots) + " request " +
                             std::to_string(index + 1) + "."},
                    };
                    request.max_output_tokens = 144;
                    request.sampling.temperature = 0.0f;
                    request.sampling.top_p = 1.0f;
                    request.sampling.top_k = 0;
                    request.sampling.repeat_penalty = 1.0f;
                    request.sampling.seed = 9000 + parallel_slots * 10 + index;
                    auto result = runtime->predict_stream(
                        *slot, request,
                        [&](const model::InferenceDelta&) {
                            sample_vram();
                            return !cancel.load();
                        },
                        &cancel);
                    (void)runtime->release_slot(*slot);
                    return result;
                }));
        }
        inferdeck::gateway::ProfileBenchmarkConcurrencyMetrics concurrency;
        concurrency.requests = parallel_slots;
        int parallel_tokens = 0;
        double longest_generation_ms = 0.0;
        double request_tps_total = 0.0;
        for (auto& future : futures) {
            auto result = future.get();
            if (!result) {
                unload();
                return foundation::Err<Metrics>(
                    result.error().code, result.error().message);
            }
            measured.prompt_tokens += result->prompt_tokens;
            measured.completion_tokens += result->completion_tokens;
            parallel_tokens += result->completion_tokens;
            longest_generation_ms = std::max(
                longest_generation_ms,
                static_cast<double>(result->generation_duration_ms));
            request_tps_total += result->tokens_per_second;
            concurrency.mtp_drafted_tokens += result->mtp_drafted_tokens;
            concurrency.mtp_accepted_tokens += result->mtp_accepted_tokens;
            if (result->mtp_drafted_tokens > 0) ++concurrency.mtp_requests;
        }
        const auto parallel_finished = std::chrono::steady_clock::now();
        const double parallel_wall_seconds = std::chrono::duration<double>(
            parallel_finished - parallel_started).count();
        const double parallel_seconds = longest_generation_ms > 0.0
            ? longest_generation_ms / 1000.0
            : parallel_wall_seconds;
        concurrency.aggregate_tokens_per_second = parallel_seconds > 0.0
            ? static_cast<double>(parallel_tokens) / parallel_seconds
            : 0.0;
        concurrency.average_request_tokens_per_second =
            request_tps_total / static_cast<double>(parallel_slots);
        measured.parallel_tokens_per_second =
            concurrency.aggregate_tokens_per_second;
        measured.concurrency.push_back(std::move(concurrency));
    }
    sample_vram();
    measured.peak_vram_mb = peak_vram.load();
    unload();
    return foundation::Ok(std::move(measured));
}

} // namespace
