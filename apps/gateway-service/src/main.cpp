/// @file main.cpp
/// @brief InferDeck gateway service entry point.
///
/// Loads configuration, initializes the LlamaEngine, and starts the
/// HTTPS server with OpenAI-compatible endpoints.

#include <iostream>
#include <string>
#include <filesystem>
#include <signal.h>

#include "Server.hpp"
#include "config/ConfigLoader.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "core/Logger.hpp"
#include "core/Config.hpp"

// Route handlers
#include "routes/ChatCompletions.hpp"
#include "routes/Completions.hpp"
#include "routes/Models.hpp"
#include "routes/Embeddings.hpp"
#include "routes/Health.hpp"
#include "routes/Metrics.hpp"
#include "routes/AudioTranscriptions.hpp"
#include "routes/AudioSpeech.hpp"
#include "routes/Images.hpp"
#include "routes/Documents.hpp"
#include "routes/FineTuningJobs.hpp"

// Global server instance for signal handling
static inferdeck::gateway::GatewayServer* g_server = nullptr;

/// Signal handler for graceful shutdown.
void SignalHandler(int signum) {
    if (g_server) {
        std::cerr << "\nReceived signal " << signum << ", shutting down...\n";
        g_server->Stop();
    }
    exit(signum == SIGINT ? 0 : 1);
}

/// Print usage information.
void PrintUsage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "\nOptions:\n"
              << "  -c, --config <path>   Path to gateway.yml (default: config/gateway.yml)\n"
              << "  -h, --help            Show this help message\n"
              << "  -v, --version         Show version\n"
              << std::endl;
}

/// Print version information.
void PrintVersion() {
    std::cout << "InferDeck Gateway v1.0.0\n"
              << "Built with C++23, Vulkan, and llama.cpp\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    // Parse command-line arguments
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

    // Initialize logger
    inferdeck::core::Logger::Get().Initialize(
        inferdeck::core::LogLevel::Info,
        "logs/gateway.log",
        true
    );
    inferdeck::core::Logger::Get().Info("InferDeck Gateway starting...");

    // Load configuration
    try {
        inferdeck::gateway::ServerConfig server_config = inferdeck::gateway::LoadConfig(config_path);

        // Initialize LlamaEngine
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

    } catch (const std::exception& e) {
        inferdeck::core::Logger::Get().Error("Failed to load configuration: " + std::string(e.what()));
        return 1;
    }

    // Create and start server
    inferdeck::gateway::GatewayServer server;
    g_server = &server;

    // Set up signal handlers
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // Register routes
    server.RegisterRoute("POST", "/v1/chat/completions",
        [](const httplib::Request& req, httplib::Response& resp) {
            if (req.get_header_value("Connection") == "upgrade" ||
                req.get_header_value("Upgrade") == "websocket") {
                // SSE streaming
                inferdeck::gateway::routes::HandleChatCompletionsStream(req, resp);
            } else {
                // Non-streaming
                inferdeck::gateway::routes::HandleChatCompletions(req, resp);
            }
        });

    server.RegisterRoute("POST", "/v1/completions",
        [](const httplib::Request& req, httplib::Response& resp) {
            if (req.get_header_value("Connection") == "upgrade") {
                inferdeck::gateway::routes::HandleCompletionsStream(req, resp);
            } else {
                inferdeck::gateway::routes::HandleCompletions(req, resp);
            }
        });

    server.RegisterRoute("GET", "/v1/models",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleModels(req, resp);
        });

    server.RegisterRoute("POST", "/v1/embeddings",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleEmbeddings(req, resp);
        });

    server.RegisterRoute("GET", "/v1/health",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleHealth(req, resp);
        });

    server.RegisterRoute("GET", "/inferdeck/metrics",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleMetrics(req, resp);
        });

    server.RegisterRoute("GET", "/inferdeck/status",
        [](const httplib::Request& req, httplib::Response& resp) {
            // Alias for /v1/health
            inferdeck::gateway::routes::HandleHealth(req, resp);
        });

    // Audio transcription (STT)
    server.RegisterRoute("POST", "/v1/audio/transcriptions",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleAudioTranscriptions(req, resp);
        });

    // Audio translation (STT)
    server.RegisterRoute("POST", "/v1/audio/translations",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleAudioTranslations(req, resp);
        });

    // Audio speech (TTS)
    server.RegisterRoute("POST", "/v1/audio/speech",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleAudioSpeech(req, resp);
        });

    // Image generation
    server.RegisterRoute("POST", "/v1/images/generate",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleImageGenerate(req, resp);
        });

    // Document CRUD
    server.RegisterRoute("GET", "/v1/documents",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDocumentsList(req, resp);
        });

    server.RegisterRoute("POST", "/v1/documents",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDocumentsCreate(req, resp);
        });

    server.RegisterRoute("GET", "/v1/documents/search",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleDocumentsSearch(req, resp);
        });

    // Fine-tuning jobs
    server.RegisterRoute("GET", "/v1/fine_tuning/jobs",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleFineTuningJobsList(req, resp);
        });

    server.RegisterRoute("POST", "/v1/fine_tuning/jobs",
        [](const httplib::Request& req, httplib::Response& resp) {
            inferdeck::gateway::routes::HandleFineTuningJobsCreate(req, resp);
        });

    // Start server
    if (!server.Start()) {
        inferdeck::core::Logger::Get().Error("Failed to start server");
        return 1;
    }

    inferdeck::core::Logger::Get().Info("Server started on " + server.GetBaseUrl());

    // Wait for shutdown
    server.WaitForReady();

    inferdeck::core::Logger::Get().Info("Server stopped");
    return 0;
}
