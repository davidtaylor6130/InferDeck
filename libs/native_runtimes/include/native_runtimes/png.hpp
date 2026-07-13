#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "foundation/result.hpp"

namespace inferdeck::native_runtimes {

foundation::Result<std::vector<std::byte>> encode_png(
    const std::uint8_t* pixels, int width, int height, int channels);

}
