#pragma once

#include "foundation/result.hpp"
#include "model/imodel.hpp"

#include <string_view>

namespace inferdeck::gateway {

foundation::Result<model::TranscriptionRequest> decode_compressed_audio(
    std::string_view content);

}
