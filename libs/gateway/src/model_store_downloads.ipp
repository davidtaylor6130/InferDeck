Result<std::uint64_t> ModelStore::install(const std::string& repo,
                                          const std::string& filename,
                                          const std::string& runtime,
                                          const std::string& modality,
                                          const std::string& model_name) {
    if (model_name.empty() || model_name.size() > 160 || safe_name(model_name) != model_name) {
        return Err<std::uint64_t>(ErrorCode::InvalidArgument, "invalid model name");
    }
    std::vector<StoreFile> artifacts;
    if (runtime == "sherpa_onnx") {
        if (filename != sherpa_bundle_name) {
            return Err<std::uint64_t>(ErrorCode::InvalidArgument,
                                      "sherpa-onnx models must be installed as a complete bundle");
        }
        auto bundle = resolve_bundle(repo, runtime, modality);
        if (!bundle) return Err<std::uint64_t>(bundle.error().code, bundle.error().message);
        artifacts = std::move(*bundle);
    } else {
        auto file = resolve_file(repo, filename, runtime, modality);
        if (!file) return Err<std::uint64_t>(file.error().code, file.error().message);
        artifacts.push_back(std::move(*file));
    }
    const auto total_size = std::accumulate(
        artifacts.begin(), artifacts.end(), std::uint64_t{0},
        [](std::uint64_t total, const StoreFile& artifact) { return total + artifact.size; });
    const auto space = std::filesystem::space(root_);
    if (space.available < total_size + 64 * 1024 * 1024) {
        return Err<std::uint64_t>(ErrorCode::Unavailable, "insufficient disk space for model artifact");
    }
    reap_completed_workers();
    std::uint64_t id = 0;
    {
        std::lock_guard lock(mutex_);
        prune_completed_jobs_locked();
        const auto active = std::count_if(
            downloads_.begin(), downloads_.end(), [](const auto& entry) {
                return entry.second.state == "queued" || entry.second.state == "downloading";
            });
        if (active >= kMaxActiveInstalls) {
            return Err<std::uint64_t>(
                ErrorCode::Unavailable,
                "model store already has the maximum number of active installs");
        }
        if (downloads_.size() >= kMaxRetainedJobs) {
            return Err<std::uint64_t>(
                ErrorCode::Unavailable,
                "model store job history is full; retry after active installs finish");
        }
        if (reserved_names_.contains(model_name) || installed_.contains(model_name) ||
            coordinator_.registry().has(model_name)) {
            return Err<std::uint64_t>(
                ErrorCode::AlreadyExists,
                "model name is already installed, registered, or being installed");
        }
        id = next_id_++;
        reserved_names_.emplace(model_name, id);
        StoreDownload job;
        job.id = id;
        job.file = artifacts.front();
        if (artifacts.size() > 1) {
            job.file.name = filename;
            job.file.size = total_size;
            job.file.sha256.clear();
        }
        job.artifacts = std::move(artifacts);
        job.model_name = model_name;
        job.bytes_total = total_size;
        downloads_[id] = std::move(job);
        cancellations_[id] = std::make_shared<std::atomic<bool>>(false);
    }
    auto started = start(id);
    if (!started) {
        std::lock_guard lock(mutex_);
        const auto reservation = reserved_names_.find(model_name);
        if (reservation != reserved_names_.end() &&
            reservation->second == id) {
            reserved_names_.erase(reservation);
        }
        downloads_.erase(id);
        cancellations_.erase(id);
        return Err<std::uint64_t>(started.error().code, started.error().message);
    }
    return Ok(id);
}

Result<void> ModelStore::start(std::uint64_t id) {
    reap_completed_workers();
    std::thread previous;
    std::shared_ptr<std::atomic<bool>> done;
    {
        std::lock_guard lock(mutex_);
        auto job = downloads_.find(id);
        if (job == downloads_.end()) return Err<void>(ErrorCode::NotFound, "download not found");
        if (job->second.state == "downloading") return Err<void>(ErrorCode::AlreadyExists, "download is already running");
        const auto active = std::count_if(
            downloads_.begin(), downloads_.end(), [id](const auto& entry) {
                return entry.first != id &&
                       (entry.second.state == "queued" || entry.second.state == "downloading");
            });
        if (active >= kMaxActiveInstalls) {
            return Err<void>(
                ErrorCode::Unavailable,
                "model store already has the maximum number of active installs");
        }
        if (workers_.size() >= kMaxRetainedJobs) {
            return Err<void>(
                ErrorCode::Unavailable,
                "model store worker history is full; retry after installs finish");
        }
        const auto reservation = reserved_names_.find(job->second.model_name);
        if ((reservation != reserved_names_.end() &&
             reservation->second != id) ||
            (reservation == reserved_names_.end() &&
             (installed_.contains(job->second.model_name) ||
              coordinator_.registry().has(job->second.model_name)))) {
            return Err<void>(
                ErrorCode::AlreadyExists,
                "model name is already installed, registered, or being installed");
        }
        if (auto worker = workers_.find(id); worker != workers_.end()) {
            previous = std::move(worker->second);
            workers_.erase(worker);
            worker_done_.erase(id);
        }
        cancellations_[id] = std::make_shared<std::atomic<bool>>(false);
        job->second.state = "queued";
        job->second.error.clear();
        reserved_names_[job->second.model_name] = id;
    }
    if (previous.joinable()) previous.join();
    done = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard lock(mutex_);
        try {
            workers_.emplace(
                id, std::thread([this, id, done] { worker_entry(id, done); }));
            worker_done_[id] = done;
        } catch (const std::exception& error) {
            downloads_.at(id).state = "failed";
            downloads_.at(id).error =
                std::string("cannot start model install worker: ") + error.what();
            const auto reservation =
                reserved_names_.find(downloads_.at(id).model_name);
            if (reservation != reserved_names_.end() &&
                reservation->second == id) {
                reserved_names_.erase(reservation);
            }
            return Err<void>(ErrorCode::Unavailable,
                             "cannot start model install worker");
        }
    }
    return Ok();
}

void ModelStore::worker_entry(
    std::uint64_t id,
    const std::shared_ptr<std::atomic<bool>>& done) noexcept {
    try {
        run(id);
    } catch (const std::exception& error) {
        fail_job(id, std::string("model install failed unexpectedly: ") + error.what());
    } catch (...) {
        fail_job(id, "model install failed unexpectedly");
    }
    done->store(true, std::memory_order_release);
}

void ModelStore::fail_job(std::uint64_t id, std::string error) noexcept {
    try {
        finish_job(id, "failed", std::move(error));
    } catch (...) {
    }
}

void ModelStore::finish_job(std::uint64_t id, std::string state,
                            std::string error) {
    std::lock_guard lock(mutex_);
    const auto job = downloads_.find(id);
    if (job == downloads_.end()) return;
    job->second.state = std::move(state);
    job->second.error = std::move(error);
    const auto reservation = reserved_names_.find(job->second.model_name);
    if (reservation != reserved_names_.end() &&
        reservation->second == id) {
        reserved_names_.erase(reservation);
    }
}

void ModelStore::reap_completed_workers() {
    std::vector<std::thread> completed;
    {
        std::lock_guard lock(mutex_);
        for (auto worker = workers_.begin(); worker != workers_.end();) {
            const auto done = worker_done_.find(worker->first);
            if (done == worker_done_.end() ||
                !done->second->load(std::memory_order_acquire)) {
                ++worker;
                continue;
            }
            completed.push_back(std::move(worker->second));
            worker_done_.erase(worker->first);
            worker = workers_.erase(worker);
        }
    }
    for (auto& worker : completed) {
        if (worker.joinable()) worker.join();
    }
}

void ModelStore::prune_completed_jobs_locked() {
    while (downloads_.size() >= kMaxRetainedJobs) {
        auto oldest = downloads_.end();
        for (auto job = downloads_.begin(); job != downloads_.end(); ++job) {
            const bool terminal = job->second.state == "installed" ||
                                  job->second.state == "failed" ||
                                  job->second.state == "cancelled";
            if (terminal && (oldest == downloads_.end() ||
                             job->first < oldest->first)) {
                oldest = job;
            }
        }
        if (oldest == downloads_.end()) return;
        cancellations_.erase(oldest->first);
        downloads_.erase(oldest);
    }
}

void ModelStore::run(std::uint64_t id) {
    StoreDownload job;
    std::shared_ptr<std::atomic<bool>> cancelled;
    {
        std::lock_guard lock(mutex_);
        job = downloads_.at(id);
        cancelled = cancellations_.at(id);
        downloads_.at(id).state = "downloading";
    }
    if (job.artifacts.size() > 1) {
        run_bundle(id, job, cancelled);
        return;
    }
    const std::filesystem::path directory = root_ / safe_name(job.file.runtime) /
                                            safe_name(job.file.repo);
    std::filesystem::create_directories(directory);
    const std::filesystem::path final_path = directory / safe_name(std::filesystem::path(job.file.name).filename().string());
    const std::filesystem::path partial_path = final_path.string() + ".partial";
    const auto partial_size = local_file_size(partial_path);
    if (partial_size > job.file.size) {
        std::error_code ignored;
        std::filesystem::remove(partial_path, ignored);
        finish_job(id, "failed",
                   "partial download exceeds the validated source size");
        return;
    }
    const std::uint64_t offset = partial_size;
    const std::string url = "https://huggingface.co/" + job.file.repo + "/resolve/" +
                            encode(job.file.revision) + "/" + encode(job.file.name);
    Result<void> downloaded = Ok();
    const auto exceeded_size = std::make_shared<std::atomic<bool>>(false);
    if (offset < job.file.size) {
        downloaded = transport_->download(
            url, token_, partial_path, offset,
            [this, id, cancelled, exceeded_size,
             expected_size = job.file.size](std::uint64_t bytes) {
                if (bytes > expected_size) {
                    exceeded_size->store(true, std::memory_order_release);
                    return false;
                }
                std::lock_guard lock(mutex_);
                downloads_.at(id).bytes_downloaded = bytes;
                return !cancelled->load();
            });
    }
    if (!downloaded) {
        if (exceeded_size->load(std::memory_order_acquire)) {
            std::error_code ignored;
            std::filesystem::remove(partial_path, ignored);
            finish_job(id, "failed",
                       "download exceeds the validated source size");
        } else {
            finish_job(id, cancelled->load() ? "cancelled" : "failed",
                       cancelled->load() ? "cancelled" : downloaded.error().message);
        }
        return;
    }
    if (local_file_size(partial_path) != job.file.size) {
        finish_job(id, "failed",
                   "downloaded size does not match source metadata");
        return;
    }
    auto checksum = sha256_file(partial_path);
    if (!checksum || lower(*checksum) != lower(job.file.sha256)) {
        std::error_code ignored;
        std::filesystem::remove(partial_path, ignored);
        finish_job(
            id, "failed",
            checksum ? "downloaded checksum does not match source metadata"
                     : checksum.error().message);
        return;
    }
    auto finalized = replace_file(partial_path, final_path);
    if (!finalized) {
        finish_job(id, "failed", finalized.error().message);
        return;
    }
    model::ModelInfo info;
    info.name = job.model_name;
    info.family = job.file.repo;
    info.runtime = job.file.runtime;
    info.modality = job.file.modality;
    info.capabilities = job.file.capabilities;
    info.gguf_path = final_path.string();
    info.vram_required_mb = static_cast<int>((job.file.size + 1024 * 1024 - 1) / (1024 * 1024));
    {
        std::lock_guard lock(mutex_);
        installed_[job.model_name] = {
            {"name", info.name}, {"family", info.family}, {"runtime", info.runtime},
            {"modality", info.modality}, {"capabilities", info.capabilities},
            {"path", final_path.string()}, {"size", job.file.size},
            {"sha256", job.file.sha256}, {"vramRequiredMb", info.vram_required_mb}
        };
        downloads_.at(id).bytes_downloaded = job.file.size;
        downloads_.at(id).installed_path = final_path.string();
    }
    auto saved = save_manifest();
    if (!saved) {
        std::error_code ignored;
        std::filesystem::remove(final_path, ignored);
        {
            std::lock_guard lock(mutex_);
            installed_.erase(job.model_name);
        }
        finish_job(id, "failed", saved.error().message);
        return;
    }
    try {
        coordinator_.registry().register_model(info);
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(final_path, ignored);
        {
            std::lock_guard lock(mutex_);
            installed_.erase(job.model_name);
        }
        (void)save_manifest();
        finish_job(id, "failed", error.what());
        return;
    }
    finish_job(id, "installed");
}

void ModelStore::run_bundle(
    std::uint64_t id, const StoreDownload& job,
    const std::shared_ptr<std::atomic<bool>>& cancelled) {
    const auto directory = root_ / safe_name(job.file.runtime) / safe_name(job.file.repo);
    const auto staging = directory / (safe_name(job.model_name) + ".bundle.partial");
    const auto destination = directory / safe_name(job.model_name);
    std::error_code error;
    std::filesystem::create_directories(staging, error);
    if (error || std::filesystem::exists(destination, error)) {
        finish_job(id, "failed", error ? error.message() : "model bundle destination already exists");
        return;
    }
    std::uint64_t completed_bytes = 0;
    for (const auto& artifact : job.artifacts) {
        if (cancelled->load()) {
            finish_job(id, "cancelled", "cancelled");
            return;
        }
        const auto relative = std::filesystem::path(artifact.name).lexically_normal();
        const auto target = (staging / relative).lexically_normal();
        if (!foundation::is_path_within(staging.lexically_normal(), target)) {
            std::filesystem::remove_all(staging, error);
            finish_job(id, "failed", "bundle artifact path escapes the staging directory");
            return;
        }
        std::filesystem::create_directories(target.parent_path(), error);
        if (error) {
            finish_job(id, "failed", error.message());
            return;
        }
        auto valid_existing = false;
        if (local_file_size(target) == artifact.size) {
            const auto checksum = sha256_file(target);
            valid_existing = checksum && lower(*checksum) == lower(artifact.sha256);
        }
        if (!valid_existing) {
            std::filesystem::remove(target, error);
            const auto partial = std::filesystem::path(target.string() + ".partial");
            auto offset = local_file_size(partial);
            if (offset > artifact.size) {
                std::filesystem::remove(partial, error);
                offset = 0;
            }
            const auto url = "https://huggingface.co/" + artifact.repo + "/resolve/" +
                             encode(artifact.revision) + "/" + encode_path(artifact.name);
            const auto downloaded = transport_->download(
                url, token_, partial, offset,
                [this, id, cancelled, completed_bytes, expected = artifact.size](std::uint64_t bytes) {
                    if (bytes > expected) return false;
                    std::lock_guard lock(mutex_);
                    downloads_.at(id).bytes_downloaded = completed_bytes + bytes;
                    return !cancelled->load();
                });
            if (!downloaded) {
                finish_job(id, cancelled->load() ? "cancelled" : "failed",
                           cancelled->load() ? "cancelled" : downloaded.error().message);
                return;
            }
            const auto checksum = sha256_file(partial);
            if (local_file_size(partial) != artifact.size || !checksum ||
                lower(*checksum) != lower(artifact.sha256)) {
                std::filesystem::remove_all(staging, error);
                finish_job(id, "failed", "bundle artifact failed size or checksum validation");
                return;
            }
            const auto finalized = replace_file(partial, target);
            if (!finalized) {
                finish_job(id, "failed", finalized.error().message);
                return;
            }
        }
        completed_bytes += artifact.size;
        {
            std::lock_guard lock(mutex_);
            downloads_.at(id).bytes_downloaded = completed_bytes;
        }
    }
    std::filesystem::rename(staging, destination, error);
    if (error) {
        finish_job(id, "failed", error.message());
        return;
    }
    model::ModelInfo info;
    info.name = job.model_name;
    info.family = job.file.repo;
    info.runtime = job.file.runtime;
    info.modality = job.file.modality;
    info.capabilities = job.file.capabilities;
    nlohmann::json artifact_manifest = nlohmann::json::object();
    for (const auto& artifact : job.artifacts) {
        auto key = artifact_key(artifact.name);
        const auto extension = lower(std::filesystem::path(artifact.name).extension().string());
        if (job.file.modality == "audio_speech" && !info.artifacts.contains("model") &&
            (extension == ".onnx" || extension == ".ort") &&
            key != "duration_predictor" && key != "text_encoder" &&
            key != "vector_estimator" && key != "vocoder") {
            key = "model";
        }
        for (std::size_t suffix = 2; info.artifacts.contains(key); ++suffix) {
            key = artifact_key(artifact.name) + "_" + std::to_string(suffix);
        }
        const auto path = destination / std::filesystem::path(artifact.name).lexically_normal();
        info.artifacts[key] = path.string();
        artifact_manifest[key] = path.string();
    }
    {
        std::lock_guard lock(mutex_);
        installed_[job.model_name] = {
            {"name", info.name}, {"family", info.family}, {"runtime", info.runtime},
            {"modality", info.modality}, {"capabilities", info.capabilities},
            {"path", destination.string()}, {"size", job.file.size},
            {"artifacts", artifact_manifest}, {"artifactCount", job.artifacts.size()},
            {"vramRequiredMb", 0}
        };
        downloads_.at(id).installed_path = destination.string();
    }
    const auto saved = save_manifest();
    if (!saved) {
        std::filesystem::remove_all(destination, error);
        {
            std::lock_guard lock(mutex_);
            installed_.erase(job.model_name);
        }
        finish_job(id, "failed", saved.error().message);
        return;
    }
    try {
        coordinator_.registry().register_model(info);
    } catch (const std::exception& registration_error) {
        std::filesystem::remove_all(destination, error);
        {
            std::lock_guard lock(mutex_);
            installed_.erase(job.model_name);
        }
        (void)save_manifest();
        finish_job(id, "failed", registration_error.what());
        return;
    }
    finish_job(id, "installed");
}

Result<void> ModelStore::cancel(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    const auto cancel = cancellations_.find(id);
    if (cancel == cancellations_.end()) return Err<void>(ErrorCode::NotFound, "download not found");
    cancel->second->store(true);
    return Ok();
}

Result<void> ModelStore::resume(std::uint64_t id) {
    reap_completed_workers();
    {
        std::lock_guard lock(mutex_);
        const auto job = downloads_.find(id);
        if (job == downloads_.end()) return Err<void>(ErrorCode::NotFound, "download not found");
        if (job->second.state != "cancelled" && job->second.state != "failed") {
            return Err<void>(ErrorCode::InvalidArgument, "only cancelled or failed downloads can resume");
        }
    }
    return start(id);
}
