#pragma once

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>

namespace inferdeck::gateway {

inline constexpr std::size_t json_body_limit = 16ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t control_body_limit = 2ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t audio_body_limit = 26ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t server_body_limit = audio_body_limit;
inline constexpr auto server_read_timeout = std::chrono::seconds{30};
inline constexpr auto server_request_read_deadline = std::chrono::seconds{30};
inline constexpr auto server_write_timeout = std::chrono::seconds{30};
inline constexpr auto server_keep_alive_timeout = std::chrono::seconds{5};

enum class ContentPolicy {
    None,
    Json,
    Multipart,
};

enum class RequestValidationStatus {
    Allowed,
    BodyNotAllowed,
    PayloadTooLarge,
    UnsupportedMediaType,
    InvalidContentLength,
    UnsupportedTransferEncoding,
};

struct RequestPolicy {
    std::size_t max_body_bytes{0};
    ContentPolicy content{ContentPolicy::None};
    bool require_content_type{false};
};

inline RequestPolicy request_policy(std::string_view method,
                                    std::string_view path,
                                    bool control_write = false) {
    if (method == "GET" || method == "HEAD" || method == "OPTIONS") {
        return {};
    }
    if (path == "/v1/audio/transcriptions") {
        return {audio_body_limit, ContentPolicy::Multipart, false};
    }
    if (path.starts_with("/api/")) {
        return {control_body_limit, ContentPolicy::Json, control_write};
    }
    return {json_body_limit, ContentPolicy::Json, control_write};
}

inline std::string normalized_media_type(const httplib::Request& req) {
    auto value = req.get_header_value("Content-Type");
    const auto separator = value.find(';');
    if (separator != std::string::npos) value.resize(separator);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    value.erase(value.begin(), first);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

inline RequestValidationStatus validate_request_headers(
    const httplib::Request& req,
    const RequestPolicy& policy) {
    const auto transfer_encoding = req.get_header_value("Transfer-Encoding");
    if (!transfer_encoding.empty()) {
        return RequestValidationStatus::UnsupportedTransferEncoding;
    }
    std::size_t content_length = 0;
    const auto length = req.get_header_value("Content-Length");
    if (!length.empty()) {
        const auto parsed = std::from_chars(length.data(), length.data() + length.size(),
                                            content_length);
        if (parsed.ec != std::errc{} || parsed.ptr != length.data() + length.size()) {
            return RequestValidationStatus::InvalidContentLength;
        }
    }
    if (policy.max_body_bytes == 0 && content_length > 0) {
        return RequestValidationStatus::BodyNotAllowed;
    }
    if (content_length > policy.max_body_bytes) {
        return RequestValidationStatus::PayloadTooLarge;
    }
    if (content_length == 0 && !policy.require_content_type) {
        return RequestValidationStatus::Allowed;
    }
    const auto media_type = normalized_media_type(req);
    if (policy.content == ContentPolicy::Json && media_type != "application/json") {
        return RequestValidationStatus::UnsupportedMediaType;
    }
    if (policy.content == ContentPolicy::Multipart &&
        media_type != "multipart/form-data") {
        return RequestValidationStatus::UnsupportedMediaType;
    }
    return RequestValidationStatus::Allowed;
}

inline RequestValidationStatus validate_request(
    const httplib::Request& req,
    const RequestPolicy& policy) {
    if (req.body.empty() && !policy.require_content_type) {
        return RequestValidationStatus::Allowed;
    }
    if (policy.max_body_bytes == 0) return RequestValidationStatus::BodyNotAllowed;
    if (req.body.size() > policy.max_body_bytes) {
        return RequestValidationStatus::PayloadTooLarge;
    }
    const auto media_type = normalized_media_type(req);
    if (policy.content == ContentPolicy::Json && media_type != "application/json") {
        return RequestValidationStatus::UnsupportedMediaType;
    }
    if (policy.content == ContentPolicy::Multipart &&
        media_type != "multipart/form-data") {
        return RequestValidationStatus::UnsupportedMediaType;
    }
    return RequestValidationStatus::Allowed;
}

}
