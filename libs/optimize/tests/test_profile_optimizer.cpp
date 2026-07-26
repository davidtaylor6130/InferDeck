#include <catch2/catch_test_macros.hpp>

#include "optimize/profile_optimizer.hpp"

using inferdeck::optimize::ProfileInput;
using inferdeck::optimize::recommend_profile;

namespace {

ProfileInput realistic_input() {
    ProfileInput input;
    input.model = "qwen3.6-27b";
    input.total_vram_mb = 32768.0;
    input.model_file_mb = 17000.0;
    input.configured_vram_mb = 24000.0;
    input.observed_tokens_per_second = 35.0;
    input.context_per_slot = 100000;
    input.slots = 4;
    input.min_slots = 1;
    input.n_batch = 2048;
    input.n_ubatch = 2048;
    input.cache_type_k = "q4_0";
    input.cache_type_v = "q8_0";
    return input;
}

}

TEST_CASE("Profile optimizer produces a fitting quality-first recommendation",
          "[optimize][profile]") {
    const auto result = recommend_profile(realistic_input());

    REQUIRE_FALSE(result.measured);
    REQUIRE(result.recommended.fits);
    CHECK(result.quality_weight == 0.60);
    CHECK(result.recommended.estimated_vram_mb <= 32768.0 - 2048.0);
    CHECK(result.recommended.context_per_slot == 100000);
    CHECK(result.recommended.slots == 4);
    REQUIRE_FALSE(result.candidates.empty());
    REQUIRE(result.candidates.size() <= 8);
}

TEST_CASE("Profile optimizer keeps per-slot context separate from slot count",
          "[optimize][profile]") {
    auto input = realistic_input();
    input.context_per_slot = 100000;
    input.slots = 4;

    const auto result = recommend_profile(input);

    CHECK(result.recommended.context_per_slot <= 100000);
    CHECK(result.recommended.slots <= 4);
    REQUIRE_FALSE(result.recommended.reasons.empty());
    CHECK(result.recommended.reasons.front().find("context tokens per slot") !=
          std::string::npos);
}

TEST_CASE("Profile optimizer rejects invalid and unbudgeted profiles",
          "[optimize][profile]") {
    auto input = realistic_input();
    input.total_vram_mb = 0.0;
    CHECK_THROWS_AS(recommend_profile(input), std::invalid_argument);

    input = realistic_input();
    input.min_slots = 5;
    CHECK_THROWS_AS(recommend_profile(input), std::invalid_argument);

    input = realistic_input();
    input.cache_type_k = "made-up";
    CHECK_THROWS_AS(recommend_profile(input), std::invalid_argument);
}

TEST_CASE("Profile optimizer demotes candidates that exceed the safety reserve",
          "[optimize][profile]") {
    auto input = realistic_input();
    input.total_vram_mb = 24500.0;

    const auto result = recommend_profile(input);

    REQUIRE(result.recommended.fits);
    CHECK(result.recommended.reserve_vram_mb >= 2048.0);
    for (std::size_t index = 1; index < result.candidates.size(); ++index) {
        CHECK(result.candidates[index - 1].overall_score >=
              result.candidates[index].overall_score);
    }
}
