#include <catch2/catch_test_macros.hpp>
#include "native_runtimes/ltx_video_backend.hpp"
#include "native_runtimes/ltx_video_encoder.hpp"
#include <stable-diffusion.h>
#include <stdexcept>

namespace {
enum class Failure { None, Throw, Cancel };
Failure failure{Failure::None};
sd_progress_cb_t callback{};
void* callback_data{};
int freed_images{};
int freed_audio{};
int context_value{};
std::string observed_backend{};
}

extern "C" {
void sd_set_progress_callback(sd_progress_cb_t cb, void* data) { callback = cb; callback_data = data; }
void sd_ctx_params_init(sd_ctx_params_t* params) { *params = {}; }
void sd_vid_gen_params_init(sd_vid_gen_params_t* params) { *params = {}; }
sd_ctx_t* new_sd_ctx(const sd_ctx_params_t* params) {
    observed_backend = params != nullptr && params->backend != nullptr ? params->backend : "";
    return reinterpret_cast<sd_ctx_t*>(&context_value);
}
void free_sd_ctx(sd_ctx_t*) {}
bool sd_ctx_supports_video_generation(const sd_ctx_t*) { return true; }
void sd_cancel_generation(sd_ctx_t*, sd_cancel_mode_t) {}
void free_sd_images(sd_image_t* images, int count) {
    for (int index = 0; index < count; ++index) delete[] images[index].data;
    delete[] images;
    ++freed_images;
}
void free_sd_audio(sd_audio_t* audio) { delete[] audio->data; delete audio; ++freed_audio; }
bool generate_video(sd_ctx_t*, const sd_vid_gen_params_t*, sd_image_t** frames, int* count, sd_audio_t** audio) {
    auto* result = new sd_image_t[1]{};
    result[0] = {512, 320, 3, new uint8_t[512 * 320 * 3]{}};
    *frames = result; *count = 1;
    *audio = new sd_audio_t{16000, 1, 160, new float[160]{}};
    if (callback) callback(1, 2, 0.0f, callback_data);
    if (failure == Failure::Throw) throw std::runtime_error("injected failure");
    return failure != Failure::Cancel;
}
}

namespace inferdeck::native_runtimes {
#if defined(_WIN32)
const char* ltx_video_content_type() noexcept { return "video/mp4"; }
#else
const char* ltx_video_content_type() noexcept { return "video/x-msvideo"; }
#endif
foundation::Result<std::vector<std::byte>> encode_ltx_video(sd_image_t*, int, int, const sd_audio_t*) {
    return foundation::Ok(std::vector<std::byte>{std::byte{1}});
}
}

inferdeck::model::ModelInfo fixture_info() {
    inferdeck::model::ModelInfo info;
    info.name = "ltx-test";
    info.artifacts["diffusion_model"] = "diffusion.gguf";
    info.artifacts["llm"] = "text.gguf";
    info.artifacts["embeddings_connectors"] = "connectors.safetensors";
    info.artifacts["vae"] = "video-vae.safetensors";
    return info;
}

TEST_CASE("LTX backend cleans native allocations after exception") {
    failure = Failure::Throw; freed_images = 0; freed_audio = 0; observed_backend.clear();
    auto backend = inferdeck::native_runtimes::make_ltx_video_backend(fixture_info());
    REQUIRE(backend->load());
    CHECK(observed_backend == "diffusion=vulkan0,te=cpu");
    auto* video = dynamic_cast<inferdeck::model::IVideoBackend*>(backend.get());
    REQUIRE(video);
    inferdeck::model::VideoGenerationRequest request;
    request.prompt = "fixture";
    const auto result = video->generate_video(0, request);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == inferdeck::foundation::ErrorCode::Internal);
    CHECK(freed_images == 1);
    CHECK(freed_audio == 1);
    CHECK(callback == nullptr);
}

TEST_CASE("LTX backend cleans cancellation and recovers") {
    failure = Failure::Cancel; freed_images = 0; freed_audio = 0;
    auto backend = inferdeck::native_runtimes::make_ltx_video_backend(fixture_info());
    REQUIRE(backend->load());
    CHECK(observed_backend == "diffusion=vulkan0,te=cpu");
    auto* video = dynamic_cast<inferdeck::model::IVideoBackend*>(backend.get());
    REQUIRE(video);
    inferdeck::model::VideoGenerationRequest request;
    request.prompt = "fixture";
    const auto cancelled = video->generate_video(0, request, [](int) { return false; });
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == inferdeck::foundation::ErrorCode::Cancelled);
    CHECK(freed_images == 1);
    CHECK(freed_audio == 1);
    failure = Failure::None;
    const auto recovered = video->generate_video(0, request);
    REQUIRE(recovered);
    CHECK(recovered->content_type == inferdeck::native_runtimes::ltx_video_content_type());
    CHECK(recovered->output_video_seconds > 0.0);
    CHECK(callback == nullptr);
}
