#include <catch2/catch_test_macros.hpp>

#include "model/imodel.hpp"
#include "native_runtimes/png.hpp"
#include <stable-diffusion.h>

#include <memory>
#include <stdexcept>

namespace inferdeck::native_runtimes {
std::unique_ptr<model::IBackend> make_stable_diffusion_backend(const model::ModelInfo& info);
}

namespace {
enum class Failure { None, Generation, Encoding, EncodingResult };
Failure g_failure{Failure::None};
sd_progress_cb_t g_callback{};
void* g_callback_data{};
sd_image_t* g_images{};
int g_free_count{};
int g_context{};
bool g_auto_fit{};
int g_reference_count{};
uint32_t g_reference_width{};
int g_steps{};
float g_guidance{};
}

extern "C" {
void sd_set_log_callback(sd_log_cb_t, void*) {}
void sd_set_progress_callback(sd_progress_cb_t callback, void* data) {
    g_callback = callback;
    g_callback_data = data;
}
void sd_ctx_params_init(sd_ctx_params_t* params) { *params = {}; }
void sd_img_gen_params_init(sd_img_gen_params_t* params) { *params = {}; }
sd_ctx_t* new_sd_ctx(const sd_ctx_params_t* params) {
    g_auto_fit = params->auto_fit;
    return reinterpret_cast<sd_ctx_t*>(&g_context);
}
void free_sd_ctx(sd_ctx_t*) {}
bool sd_ctx_supports_image_generation(const sd_ctx_t*) { return true; }
void sd_cancel_generation(sd_ctx_t*, sd_cancel_mode_t) {}
void free_sd_images(sd_image_t* images, int count) {
    for (int index = 0; index < count; ++index) delete[] images[index].data;
    delete[] images;
    g_images = nullptr;
    ++g_free_count;
}
bool generate_image(sd_ctx_t*, const sd_img_gen_params_t* params, sd_image_t** images, int* count) {
    g_steps = params->sample_params.sample_steps;
    g_guidance = params->sample_params.guidance.txt_cfg;
    g_reference_count = params->ref_images_count;
    g_reference_width = params->ref_images_count > 0 ? params->ref_images[0].width : 0;
    g_images = new sd_image_t[1]{};
    g_images[0].width = 2;
    g_images[0].height = 2;
    g_images[0].channel = 3;
    g_images[0].data = new uint8_t[12]{};
    *images = g_images;
    *count = 1;
    if (g_failure == Failure::Generation) throw std::runtime_error("injected generation failure");
    if (g_callback) g_callback(1, 1, 0.0f, g_callback_data);
    return true;
}
}

namespace inferdeck::native_runtimes {
foundation::Result<std::vector<std::byte>> encode_png(const std::uint8_t*, int, int, int) {
    if (g_failure == Failure::Encoding) throw std::runtime_error("injected encoding failure");
    if (g_failure == Failure::EncodingResult) {
        return foundation::Err<std::vector<std::byte>>(foundation::ErrorCode::InvalidArgument, "injected invalid pixels");
    }
    return foundation::Ok(std::vector<std::byte>{std::byte{1}});
}
foundation::Result<DecodedImage> decode_image_rgb(std::span<const std::byte> encoded) {
    if (encoded.size() < 2) {
        return foundation::Err<DecodedImage>(foundation::ErrorCode::InvalidArgument, "injected unreadable image");
    }
    return foundation::Ok(DecodedImage{4, 2, std::vector<std::uint8_t>(24)});
}
}

TEST_CASE("Image generation cleans native resources on exceptions and remains usable",
          "[native][image-cleanup]") {
    using namespace inferdeck;
    g_failure = Failure::None;
    g_free_count = 0;
    struct Cleanup {
        ~Cleanup() {
            sd_set_progress_callback(nullptr, nullptr);
            if (g_images) free_sd_images(g_images, 1);
            g_failure = Failure::None;
        }
    } cleanup;
    model::ModelInfo info;
    info.name = "fake-image";
    info.gguf_path = "fake.gguf";
    std::unique_ptr<model::IBackend> backend = native_runtimes::make_stable_diffusion_backend(info);
    REQUIRE(backend->load());
    model::IImageBackend* images = dynamic_cast<model::IImageBackend*>(backend.get());
    REQUIRE(images);
    SECTION("native generation throws after allocating images") { g_failure = Failure::Generation; }
    SECTION("encoding throws after native generation") { g_failure = Failure::Encoding; }
    SECTION("encoding returns an error") { g_failure = Failure::EncodingResult; }
    model::ImageGenerationRequest request;
    request.prompt = "fixture";
    foundation::Result<model::ImageGenerationResult> failed =
        foundation::Err<model::ImageGenerationResult>(foundation::ErrorCode::Internal, "not run");
    CHECK_NOTHROW(failed = images->generate_images(0, request, [](int) { return true; }));
    CHECK_FALSE(failed);
    CHECK(g_callback == nullptr);
    CHECK(g_callback_data == nullptr);
    CHECK(g_free_count == 1);
    CHECK(g_images == nullptr);
    if (g_images) free_sd_images(g_images, 1);
    sd_set_progress_callback(nullptr, nullptr);
    g_failure = Failure::None;
    const foundation::Result<model::ImageGenerationResult> recovered =
        images->generate_images(0, request, [](int) { return true; });
    REQUIRE(recovered);
    CHECK(recovered->png_images.size() == 1);
    CHECK(g_callback == nullptr);
    CHECK(g_callback_data == nullptr);
    CHECK(g_free_count == 2);
    REQUIRE(backend->unload());
}

TEST_CASE("Explicit CPU image backend disables GPU auto-fit", "[native][image-cpu]") {
    using namespace inferdeck;
    model::ModelInfo info;
    info.name = "cpu-image";
    info.gguf_path = "fake.gguf";
    info.artifacts["backend"] = "cpu";
    std::unique_ptr<model::IBackend> backend = native_runtimes::make_stable_diffusion_backend(info);
    REQUIRE(backend->load());
    CHECK_FALSE(g_auto_fit);
    REQUIRE(backend->unload());
}

TEST_CASE("Reference images reach the native edit call and bad ones are rejected",
          "[native][image-edit]") {
    using namespace inferdeck;
    model::ModelInfo info;
    info.name = "edit-image";
    info.artifacts["diffusion_model"] = "fake-diffusion.gguf";
    info.artifacts["guidance_scale"] = "6.0";
    std::unique_ptr<model::IBackend> backend = native_runtimes::make_stable_diffusion_backend(info);
    REQUIRE(backend->load());
    model::IImageBackend* images = dynamic_cast<model::IImageBackend*>(backend.get());
    REQUIRE(images);
    model::ImageGenerationRequest request;
    request.prompt = "make it red";
    request.reference_images = {{std::byte{1}, std::byte{2}}, {std::byte{3}, std::byte{4}}};
    REQUIRE(images->generate_images(0, request, {}));
    CHECK(g_reference_count == 2);
    CHECK(g_reference_width == 4);
    CHECK(g_steps == 20);
    CHECK(g_guidance == 6.0f);
    request.steps = 8;
    request.guidance_scale = 1.0f;
    REQUIRE(images->generate_images(0, request, {}));
    CHECK(g_steps == 8);
    CHECK(g_guidance == 1.0f);
    request.reference_images = {{std::byte{1}}};
    const auto rejected = images->generate_images(0, request, {});
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == foundation::ErrorCode::InvalidArgument);
    request.reference_images.clear();
    REQUIRE(images->generate_images(0, request, {}));
    CHECK(g_reference_count == 0);
    REQUIRE(backend->unload());
}
