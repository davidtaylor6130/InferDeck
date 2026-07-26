#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "model/imodel.hpp"

namespace inferdeck::gateway {

class Utf8StreamBuffer {
public:
    std::string on_chunk(std::string_view chunk);
    std::string finish();

    [[nodiscard]] bool empty() const noexcept { return pending_.empty(); }

private:
    std::string pending_;
};

class InferenceDeltaUtf8Buffer {
public:
    model::InferenceDelta on_delta(const model::InferenceDelta& input);
    model::InferenceDelta finish();

private:
    struct ToolCallBuffers {
        Utf8StreamBuffer id;
        Utf8StreamBuffer type;
        Utf8StreamBuffer function_name;
        Utf8StreamBuffer function_arguments;
    };

    Utf8StreamBuffer content_;
    Utf8StreamBuffer reasoning_;
    std::map<std::size_t, ToolCallBuffers> tool_calls_;
};

class StreamingSanitizer {
public:
    StreamingSanitizer();

    std::string on_token(const std::string& token);

    std::string finish();

    [[nodiscard]] std::size_t total_raw() const noexcept { return total_raw_; }
    [[nodiscard]] std::size_t total_clean() const noexcept { return total_clean_; }
    [[nodiscard]] int think_depth() const noexcept { return think_depth_; }
    [[nodiscard]] int channel_depth() const noexcept { return channel_depth_; }

private:
    enum class TagKind { OpenThink, OpenChannel, Remove };

    struct Tag {
        std::string text;
        TagKind kind;
    };

    static const std::vector<Tag>& tags();

    void flush(std::string& out, bool force);

    std::string buffer_;
    int think_depth_{0};
    int channel_depth_{0};
    std::size_t total_raw_{0};
    std::size_t total_clean_{0};
};

} // namespace inferdeck::gateway
