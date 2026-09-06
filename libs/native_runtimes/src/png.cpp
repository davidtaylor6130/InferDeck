#include "native_runtimes/png.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace inferdeck::native_runtimes {

namespace {

#ifdef _WIN32
class ComApartment {
public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() { if (SUCCEEDED(result_)) CoUninitialize(); }
    bool available() const { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }
    HRESULT result() const { return result_; }
private:
    HRESULT result_;
};

foundation::Result<std::vector<std::byte>> png_error(
    const char* operation, HRESULT status) {
    std::ostringstream message;
    message << operation << " (HRESULT 0x" << std::hex
            << static_cast<unsigned long>(status) << ")";
    return foundation::Err<std::vector<std::byte>>(
        foundation::ErrorCode::Internal, message.str());
}
#else

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

#endif

}

foundation::Result<std::vector<std::byte>> encode_png(
    const std::uint8_t* pixels, int width, int height, int channels) {
    if (!pixels || width < 1 || height < 1 || (channels != 3 && channels != 4) ||
        static_cast<std::uint64_t>(width) * height * channels > std::numeric_limits<std::uint32_t>::max()) {
        return foundation::Err<std::vector<std::byte>>(foundation::ErrorCode::InvalidArgument,
                                                       "invalid image buffer");
    }
#ifdef _WIN32
    const ComApartment apartment;
    if (!apartment.available()) {
        return png_error("cannot initialize PNG encoder", apartment.result());
    }
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT status = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()));
    if (SUCCEEDED(status)) status = CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf());
    if (SUCCEEDED(status)) status = factory->CreateEncoder(
        GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (SUCCEEDED(status)) status = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(status)) status = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr);
    if (SUCCEEDED(status)) status = frame->Initialize(nullptr);
    if (SUCCEEDED(status)) status = frame->SetSize(width, height);
    const WICPixelFormatGUID requested_format = channels == 3
        ? GUID_WICPixelFormat24bppRGB : GUID_WICPixelFormat32bppRGBA;
    WICPixelFormatGUID format = requested_format;
    if (SUCCEEDED(status)) status = frame->SetPixelFormat(&format);
    const UINT stride = static_cast<UINT>(width) * channels;
    if (SUCCEEDED(status) && IsEqualGUID(format, requested_format)) {
        status = frame->WritePixels(
            static_cast<UINT>(height), stride, stride * static_cast<UINT>(height),
            const_cast<BYTE*>(pixels));
    } else if (SUCCEEDED(status)) {
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICFormatConverter> converter;
        status = factory->CreateBitmapFromMemory(
            static_cast<UINT>(width), static_cast<UINT>(height), requested_format,
            stride, stride * static_cast<UINT>(height), const_cast<BYTE*>(pixels),
            bitmap.GetAddressOf());
        if (SUCCEEDED(status)) status = factory->CreateFormatConverter(converter.GetAddressOf());
        if (SUCCEEDED(status)) status = converter->Initialize(
            bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(status)) status = frame->WriteSource(converter.Get(), nullptr);
    }
    if (SUCCEEDED(status)) status = frame->Commit();
    if (SUCCEEDED(status)) status = encoder->Commit();
    STATSTG statistics{};
    if (SUCCEEDED(status)) status = stream->Stat(&statistics, STATFLAG_NONAME);
    if (FAILED(status) || statistics.cbSize.QuadPart == 0 ||
        statistics.cbSize.QuadPart > std::numeric_limits<std::size_t>::max()) {
        return png_error("Windows PNG encoding failed", FAILED(status) ? status : E_UNEXPECTED);
    }
    std::vector<std::byte> output(static_cast<std::size_t>(statistics.cbSize.QuadPart));
    status = stream->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr);
    std::size_t position = 0;
    while (SUCCEEDED(status) && position < output.size()) {
        const ULONG requested = static_cast<ULONG>(std::min<std::size_t>(
            output.size() - position, std::numeric_limits<ULONG>::max()));
        ULONG received = 0;
        status = stream->Read(output.data() + position, requested, &received);
        if (received != requested) status = E_FAIL;
        position += received;
    }
    if (FAILED(status)) {
        return png_error("cannot read encoded PNG", status);
    }
    return foundation::Ok(std::move(output));
#else
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
#endif
}

}
