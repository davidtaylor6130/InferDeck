Result<std::uint64_t> ModelStore::quantize(
    const std::string& source_model,
    const std::string& output_model,
    const std::string& quantization,
    int threads) {
    if (source_model.empty() || source_model.size() > 160 ||
        safe_name(source_model) != source_model) {
        return Err<std::uint64_t>(ErrorCode::InvalidArgument,
                                  "invalid source model name");
    }
    if (output_model.empty() || output_model.size() > 160 ||
        safe_name(output_model) != output_model ||
        output_model == source_model) {
        return Err<std::uint64_t>(ErrorCode::InvalidArgument,
                                  "invalid output model name");
    }
    const std::string normalized_quantization = lower(quantization);
    if (normalized_quantization != "q4_k_m" &&
        normalized_quantization != "q5_k_m" &&
        normalized_quantization != "q6_k" &&
        normalized_quantization != "q8_0") {
        return Err<std::uint64_t>(
            ErrorCode::InvalidArgument,
            "quantization must be Q4_K_M, Q5_K_M, Q6_K, or Q8_0");
    }
    if (threads < 0 || threads > 64) {
        return Err<std::uint64_t>(ErrorCode::InvalidArgument,
                                  "threads must be between 0 and 64");
    }
    reap_completed_workers();
    nlohmann::json source_entry;
    {
        std::lock_guard lock(mutex_);
        prune_completed_quantizations_locked();
        if (!installed_.contains(source_model)) {
            return Err<std::uint64_t>(ErrorCode::NotFound,
                                      "managed source model not found");
        }
        const auto active = std::count_if(
            quantizations_.begin(), quantizations_.end(), [](const auto& entry) {
                return entry.second.state == "queued" ||
                       entry.second.state == "quantizing";
            });
        if (active > 0) {
            return Err<std::uint64_t>(ErrorCode::Unavailable,
                                      "a quantization job is already active");
        }
        if (reserved_names_.contains(output_model) ||
            installed_.contains(output_model) ||
            coordinator_.registry().has(output_model)) {
            return Err<std::uint64_t>(
                ErrorCode::AlreadyExists,
                "output model is already installed, registered, or reserved");
        }
        source_entry = installed_.at(source_model);
    }
    if (source_entry.value("runtime", "") != "llama_cpp") {
        return Err<std::uint64_t>(ErrorCode::InvalidArgument,
                                  "only llama.cpp GGUF models can be quantized");
    }
    if (coordinator_.active_request_count() > 0 ||
        coordinator_.queued_request_count() > 0 ||
        coordinator_.swap_in_progress()) {
        return Err<std::uint64_t>(
            ErrorCode::Unavailable,
            "quantization waits until active requests, queued requests, and swaps are zero");
    }
    if (coordinator_.is_loaded(source_model) ||
        coordinator_.active_request_count(source_model) > 0) {
        return Err<std::uint64_t>(ErrorCode::Unavailable,
                                  "the source model must be unloaded");
    }

    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(root_, error);
    const auto source_path = std::filesystem::weakly_canonical(
        source_entry.value("path", ""), error);
    if (error || !std::filesystem::is_regular_file(source_path, error) ||
        lower(source_path.extension().string()) != ".gguf" ||
        !foundation::is_path_within(root, source_path) ||
        foundation::is_path_within(source_path, root)) {
        return Err<std::uint64_t>(
            ErrorCode::InvalidArgument,
            "managed source must be a GGUF file inside the model store");
    }
    const std::uint64_t source_size = local_file_size(source_path);
    const auto space = std::filesystem::space(root_, error);
    if (error || source_size == 0 ||
        space.available < source_size + 64ULL * 1024ULL * 1024ULL) {
        return Err<std::uint64_t>(ErrorCode::Unavailable,
                                  "insufficient disk space for quantization");
    }

    const auto output_directory = root_ / "llama_cpp" / "quantized" /
                                  safe_name(output_model);
    const auto output_path = output_directory /
        (safe_name(output_model) + "-" + normalized_quantization + ".gguf");
    const auto partial_path = std::filesystem::path(output_path.string() + ".partial");
    if (std::filesystem::exists(output_directory, error) ||
        std::filesystem::exists(output_path, error) ||
        std::filesystem::exists(partial_path, error)) {
        return Err<std::uint64_t>(ErrorCode::AlreadyExists,
                                  "quantization output path already exists");
    }
    if (maintenance_resource_) {
        ComputeResource expected = ComputeResource::None;
        if (!maintenance_resource_->compare_exchange_strong(
                expected, ComputeResource::Cpu)) {
            return Err<std::uint64_t>(
                ErrorCode::Unavailable,
                "InferDeck is already running maintenance work");
        }
        quantization_resource_reserved_.store(true, std::memory_order_release);
    }
    if (coordinator_.active_request_count() > 0 ||
        coordinator_.queued_request_count() > 0 ||
        coordinator_.swap_in_progress() ||
        coordinator_.is_loaded(source_model)) {
        release_quantization_resource();
        return Err<std::uint64_t>(
            ErrorCode::Unavailable,
            "quantization waits until active requests, queued requests, and swaps are zero and the source is unloaded");
    }

    std::uint64_t id = 0;
    {
        std::lock_guard lock(mutex_);
        if (!installed_.contains(source_model)) {
            release_quantization_resource();
            return Err<std::uint64_t>(ErrorCode::NotFound,
                                      "managed source model not found");
        }
        const auto active = std::count_if(
            quantizations_.begin(), quantizations_.end(), [](const auto& entry) {
                return entry.second.state == "queued" ||
                       entry.second.state == "quantizing";
            });
        if (active > 0) {
            release_quantization_resource();
            return Err<std::uint64_t>(ErrorCode::Unavailable,
                                      "a quantization job is already active");
        }
        if (reserved_names_.contains(output_model) ||
            installed_.contains(output_model) ||
            coordinator_.registry().has(output_model)) {
            release_quantization_resource();
            return Err<std::uint64_t>(
                ErrorCode::AlreadyExists,
                "output model is already installed, registered, or reserved");
        }
        id = next_id_++;
        StoreQuantization job;
        job.id = id;
        job.source_model = source_model;
        job.output_model = output_model;
        job.quantization = normalized_quantization;
        job.threads = threads;
        job.source_path = source_path.string();
        job.output_path = output_path.string();
        quantizations_[id] = std::move(job);
        reserved_names_[output_model] = id;
    }
    auto started = start_quantization(id);
    if (!started) {
        std::lock_guard lock(mutex_);
        reserved_names_.erase(output_model);
        quantizations_.erase(id);
        release_quantization_resource();
        return Err<std::uint64_t>(started.error().code,
                                  started.error().message);
    }
    return Ok(id);
}

Result<void> ModelStore::start_quantization(std::uint64_t id) {
    reap_completed_workers();
    const auto done = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard lock(mutex_);
    auto job = quantizations_.find(id);
    if (job == quantizations_.end()) {
        return Err<void>(ErrorCode::NotFound, "quantization job not found");
    }
    if (workers_.size() >= kMaxRetainedJobs) {
        return Err<void>(ErrorCode::Unavailable,
                         "model store worker history is full");
    }
    try {
        auto [worker, inserted] = workers_.try_emplace(id);
        if (!inserted) {
            return Err<void>(ErrorCode::AlreadyExists,
                             "quantization worker already exists");
        }
        worker_done_[id] = done;
        worker->second = std::thread([this, id, done] {
            quantization_worker_entry(id, done);
        });
    } catch (const std::exception& error) {
        workers_.erase(id);
        worker_done_.erase(id);
        job->second.state = "failed";
        job->second.error =
            std::string("cannot start quantization worker: ") + error.what();
        reserved_names_.erase(job->second.output_model);
        return Err<void>(ErrorCode::Unavailable,
                         "cannot start quantization worker");
    }
    return Ok();
}

void ModelStore::quantization_worker_entry(
    std::uint64_t id,
    const std::shared_ptr<std::atomic<bool>>& done) noexcept {
    try {
        run_quantization(id);
    } catch (const std::exception& error) {
        fail_quantization(
            id, std::string("quantization failed unexpectedly: ") + error.what());
    } catch (...) {
        fail_quantization(id, "quantization failed unexpectedly");
    }
    done->store(true, std::memory_order_release);
}

void ModelStore::run_quantization(std::uint64_t id) {
    StoreQuantization job;
    nlohmann::json source_entry;
    bool source_missing = false;
    {
        std::lock_guard lock(mutex_);
        job = quantizations_.at(id);
        quantizations_.at(id).state = "quantizing";
        if (!installed_.contains(job.source_model)) {
            source_missing = true;
        } else {
            source_entry = installed_.at(job.source_model);
        }
    }
    if (source_missing) {
        finish_quantization(id, "failed", "managed source model was removed");
        return;
    }
    if (coordinator_.active_request_count() > 0 ||
        coordinator_.queued_request_count() > 0 ||
        coordinator_.swap_in_progress() ||
        coordinator_.is_loaded(job.source_model)) {
        finish_quantization(
            id, "failed",
            "server became busy before quantization started");
        return;
    }
    const auto source_info = coordinator_.registry().get_info_result(job.source_model);
    if (!source_info || source_info->runtime != "llama_cpp") {
        finish_quantization(id, "failed",
                            "source model is no longer registered for llama.cpp");
        return;
    }

    const std::filesystem::path source_path(job.source_path);
    const std::filesystem::path output_path(job.output_path);
    const std::filesystem::path partial_path(output_path.string() + ".partial");
    const auto output_directory = output_path.parent_path();
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(root_, error);
    const auto verified_source = std::filesystem::weakly_canonical(source_path, error);
    if (error || !std::filesystem::is_regular_file(verified_source, error) ||
        !foundation::is_path_within(root, verified_source) ||
        foundation::is_path_within(verified_source, root)) {
        finish_quantization(id, "failed", "managed source model is unavailable");
        return;
    }
    const auto output_ready = create_confined_directory(root, output_directory);
    if (!output_ready) {
        finish_quantization(id, "failed", output_ready.error().message);
        return;
    }
    const auto cleanup_output = [&] {
        std::error_code ignored;
        std::filesystem::remove(partial_path, ignored);
        std::filesystem::remove(output_path, ignored);
        std::filesystem::remove(output_directory, ignored);
    };

    auto quantized = quantizer_->quantize(
        verified_source, partial_path, job.quantization, job.threads);
    if (!quantized) {
        cleanup_output();
        finish_quantization(id, "failed", quantized.error().message);
        return;
    }
    const std::uint64_t output_size = local_file_size(partial_path);
    if (output_size == 0) {
        cleanup_output();
        finish_quantization(id, "failed",
                            "quantizer did not produce a model artifact");
        return;
    }
    const auto checksum = sha256_file(partial_path);
    if (!checksum) {
        cleanup_output();
        finish_quantization(id, "failed", checksum.error().message);
        return;
    }
    const auto finalized = install_new_file(partial_path, output_path);
    if (!finalized) {
        cleanup_output();
        finish_quantization(id, "failed", finalized.error().message);
        return;
    }

    model::ModelInfo output_info = *source_info;
    output_info.name = job.output_model;
    output_info.gguf_path = output_path.string();
    output_info.vram_required_mb = static_cast<int>(
        (output_size + 1024 * 1024 - 1) / (1024 * 1024));
    nlohmann::json output_entry = source_entry;
    output_entry["name"] = output_info.name;
    output_entry["path"] = output_path.string();
    output_entry["size"] = output_size;
    output_entry["sha256"] = *checksum;
    output_entry["vramRequiredMb"] = output_info.vram_required_mb;
    output_entry["quantization"] = job.quantization;
    output_entry["sourceModel"] = job.source_model;
    bool output_name_unavailable = false;
    {
        std::lock_guard lock(mutex_);
        if (installed_.contains(job.output_model) ||
            coordinator_.registry().has(job.output_model)) {
            output_name_unavailable = true;
        } else {
            installed_[job.output_model] = output_entry;
        }
    }
    if (output_name_unavailable) {
        cleanup_output();
        finish_quantization(id, "failed",
                            "output model name became unavailable");
        return;
    }
    const auto saved = save_manifest();
    if (!saved) {
        {
            std::lock_guard lock(mutex_);
            installed_.erase(job.output_model);
        }
        cleanup_output();
        finish_quantization(id, "failed", saved.error().message);
        return;
    }
    try {
        coordinator_.registry().register_model(output_info);
    } catch (const std::exception& registration_error) {
        {
            std::lock_guard lock(mutex_);
            installed_.erase(job.output_model);
        }
        (void)save_manifest();
        cleanup_output();
        finish_quantization(id, "failed", registration_error.what());
        return;
    }
    {
        std::lock_guard lock(mutex_);
        auto& completed = quantizations_.at(id);
        completed.output_size = output_size;
        completed.output_sha256 = *checksum;
    }
    finish_quantization(id, "installed");
}

void ModelStore::fail_quantization(std::uint64_t id,
                                   std::string error) noexcept {
    try {
        finish_quantization(id, "failed", std::move(error));
    } catch (...) {
    }
}

void ModelStore::finish_quantization(
    std::uint64_t id, std::string state, std::string error) {
    release_quantization_resource();
    std::lock_guard lock(mutex_);
    const auto job = quantizations_.find(id);
    if (job == quantizations_.end()) return;
    job->second.state = std::move(state);
    job->second.error = std::move(error);
    const auto reservation = reserved_names_.find(job->second.output_model);
    if (reservation != reserved_names_.end() && reservation->second == id) {
        reserved_names_.erase(reservation);
    }
}

void ModelStore::prune_completed_quantizations_locked() {
    while (quantizations_.size() >= kMaxRetainedJobs) {
        auto oldest = quantizations_.end();
        for (auto job = quantizations_.begin(); job != quantizations_.end(); ++job) {
            const bool terminal = job->second.state == "installed" ||
                                  job->second.state == "failed";
            if (terminal && (oldest == quantizations_.end() ||
                             job->first < oldest->first)) {
                oldest = job;
            }
        }
        if (oldest == quantizations_.end()) return;
        quantizations_.erase(oldest);
    }
}

std::vector<StoreQuantization> ModelStore::quantizations() const {
    std::lock_guard lock(mutex_);
    std::vector<StoreQuantization> result;
    result.reserve(quantizations_.size());
    for (const auto& [_, job] : quantizations_) result.push_back(job);
    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right) {
                  return left.id > right.id;
              });
    return result;
}
