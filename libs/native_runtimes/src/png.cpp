#include "native_runtimes/png.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace inferdeck::native_runtimes {

namespace {

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    output.push_back(static_cast<std::byte>((value >> 24) & 0xff));
    output.push_back(static_cast<std::byte>((value >> 16) & 0xff));
    output.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    output.push_back(static_cast<std::byte>(value & 0xff));
}

std::uint32_t crc32(const std::byte* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= std::to_integer<std::uint8_t>(data[index]);
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

void chunk(std::vector<std::byte>& output, const std::array<char, 4>& type,
           const std::vector<std::byte>& data) {
    append_u32(output, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = output.size();
    for (char value : type) output.push_back(static_cast<std::byte>(value));
    output.insert(output.end(), data.begin(), data.end());
    append_u32(output, crc32(output.data() + start, output.size() - start));
}

std::uint32_t adler32(const std::vector<std::byte>& data) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::byte value : data) {
        a = (a + std::to_integer<std::uint8_t>(value)) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

}

foundation::Result<std::vector<std::byte>> encode_png(
    const std::uint8_t* pixels, int width, int height, int channels) {
    if (!pixels || width < 1 || height < 1 || (channels != 3 && channels != 4) ||
        static_cast<std::uint64_t>(width) * height * channels > std::numeric_limits<std::uint32_t>::max()) {
        return foundation::Err<std::vector<std::byte>>(foundation::ErrorCode::InvalidArgument,
                                                       "invalid image buffer");
    }
    const std::size_t stride = static_cast<std::size_t>(width) * channels;
    std::vector<std::byte> filtered;
    filtered.reserve((stride + 1) * height);
    for (int row = 0; row < height; ++row) {
        filtered.push_back(std::byte{0});
        const auto* begin = reinterpret_cast<const std::byte*>(pixels + static_cast<std::size_t>(row) * stride);
        filtered.insert(filtered.end(), begin, begin + stride);
    }
    std::vector<std::byte> deflate{std::byte{0x78}, std::byte{0x01}};
    for (std::size_t position = 0; position < filtered.size();) {
        const std::size_t size = std::min<std::size_t>(65535, filtered.size() - position);
        const bool final = position + size == filtered.size();
        deflate.push_back(final ? std::byte{1} : std::byte{0});
        const auto length = static_cast<std::uint16_t>(size);
        const auto inverse = static_cast<std::uint16_t>(~length);
        deflate.push_back(static_cast<std::byte>(length & 0xff));
        deflate.push_back(static_cast<std::byte>((length >> 8) & 0xff));
        deflate.push_back(static_cast<std::byte>(inverse & 0xff));
        deflate.push_back(static_cast<std::byte>((inverse >> 8) & 0xff));
        deflate.insert(deflate.end(), filtered.begin() + position, filtered.begin() + position + size);
        position += size;
    }
    append_u32(deflate, adler32(filtered));

    std::vector<std::byte> output{
        std::byte{0x89}, std::byte{0x50}, std::byte{0x4e}, std::byte{0x47},
        std::byte{0x0d}, std::byte{0x0a}, std::byte{0x1a}, std::byte{0x0a}};
    std::vector<std::byte> header;
    append_u32(header, static_cast<std::uint32_t>(width));
    append_u32(header, static_cast<std::uint32_t>(height));
    header.push_back(std::byte{8});
    header.push_back(channels == 3 ? std::byte{2} : std::byte{6});
    header.insert(header.end(), 3, std::byte{0});
    chunk(output, {'I', 'H', 'D', 'R'}, header);
    chunk(output, {'I', 'D', 'A', 'T'}, deflate);
    chunk(output, {'I', 'E', 'N', 'D'}, {});
    return foundation::Ok(std::move(output));
}

}
