#pragma once

namespace inferdeck::native_runtimes {

constexpr int image_vae_tile_size(int width, int height) noexcept {
    return width > 512 || height > 512 ? 256 : 0;
}

}
