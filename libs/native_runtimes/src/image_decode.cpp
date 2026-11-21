#include "native_runtimes/png.hpp"

#include <cstdlib>
#include <limits>
#include <memory>

#include "media_io.h"
#include "model/image_probe.hpp"

namespace inferdeck::native_runtimes {

foundation::Result<DecodedImage> decode_image_rgb(std::span<const std::byte> encoded)
{
    constexpr int max_dimension = static_cast<int>(model::kMaxReferenceImageSide);
    if (encoded.empty() || encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return foundation::Err<DecodedImage>(foundation::ErrorCode::InvalidArgument,
                                             "reference image is empty or too large");
    }
    const auto probe = model::probe_image(encoded);
    if (!probe || !model::reference_image_within_budget(*probe))
    {
        return foundation::Err<DecodedImage>(
            foundation::ErrorCode::InvalidArgument,
            "reference image is not a readable PNG, JPEG, or BMP within 8192 pixels per side and 16 megapixels");
    }
    DecodedImage image;
    int channels = 0;
    const std::unique_ptr<std::uint8_t, decltype(&std::free)> pixels(
        load_image_from_memory(reinterpret_cast<const char*>(encoded.data()),
                               static_cast<int>(encoded.size()),
                               image.width, image.height, channels, 0, 0, 3),
        &std::free);
    if (!pixels || image.width < 1 || image.height < 1 ||
        image.width > max_dimension || image.height > max_dimension)
    {
        return foundation::Err<DecodedImage>(
            foundation::ErrorCode::InvalidArgument,
            "reference image is not a readable PNG, JPEG, or BMP of at most 8192x8192 pixels");
    }
    const std::size_t size = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3;
    image.rgb.assign(pixels.get(), pixels.get() + size);
    return foundation::Ok(std::move(image));
}

}
