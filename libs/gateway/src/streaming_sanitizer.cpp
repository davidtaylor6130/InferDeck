#include "gateway/streaming_sanitizer.hpp"

#include <algorithm>
#include <utility>

namespace inferdeck::gateway {

const std::vector<StreamingSanitizer::Tag>& StreamingSanitizer::tags() {
    static const std::vector<Tag> k_tags = {
        {"<think>", TagKind::OpenThink},
        {"<|channel|>analysis<|message|>", TagKind::OpenChannel},
        {"<|im_start|>assistant", TagKind::Remove},
        {"<|im_start|>system", TagKind::Remove},
        {"<|im_start|>user", TagKind::Remove},
        {"<|im_start|>", TagKind::Remove},
        {"<|channel|>final<|message|>", TagKind::Remove},
        {"<|channel|>commentary<|message|>", TagKind::Remove},
        {"<|start|>assistant", TagKind::Remove},
        {"<|start|>system", TagKind::Remove},
        {"<|start|>user", TagKind::Remove},
        {"<|start|>", TagKind::Remove},
        {"<|channel|>analysis", TagKind::Remove},
        {"<|channel|>final", TagKind::Remove},
        {"<|channel|>commentary", TagKind::Remove},
        {"<|channel|>", TagKind::Remove},
        {"<|message|>", TagKind::Remove},
        {"<|end|>", TagKind::Remove},
    };
    return k_tags;
}

StreamingSanitizer::StreamingSanitizer() = default;

void StreamingSanitizer::flush(std::string& out, bool force) {
    int i = 0;
    const int n = static_cast<int>(buffer_.size());
    const auto& k_tags = tags();

    while (i < n) {
        if (think_depth_ > 0) {
            auto pos = buffer_.find("</think>", static_cast<std::size_t>(i));
            if (pos == std::string::npos) {
                if (!force) return;
                i = n;
                continue;
            }
            think_depth_--;
            i = static_cast<int>(pos) + 8;
            continue;
        }
        if (channel_depth_ > 0) {
            auto pos = buffer_.find("<|end|>", static_cast<std::size_t>(i));
            if (pos == std::string::npos) {
                if (!force) return;
                i = n;
                continue;
            }
            channel_depth_--;
            i = static_cast<int>(pos) + 7;
            continue;
        }

        auto lt = buffer_.find('<', static_cast<std::size_t>(i));
        if (lt == std::string::npos) {
            out.append(buffer_, static_cast<std::size_t>(i), std::string::npos);
            i = n;
            break;
        }

        if (static_cast<int>(lt) > i) {
            out.append(buffer_, static_cast<std::size_t>(i),
                       static_cast<std::size_t>(lt) - static_cast<std::size_t>(i));
        }
        i = static_cast<int>(lt);

        bool matched = false;
        for (const auto& tag : k_tags) {
            const int tlen = static_cast<int>(tag.text.size());
            if (n - i >= tlen &&
                buffer_.compare(static_cast<std::size_t>(i),
                                static_cast<std::size_t>(tlen), tag.text) == 0) {
                if (tag.kind == TagKind::OpenThink) ++think_depth_;
                else if (tag.kind == TagKind::OpenChannel) ++channel_depth_;
                i += tlen;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        bool is_prefix = false;
        for (const auto& tag : k_tags) {
            const int tlen = static_cast<int>(tag.text.size());
            const int cmp_len = std::min(tlen, n - i);
            if (cmp_len > 0 &&
                buffer_.compare(static_cast<std::size_t>(i),
                                static_cast<std::size_t>(cmp_len), tag.text, 0,
                                static_cast<std::size_t>(cmp_len)) == 0) {
                is_prefix = true;
                break;
            }
        }
        if (is_prefix && !force) {
            break;
        }
        out.push_back('<');
        ++i;
    }
    if (i > 0) {
        buffer_.erase(0, static_cast<std::size_t>(i));
    }
}

std::string StreamingSanitizer::on_token(const std::string& token) {
    total_raw_ += token.size();
    buffer_ += token;
    std::string out;
    flush(out, false);
    total_clean_ += out.size();
    return out;
}

std::string StreamingSanitizer::finish() {
    std::string out;
    flush(out, true);
    total_clean_ += out.size();
    buffer_.clear();
    think_depth_ = 0;
    channel_depth_ = 0;
    return out;
}

} // namespace inferdeck::gateway
