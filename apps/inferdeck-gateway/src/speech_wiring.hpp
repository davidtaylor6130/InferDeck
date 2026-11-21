#pragma once

#include "config_types.hpp"
#include "gateway/openai_error.hpp"
#include "gateway/request_security.hpp"
#include "gateway/routes.hpp"

#include <cstddef>

namespace inferdeck::gateway {

inline void apply_speech_deps(GatewayDeps& deps, const GatewayConfig& cfg) {
    deps.speech_admission_timeout_ms = cfg.speech_admission_timeout_ms;
    deps.transcription_max_body_bytes = static_cast<std::size_t>(cfg.transcription_max_body_mb) << 20;
    deps.transcription_decoded_budget_bytes = static_cast<std::size_t>(cfg.transcription_decoded_budget_mb) << 20;
}

inline void write_validation_error(httplib::Response& resp,
                                   RequestValidationStatus validation) {
    if (validation == RequestValidationStatus::BodyNotAllowed) {
        write_error(resp, 400, "body_not_allowed",
                    "request body is not allowed for this endpoint");
    } else if (validation == RequestValidationStatus::PayloadTooLarge) {
        write_error(resp, 413, "request_too_large",
                    "request body exceeds the endpoint limit");
    } else if (validation == RequestValidationStatus::UnsupportedMediaType) {
        write_error(resp, 415, "unsupported_media_type",
                    "request Content-Type is not supported by this endpoint");
    } else if (validation == RequestValidationStatus::InvalidContentLength) {
        write_error(resp, 400, "invalid_content_length",
                    "request Content-Length is invalid");
    } else if (validation == RequestValidationStatus::UnsupportedTransferEncoding) {
        write_error(resp, 411, "content_length_required",
                    "request Transfer-Encoding is not supported for this endpoint");
    }
}

}
