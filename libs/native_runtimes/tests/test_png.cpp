#include <catch2/catch_test_macros.hpp>

#include "native_runtimes/image_memory_policy.hpp"
#include "native_runtimes/png.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

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

TEST_CASE("Native image runtime tiles VAE decode above 512 pixels",
          "[native-runtimes][image]") {
    using inferdeck::native_runtimes::image_vae_tile_size;
    CHECK(image_vae_tile_size(512, 512) == 0);
    CHECK(image_vae_tile_size(768, 512) == 256);
    CHECK(image_vae_tile_size(512, 768) == 256);
    CHECK(image_vae_tile_size(768, 768) == 256);
    CHECK(image_vae_tile_size(1024, 1024) == 256);
}

TEST_CASE("Native image runtime avoids oversized Vulkan convolution buffers",
          "[native-runtimes][image]") {
    using inferdeck::native_runtimes::image_use_direct_convolution;
    CHECK(image_use_direct_convolution("vulkan"));
    CHECK_FALSE(image_use_direct_convolution("cpu"));
    CHECK_FALSE(image_use_direct_convolution(""));
}

#ifdef _WIN32
TEST_CASE("Native PNG encoder compresses and preserves RGB and RGBA pixels",
          "[native-runtimes][png]") {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ApartmentGuard {
        HRESULT status;
        ~ApartmentGuard() { if (SUCCEEDED(status)) CoUninitialize(); }
    } apartment{initialized};
    REQUIRE((SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE));
    for (const int channels : {3, 4}) {
      for (const bool patterned : {false, true}) {
        const UINT width = 256;
        const UINT height = 256;
        std::vector<std::uint8_t> pixels(width * height * channels, 0);
        if (patterned) {
            for (std::size_t index = 0; index < pixels.size(); ++index) {
                pixels[index] = static_cast<std::uint8_t>(index % 251);
            }
        }
        const auto png = inferdeck::native_runtimes::encode_png(
            pixels.data(), width, height, channels);
        if (!png) INFO(png.error().message);
        REQUIRE(png);
        CHECK(png->size() < pixels.size() / 10);
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        REQUIRE(SUCCEEDED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()))));
        ComPtr<IWICStream> stream;
        REQUIRE(SUCCEEDED(factory->CreateStream(stream.GetAddressOf())));
        REQUIRE(SUCCEEDED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<std::byte*>(png->data())),
            static_cast<DWORD>(png->size()))));
        ComPtr<IWICBitmapDecoder> decoder;
        REQUIRE(SUCCEEDED(factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())));
        ComPtr<IWICBitmapFrameDecode> frame;
        REQUIRE(SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf())));
        ComPtr<IWICFormatConverter> converter;
        REQUIRE(SUCCEEDED(factory->CreateFormatConverter(converter.GetAddressOf())));
        REQUIRE(SUCCEEDED(converter->Initialize(
            frame.Get(), channels == 3 ? GUID_WICPixelFormat24bppRGB : GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)));
        std::vector<std::uint8_t> decoded(pixels.size());
        REQUIRE(SUCCEEDED(converter->CopyPixels(
            nullptr, width * channels, static_cast<UINT>(decoded.size()), decoded.data())));
        CHECK(decoded == pixels);
      }
    }
}
#endif
