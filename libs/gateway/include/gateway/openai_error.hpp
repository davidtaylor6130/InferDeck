#pragma once

#include "foundation/result.hpp"

#include <string>

namespace inferdeck::gateway {

struct OpenAIErrorMapping {
    int status{500};
    std::string type{"server_error"};
    std::string code{"inference_error"};
    std::string parameter;
};

OpenAIErrorMapping map_openai_error(foundation::ErrorCode code,
                                    std::string parameter = {});

}
