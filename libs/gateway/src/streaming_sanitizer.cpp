#include "gateway/streaming_sanitizer.hpp"

#include <algorithm>
#include <utility>

namespace inferdeck::gateway {

namespace {

bool is_utf8_continuation(unsigned char byte) {
    return (byte & 0xc0u) == 0x80u;
}

int utf8_sequence_length(unsigned char lead) {
    if (lead >= 0xc2u && lead <= 0xdfu) return 2;
    if (lead >= 0xe0u && lead <= 0xefu) return 3;
    if (lead >= 0xf0u && lead <= 0xf4u) return 4;
    return 0;
}

std::size_t incomplete_utf8_suffix(const std::string& value) {
    if (value.empty()) return value.size();

    std::size_t lead_pos = value.size();
    std::size_t continuation_count = 0;
    while (lead_pos > 0 && continuation_count < 3 &&
           is_utf8_continuation(static_cast<unsigned char>(value[lead_pos - 1]))) {
        --lead_pos;
        ++continuation_count;
    }
    if (lead_pos == 0) return value.size();

    --lead_pos;
    const int expected = utf8_sequence_length(
        static_cast<unsigned char>(value[lead_pos]));
    const auto present = static_cast<int>(continuation_count + 1);
    return expected > present ? lead_pos : value.size();
}

}

std::string Utf8StreamBuffer::on_chunk(std::string_view chunk) {
    pending_.append(chunk);
    const std::size_t suffix = incomplete_utf8_suffix(pending_);
    if (suffix == pending_.size()) {
        std::string out;
        out.swap(pending_);
        return out;
    }

    std::string out = pending_.substr(0, suffix);
    pending_.erase(0, suffix);
    return out;
}

std::string Utf8StreamBuffer::finish() {
    std::string out;
    out.swap(pending_);
    return out;
}

model::InferenceDelta InferenceDeltaUtf8Buffer::on_delta(
    const model::InferenceDelta& input) {
    model::InferenceDelta output;
    output.content = content_.on_chunk(input.content);
    output.reasoning_text = reasoning_.on_chunk(input.reasoning_text);
    for (const auto& call : input.tool_calls) {
        auto& buffers = tool_calls_[call.index];
        model::ToolCallDelta clean;
        clean.index = call.index;
        clean.id = buffers.id.on_chunk(call.id);
        clean.type = buffers.type.on_chunk(call.type);
        clean.function_name = buffers.function_name.on_chunk(call.function_name);
        clean.function_arguments = buffers.function_arguments.on_chunk(call.function_arguments);
        if (!clean.id.empty() || !clean.type.empty() ||
            !clean.function_name.empty() || !clean.function_arguments.empty()) {
            output.tool_calls.push_back(std::move(clean));
        }
    }
    return output;
}

model::InferenceDelta InferenceDeltaUtf8Buffer::finish() {
    model::InferenceDelta output;
    output.content = content_.finish();
    output.reasoning_text = reasoning_.finish();
    for (auto& [index, buffers] : tool_calls_) {
        model::ToolCallDelta clean;
        clean.index = index;
        clean.id = buffers.id.finish();
        clean.type = buffers.type.finish();
        clean.function_name = buffers.function_name.finish();
        clean.function_arguments = buffers.function_arguments.finish();
        if (!clean.id.empty() || !clean.type.empty() ||
            !clean.function_name.empty() || !clean.function_arguments.empty()) {
            output.tool_calls.push_back(std::move(clean));
        }
    }
    return output;
}

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
