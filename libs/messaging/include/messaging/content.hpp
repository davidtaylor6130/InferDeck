#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace inferdeck::messaging {

struct TextContent {
    std::string text{};
};

struct ImageContent {
    enum class Source { Base64, Url };

    Source source{Source::Base64};
    std::string data{};
    std::string mime_type{"image/png"};
    std::optional<std::string> detail{};
};

struct AudioContent {
    std::string data{};
    std::string mime_type{"audio/wav"};
    std::string format{"wav"};
};

struct ToolCallFunction {
    std::string name{};
    std::string arguments{};
};

struct ToolCallContent {
    std::string id{};
    std::string type{"function"};
    ToolCallFunction function{};
};

struct ToolResultContent {
    std::string tool_call_id{};
    std::string content{};
    bool is_error{false};
};

struct ReasoningContent {
    std::string text{};
    std::optional<std::string> summary{};
};

struct DeveloperContent {
    std::string text{};
};

using Content = std::variant<
    TextContent,
    ImageContent,
    AudioContent,
    ToolCallContent,
    ToolResultContent,
    ReasoningContent,
    DeveloperContent
>;

inline std::size_t index_of(const Content& c) { return c.index(); }

template <typename T>
[[nodiscard]] bool is(const Content& c) noexcept {
    return std::holds_alternative<T>(c);
}

template <typename T>
[[nodiscard]] const T& as(const Content& c) {
    return std::get<T>(c);
}

template <typename T>
[[nodiscard]] T* try_as(Content& c) noexcept {
    return std::get_if<T>(&c);
}

template <typename T>
[[nodiscard]] const T* try_as(const Content& c) noexcept {
    return std::get_if<T>(&c);
}

} // namespace inferdeck::messaging
