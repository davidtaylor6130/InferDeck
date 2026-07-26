#include <catch2/catch_test_macros.hpp>

#include "native_runtimes/png.hpp"

TEST_CASE("Native image runtime encodes valid PNG framing", "[native-runtimes]") {
    const std::uint8_t pixels[] = {255, 0, 0, 0, 255, 0};
    auto png = inferdeck::native_runtimes::encode_png(pixels, 2, 1, 3);
    REQUIRE(png);
    REQUIRE(png->size() > 50);
    CHECK(std::to_integer<unsigned int>((*png)[0]) == 0x89);
    CHECK(std::to_integer<unsigned int>((*png)[1]) == 0x50);
    CHECK(std::to_integer<unsigned int>((*png)[2]) == 0x4e);
    CHECK(std::to_integer<unsigned int>((*png)[3]) == 0x47);
    CHECK(std::to_integer<unsigned int>((*png)[png->size() - 5]) == 0x44);
}
