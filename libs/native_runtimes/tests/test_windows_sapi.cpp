#include <catch2/catch_test_macros.hpp>

#include "model/imodel.hpp"
#include "model/model_registry.hpp"
#include "native_runtimes/runtime_factories.hpp"

#include <objbase.h>

#include <cstddef>
#include <future>
#include <string>
#include <utility>

TEST_CASE("Windows SAPI synthesizes a WAV in-process",
          "[native-runtimes][windows-sapi][integration]") {
    inferdeck::model::ModelRegistry registry;
    inferdeck::native_runtimes::register_factories(registry);
    REQUIRE(registry.has_factory("windows_sapi"));

    inferdeck::model::ModelInfo info;
    info.name = "windows-sapi-test";
    info.runtime = "windows_sapi";
    info.modality = "audio_speech";
    info.capabilities = {"audio_speech"};
    info.n_slots = 1;
    info.min_slots = 1;
    registry.register_model(info);

    auto created = registry.create_result(info.name);
    REQUIRE(created);
    auto backend = std::move(created.value());
    REQUIRE(backend->load());
    auto* speech = dynamic_cast<inferdeck::model::ISpeechBackend*>(backend.get());
    REQUIRE(speech);

    inferdeck::model::SpeechRequest request;
    request.input = "InferDeck local speech is ready.";
    request.voice = "default";
    request.format = "wav";
    request.speed = 1.0f;
    std::size_t streamed_bytes = 0;
    auto future = std::async(std::launch::async, [&] {
        return speech->synthesize(
            0, request,
            [&streamed_bytes](const std::byte*, std::size_t size) {
                streamed_bytes += size;
                return true;
            });
    });
    REQUIRE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    auto result = future.get();
    REQUIRE(result);
    REQUIRE(result->content_type == "audio/wav");
    REQUIRE(result->bytes.size() > 44);
    REQUIRE(streamed_bytes == result->bytes.size());
    REQUIRE(static_cast<char>(result->bytes[0]) == 'R');
    REQUIRE(static_cast<char>(result->bytes[1]) == 'I');
    REQUIRE(static_cast<char>(result->bytes[2]) == 'F');
    REQUIRE(static_cast<char>(result->bytes[3]) == 'F');
    REQUIRE(backend->unload());
}

TEST_CASE("Windows SAPI rejects an incompatible COM apartment",
          "[native-runtimes][windows-sapi][integration]") {
    inferdeck::model::ModelRegistry registry;
    inferdeck::native_runtimes::register_factories(registry);

    inferdeck::model::ModelInfo info;
    info.name = "windows-sapi-mta-test";
    info.runtime = "windows_sapi";
    info.modality = "audio_speech";
    info.capabilities = {"audio_speech"};
    registry.register_model(info);

    auto created = registry.create_result(info.name);
    REQUIRE(created);
    auto backend = std::move(created.value());
    REQUIRE(backend->load());
    auto* speech = dynamic_cast<inferdeck::model::ISpeechBackend*>(backend.get());
    REQUIRE(speech);

    inferdeck::model::SpeechRequest request;
    request.input = "InferDeck COM apartment check.";
    request.voice = "default";
    request.format = "wav";
    request.speed = 1.0f;

    const HRESULT initialized =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto result = speech->synthesize(0, request, {});
    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }

    REQUIRE(SUCCEEDED(initialized));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            inferdeck::foundation::ErrorCode::Unavailable);
    REQUIRE(backend->unload());
}
