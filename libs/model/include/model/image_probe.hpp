#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace inferdeck::model {

struct ImageProbe {
    std::string_view media_type;
    std::int64_t width{0};
    std::int64_t height{0};
};

inline constexpr std::int64_t kMaxReferenceImageSide = 8192;
inline constexpr std::int64_t kMaxReferenceImagePixels = 16LL * 1024 * 1024;

inline std::optional<ImageProbe> probe_image(std::span<const std::byte> data) {
    const auto at = [&](std::size_t index) {
        return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[index]));
    };
    const auto be32 = [&](std::size_t index) {
        return static_cast<std::int64_t>((at(index) << 24) | (at(index + 1) << 16) |
                                         (at(index + 2) << 8) | at(index + 3));
    };
    const auto le32 = [&](std::size_t index) {
        return static_cast<std::int32_t>(at(index) | (at(index + 1) << 8) |
                                         (at(index + 2) << 16) | (at(index + 3) << 24));
    };
    if (data.size() >= 24 && at(0) == 0x89 && at(1) == 'P' && at(2) == 'N' && at(3) == 'G' &&
        at(12) == 'I' && at(13) == 'H' && at(14) == 'D' && at(15) == 'R') {
        return ImageProbe{"image/png", be32(16), be32(20)};
    }
    if (data.size() >= 26 && at(0) == 'B' && at(1) == 'M') {
        const std::int64_t height = le32(22);
        return ImageProbe{"image/bmp", le32(18), height < 0 ? -height : height};
    }
    if (data.size() >= 4 && at(0) == 0xFF && at(1) == 0xD8) {
        std::size_t index = 2;
        while (index + 9 < data.size()) {
            if (at(index) != 0xFF) return std::nullopt;
            const std::uint32_t marker = at(index + 1);
            if (marker == 0xFF) { ++index; continue; }
            if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
                index += 2;
                continue;
            }
            const std::size_t length = (at(index + 2) << 8) | at(index + 3);
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
                marker != 0xCC) {
                return ImageProbe{"image/jpeg", (at(index + 7) << 8) | at(index + 8),
                                  (at(index + 5) << 8) | at(index + 6)};
            }
            if (length < 2) return std::nullopt;
            index += 2 + length;
        }
    }
    return std::nullopt;
}

inline bool reference_image_within_budget(const ImageProbe& probe) {
    return probe.width >= 1 && probe.height >= 1 && probe.width <= kMaxReferenceImageSide &&
           probe.height <= kMaxReferenceImageSide &&
           probe.width * probe.height <= kMaxReferenceImagePixels;
}

}
