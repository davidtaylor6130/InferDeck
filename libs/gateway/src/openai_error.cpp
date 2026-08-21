#include "gateway/openai_error.hpp"

#include <utility>

namespace inferdeck::gateway {

OpenAIErrorMapping map_openai_error(foundation::ErrorCode code,
                                    std::string parameter) {
    OpenAIErrorMapping mapped;
    mapped.parameter = std::move(parameter);
    switch (code) {
        case foundation::ErrorCode::InvalidArgument:
        case foundation::ErrorCode::ParseError:
            mapped.status = 400;
            mapped.type = "invalid_request_error";
            mapped.code = "invalid_request_error";
            break;
        case foundation::ErrorCode::ContextLengthExceeded:
            mapped.status = 400;
            mapped.type = "invalid_request_error";
            mapped.code = "context_length_exceeded";
            break;
        case foundation::ErrorCode::NotFound:
            mapped.status = 404;
            mapped.type = "invalid_request_error";
            mapped.code = "model_not_found";
            break;
        case foundation::ErrorCode::Cancelled:
            mapped.status = 499;
            mapped.type = "server_error";
            mapped.code = "cancelled";
            break;
        case foundation::ErrorCode::Timeout:
            mapped.status = 504;
            mapped.type = "server_error";
            mapped.code = "timeout";
            break;
        case foundation::ErrorCode::Unavailable:
        case foundation::ErrorCode::NotLoaded:
            mapped.status = 503;
            mapped.type = "server_error";
            mapped.code = "unavailable";
            break;
        default:
            break;
    }
    return mapped;
}

}
