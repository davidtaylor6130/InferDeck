#include "WhisperRuntime.hpp"

#include "core/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace inferdeck::gateway {
namespace {

std::string Trim(std::string value) {
    auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last) return "";
    return std::string(first, last);
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "";
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

int RunCommand(const std::string& command) {
#ifdef _WIN32
    return std::system(("cmd.exe /S /C \"" + command + "\"").c_str());
#else
    return std::system(command.c_str());
#endif
}

} // namespace

WhisperRuntime& WhisperRuntime::Get() {
    static WhisperRuntime runtime;
    return runtime;
}

void WhisperRuntime::Configure(const ServerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    configured_ = true;
    current_model_ = ResolveModelPath(config_.whisper_model);
    if (current_model_.empty()) current_model_ = config_.whisper_model;
    running_ = IsConfiguredLocked();
    last_error_.clear();
}

bool WhisperRuntime::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = IsConfiguredLocked();
    if (!running_) last_error_ = "Whisper executable or model is not configured.";
    return running_;
}

void WhisperRuntime::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
}

bool WhisperRuntime::Restart() {
    Stop();
    return Start();
}

bool WhisperRuntime::LoadModel(const std::string& model) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto resolved = ResolveModelPath(model);
    if (resolved.empty()) {
        last_error_ = "No Whisper model matched '" + model + "'.";
        return false;
    }
    current_model_ = resolved;
    running_ = IsConfiguredLocked();
    return true;
}

std::size_t WhisperRuntime::RescanModels() {
    return ModelsJson().size();
}

WhisperResult WhisperRuntime::Transcribe(const std::string& audio_path, const nlohmann::json& params, bool translate) {
    auto start = std::chrono::steady_clock::now();
    std::string executable;
    std::string model;
    std::string language;
    std::string extra_args;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++queued_;
        if (!running_ || !IsConfiguredLocked()) {
            ++failed_;
            --queued_;
            return {false, "", "", current_model_, "whisper_not_ready", "Whisper runtime is not configured or started.", 0.0};
        }
        --queued_;
        ++running_jobs_;
        executable = config_.whisper_executable;
        model = current_model_.empty() ? ResolveModelPath(config_.whisper_model) : current_model_;
        language = params.value("language", config_.whisper_language);
        extra_args = config_.whisper_extra_args;
    }

    WhisperResult result;
    result.language = language.empty() || language == "auto" ? "" : language;
    result.model = model;

    auto prefix = std::filesystem::temp_directory_path() /
        ("inferdeck-whisper-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string command = Quote(executable) + " -m " + Quote(model) + " -f " + Quote(audio_path) + " -nt -np -otxt -of " + Quote(prefix.string());
    if (!language.empty() && language != "auto") command += " -l " + language;
    if (translate || params.value("task", std::string{}) == "translate") command += " -tr";
    if (!extra_args.empty()) command += " " + extra_args;

    auto exit_code = RunCommand(CommandForRuntimeDirectory(executable, command));
    result.text = Trim(ReadFile(prefix.string() + ".txt"));
    std::error_code ec;
    std::filesystem::remove(prefix.string() + ".txt", ec);

    auto end = std::chrono::steady_clock::now();
    result.duration_seconds = std::chrono::duration<double>(end - start).count();
    result.ok = exit_code == 0 && !result.text.empty();
    if (!result.ok) {
        result.error_code = "transcription_failed";
        result.error_message = "whisper.cpp did not produce transcription text.";
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_jobs_ > 0) --running_jobs_;
        if (result.ok) {
            ++completed_;
            last_text_ = result.text;
            last_duration_seconds_ = result.duration_seconds;
            last_error_.clear();
        } else {
            ++failed_;
            last_duration_seconds_ = result.duration_seconds;
            last_error_ = result.error_message;
        }
    }
    return result;
}

nlohmann::json WhisperRuntime::StatusJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        {"id", "whisper"},
        {"name", "Whisper"},
        {"kind", "whisper_cpp"},
        {"status", running_ ? "running" : (config_.whisper_enabled ? "stopped" : "not_configured")},
        {"managed", true},
        {"pid", nullptr},
        {"backend", config_.whisper_backend},
        {"executable", config_.whisper_executable},
        {"modelDirectory", config_.whisper_model_directory},
        {"currentModel", current_model_},
        {"language", config_.whisper_language},
        {"task", config_.whisper_task},
        {"activity", ActivityJsonLocked()},
        {"lastError", last_error_.empty() ? nlohmann::json(nullptr) : nlohmann::json(last_error_)}
    };
}

nlohmann::json WhisperRuntime::ActivityJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ActivityJsonLocked();
}

nlohmann::json WhisperRuntime::ActivityJsonLocked() const {
    return {
        {"queued", queued_},
        {"running", running_jobs_},
        {"completed", completed_},
        {"failed", failed_},
        {"lastText", last_text_},
        {"lastDurationSeconds", last_duration_seconds_}
    };
}

nlohmann::json WhisperRuntime::ModelsJson() const {
    std::vector<std::filesystem::path> files;
    if (!config_.whisper_model.empty()) {
        auto resolved = ResolveModelPath(config_.whisper_model);
        if (!resolved.empty()) files.emplace_back(resolved);
    }
    if (!config_.whisper_model_directory.empty() && std::filesystem::exists(config_.whisper_model_directory)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(config_.whisper_model_directory)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".bin" || ext == ".gguf" || ext == ".ggml") files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());

    nlohmann::json models = nlohmann::json::array();
    for (const auto& path : files) {
        models.push_back({
            {"id", NormalizeModelId(path.stem().string())},
            {"name", path.filename().string()},
            {"path", path.string()},
            {"loaded", path.string() == current_model_},
            {"details", {{"backend", "whisper.cpp"}, {"format", path.extension().string()}}}
        });
    }
    return models;
}

std::string WhisperRuntime::ResolveModelPath(const std::string& requested) const {
    if (requested.empty()) return "";
    std::filesystem::path direct(requested);
    if (std::filesystem::exists(direct)) return direct.string();
    if (config_.whisper_model_directory.empty() || !std::filesystem::exists(config_.whisper_model_directory)) return "";
    auto wanted = NormalizeModelId(requested);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(config_.whisper_model_directory)) {
        if (!entry.is_regular_file()) continue;
        auto stem = NormalizeModelId(entry.path().stem().string());
        auto filename = NormalizeModelId(entry.path().filename().string());
        if (stem == wanted || filename == wanted) return entry.path().string();
    }
    return "";
}

std::string WhisperRuntime::NormalizeModelId(const std::string& value) const {
    std::string out = value;
    std::replace(out.begin(), out.end(), '_', '-');
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string WhisperRuntime::Quote(const std::string& value) const {
    std::string escaped = value;
    std::string::size_type pos = 0;
    while ((pos = escaped.find('"', pos)) != std::string::npos) {
        escaped.insert(pos, "\\");
        pos += 2;
    }
    return "\"" + escaped + "\"";
}

std::string WhisperRuntime::CommandForRuntimeDirectory(const std::string& executable, const std::string& command) const {
    auto runtime_dir = std::filesystem::path(executable).parent_path().string();
    if (runtime_dir.empty()) return command;
#ifdef _WIN32
    return "cd /d " + Quote(runtime_dir) + " && " + command;
#else
    return "cd " + Quote(runtime_dir) + " && " + command;
#endif
}

bool WhisperRuntime::IsConfiguredLocked() const {
    return config_.whisper_enabled &&
        !config_.whisper_executable.empty() &&
        !current_model_.empty() &&
        std::filesystem::exists(config_.whisper_executable) &&
        std::filesystem::exists(current_model_);
}

} // namespace inferdeck::gateway
