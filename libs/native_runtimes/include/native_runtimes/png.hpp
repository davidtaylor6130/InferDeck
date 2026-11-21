#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "foundation/result.hpp"

namespace inferdeck::native_runtimes {

foundation::Result<std::vector<std::byte>> encode_png(
    const std::uint8_t* pixels, int width, int height, int channels);

struct DecodedImage {
    int width{0};
    int height{0};
    std::vector<std::uint8_t> rgb;
};

/// <summary>Decodes PNG, JPEG, or BMP bytes to tightly packed 8-bit RGB.</summary>
foundation::Result<DecodedImage> decode_image_rgb(std::span<const std::byte> encoded);

}
