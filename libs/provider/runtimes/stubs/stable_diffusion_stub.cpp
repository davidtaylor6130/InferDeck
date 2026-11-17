// Test double for the stable-diffusion.cpp C API, so the real image and video
// backends can be packaged and checked as provider DLLs without a GPU.
#include <stable-diffusion.h>

#include <cstdint>
#include <span>
#include <vector>

#include "foundation/result.hpp"
#include "native_runtimes/ltx_video_encoder.hpp"
#include "native_runtimes/png.hpp"

namespace {
sd_progress_cb_t g_callback{};
void* g_callback_data{};
int g_context{};
}

extern "C" {
void sd_set_log_callback(sd_log_cb_t, void*) {}
void sd_set_progress_callback(sd_progress_cb_t callback, void* data) { g_callback = callback; g_callback_data = data; }
void sd_ctx_params_init(sd_ctx_params_t* params) { *params = {}; }
void sd_img_gen_params_init(sd_img_gen_params_t* params) { *params = {}; }
void sd_vid_gen_params_init(sd_vid_gen_params_t* params) { *params = {}; }
sd_ctx_t* new_sd_ctx(const sd_ctx_params_t*) { return reinterpret_cast<sd_ctx_t*>(&g_context); }
void free_sd_ctx(sd_ctx_t*) {}
bool sd_ctx_supports_image_generation(const sd_ctx_t*) { return true; }
bool sd_ctx_supports_video_generation(const sd_ctx_t*) { return true; }
void sd_cancel_generation(sd_ctx_t*, sd_cancel_mode_t) {}
void free_sd_images(sd_image_t* images, int count) {
    for (int index = 0; index < count; ++index) delete[] images[index].data;
    delete[] images;
}
void free_sd_audio(sd_audio_t* audio) { delete[] audio->data; delete audio; }
void free_sd_image_generation_result(sd_image_generation_result_t* result) {
    if (!result) return;
    for (int index = 0; index < result->count; ++index) delete[] result->items[index].image.data;
    delete[] result->items;
    delete result;
}
void free_sd_video_generation_result(sd_video_generation_result_t* result) {
    if (!result) return;
    free_sd_images(result->frames, result->frame_count);
    if (result->audio) free_sd_audio(result->audio);
    delete result;
}
bool generate_image(sd_ctx_t*, const sd_img_gen_params_t*, sd_image_generation_result_t** result_out) {
    auto* result = new sd_image_generation_result_t{new sd_image_result_t[1]{}, 1};
    result->items[0].image = {2, 2, 3, new uint8_t[12]{}};
    *result_out = result;
    if (g_callback) g_callback(1, 1, 0.0f, g_callback_data);
    return true;
}
bool generate_video(sd_ctx_t*, const sd_vid_gen_params_t* params, sd_video_generation_result_t** result_out) {
    const auto width = static_cast<uint32_t>(params->width);
    const auto height = static_cast<uint32_t>(params->height);
    auto* frames = new sd_image_t[1]{};
    frames[0] = {width, height, 3, new uint8_t[width * height * 3]{}};
    *result_out = new sd_video_generation_result_t{frames, 1, new sd_audio_t{16000, 1, 160, new float[160]{}}, 0, nullptr};
    if (g_callback) g_callback(1, 2, 0.0f, g_callback_data);
    return true;
}
}

namespace inferdeck::native_runtimes {
foundation::Result<std::vector<std::byte>> encode_png(const std::uint8_t*, int, int, int) {
    return foundation::Ok(std::vector<std::byte>{std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'}});
}
foundation::Result<DecodedImage> decode_image_rgb(std::span<const std::byte>) {
    return foundation::Ok(DecodedImage{2, 2, std::vector<std::uint8_t>(12)});
}
const char* ltx_video_content_type() noexcept { return "video/mp4"; }
foundation::Result<std::vector<std::byte>> encode_ltx_video(sd_image_t*, int, int, const sd_audio_t*) {
    return foundation::Ok(std::vector<std::byte>{std::byte{1}, std::byte{2}});
}
}
