Result<nlohmann::json> ModelStore::search(const std::string& query,
                                          const std::string& runtime,
                                          const std::string& modality, int limit,
                                          const std::string& sort,
                                          bool include_gated) {
    const std::unordered_set<std::string> runtimes = {
        "", "llama_cpp", "stable_diffusion_cpp", "ace_step_cpp",
        "whisper_cpp", "sherpa_onnx"};
    const std::unordered_set<std::string> modalities = {
        "", "text", "embedding", "image", "audio_generation",
        "audio_transcription", "audio_speech"};
    const std::unordered_map<std::string, std::string> sort_fields = {
        {"trending", "trendingScore"}, {"downloads", "downloads"},
        {"likes", "likes"}, {"recent", "lastModified"}};
    const auto sort_field = sort_fields.find(sort);
    if (query.size() > 200 || limit < 1 || limit > 100 ||
        !runtimes.contains(runtime) || !modalities.contains(modality) ||
        sort_field == sort_fields.end()) {
        return Err<nlohmann::json>(ErrorCode::InvalidArgument, "invalid model search");
    }
    std::string url = "https://huggingface.co/api/models?";
    bool first_parameter = true;
    const auto add_parameter = [&url, &first_parameter](
                                   const std::string& key,
                                   const std::string& value) {
        if (!first_parameter) url += '&';
        first_parameter = false;
        url += key + '=' + encode(value);
    };
    std::string effective_query = query;
    if (effective_query.empty() && runtime == "ace_step_cpp") {
        effective_query = "ACE-Step";
    } else if (effective_query.empty() && runtime == "whisper_cpp") {
        effective_query = "whisper ggml";
    } else if (effective_query.empty() && runtime == "sherpa_onnx") {
        effective_query = modality == "audio_speech"
            ? "sherpa onnx text to speech"
            : "sherpa onnx speech recognition";
    }
    if (!effective_query.empty()) add_parameter("search", effective_query);
    if (runtime == "llama_cpp" || runtime == "ace_step_cpp") {
        add_parameter("filter", "gguf");
    } else if (runtime == "stable_diffusion_cpp" || modality == "image") {
        add_parameter("pipeline_tag", "text-to-image");
    } else if (runtime == "whisper_cpp" ||
               modality == "audio_transcription") {
        add_parameter("pipeline_tag", "automatic-speech-recognition");
    } else if (modality == "audio_speech") {
        add_parameter("pipeline_tag", "text-to-speech");
    }
    if (!include_gated) add_parameter("gated", "false");
    add_parameter("full", "true");
    add_parameter("sort", sort_field->second);
    add_parameter("direction", "-1");
    add_parameter("limit", std::to_string(std::min(100, limit * 3)));
    auto response = transport_->get_json(url, token_);
    if (!response) return Err<nlohmann::json>(response.error().code, response.error().message);
    nlohmann::json results = nlohmann::json::array();
    if (!response->is_array()) return Err<nlohmann::json>(ErrorCode::ParseError, "invalid model source response");
    for (const auto& item : *response) {
        if (!item.is_object()) continue;
        const std::string id = catalogue_string(item, "id");
        const std::string pipeline = catalogue_string(item, "pipeline_tag");
        if (id.empty() || catalogue_boolean(item, "private") ||
            (!include_gated && catalogue_item_is_gated(item))) continue;
        const auto tags_value = item.find("tags");
        const nlohmann::json tags =
            tags_value != item.end() && tags_value->is_array()
                ? *tags_value : nlohmann::json::array();
        const std::string searchable = lower(id + " " + tags.dump());
        std::string inferred_runtime = infer_runtime("", pipeline, searchable);
        if (lower(pipeline).find("automatic-speech-recognition") != std::string::npos &&
            (searchable.find("onnx") != std::string::npos ||
             searchable.find("sherpa") != std::string::npos)) {
            inferred_runtime = "sherpa_onnx";
        }
        const std::string inferred_modality = infer_modality(inferred_runtime, pipeline);
        if (!runtime.empty() && inferred_runtime != runtime) continue;
        if (!modality.empty() && inferred_modality != modality) continue;
        const auto siblings_value = item.find("siblings");
        const nlohmann::json siblings =
            siblings_value != item.end() && siblings_value->is_array()
                ? *siblings_value : nlohmann::json::array();
        const auto compatibility = catalogue_compatibility(
            siblings, inferred_runtime, inferred_modality);
        if (!compatibility) continue;
        const auto downloads = catalogue_integer(item, "downloads");
        const auto likes = catalogue_integer(item, "likes");
        const auto trending_score = catalogue_number(item, "trendingScore");
        const std::string last_modified =
            catalogue_string(item, "lastModified");
        const bool has_vision =
            lower(pipeline).find("image-text") != std::string::npos ||
            lower(pipeline).find("visual-question") != std::string::npos ||
            searchable.find("vision") != std::string::npos ||
            searchable.find("multimodal") != std::string::npos;
        results.push_back({
            {"id", id}, {"pipeline", pipeline}, {"runtime", inferred_runtime},
            {"modality", inferred_modality}, {"downloads", downloads},
            {"likes", likes}, {"private", false},
            {"gated", catalogue_item_is_gated(item)},
            {"lastModified", last_modified},
            {"trendingScore", trending_score},
            {"license", catalogue_license(tags)},
            {"format", compatibility->format},
            {"compatibleArtifacts", compatibility->artifacts},
            {"hasVision", has_vision},
            {"recommended", trending_score > 0 || downloads >= 1000 || likes >= 25}
        });
    }
    std::sort(results.begin(), results.end(), [&sort](const auto& left, const auto& right) {
        if (sort == "trending") {
            const auto left_value = left.value("trendingScore", 0.0);
            const auto right_value = right.value("trendingScore", 0.0);
            if (left_value != right_value) return left_value > right_value;
        } else if (sort == "likes") {
            const auto left_value = left.value("likes", 0LL);
            const auto right_value = right.value("likes", 0LL);
            if (left_value != right_value) return left_value > right_value;
        } else if (sort == "recent") {
            const auto left_value = left.value("lastModified", "");
            const auto right_value = right.value("lastModified", "");
            if (left_value != right_value) return left_value > right_value;
        } else {
            const auto left_value = left.value("downloads", 0LL);
            const auto right_value = right.value("downloads", 0LL);
            if (left_value != right_value) return left_value > right_value;
        }
        const auto left_downloads = left.value("downloads", 0LL);
        const auto right_downloads = right.value("downloads", 0LL);
        if (left_downloads != right_downloads) return left_downloads > right_downloads;
        return left.value("likes", 0LL) > right.value("likes", 0LL);
    });
    if (results.size() > static_cast<std::size_t>(limit)) {
        results.erase(results.begin() + limit, results.end());
    }
    return Ok(std::move(results));
}

Result<nlohmann::json> ModelStore::inspect(const std::string& repo) {
    if (!valid_repo(repo)) return Err<nlohmann::json>(ErrorCode::InvalidArgument, "invalid repository id");
    auto response = transport_->get_json(
        "https://huggingface.co/api/models/" + repo + "?blobs=true", token_);
    if (!response) return Err<nlohmann::json>(response.error().code, response.error().message);
    if (!response->is_object()) {
        return Err<nlohmann::json>(
            ErrorCode::ParseError, "invalid model source response");
    }
    std::string revision = catalogue_string(*response, "sha");
    if (revision.empty()) revision = "main";
    const std::string pipeline =
        catalogue_string(*response, "pipeline_tag");
    const auto tags_value = response->find("tags");
    const nlohmann::json tags =
        tags_value != response->end() && tags_value->is_array()
            ? *tags_value : nlohmann::json::array();
    const std::string repository_searchable =
        lower(repo + " " + pipeline + " " + tags.dump());
    const auto siblings_value = response->find("siblings");
    const nlohmann::json siblings =
        siblings_value != response->end() && siblings_value->is_array()
            ? *siblings_value : nlohmann::json::array();
    const bool sherpa_asr_repository =
        lower(pipeline).find("automatic-speech-recognition") != std::string::npos &&
        std::any_of(siblings.begin(), siblings.end(), [](const auto& sibling) {
            const auto name = lower(sibling.value("rfilename", ""));
            return name.ends_with(".onnx") || name.ends_with(".ort");
        });
    nlohmann::json files = nlohmann::json::array();
    for (const auto& sibling : siblings) {
        const std::string filename =
            catalogue_string(sibling, "rfilename");
        const std::string runtime = sherpa_asr_repository
            ? "sherpa_onnx"
            : infer_runtime(filename, pipeline, repository_searchable);
        if (filename.empty() || !compatible_extension(filename, runtime)) continue;
        if ((runtime == "llama_cpp" && !is_primary_llama_artifact(filename)) ||
            (runtime == "stable_diffusion_cpp" &&
             !is_primary_image_artifact(filename)) ||
            (runtime == "whisper_cpp" &&
             !is_primary_whisper_artifact(filename))) {
            continue;
        }
        const auto lfs_value = sibling.find("lfs");
        const nlohmann::json lfs =
            lfs_value != sibling.end() && lfs_value->is_object()
                ? *lfs_value : nlohmann::json::object();
        const std::uint64_t lfs_size = catalogue_unsigned(lfs, "size");
        const std::uint64_t size = lfs_size > 0
            ? lfs_size : catalogue_unsigned(sibling, "size");
        std::string sha = catalogue_string(lfs, "sha256");
        if (sha.empty()) sha = catalogue_string(lfs, "oid");
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
    const auto ace_text_encoder =
        preferred_ace_artifact(files, "text_encoder");
    const auto ace_vae = preferred_ace_artifact(files, "vae");
    std::uint64_t ace_support_size = 0;
    std::size_t ace_support_count = 0;
    for (const auto& file : files) {
        const std::string name = file.value("name", "");
        if ((ace_text_encoder && name == *ace_text_encoder) ||
            (ace_vae && name == *ace_vae)) {
            ace_support_size += file.value("size", std::uint64_t{0});
            ++ace_support_count;
        }
    }
    std::vector<nlohmann::json> ace_dits;
    for (const auto& file : files) {
        const std::string name = file.value("name", "");
        if (file.value("runtime", "") == "ace_step_cpp" &&
            artifact_key(name) == "dit" && valid_artifact_path(name)) {
            ace_dits.push_back(file);
        }
    }
    for (const auto& dit : ace_dits) {
        const std::string dit_name = dit.value("name", "");
        const std::uint64_t ace_variant_size =
            ace_support_size + dit.value("size", std::uint64_t{0});
        files.push_back({
            {"repo", repo}, {"revision", revision},
            {"name", ace_step_bundle_name(dit_name)},
            {"variant", dit_name},
            {"size", ace_variant_size}, {"sha256", ""},
            {"runtime", "ace_step_cpp"}, {"modality", "audio_generation"},
            {"capabilities", capabilities_for("ace_step_cpp", "audio_generation")},
            {"format", "bundle"},
            {"quantization", infer_quantization(dit_name)},
            {"compatible", ace_support_count == 2 &&
                               dit.value("compatible", false)},
            {"artifactCount", ace_support_count + 1},
            {"estimatedRamMb", static_cast<std::uint64_t>(
                (ace_variant_size + 1024 * 1024 - 1) / (1024 * 1024))},
            {"estimatedVramMb", static_cast<std::uint64_t>(
                (ace_variant_size + 1024 * 1024 - 1) / (1024 * 1024))}
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
    const std::string& modality, const std::string& bundle_name) {
    if (runtime != "sherpa_onnx" && runtime != "ace_step_cpp") {
        return Err<std::vector<StoreFile>>(ErrorCode::InvalidArgument,
                                           "runtime does not use bundle installation");
    }
    auto details = inspect(repo);
    if (!details) return Err<std::vector<StoreFile>>(details.error().code, details.error().message);
    const auto selected_ace_dit = runtime == "ace_step_cpp"
        ? ace_step_bundle_dit(bundle_name)
        : std::optional<std::string>{};
    if (runtime == "ace_step_cpp" && !selected_ace_dit) {
        return Err<std::vector<StoreFile>>(
            ErrorCode::InvalidArgument, "invalid ACE-Step bundle selection");
    }
    const auto ace_text_encoder = runtime == "ace_step_cpp"
        ? preferred_ace_artifact(details->at("files"), "text_encoder")
        : std::optional<std::string>{};
    const auto ace_vae = runtime == "ace_step_cpp"
        ? preferred_ace_artifact(details->at("files"), "vae")
        : std::optional<std::string>{};
    const bool exposed_ace_bundle = runtime != "ace_step_cpp" ||
        std::any_of(details->at("files").begin(), details->at("files").end(),
                    [&bundle_name](const auto& file) {
                        return file.value("name", "") == bundle_name &&
                               file.value("compatible", false);
                    });
    if (!exposed_ace_bundle || (runtime == "ace_step_cpp" &&
        (!ace_text_encoder || !ace_vae))) {
        return Err<std::vector<StoreFile>>(
            ErrorCode::InvalidArgument,
            "repository does not expose the selected complete verified runtime bundle");
    }
    std::vector<StoreFile> artifacts;
    std::unordered_set<std::string> keys;
    for (const auto& file : details->at("files")) {
        const std::string name = file.value("name", "");
        if (name == sherpa_bundle_name || is_ace_step_bundle(name) ||
            file.value("runtime", "") != runtime ||
            file.value("modality", "") != modality || !file.value("compatible", false) ||
            !valid_artifact_path(name)) {
            continue;
        }
        if (runtime == "ace_step_cpp" &&
            name != *selected_ace_dit && name != *ace_text_encoder &&
            name != *ace_vae) {
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
        if (modality == "audio_speech" &&
            (extension == ".onnx" || extension == ".ort")) {
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
        keys.contains("vector_estimator");
    const bool complete = runtime == "ace_step_cpp"
        ? modality == "audio_generation" && artifacts.size() == 3 &&
          contains_all({"text_encoder", "dit", "vae"})
        : modality == "audio_transcription"
            ? contains_all({"encoder", "decoder", "joiner", "tokens"})
            : supertonic
                ? contains_all({"duration_predictor", "text_encoder",
                                "vector_estimator", "vocoder", "tts_json",
                                "unicode_indexer", "voice_style"})
                : contains_all({"model", "tokens"});
    if (!complete || artifacts.size() < 2) {
        return Err<std::vector<StoreFile>>(ErrorCode::InvalidArgument,
                                           "repository does not expose a complete verified runtime bundle");
    }
    return Ok(std::move(artifacts));
}
