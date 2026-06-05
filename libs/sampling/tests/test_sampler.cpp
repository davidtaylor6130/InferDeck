#include <catch2/catch_test_macros.hpp>

#include "sampling/SamplerParams.hpp"
#include "sampling/family.hpp"
#include "sampling/family_defaults.hpp"
#include "sampling/sampler.hpp"

using namespace inferdeck::sampling;

TEST_CASE("Qwen3.6 chat defaults", "[sampling][family]") {
    auto p = defaults_for_family(kFamilyQwen3_6);
    REQUIRE(p.temperature.value() == 0.7f);
    REQUIRE(p.top_p.value() == 0.8f);
    REQUIRE(p.top_k.value() == 20);
    REQUIRE(p.min_p.value() == 0.0f);
    REQUIRE(p.repetition_penalty.value() == 1.05f);
    REQUIRE(p.dry_multiplier.value() == 0.0f);
    REQUIRE(p.penalty_last_n.value() == 64);
}

TEST_CASE("Qwen3-Coder-Next coding defaults", "[sampling][family]") {
    auto p = defaults_for_family(kFamilyQwen3CoderNext);
    REQUIRE(p.temperature.value() == 0.6f);
    REQUIRE(p.top_p.value() == 0.95f);
    REQUIRE(p.top_k.value() == 20);
    REQUIRE(p.min_p.value() == 0.0f);
    REQUIRE(p.repetition_penalty.value() == 1.0f);
    REQUIRE(p.dry_multiplier.value() == 0.0f);
    REQUIRE(p.penalty_last_n.value() == 256);
}

TEST_CASE("Qwen3 and Qwen3.5 share Qwen3.6 defaults", "[sampling][family]") {
    auto a = defaults_for_family(kFamilyQwen3);
    auto b = defaults_for_family(kFamilyQwen3_5);
    auto c = defaults_for_family(kFamilyQwen3_6);
    REQUIRE(*a.temperature == *b.temperature);
    REQUIRE(*b.temperature == *c.temperature);
    REQUIRE(*a.top_p == *b.top_p);
    REQUIRE(*a.top_k == *b.top_k);
}

TEST_CASE("Unknown family falls back to safe defaults", "[sampling][family]") {
    auto p = defaults_for_family("llama-3");
    REQUIRE(p.temperature.value() == 0.8f);
    REQUIRE(p.top_p.value() == 0.95f);
    REQUIRE(p.top_k.value() == 40);
    REQUIRE(p.repetition_penalty.value() == 1.1f);
}

TEST_CASE("is_known_family returns true for Qwen3, Qwen3-Coder", "[sampling][family]") {
    REQUIRE(is_known_family(kFamilyQwen3_6));
    REQUIRE(is_known_family(kFamilyQwen3Coder));
    REQUIRE(is_known_family(kFamilyQwen3CoderNext));
    REQUIRE_FALSE(is_known_family("gpt-4"));
    REQUIRE_FALSE(is_known_family("llama"));
}

TEST_CASE("User override temperature only, others from base", "[sampling][override]") {
    auto base = defaults_for_family(kFamilyQwen3_6);
    SamplerParams overrides;
    overrides.temperature = 0.3f;

    auto merged = apply_user_overrides(base, overrides);
    REQUIRE(*merged.temperature == 0.3f);
    REQUIRE(*merged.top_p == *base.top_p);
    REQUIRE(*merged.top_k == *base.top_k);
    REQUIRE(*merged.repetition_penalty == *base.repetition_penalty);
}

TEST_CASE("User override top_p, top_k, min_p", "[sampling][override]") {
    auto base = defaults_for_family(kFamilyQwen3_6);
    SamplerParams overrides;
    overrides.top_p = 0.99f;
    overrides.top_k = 100;
    overrides.min_p = 0.01f;

    auto merged = apply_user_overrides(base, overrides);
    REQUIRE(*merged.top_p == 0.99f);
    REQUIRE(*merged.top_k == 100);
    REQUIRE(*merged.min_p == 0.01f);
}

TEST_CASE("Zero frequency_penalty is preserved through override", "[sampling][override]") {
    auto base = defaults_for_family(kFamilyQwen3_6);
    SamplerParams overrides;
    overrides.frequency_penalty = 0.0f;

    auto merged = apply_user_overrides(base, overrides);
    REQUIRE(merged.frequency_penalty.has_value());
    REQUIRE(*merged.frequency_penalty == 0.0f);
}

TEST_CASE("DRY can be enabled via override", "[sampling][override]") {
    auto base = defaults_for_family(kFamilyQwen3_6);
    REQUIRE(*base.dry_multiplier == 0.0f);

    SamplerParams overrides;
    overrides.dry_multiplier = 0.8f;
    overrides.dry_penalty_last_n = 1024;

    auto merged = apply_user_overrides(base, overrides);
    REQUIRE(*merged.dry_multiplier == 0.8f);
    REQUIRE(*merged.dry_penalty_last_n == 1024);
}

TEST_CASE("Grammar triggers are propagated", "[sampling][override]") {
    auto base = defaults_for_family(kFamilyQwen3_6);
    SamplerParams overrides;
    overrides.grammar = R"({ "type": "object" })";
    overrides.grammar_triggers = std::vector<GrammarTrigger>{
        {GrammarTriggerType::Word, "function_call", 0}
    };

    auto merged = apply_user_overrides(base, overrides);
    REQUIRE(*merged.grammar == R"({ "type": "object" })");
    REQUIRE(merged.grammar_triggers.has_value());
    REQUIRE(merged.grammar_triggers->size() == 1);
    REQUIRE(merged.grammar_triggers->at(0).value == "function_call");
}

TEST_CASE("Seed override applied", "[sampling][override]") {
    auto base = defaults_for_family(kFamilyQwen3_6);

    SamplerParams overrides;
    overrides.seed = 42;

    auto merged = apply_user_overrides(base, overrides);
    REQUIRE(merged.seed.has_value());
    REQUIRE(*merged.seed == 42u);
}

TEST_CASE("params_to_yaml produces readable string", "[sampling][format]") {
    SamplerParams p;
    p.temperature = 0.5f;
    p.top_p = 0.9f;
    p.top_k = 50;
    auto s = params_to_yaml(p);
    REQUIRE(s.find("temp: 0.500") != std::string::npos);
    REQUIRE(s.find("top_p: 0.900") != std::string::npos);
    REQUIRE(s.find("top_k: 50") != std::string::npos);
}

TEST_CASE("SamplerChain default ctor is null", "[sampling][chain]") {
    SamplerChain chain;
    REQUIRE(!chain);
    REQUIRE(chain.raw() == nullptr);
}

TEST_CASE("SamplerChain move transfers ownership", "[sampling][chain]") {
    SamplerChain a;
    SamplerChain b(std::move(a));
    REQUIRE(!a);
    REQUIRE(!b);
}
