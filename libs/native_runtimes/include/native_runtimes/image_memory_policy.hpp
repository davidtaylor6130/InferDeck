#pragma once

#include <string_view>

namespace inferdeck::native_runtimes {

constexpr int image_vae_tile_size(int width, int height) noexcept {
    return width > 512 || height > 512 ? 256 : 0;
}

constexpr bool image_use_direct_convolution(std::string_view backend) noexcept {
    return backend == "vulkan";
}

}
