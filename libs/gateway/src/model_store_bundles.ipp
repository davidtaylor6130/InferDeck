namespace {

struct BundleComponent {
    std::string key;
    std::string repo;
    std::string file;
    std::string revision;
    bool required{true};
};

struct BundleRecipe {
    std::string id;
    std::string name;
    std::string family;
    std::string runtime;
    std::string modality;
    std::string license;
    std::string license_url;
    std::vector<std::string> capabilities;
    std::map<std::string, std::string> settings;
    int vram_required_mb{0};
    std::vector<BundleComponent> components;
};

bool valid_bundle_key(const std::string& key) {
    return !key.empty() && key.size() <= 64 &&
           std::all_of(key.begin(), key.end(), [](unsigned char c) {
               return std::islower(c) || std::isdigit(c) || c == '_';
           });
}

bool valid_bundle_revision(const std::string& revision) {
    return revision.size() <= 100 &&
           std::all_of(revision.begin(), revision.end(), [](unsigned char c) {
               return std::isalnum(c) || c == '-' || c == '_' || c == '.';
           });
}

bool valid_bundle_file(const std::string& file) {
    const auto path = std::filesystem::path(file);
    return file.size() <= 300 && file.find(':') == std::string::npos &&
           file.find('\\') == std::string::npos && !path.has_root_name() &&
           !path.has_root_directory() && valid_artifact_path(file);
}

Result<std::vector<BundleRecipe>> parse_bundle_recipes(const nlohmann::json& document) {
    using Recipes = std::vector<BundleRecipe>;
    if (!document.is_object() || !document.contains("bundles") ||
        !document["bundles"].is_array()) {
        return Err<Recipes>(ErrorCode::ParseError, "bundle file must be an object with a bundles array");
    }
    const std::unordered_set<std::string> runtimes = {
        "llama_cpp", "stable_diffusion_cpp", "ace_step_cpp",
        "whisper_cpp", "sherpa_onnx", "ltx_video_cpp"};
    Recipes recipes;
    std::unordered_set<std::string> ids;
    for (const auto& item : document["bundles"]) {
        BundleRecipe recipe;
        recipe.id = catalogue_string(item, "id");
        const auto fail = [&recipe](const std::string& message) {
            return Err<Recipes>(ErrorCode::ParseError,
                                "bundle " + (recipe.id.empty() ? std::string("<no id>") : recipe.id) +
                                    ": " + message);
        };
        if (!item.is_object()) return fail("must be an object");
        if (!valid_model_name(recipe.id) || !ids.insert(lower(recipe.id)).second) {
            return fail("id must be a unique safe name");
        }
        recipe.name = catalogue_string(item, "name");
        recipe.family = catalogue_string(item, "family");
        recipe.runtime = catalogue_string(item, "runtime");
        recipe.modality = catalogue_string(item, "modality");
        recipe.license = catalogue_string(item, "license");
        recipe.license_url = catalogue_string(item, "licenseUrl");
        if (recipe.name.empty()) recipe.name = recipe.id;
        if (recipe.family.empty()) recipe.family = recipe.id;
        if (!runtimes.contains(recipe.runtime)) return fail("unsupported runtime");
        if (recipe.modality.empty()) return fail("modality is required");
        if (recipe.license.empty()) return fail("license is required");
        const auto vram = catalogue_integer(item, "vramRequiredMb");
        if (vram < 0 || vram > 1024 * 1024) return fail("vramRequiredMb is out of range");
        recipe.vram_required_mb = static_cast<int>(vram);
        if (item.contains("capabilities")) {
            if (!item["capabilities"].is_array() || item["capabilities"].empty()) {
                return fail("capabilities must be a non-empty array");
            }
            for (const auto& capability : item["capabilities"]) {
                if (!capability.is_string()) return fail("capabilities must be strings");
                recipe.capabilities.push_back(capability.get<std::string>());
            }
        } else {
            recipe.capabilities = capabilities_for(recipe.runtime, recipe.modality);
        }
        if (item.contains("artifacts")) {
            if (!item["artifacts"].is_object()) return fail("artifacts must be an object");
            for (const auto& [key, value] : item["artifacts"].items()) {
                if (!valid_bundle_key(key) || !value.is_string()) {
                    return fail("artifact " + key + " must be a lower_snake_case key with a string value");
                }
                recipe.settings[key] = value.get<std::string>();
            }
        }
        if (!item.contains("components") || !item["components"].is_array()) {
            return fail("components must be an array");
        }
        std::unordered_set<std::string> keys;
        std::unordered_set<std::string> files;
        for (const auto& entry : item["components"]) {
            BundleComponent component;
            component.key = catalogue_string(entry, "key");
            component.repo = catalogue_string(entry, "repo");
            component.file = catalogue_string(entry, "file");
            component.revision = catalogue_string(entry, "revision");
            if (entry.is_object() && entry.contains("required")) {
                if (!entry["required"].is_boolean()) return fail("component required must be a boolean");
                component.required = entry["required"].get<bool>();
            }
            const std::string label = "component " + component.key;
            if (!valid_bundle_key(component.key) || recipe.settings.contains(component.key) ||
                !keys.insert(component.key).second) {
                return fail(label + " needs a unique lower_snake_case key");
            }
            if (!valid_repo(component.repo)) return fail(label + " has an invalid repo");
            if (!valid_bundle_file(component.file)) return fail(label + " file must be a relative path inside the repo");
            if (!compatible_extension(component.file, recipe.runtime)) {
                return fail(label + " file type is not supported by " + recipe.runtime);
            }
            if (!valid_bundle_revision(component.revision)) return fail(label + " has an invalid revision");
            if (!files.insert(lower(std::filesystem::path(component.file).lexically_normal().generic_string())).second) {
                return fail(label + " file collides with another component");
            }
            recipe.components.push_back(std::move(component));
        }
        if (std::none_of(recipe.components.begin(), recipe.components.end(),
                         [](const BundleComponent& component) { return component.required; })) {
            return fail("at least one required component is needed");
        }
        recipes.push_back(std::move(recipe));
    }
    return Ok(std::move(recipes));
}

Result<std::vector<BundleRecipe>> read_bundle_recipes(const std::filesystem::path& path) {
    std::error_code error;
    if (path.empty() || !std::filesystem::exists(path, error)) return Ok(std::vector<BundleRecipe>{});
    std::ifstream input(path, std::ios::binary);
    if (!input) return Err<std::vector<BundleRecipe>>(ErrorCode::IoError, "cannot read model bundle file");
    try {
        return parse_bundle_recipes(nlohmann::json::parse(input));
    } catch (const std::exception& parse_error) {
        return Err<std::vector<BundleRecipe>>(
            ErrorCode::ParseError, std::string("model bundle file is not valid JSON: ") + parse_error.what());
    }
}

}

void ModelStore::set_bundle_recipes(std::filesystem::path path) {
    std::lock_guard lock(mutex_);
    bundle_recipes_path_ = std::move(path);
}

Result<nlohmann::json> ModelStore::bundles() const {
    std::filesystem::path path;
    {
        std::lock_guard lock(mutex_);
        path = bundle_recipes_path_;
    }
    auto recipes = read_bundle_recipes(path);
    if (!recipes) return Err<nlohmann::json>(recipes.error().code, recipes.error().message);
    nlohmann::json result = nlohmann::json::array();
    for (const auto& recipe : *recipes) {
        nlohmann::json components = nlohmann::json::array();
        for (const auto& component : recipe.components) {
            components.push_back({
                {"key", component.key}, {"repo", component.repo}, {"file", component.file},
                {"revision", component.revision.empty() ? "main" : component.revision},
                {"required", component.required}
            });
        }
        result.push_back({
            {"id", recipe.id}, {"name", recipe.name}, {"family", recipe.family},
            {"runtime", recipe.runtime}, {"modality", recipe.modality},
            {"capabilities", recipe.capabilities}, {"license", recipe.license},
            {"licenseUrl", recipe.license_url}, {"vramRequiredMb", recipe.vram_required_mb},
            {"artifacts", recipe.settings}, {"format", "bundle"},
            {"artifactCount", recipe.components.size()}, {"components", std::move(components)}
        });
    }
    return Ok(std::move(result));
}

Result<std::uint64_t> ModelStore::install_bundle(const std::string& bundle_id,
                                                 const std::string& model_name) {
    if (!valid_model_name(model_name)) {
        return Err<std::uint64_t>(ErrorCode::InvalidArgument, "invalid model name");
    }
    std::filesystem::path path;
    {
        std::lock_guard lock(mutex_);
        path = bundle_recipes_path_;
    }
    auto recipes = read_bundle_recipes(path);
    if (!recipes) return Err<std::uint64_t>(recipes.error().code, recipes.error().message);
    const auto recipe = std::find_if(recipes->begin(), recipes->end(),
                                     [&bundle_id](const BundleRecipe& item) { return item.id == bundle_id; });
    if (recipe == recipes->end()) return Err<std::uint64_t>(ErrorCode::NotFound, "model bundle not found");
    std::unordered_map<std::string, nlohmann::json> listings;
    std::vector<StoreFile> artifacts;
    for (const auto& component : recipe->components) {
        const std::string listing_key = component.repo + "@" + component.revision;
        if (!listings.contains(listing_key)) {
            const std::string url = "https://huggingface.co/api/models/" + component.repo +
                (component.revision.empty() ? "" : "/revision/" + encode(component.revision)) + "?blobs=true";
            auto response = transport_->get_json(url, token_);
            if (!response) return Err<std::uint64_t>(response.error().code, response.error().message);
            if (!response->is_object()) {
                return Err<std::uint64_t>(ErrorCode::ParseError, "invalid model source response");
            }
            listings.emplace(listing_key, std::move(*response));
        }
        const auto& listing = listings.at(listing_key);
        const auto siblings = listing.find("siblings");
        const nlohmann::json* sibling = nullptr;
        if (siblings != listing.end() && siblings->is_array()) {
            for (const auto& candidate : *siblings) {
                if (catalogue_string(candidate, "rfilename") == component.file) sibling = &candidate;
            }
        }
        if (!sibling) {
            if (!component.required) continue;
            return Err<std::uint64_t>(ErrorCode::NotFound, "bundle component " + component.key + " (" +
                                                               component.repo + "/" + component.file + ") not found");
        }
        const auto lfs_value = sibling->find("lfs");
        const nlohmann::json lfs = lfs_value != sibling->end() && lfs_value->is_object()
            ? *lfs_value : nlohmann::json::object();
        StoreFile artifact;
        artifact.repo = component.repo;
        artifact.revision = catalogue_string(listing, "sha");
        if (artifact.revision.empty()) artifact.revision = component.revision.empty() ? "main" : component.revision;
        artifact.name = component.file;
        artifact.size = catalogue_unsigned(lfs, "size");
        artifact.sha256 = catalogue_string(lfs, "sha256");
        if (artifact.sha256.empty()) artifact.sha256 = catalogue_string(lfs, "oid");
        artifact.runtime = recipe->runtime;
        artifact.modality = recipe->modality;
        artifact.capabilities = recipe->capabilities;
        artifact.artifact_key = component.key;
        if (artifact.size == 0 || artifact.sha256.size() != 64) {
            return Err<std::uint64_t>(ErrorCode::InvalidArgument,
                                      "bundle component " + component.key +
                                          " has no verifiable size and SHA-256 metadata");
        }
        artifacts.push_back(std::move(artifact));
    }
    StoreBundle bundle{recipe->id, recipe->family, recipe->settings, recipe->vram_required_mb};
    return enqueue(std::move(artifacts), recipe->id, model_name, std::move(bundle));
}
