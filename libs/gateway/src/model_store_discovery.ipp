Result<nlohmann::json> ModelStore::search(const std::string& query,
                                          const std::string& runtime,
                                          const std::string& modality, int limit) {
    if (query.empty() || query.size() > 200 || limit < 1 || limit > 100) {
        return Err<nlohmann::json>(ErrorCode::InvalidArgument, "invalid model search");
    }
    auto response = transport_->get_json(
        "https://huggingface.co/api/models?search=" + encode(query) +
        "&full=true&sort=downloads&direction=-1&limit=" +
        std::to_string(limit), token_);
    if (!response) return Err<nlohmann::json>(response.error().code, response.error().message);
    nlohmann::json results = nlohmann::json::array();
    if (!response->is_array()) return Err<nlohmann::json>(ErrorCode::ParseError, "invalid model source response");
    for (const auto& item : *response) {
        const std::string id = item.value("id", "");
        const std::string pipeline = item.value("pipeline_tag", "");
        if (id.empty()) continue;
        const auto tags = item.value("tags", nlohmann::json::array());
        const std::string searchable = lower(id + " " + tags.dump());
        std::string inferred_runtime = infer_runtime("", pipeline);
        if (lower(pipeline).find("automatic-speech-recognition") != std::string::npos &&
            (searchable.find("onnx") != std::string::npos ||
             searchable.find("sherpa") != std::string::npos)) {
            inferred_runtime = "sherpa_onnx";
        }
        const std::string inferred_modality = infer_modality(inferred_runtime, pipeline);
        if (!runtime.empty() && inferred_runtime != runtime) continue;
        if (!modality.empty() && inferred_modality != modality) continue;
        const auto downloads = item.value("downloads", 0);
        const auto likes = item.value("likes", 0);
        const bool has_vision =
            lower(pipeline).find("image-text") != std::string::npos ||
            lower(pipeline).find("visual-question") != std::string::npos ||
            searchable.find("vision") != std::string::npos ||
            searchable.find("multimodal") != std::string::npos;
        results.push_back({
            {"id", id}, {"pipeline", pipeline}, {"runtime", inferred_runtime},
            {"modality", inferred_modality}, {"downloads", downloads},
            {"likes", likes}, {"private", item.value("private", false)},
            {"gated", item.value("gated", nlohmann::json(false))},
            {"lastModified", item.value("lastModified", "")},
            {"hasVision", has_vision},
            {"recommended", downloads >= 1000 || likes >= 25}
        });
    }
    std::sort(results.begin(), results.end(), [](const auto& left, const auto& right) {
        const auto left_downloads = left.value("downloads", 0LL);
        const auto right_downloads = right.value("downloads", 0LL);
        if (left_downloads != right_downloads) return left_downloads > right_downloads;
        return left.value("likes", 0LL) > right.value("likes", 0LL);
    });
    return Ok(std::move(results));
}

Result<nlohmann::json> ModelStore::inspect(const std::string& repo) {
    if (!valid_repo(repo)) return Err<nlohmann::json>(ErrorCode::InvalidArgument, "invalid repository id");
    auto response = transport_->get_json(
        "https://huggingface.co/api/models/" + repo + "?blobs=true", token_);
    if (!response) return Err<nlohmann::json>(response.error().code, response.error().message);
    const std::string revision = response->value("sha", "main");
    const std::string pipeline = response->value("pipeline_tag", "");
    const auto siblings = response->value("siblings", nlohmann::json::array());
    const bool sherpa_asr_repository =
        lower(pipeline).find("automatic-speech-recognition") != std::string::npos &&
        std::any_of(siblings.begin(), siblings.end(), [](const auto& sibling) {
            const auto name = lower(sibling.value("rfilename", ""));
            return name.ends_with(".onnx") || name.ends_with(".ort");
        });
    nlohmann::json files = nlohmann::json::array();
    for (const auto& sibling : siblings) {
        const std::string filename = sibling.value("rfilename", "");
        const std::string runtime = sherpa_asr_repository
            ? "sherpa_onnx" : infer_runtime(filename, pipeline);
        if (filename.empty() || !compatible_extension(filename, runtime)) continue;
        const auto lfs = sibling.value("lfs", nlohmann::json::object());
        const std::uint64_t size = lfs.value("size", sibling.value("size", std::uint64_t{0}));
        const std::string sha = lfs.value("sha256", lfs.value("oid", ""));
        const std::string modality = infer_modality(runtime, pipeline);
        files.push_back({
            {"repo", repo}, {"revision", revision}, {"name", filename},
            {"size", size}, {"sha256", sha}, {"runtime", runtime},
            {"modality", modality}, {"capabilities", capabilities_for(runtime, modality)},
            {"format", lower(std::filesystem::path(filename).extension().string())},
            {"quantization", infer_quantization(filename)}, {"compatible", size > 0 && sha.size() == 64},
            {"estimatedRamMb", static_cast<std::uint64_t>((size + 1024 * 1024 - 1) / (1024 * 1024))},
            {"estimatedVramMb", static_cast<std::uint64_t>((size + 1024 * 1024 - 1) / (1024 * 1024))}
        });
    }
    std::uint64_t bundle_size = 0;
    std::size_t bundle_count = 0;
    std::unordered_set<std::string> bundle_keys;
    bool bundle_verified = true;
    for (const auto& file : files) {
        if (file.value("runtime", "") != "sherpa_onnx") continue;
        const std::string name = file.value("name", "");
        if (!valid_artifact_path(name)) continue;
        ++bundle_count;
        bundle_size += file.value("size", std::uint64_t{0});
        bundle_keys.insert(artifact_key(name));
        const auto extension = lower(std::filesystem::path(name).extension().string());
        if (infer_modality("sherpa_onnx", pipeline) == "audio_speech" &&
            (extension == ".onnx" || extension == ".ort")) {
            bundle_keys.insert("model");
        }
        bundle_verified = bundle_verified && file.value("compatible", false);
    }
    if (bundle_count > 0) {
        const auto modality = infer_modality("sherpa_onnx", pipeline);
        const auto contains_all = [&bundle_keys](std::initializer_list<const char*> keys) {
            return std::all_of(keys.begin(), keys.end(), [&bundle_keys](const char* key) {
                return bundle_keys.contains(key);
            });
        };
        const bool complete_asr = contains_all({"encoder", "decoder", "joiner", "tokens"});
        const bool supertonic = bundle_keys.contains("duration_predictor") ||
            bundle_keys.contains("text_encoder") || bundle_keys.contains("vector_estimator");
        const bool complete_tts = supertonic
            ? contains_all({"duration_predictor", "text_encoder", "vector_estimator",
                            "vocoder", "tts_json", "unicode_indexer", "voice_style"})
            : contains_all({"model", "tokens"});
        const bool compatible = bundle_verified &&
            (modality == "audio_transcription" ? complete_asr : complete_tts);
        files.push_back({
            {"repo", repo}, {"revision", revision}, {"name", std::string(sherpa_bundle_name)},
            {"size", bundle_size}, {"sha256", ""}, {"runtime", "sherpa_onnx"},
            {"modality", modality},
            {"capabilities", capabilities_for("sherpa_onnx", modality)},
            {"format", "bundle"}, {"quantization", "multi-file"},
            {"compatible", compatible}, {"artifactCount", bundle_count},
            {"estimatedRamMb", static_cast<std::uint64_t>((bundle_size + 1024 * 1024 - 1) / (1024 * 1024))},
            {"estimatedVramMb", 0}
        });
    }
    return Ok(nlohmann::json{{"id", repo}, {"revision", revision},
                              {"pipeline", pipeline}, {"files", std::move(files)}});
}

Result<StoreFile> ModelStore::resolve_file(const std::string& repo,
                                           const std::string& filename,
                                           const std::string& runtime,
                                           const std::string& modality) {
    auto details = inspect(repo);
    if (!details) return Err<StoreFile>(details.error().code, details.error().message);
    for (const auto& file : details->at("files")) {
        if (file.value("name", "") != filename) continue;
        if (file.value("runtime", "") != runtime || file.value("modality", "") != modality) {
            return Err<StoreFile>(ErrorCode::InvalidArgument, "runtime or modality is incompatible with artifact");
        }
        StoreFile result;
        result.repo = repo;
        result.revision = file.value("revision", details->value("revision", "main"));
        result.name = filename;
        result.sha256 = file.value("sha256", "");
        result.size = file.value("size", std::uint64_t{0});
        result.runtime = runtime;
        result.modality = modality;
        result.capabilities = file.value("capabilities", capabilities_for(runtime, modality));
        if (result.size == 0 || result.sha256.size() != 64) {
            return Err<StoreFile>(ErrorCode::InvalidArgument, "artifact has no verifiable size and SHA-256 metadata");
        }
        return Ok(std::move(result));
    }
    return Err<StoreFile>(ErrorCode::NotFound, "compatible artifact not found");
}

Result<std::vector<StoreFile>> ModelStore::resolve_bundle(
    const std::string& repo, const std::string& runtime,
    const std::string& modality) {
    if (runtime != "sherpa_onnx") {
        return Err<std::vector<StoreFile>>(ErrorCode::InvalidArgument,
                                           "only sherpa-onnx repositories use bundle installation");
    }
    auto details = inspect(repo);
    if (!details) return Err<std::vector<StoreFile>>(details.error().code, details.error().message);
    std::vector<StoreFile> artifacts;
    std::unordered_set<std::string> keys;
    for (const auto& file : details->at("files")) {
        const std::string name = file.value("name", "");
        if (name == sherpa_bundle_name || file.value("runtime", "") != runtime ||
            file.value("modality", "") != modality || !file.value("compatible", false) ||
            !valid_artifact_path(name)) {
            continue;
        }
        StoreFile artifact;
        artifact.repo = repo;
        artifact.revision = file.value("revision", details->value("revision", "main"));
        artifact.name = name;
        artifact.sha256 = file.value("sha256", "");
        artifact.size = file.value("size", std::uint64_t{0});
        artifact.runtime = runtime;
        artifact.modality = modality;
        artifact.capabilities = file.value("capabilities", capabilities_for(runtime, modality));
        if (artifact.size == 0 || artifact.sha256.size() != 64) continue;
        keys.insert(artifact_key(name));
        const auto extension = lower(std::filesystem::path(name).extension().string());
        if (modality == "audio_speech" && (extension == ".onnx" || extension == ".ort")) {
            keys.insert("model");
        }
        artifacts.push_back(std::move(artifact));
    }
    const auto contains_all = [&keys](std::initializer_list<const char*> required) {
        return std::all_of(required.begin(), required.end(), [&keys](const char* key) {
            return keys.contains(key);
        });
    };
    const bool supertonic = keys.contains("duration_predictor") ||
        keys.contains("text_encoder") || keys.contains("vector_estimator");
    const bool complete = modality == "audio_transcription"
        ? contains_all({"encoder", "decoder", "joiner", "tokens"})
        : supertonic
            ? contains_all({"duration_predictor", "text_encoder", "vector_estimator",
                            "vocoder", "tts_json", "unicode_indexer", "voice_style"})
            : contains_all({"model", "tokens"});
    if (!complete || artifacts.size() < 2) {
        return Err<std::vector<StoreFile>>(ErrorCode::InvalidArgument,
                                           "sherpa-onnx repository does not expose a complete verified runtime bundle");
    }
    return Ok(std::move(artifacts));
}
