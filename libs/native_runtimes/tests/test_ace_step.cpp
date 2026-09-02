#include <catch2/catch_test_macros.hpp>

#include "model/imodel.hpp"
#include "model/model_registry.hpp"
#include "native_runtimes/runtime_factories.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace inferdeck;

TEST_CASE("ACE-Step runtime rejects incomplete artifact configuration",
          "[native][audio-generation]") {
    model::ModelRegistry registry;
    native_runtimes::register_factories(registry);
    const std::vector<std::string> runtimes =
        native_runtimes::available_runtimes();
    REQUIRE(std::find(runtimes.begin(), runtimes.end(), "ace_step_cpp") !=
            runtimes.end());

    model::ModelInfo info;
    info.name = "incomplete-audio";
    info.runtime = "ace_step_cpp";
    info.modality = "audio_generation";
    info.capabilities = {"audio_generation"};
    info.n_slots = 4;
    registry.register_model(info);

    foundation::Result<std::unique_ptr<model::IBackend>> created =
        registry.create_result(info.name);
    REQUIRE(created);
    REQUIRE(dynamic_cast<model::IAudioGenerationBackend*>(
                created->get()) != nullptr);
    CHECK((*created)->info().n_slots == 1);
    CHECK((*created)->info().supports("audio_generation"));
    const foundation::Result<void> loaded = (*created)->load();
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().code == foundation::ErrorCode::InvalidArgument);
}
