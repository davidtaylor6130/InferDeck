Result<void> ModelStore::remove(const std::string& model_name) {
    return retire(model_name, false);
}

Result<void> ModelStore::archive(const std::string& model_name) {
    return retire(model_name, true);
}

Result<void> ModelStore::retire(const std::string& model_name, bool archive_artifact) {
    nlohmann::json entry;
    {
        std::lock_guard lock(mutex_);
        if (!installed_.contains(model_name)) return Err<void>(ErrorCode::NotFound, "installed model not found");
        entry = installed_.at(model_name);
    }
    if (coordinator_.is_loaded(model_name) || coordinator_.active_request_count(model_name) > 0) {
        return Err<void>(ErrorCode::Unavailable, "active or loaded model cannot be archived or deleted");
    }
    const auto info = coordinator_.registry().get_info_result(model_name);
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(root_, error);
    const auto path = std::filesystem::weakly_canonical(entry.value("path", ""), error);
    if (error || !foundation::is_path_within(root, path) ||
        foundation::is_path_within(path, root)) {
        return Err<void>(ErrorCode::InvalidArgument, "installed artifact is outside the model store");
    }
    std::filesystem::path archived_path;
    if (archive_artifact) {
        const auto archive_root = std::filesystem::weakly_canonical(archive_root_, error);
        if (error || archive_root.empty()) {
            return Err<void>(ErrorCode::InvalidArgument, "model archive directory is unavailable");
        }
        const auto archive_directory = archive_root / safe_name(model_name);
        std::filesystem::create_directories(archive_directory, error);
        if (error) return Err<void>(ErrorCode::IoError, error.message());
        archived_path = archive_directory / path.filename();
        if (std::filesystem::exists(archived_path, error)) {
            return Err<void>(ErrorCode::AlreadyExists, "an archived artifact already exists for this model");
        }
    }
    {
        std::lock_guard lock(mutex_);
        installed_.erase(model_name);
    }
    auto saved = save_manifest();
    if (!saved) {
        std::lock_guard lock(mutex_);
        installed_[model_name] = entry;
        return saved;
    }
    const auto unregistered = coordinator_.unregister(model_name);
    if (!unregistered && unregistered.error().code != ErrorCode::NotFound) {
        {
            std::lock_guard lock(mutex_);
            installed_[model_name] = entry;
        }
        (void)save_manifest();
        return unregistered;
    }
    if (archive_artifact) {
        std::filesystem::rename(path, archived_path, error);
    } else if (std::filesystem::is_directory(path, error)) {
        if (std::filesystem::remove_all(path, error) == 0 && !error) {
            error = std::make_error_code(std::errc::io_error);
        }
    } else if (!std::filesystem::remove(path, error) || error) {
        if (!error) error = std::make_error_code(std::errc::io_error);
    }
    if (error) {
        if (info) coordinator_.registry().register_model(*info);
        {
            std::lock_guard lock(mutex_);
            installed_[model_name] = entry;
        }
        (void)save_manifest();
        return Err<void>(ErrorCode::IoError, error.message());
    }
    return Ok();
}

std::vector<StoreDownload> ModelStore::downloads() const {
    std::lock_guard lock(mutex_);
    std::vector<StoreDownload> result;
    result.reserve(downloads_.size());
    for (const auto& [_, download] : downloads_) result.push_back(download);
    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right) { return left.id > right.id; });
    return result;
}

nlohmann::json ModelStore::installed() const {
    std::lock_guard lock(mutex_);
    return installed_;
}

nlohmann::json ModelStore::library() const {
    nlohmann::json result = nlohmann::json::array();
    const auto managed = installed();
    std::unordered_map<std::string, std::string> configured_files;

    auto normalized_path = [](const std::filesystem::path& path) {
        std::error_code error;
        auto normalized = std::filesystem::weakly_canonical(path, error).string();
#ifdef _WIN32
        normalized = lower(std::move(normalized));
#endif
        return error ? std::string{} : normalized;
    };

    for (const auto& name : coordinator_.registry().list()) {
        auto info_result = coordinator_.registry().get_info_result(name);
        if (!info_result) continue;
        const auto& info = *info_result;
        std::vector<std::filesystem::path> paths;
        if (!info.gguf_path.empty()) paths.emplace_back(info.gguf_path);
        if (!info.mmproj_path.empty()) paths.emplace_back(info.mmproj_path);
        for (const auto& [_, value] : info.artifacts) {
            std::error_code error;
            if (!value.empty() && std::filesystem::is_regular_file(value, error)) {
                paths.emplace_back(value);
            }
        }
        std::uint64_t size = 0;
        std::size_t artifact_count = 0;
        std::string display_path;
        std::string quantization{"unknown"};
        for (const auto& path : paths) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error)) continue;
            const auto key = normalized_path(path);
            if (!key.empty()) configured_files[key] = name;
            size += local_file_size(path);
            ++artifact_count;
            if (display_path.empty()) display_path = path.parent_path().string();
            if (path.extension() == ".gguf" && path.filename().string().find("mmproj") == std::string::npos) {
                quantization = infer_quantization(path.filename().string());
            }
        }
        result.push_back({
            {"id", "registered:" + name},
            {"name", name},
            {"family", info.family},
            {"runtime", info.runtime},
            {"modality", info.modality},
            {"capabilities", info.capabilities},
            {"path", display_path.empty() ? info.gguf_path : display_path},
            {"size", size},
            {"vramRequiredMb", info.vram_required_mb},
            {"quantization", quantization},
            {"artifactCount", artifact_count},
            {"hasVision", info.has_vision || !info.mmproj_path.empty()},
            {"configured", true},
            {"managed", managed.contains(name)}
        });
    }

    struct DiskGroup {
        std::filesystem::path directory;
        std::uint64_t size{0};
        std::size_t count{0};
        std::string runtime;
        std::string modality;
        std::string quantization{"unknown"};
        bool has_vision{false};
    };
    std::map<std::string, DiskGroup> groups;
    const auto library_root = lower(root_.filename().string()) == "store"
        ? root_.parent_path()
        : root_;
    std::error_code archive_error;
    const auto normalized_archive_root = std::filesystem::weakly_canonical(archive_root_, archive_error);
    std::error_code scan_error;
    std::size_t visited = 0;
    if (std::filesystem::is_directory(library_root, scan_error)) {
        std::filesystem::recursive_directory_iterator iterator(
            library_root,
            std::filesystem::directory_options::skip_permission_denied,
            scan_error);
        const std::filesystem::recursive_directory_iterator end;
        for (; iterator != end && !scan_error && visited < 4096;
             iterator.increment(scan_error), ++visited) {
            if (!iterator->is_regular_file(scan_error)) continue;
            const auto path = iterator->path();
            if (!archive_error && foundation::is_path_within(normalized_archive_root, path)) {
                if (iterator->is_directory(scan_error)) iterator.disable_recursion_pending();
                continue;
            }
            const auto extension = lower(path.extension().string());
            if (extension != ".gguf" && extension != ".onnx" &&
                extension != ".ort" && extension != ".bin" &&
                extension != ".safetensors") {
                continue;
            }
            const auto normalized = normalized_path(path);
            if (normalized.empty() || configured_files.contains(normalized)) continue;
            const auto directory_key = normalized_path(path.parent_path());
            if (directory_key.empty()) continue;
            auto& group = groups[directory_key];
            group.directory = path.parent_path();
            group.size += local_file_size(path);
            ++group.count;
            const auto searchable = lower(path.string());
            const auto file_runtime = infer_runtime(path.filename().string(), "");
            if (group.runtime.empty() || file_runtime != "llama_cpp") {
                group.runtime = file_runtime;
            }
            group.modality =
                searchable.find("\\stt\\") != std::string::npos ||
                searchable.find("/stt/") != std::string::npos
                    ? "audio_transcription"
                    : searchable.find("\\tts\\") != std::string::npos ||
                      searchable.find("/tts/") != std::string::npos
                        ? "audio_speech"
                        : infer_modality(group.runtime, "");
            const auto quantization = infer_quantization(path.filename().string());
            if (quantization != "unknown") group.quantization = quantization;
            if (searchable.find("mmproj") != std::string::npos) group.has_vision = true;
        }
    }
    for (const auto& [key, group] : groups) {
        std::error_code relative_error;
        const auto relative = std::filesystem::relative(
            group.directory, library_root, relative_error);
        const auto label = relative_error ? group.directory.filename().string() : relative.string();
        result.push_back({
            {"id", "disk:" + key},
            {"name", label},
            {"family", "On-disk artifact"},
            {"runtime", group.runtime},
            {"modality", group.modality},
            {"capabilities", capabilities_for(group.runtime, group.modality)},
            {"path", group.directory.string()},
            {"size", group.size},
            {"vramRequiredMb", static_cast<std::uint64_t>((group.size + 1024 * 1024 - 1) / (1024 * 1024))},
            {"quantization", group.quantization},
            {"artifactCount", group.count},
            {"hasVision", group.has_vision},
            {"configured", false},
            {"managed", false}
        });
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        const bool left_configured = left.value("configured", false);
        const bool right_configured = right.value("configured", false);
        if (left_configured != right_configured) return left_configured > right_configured;
        return left.value("name", "") < right.value("name", "");
    });
    return result;
}

void ModelStore::load_manifest() {
    std::ifstream input(root_ / "installed.json", std::ios::binary);
    if (!input) return;
    try {
        nlohmann::json manifest;
        input >> manifest;
        if (!manifest.is_object()) return;
        std::error_code error;
        const auto root = std::filesystem::weakly_canonical(root_, error);
        for (auto item = manifest.begin(); item != manifest.end(); ++item) {
            const auto path = std::filesystem::weakly_canonical(item.value().value("path", ""), error);
            const bool path_exists = std::filesystem::is_regular_file(path, error) ||
                                     std::filesystem::is_directory(path, error);
            if (error || !path_exists || !foundation::is_path_within(root, path)) continue;
            model::ModelInfo info;
            info.name = item.key();
            info.family = item.value().value("family", "");
            info.runtime = item.value().value("runtime", "llama_cpp");
            info.modality = item.value().value("modality", "text");
            info.capabilities = item.value().value("capabilities", capabilities_for(info.runtime, info.modality));
            if (std::filesystem::is_regular_file(path, error)) info.gguf_path = path.string();
            if (item.value().contains("artifacts") && item.value()["artifacts"].is_object()) {
                for (auto artifact = item.value()["artifacts"].begin();
                     artifact != item.value()["artifacts"].end(); ++artifact) {
                    const auto artifact_path = std::filesystem::weakly_canonical(
                        artifact.value().get<std::string>(), error);
                    if (!error && std::filesystem::is_regular_file(artifact_path, error) &&
                        foundation::is_path_within(path, artifact_path)) {
                        info.artifacts[artifact.key()] = artifact_path.string();
                    }
                }
            }
            if (std::filesystem::is_directory(path, error) && info.artifacts.empty()) continue;
            info.vram_required_mb = item.value().value("vramRequiredMb", 0);
            if (!coordinator_.registry().has(info.name)) coordinator_.registry().register_model(info);
            installed_[item.key()] = item.value();
        }
    } catch (...) {
        installed_ = nlohmann::json::object();
    }
}

Result<void> ModelStore::save_manifest() {
    std::lock_guard manifest_lock(manifest_mutex_);
    nlohmann::json snapshot;
    {
        std::lock_guard lock(mutex_);
        snapshot = installed_;
    }
    const auto temporary = root_ / "installed.json.tmp";
    const auto destination = root_ / "installed.json";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << snapshot.dump(2);
    output.flush();
    if (!output) return Err<void>(ErrorCode::IoError, "cannot write model store manifest");
    output.close();
    return replace_file(temporary, destination);
}
