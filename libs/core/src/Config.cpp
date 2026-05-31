#include "core/Config.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace inferdeck::core {

Config::Config() = default;
Config::~Config() = default;

namespace {
    std::string trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\n\r");
        std::string result = s.substr(start, end - start + 1);
        if ((result.front() == '"' && result.back() == '"') ||
            (result.front() == '\'' && result.back() == '\'')) {
            result = result.substr(1, result.size() - 2);
        }
        return result;
    }

    std::string to_lower(const std::string& s) {
        std::string result = s;
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return result;
    }

    Config::FullConfig ParseYaml(const std::string& yaml_content) {
        Config::FullConfig config;
        std::istringstream stream(yaml_content);
        std::string line;
        std::string current_section;

        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            size_t comment_pos = line.find('#');
            if (comment_pos != std::string::npos) {
                line = line.substr(0, comment_pos);
            }
            if (line.empty()) continue;

            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            line = line.substr(start);

            size_t colon_pos = line.find(':');
            if (colon_pos == std::string::npos) continue;

            std::string key = trim(line.substr(0, colon_pos));
            std::string raw_value = trim(line.substr(colon_pos + 1));
            std::string lower_key = to_lower(key);

            if (raw_value.empty() && lower_key == "server") {
                current_section = "server";
                continue;
            } else if (raw_value.empty() && lower_key == "model") {
                current_section = "model";
                continue;
            } else if (raw_value.empty() && lower_key == "whisper") {
                current_section = "whisper";
                continue;
            } else if (raw_value.empty() && lower_key == "gpu") {
                current_section = "gpu";
                continue;
            } else if (raw_value.empty() && lower_key == "queue") {
                current_section = "queue";
                continue;
            } else if (raw_value.empty() && (lower_key == "logging" || lower_key == "log")) {
                current_section = "logging";
                continue;
            } else if (raw_value.empty() && lower_key == "metrics") {
                current_section = "metrics";
                continue;
            }

            if (raw_value.empty()) continue;

            if (current_section == "server") {
                if (lower_key == "host") config.server.host = raw_value;
                else if (lower_key == "dashboardhost") config.server.host = raw_value;
                else if (lower_key == "port" || lower_key == "dashboardport") config.server.port = std::stoi(raw_value);
                else if (lower_key == "apiport" || lower_key == "proxyport") config.server.api_port = std::stoi(raw_value);
                else if (lower_key == "proxyhost" && config.server.host.empty()) config.server.host = raw_value;
                else if (lower_key == "enabled") config.server.tls_enabled = (raw_value == "true" || raw_value == "1");
                else if (lower_key == "cert_file") config.server.cert_file = raw_value;
                else if (lower_key == "key_file") config.server.key_file = raw_value;
                else if (lower_key == "request_timeout_ms") config.server.request_timeout_ms = std::stoi(raw_value);
            } else if (current_section == "model") {
                if (lower_key == "path") config.model.path = raw_value;
                else if (lower_key == "directory") config.model.directory = raw_value;
                else if (lower_key == "precision") config.model.precision = raw_value;
                else if (lower_key == "n_gpu_layers" || lower_key == "gpu_layers") config.model.n_gpu_layers = std::stoi(raw_value);
                else if (lower_key == "context_size" || lower_key == "context_length") config.model.context_size = std::stoi(raw_value);
                else if (lower_key == "batch_size") config.model.batch_size = std::stoi(raw_value);
                else if (lower_key == "flash_attn") config.model.flash_attn = (raw_value == "true" || raw_value == "1");
                else if (lower_key == "cache_type_k") config.model.cache_type_k = raw_value;
                else if (lower_key == "cache_type_v") config.model.cache_type_v = raw_value;
                else if (lower_key == "split_mode") config.model.split_mode = raw_value;
                else if (lower_key == "reasoning_format") config.model.reasoning_format = raw_value;
                else if (lower_key == "fit_target") config.model.fit_target = std::stoi(raw_value);
                else if (lower_key == "parallel") config.model.parallel = std::stoi(raw_value);
                else if (lower_key == "kv_unified") config.model.kv_unified = (raw_value == "true" || raw_value == "1");
                else if (lower_key == "mmproj_path" || lower_key == "mmproj") config.model.mmproj_path = raw_value;
            } else if (current_section == "whisper") {
                if (lower_key == "enabled") config.whisper.enabled = (raw_value == "true" || raw_value == "1");
                else if (lower_key == "executable" || lower_key == "executable_path") config.whisper.executable = raw_value;
                else if (lower_key == "model_directory" || lower_key == "modeldirectory" || lower_key == "directory") config.whisper.model_directory = raw_value;
                else if (lower_key == "model" || lower_key == "default_model") config.whisper.model = raw_value;
                else if (lower_key == "backend") config.whisper.backend = raw_value;
                else if (lower_key == "language" || lower_key == "default_language") config.whisper.language = raw_value;
                else if (lower_key == "task" || lower_key == "default_task") config.whisper.task = raw_value;
                else if (lower_key == "extra_args" || lower_key == "flags") config.whisper.extra_args = raw_value;
            } else if (current_section == "gpu") {
                if (lower_key == "device_id") config.gpu.device_id = std::stoi(raw_value);
            } else if (current_section == "queue") {
                if (lower_key == "worker_threads") config.queue.worker_threads = std::stoi(raw_value);
                else if (lower_key == "max_queue_size") config.queue.max_queue_size = std::stoi(raw_value);
            } else if (current_section == "logging") {
                if (lower_key == "level") config.logging.level = raw_value;
                else if (lower_key == "file") config.logging.file = raw_value;
            } else if (current_section == "metrics") {
                if (lower_key == "enabled") config.metrics_enabled = (raw_value == "true" || raw_value == "1");
                else if (lower_key == "endpoint") config.metrics_endpoint = raw_value;
            }
        }

        return config;
    }
}

Config::FullConfig Config::Load(const std::filesystem::path& config_path) {
    std::ifstream file(config_path);
    if (!file.is_open()) {
        return FullConfig{};
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return ParseYaml(buffer.str());
}

Config::FullConfig Config::LoadFromString(const std::string& yaml_content) {
    return ParseYaml(yaml_content);
}

void Config::Save(const std::filesystem::path& config_path, const FullConfig& config) {
    std::ofstream file(config_path);
    if (!file.is_open()) {
        return;
    }

    file << "# InferDeck Gateway Configuration\n";
    file << "# Generated by InferDeck installer\n\n";

    file << "server:\n";
    file << "  host: \"" << config.server.host << "\"\n";
    file << "  port: " << config.server.port << "\n";
    file << "  request_timeout_ms: " << config.server.request_timeout_ms << "\n";
    file << "  tls:\n";
    file << "    enabled: " << (config.server.tls_enabled ? "true" : "false") << "\n";
    file << "    cert_file: \"" << config.server.cert_file << "\"\n";
    file << "    key_file: \"" << config.server.key_file << "\"\n\n";

    file << "model:\n";
    file << "  path: \"" << config.model.path << "\"\n";
    file << "  precision: \"" << config.model.precision << "\"\n";
    file << "  n_gpu_layers: " << config.model.n_gpu_layers << "\n";
    file << "  context_size: " << config.model.context_size << "\n";
    file << "  batch_size: " << config.model.batch_size << "\n";
    file << "  flash_attn: " << (config.model.flash_attn ? "true" : "false") << "\n";
    file << "  cache_type_k: \"" << config.model.cache_type_k << "\"\n";
    file << "  cache_type_v: \"" << config.model.cache_type_v << "\"\n";
    file << "  split_mode: \"" << config.model.split_mode << "\"\n";
    file << "  fit_target: " << config.model.fit_target << "\n";
    if (!config.model.reasoning_format.empty())
        file << "  reasoning_format: \"" << config.model.reasoning_format << "\"\n";
    file << "  parallel: " << config.model.parallel << "\n";
    file << "  kv_unified: " << (config.model.kv_unified ? "true" : "false") << "\n";
    if (!config.model.mmproj_path.empty())
        file << "  mmproj_path: \"" << config.model.mmproj_path << "\"\n";
    else
        file << "  mmproj_path: \"\"\n";
    file << "\n";

    file << "gpu:\n";
    file << "  device_id: " << config.gpu.device_id << "\n\n";

    file << "queue:\n";
    file << "  worker_threads: " << config.queue.worker_threads << "\n";
    file << "  max_queue_size: " << config.queue.max_queue_size << "\n\n";

    file << "logging:\n";
    file << "  level: \"" << config.logging.level << "\"\n";
    file << "  file: \"" << config.logging.file << "\"\n\n";

    file << "metrics:\n";
    file << "  enabled: " << (config.metrics_enabled ? "true" : "false") << "\n";
    file << "  endpoint: \"" << config.metrics_endpoint << "\"\n";

    file.close();
}

std::string Config::Get(const std::string& key) {
    static Config instance;
    auto it = instance.flat_config_.find(key);
    return it != instance.flat_config_.end() ? it->second : "";
}

int Config::GetInt(const std::string& key, int default_value) {
    static Config instance;
    auto it = instance.flat_config_.find(key);
    if (it == instance.flat_config_.end()) {
        return default_value;
    }
    try {
        return std::stoi(it->second);
    } catch (...) {
        return default_value;
    }
}

bool Config::GetBool(const std::string& key, bool default_value) {
    static Config instance;
    auto it = instance.flat_config_.find(key);
    if (it == instance.flat_config_.end()) {
        return default_value;
    }
    return it->second == "true" || it->second == "1";
}

} // namespace inferdeck::core
