#include <catch2/catch_test_macros.hpp>
#include "native_runtimes/ltx_video_encoder.hpp"
#include <cstdlib>
#include <fstream>
#include <string>

namespace {
bool contains_bytes(const std::vector<std::byte>& bytes, const char* needle) {
    const auto* begin = reinterpret_cast<const char*>(bytes.data());
    return std::string(begin, begin + bytes.size()).find(needle) != std::string::npos;
}
}

TEST_CASE("LTX encoder produces browser-playable video with audio") {
    sd_image_t frames[3]{};
    std::vector<std::vector<uint8_t>> pixels(3, std::vector<uint8_t>(64 * 64 * 3));
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        for (std::size_t pixel = 0; pixel < pixels[index].size(); pixel += 3) {
            pixels[index][pixel] = index == 0 ? 255 : 0;
            pixels[index][pixel + 1] = index == 1 ? 255 : 0;
            pixels[index][pixel + 2] = index == 2 ? 255 : 0;
        }
        frames[index] = {64, 64, 3, pixels[index].data()};
    }
    std::vector<float> samples(9600, 0.0f);
    sd_audio_t audio{48000, 2, 4800, samples.data()};
    const auto encoded = inferdeck::native_runtimes::encode_ltx_video(frames, 3, 24, &audio);
    if (!encoded) FAIL_CHECK(encoded.error().message);
    REQUIRE(encoded);
    REQUIRE(encoded->size() > 64);
#if defined(_WIN32)
    CHECK(inferdeck::native_runtimes::ltx_video_content_type() == std::string("video/mp4"));
    CHECK((*encoded)[4] == std::byte{0x66});
    CHECK((*encoded)[5] == std::byte{0x74});
    CHECK((*encoded)[6] == std::byte{0x79});
    CHECK((*encoded)[7] == std::byte{0x70});
    CHECK(contains_bytes(*encoded, "avc1"));
    CHECK(contains_bytes(*encoded, "mp4a"));
    const char* fixture_path = std::getenv("INFERDECK_VIDEO_TEST_OUTPUT");
    if (fixture_path != nullptr && *fixture_path != '\0') {
        std::ofstream fixture(fixture_path, std::ios::binary | std::ios::trunc);
        REQUIRE(fixture);
        fixture.write(reinterpret_cast<const char*>(encoded->data()),
                      static_cast<std::streamsize>(encoded->size()));
        REQUIRE(fixture);
    }
    const auto video_only = inferdeck::native_runtimes::encode_ltx_video(frames, 3, 24, nullptr);
    REQUIRE(video_only);
    CHECK_FALSE(contains_bytes(*video_only, "mp4a"));
#else
    CHECK(inferdeck::native_runtimes::ltx_video_content_type() == std::string("video/x-msvideo"));
    CHECK((*encoded)[0] == std::byte{0x52});
    CHECK((*encoded)[1] == std::byte{0x49});
    CHECK((*encoded)[2] == std::byte{0x46});
    CHECK((*encoded)[3] == std::byte{0x46});
#endif
}

#if defined(_WIN32)
TEST_CASE("LTX encoder rejects invalid frame shapes") {
    std::vector<std::uint8_t> first_pixels(32 * 32 * 3, 0);
    std::vector<std::uint8_t> second_pixels(30 * 32 * 3, 0);
    sd_image_t frames[2] = {
        {32, 32, 3, first_pixels.data()},
        {30, 32, 3, second_pixels.data()},
    };
    const auto null_result = inferdeck::native_runtimes::encode_ltx_video(nullptr, 1, 24, nullptr);
    REQUIRE_FALSE(null_result);
    CHECK(null_result.error().code == inferdeck::foundation::ErrorCode::InvalidArgument);
    const auto mismatched = inferdeck::native_runtimes::encode_ltx_video(frames, 2, 24, nullptr);
    REQUIRE_FALSE(mismatched);
    CHECK(mismatched.error().code == inferdeck::foundation::ErrorCode::InvalidArgument);
    sd_image_t odd_frame{31, 32, 3, first_pixels.data()};
    const auto odd = inferdeck::native_runtimes::encode_ltx_video(&odd_frame, 1, 24, nullptr);
    REQUIRE_FALSE(odd);
    CHECK(odd.error().code == inferdeck::foundation::ErrorCode::InvalidArgument);
}

TEST_CASE("LTX encoder rejects AAC sample rates unsupported by Media Foundation") {
    std::vector<std::uint8_t> pixels(32 * 32 * 3, 0);
    sd_image_t frame{32, 32, 3, pixels.data()};
    std::vector<float> samples(1600, 0.0f);
    sd_audio_t audio{16000, 1, samples.size(), samples.data()};
    const auto result = inferdeck::native_runtimes::encode_ltx_video(&frame, 1, 24, &audio);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == inferdeck::foundation::ErrorCode::InvalidArgument);
}
#endif