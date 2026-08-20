#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace inferdeck::gateway {

inline constexpr std::string_view kOpenAICompatibilityBaseline =
    "2026-08-20";
inline constexpr std::string_view kControlApiBase =
    "/api/inferdeck/v1";
inline constexpr std::string_view kOpenAIDerivativeBase =
    "/compat/openai-derivative/v1";
inline constexpr std::string_view kAnthropicCompatibilityBase =
    "/compat/anthropic/v1";
inline constexpr std::string_view kDefaultCompatibilityProfile =
    "strict_openai";

enum class StrictOpenAIRoute : std::size_t {
    Models,
    ChatCompletions,
    Responses,
    Embeddings,
    ImageGenerations,
    AudioSpeech,
    AudioTranscriptions,
};

enum class OpenAIDerivativeRoute : std::size_t {
    ChatCompletions,
    Responses,
    Embeddings,
    ImageGenerations,
};

struct RouteManifestEntry {
    std::string_view method;
    std::string_view path;
    std::string_view pattern;
};

inline constexpr std::array<RouteManifestEntry, 7> kStrictOpenAIRoutes{{
    {"GET", "/v1/models", R"(^/v1/models$)"},
    {"POST", "/v1/chat/completions", R"(^/v1/chat/completions$)"},
    {"POST", "/v1/responses", R"(^/v1/responses$)"},
    {"POST", "/v1/embeddings", R"(^/v1/embeddings$)"},
    {"POST", "/v1/images/generations", R"(^/v1/images/generations$)"},
    {"POST", "/v1/audio/speech", R"(^/v1/audio/speech$)"},
    {"POST", "/v1/audio/transcriptions",
     R"(^/v1/audio/transcriptions$)"},
}};

inline constexpr std::array<RouteManifestEntry, 4> kOpenAIDerivativeRoutes{{
    {"POST", "/compat/openai-derivative/v1/chat/completions",
     R"(^/compat/openai-derivative/v1/chat/completions$)"},
    {"POST", "/compat/openai-derivative/v1/responses",
     R"(^/compat/openai-derivative/v1/responses$)"},
    {"POST", "/compat/openai-derivative/v1/embeddings",
     R"(^/compat/openai-derivative/v1/embeddings$)"},
    {"POST", "/compat/openai-derivative/v1/images/generations",
     R"(^/compat/openai-derivative/v1/images/generations$)"},
}};

constexpr const RouteManifestEntry& strict_openai_route(
    StrictOpenAIRoute route) {
    return kStrictOpenAIRoutes[static_cast<std::size_t>(route)];
}

constexpr const RouteManifestEntry& openai_derivative_route(
    OpenAIDerivativeRoute route) {
    return kOpenAIDerivativeRoutes[static_cast<std::size_t>(route)];
}

constexpr bool is_strict_openai_route(std::string_view method,
                                      std::string_view path) {
    for (const auto& route : kStrictOpenAIRoutes) {
        if (route.method == method && route.path == path) return true;
    }
    return false;
}

inline std::string control_api_path(std::string_view suffix) {
    return std::string(kControlApiBase) + std::string(suffix);
}

inline std::string control_api_pattern(std::string_view suffix) {
    return "^" + control_api_path(suffix) + "$";
}

}
