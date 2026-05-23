#include <iostream>
#include <string>
#include <filesystem>
#include <signal.h>

#include "Server.hpp"
#include "config/ConfigLoader.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "core/Logger.hpp"
#include "core/Config.hpp"
#include "WhisperRuntime.hpp"

#include "routes/ChatCompletions.hpp"
#include "routes/Completions.hpp"
#include "routes/Models.hpp"
#include "routes/Embeddings.hpp"
#include "routes/Health.hpp"
#include "routes/AudioTranscriptions.hpp"
#include "routes/AudioSpeech.hpp"
#include "routes/Images.hpp"
#include "routes/Metrics.hpp"
#include "routes/Documents.hpp"
#include "routes/FineTuningJobs.hpp"
#include "routes/OllamaCompat.hpp"
#include "routes/Dashboard.hpp"

static inferdeck::gateway::GatewayServer* g_server = nullptr;

void SignalHandler(int signum) {
    if (g_server) {
        std::cerr << "\nReceived signal " << signum << ", shutting down...\n";
        g_server->Stop();
    }
    exit(signum == SIGINT ? 0 : 1);
}

void PrintUsage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "\nOptions:\n"
              << "  -c, --config <path>   Path to gateway.yml (default: config/gateway.yml)\n"
              << "  -h, --help            Show this help message\n"
              << "  -v, --version         Show version\n"
              << std::endl;
}

void PrintVersion() {
    std::cout << "InferDeck Gateway v1.0.0\n"
              << "Built with C++23, Vulkan, and llama.cpp\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    std::filesystem::path config_path = inferdeck::gateway::GetDefaultConfigPath();

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            PrintVersion();
            return 0;
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                config_path = argv[i + 1];
                i++;
            } else {
                std::cerr << "Error: --config requires a path argument\n";
                return 1;
            }
        }
    }

    inferdeck::core::Logger::Get().Initialize(
        inferdeck::core::LogLevel::Info,
        "logs/gateway.log",
        true
    );
    inferdeck::core::Logger::Get().Info("InferDeck Gateway starting...");

    inferdeck::gateway::ServerConfig server_config;
    try {
        server_config = inferdeck::gateway::LoadConfig(config_path);

        auto& engine = inferdeck::core::LlamaEngine::Get();
        bool engine_ok = engine.Initialize(
            server_config.model_path,
            server_config.precision,
            server_config.n_gpu_layers,
            server_config.context_size
        );

        if (!engine_ok) {
            inferdeck::core::Logger::Get().Error("Failed to initialize LlamaEngine");
            return 1;
        }

        inferdeck::core::Logger::Get().Info("LlamaEngine initialized successfully");
        inferdeck::core::Logger::Get().Info("Model: " + engine.GetModelName());
        inferdeck::core::Logger::Get().Info("Precision: " + engine.GetPrecision());
        inferdeck::gateway::WhisperRuntime::Get().Configure(server_config);

    } catch (const std::exception& e) {
        inferdeck::core::Logger::Get().Error("Failed to load configuration: " + std::string(e.what()));
        return 1;
    }

    inferdeck::gateway::GatewayServer server;
    g_server = &server;
    auto gateway_started_at = std::chrono::steady_clock::now();

    if (!server.Initialize(server_config)) {
        inferdeck::core::Logger::Get().Error("Failed to initialize server");
        return 1;
    }

    // Mount dashboard static files on port 8080
    std::filesystem::path public_dir = std::filesystem::current_path() / "apps" / "gateway-service" / "public" / "dashboard";
    if (std::filesystem::exists(public_dir)) {
        server.SetDashboardMountPoint("/", public_dir.string());
    } else {
        inferdeck::core::Logger::Get().Warn("Dashboard directory not found: " + public_dir.string());
    }

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // ============================================================
    // API SERVER (port 11434) - OpenAI-compatible API ONLY
    // ============================================================

    server.RegisterApiRoute("POST", "/v1/chat/completions",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleChatCompletions(req, resp);
        });

    server.RegisterApiRoute("POST", "/v1/completions",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleCompletions(req, resp);
        });

    server.RegisterApiRoute("GET", "/v1/models",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleModels(req, resp);
        });

    server.RegisterApiRoute("POST", "/v1/embeddings",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleEmbeddings(req, resp);
        });

    server.RegisterApiRoute("POST", "/v1/audio/transcriptions",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleAudioTranscriptions(req, resp);
        });

    server.RegisterApiRoute("POST", "/v1/audio/translations",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleAudioTranslations(req, resp);
        });

    server.RegisterApiRoute("POST", "/v1/audio/speech",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleAudioSpeech(req, resp);
        });

    server.RegisterApiRoute("POST", "/v1/images/generate",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleImageGenerate(req, resp);
        });

    server.RegisterApiRoute("GET", "/health",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleHealth(req, resp);
        });

    server.RegisterApiRoute("GET", "/v1/health",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleHealth(req, resp);
        });

    server.RegisterApiRoute("GET", "/api/version",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleOllamaVersion(req, resp);
        });

    server.RegisterApiRoute("GET", "/api/tags",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleOllamaTags(req, resp);
        });

    server.RegisterApiRoute("POST", "/api/chat",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleOllamaChat(req, resp);
        });

    // ============================================================
    // DASHBOARD SERVER (port 8080) - Dashboard + Custom Routes
    // ============================================================

    server.RegisterDashboardRoute("GET", "/inferdeck/metrics",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleMetrics(req, resp);
        });

    server.RegisterDashboardRoute("GET", "/inferdeck/status",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleHealth(req, resp);
        });

    server.RegisterDashboardRoute("GET", "/api/health",
        [server_config, gateway_started_at](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardHealth(req, resp, server_config, gateway_started_at);
        });

    server.RegisterDashboardRoute("GET", "/api/status",
        [server_config, gateway_started_at](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardStatus(req, resp, server_config, gateway_started_at);
        });

    server.RegisterDashboardRoute("GET", "/api/models",
        [server_config](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardModels(req, resp, server_config);
        });

    server.RegisterDashboardRoute("GET", "/api/models/running",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardRunningModels(req, resp);
        });

    server.RegisterDashboardRoute("POST", "/api/models/load",
        [server_config](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardLoadModel(req, resp, server_config);
        });

    server.RegisterDashboardRoute("POST", "/api/models/unload",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardUnloadModel(req, resp);
        });

    server.RegisterDashboardRoute("POST", "/api/models/rescan",
        [server_config](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardRescanModels(req, resp, server_config);
        });

    server.RegisterDashboardRoute("GET", "/api/services",
        [server_config, gateway_started_at](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardServices(req, resp, server_config, gateway_started_at);
        });

    server.RegisterDashboardRoute("GET", "/api/whisper/status",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardWhisperStatus(req, resp);
        });

    server.RegisterDashboardRoute("POST", "/api/whisper/start",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardWhisperStart(req, resp);
        });

    server.RegisterDashboardRoute("POST", "/api/whisper/stop",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardWhisperStop(req, resp);
        });

    server.RegisterDashboardRoute("POST", "/api/whisper/restart",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardWhisperRestart(req, resp);
        });

    server.RegisterDashboardRoute("POST", "/api/whisper/load",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardWhisperLoadModel(req, resp);
        });

    server.RegisterDashboardRoute("POST", "/api/whisper/rescan",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardWhisperRescan(req, resp);
        });

    server.RegisterDashboardRoute("POST", R"(/api/services/([^/]+)/start)",
        [server_config](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardStartService(req, resp, server_config);
        });

    server.RegisterDashboardRoute("POST", R"(/api/services/([^/]+)/stop)",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardStopService(req, resp);
        });

    server.RegisterDashboardRoute("POST", R"(/api/services/([^/]+)/restart)",
        [server_config](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardRestartService(req, resp, server_config);
        });

    server.RegisterDashboardRoute("GET", "/api/jobs",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardJobs(req, resp);
        });

    server.RegisterDashboardRoute("GET", R"(/api/jobs/([^/]+))",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardJobDetail(req, resp);
        });

    server.RegisterDashboardRoute("GET", R"(/api/jobs/([^/]+)/events)",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardJobEvents(req, resp);
        });

    server.RegisterDashboardRoute("GET", R"(/api/jobs/([^/]+)/result)",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardJobResult(req, resp);
        });

    server.RegisterDashboardRoute("POST", R"(/api/jobs/([^/]+)/cancel)",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardCancelJob(req, resp);
        });

    server.RegisterDashboardRoute("POST", R"(/api/jobs/([^/]+)/retry)",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardRetryJob(req, resp);
        });

    server.RegisterDashboardRoute("POST", "/api/queue/pause",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardQueueAction(req, resp, "pause");
        });

    server.RegisterDashboardRoute("POST", "/api/queue/resume",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardQueueAction(req, resp, "resume");
        });

    server.RegisterDashboardRoute("POST", "/api/queue/clear-failed",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardQueueAction(req, resp, "clear-failed");
        });

    server.RegisterDashboardRoute("GET", "/api/logs",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardLogs(req, resp);
        });

    server.RegisterDashboardRoute("GET", "/api/events/stream",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDashboardEvents(req, resp);
        });

    server.RegisterDashboardRoute("GET", "/v1/documents",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDocumentsList(req, resp);
        });

    server.RegisterDashboardRoute("POST", "/v1/documents",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDocumentsCreate(req, resp);
        });

    server.RegisterDashboardRoute("GET", "/v1/documents/search",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDocumentsSearch(req, resp);
        });

    server.RegisterDashboardRoute("GET", "/v1/fine_tuning/jobs",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleFineTuningJobsList(req, resp);
        });

    server.RegisterDashboardRoute("POST", "/v1/fine_tuning/jobs",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleFineTuningJobsCreate(req, resp);
        });

    if (!server.Start()) {
        inferdeck::core::Logger::Get().Error("Failed to start server");
        return 1;
    }

    inferdeck::core::Logger::Get().Info("Dashboard: " + server.GetDashboardUrl());
    inferdeck::core::Logger::Get().Info("API: " + server.GetApiUrl());

    server.WaitForReady();

    inferdeck::core::Logger::Get().Info("Server stopped");
    return 0;
}
