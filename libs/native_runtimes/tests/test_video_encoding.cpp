#include <catch2/catch_test_macros.hpp>
#include <media_io.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

TEST_CASE("Native video encoder produces AVI with video and audio streams", "[video][encoder]") {
    std::array<std::uint8_t, 16 * 16 * 3> pixels{};
    pixels.fill(120);
    std::array<sd_image_t, 9> frames{};
    for (sd_image_t& frame : frames) {
        frame = sd_image_t{16, 16, 3, pixels.data()};
    }
    std::vector<float> samples(18000, 0.0f);
    sd_audio_t audio{48000, 1, samples.size(), samples.data()};
    const std::vector<std::uint8_t> bytes = create_video_from_sd_images_to_vector(
        "avi", frames.data(), static_cast<int>(frames.size()), 24, 90, &audio);
    REQUIRE(bytes.size() > 128);
    const std::string contents(bytes.begin(), bytes.end());
    CHECK(contents.substr(0, 4) == "RIFF");
    CHECK(contents.substr(8, 4) == "AVI ");
    CHECK(contents.find("vids") != std::string::npos);
    CHECK(contents.find("auds") != std::string::npos);
    CHECK(contents.find("movi") != std::string::npos);
    CHECK(contents.find("idx1") != std::string::npos);
}
