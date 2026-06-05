#pragma once

#include <string>
#include <utility>
#include <vector>

namespace inferdeck::gateway {

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
