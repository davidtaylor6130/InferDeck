#include <catch2/catch_test_macros.hpp>

#include "parity/runner.hpp"

using namespace inferdeck::parity;

namespace {

ParityResult fake_runner_correct(const ParityPrompt& p) {
  ParityResult r;
  r.baseline_text = p.reference;
  r.candidate_text = p.reference;
  r.baseline_tokens = 1;
  r.candidate_tokens = 1;
  r.matched_tokens = 1;
  r.score = 1.0;
  r.ok = true;
  return r;
}

ParityResult fake_runner_wrong(const ParityPrompt& p) {
  (void)p;
  ParityResult r;
  r.baseline_text = "the capital of france is paris";
  r.candidate_text = "i have no idea";
  r.score = 0.0;
  r.ok = false;
  return r;
}

}

TEST_CASE("ParityRunner: all-pass run is ok", "[parity][runner]") {
  ParityRunner r;
  ParityPromptSet s;
  auto run = r.run(s, "test-model", fake_runner_correct);
  REQUIRE(run.ok);
  REQUIRE(run.passing == s.size());
  REQUIRE(run.failing == 0);
  REQUIRE(run.overall_score == 1.0);
}

TEST_CASE("ParityRunner: all-fail run is not ok", "[parity][runner]") {
  ParityRunner r;
  ParityPromptSet s;
  auto run = r.run(s, "test-model", fake_runner_wrong);
  REQUIRE_FALSE(run.ok);
  REQUIRE(run.failing == s.size());
  REQUIRE(run.passing == 0);
  REQUIRE(run.overall_score < 0.5);
}

TEST_CASE("ParityRunner: entries populated in prompt order", "[parity][runner]") {
  ParityRunner r;
  ParityPromptSet s;
  auto run = r.run(s, "m", [](const ParityPrompt& p) {
    ParityResult res;
    res.baseline_text = p.reference;
    res.candidate_text = p.reference;
    res.matched_tokens = 1;
    res.baseline_tokens = 1;
    res.candidate_tokens = 1;
    res.score = 1.0;
    res.ok = true;
    return res;
  });
  REQUIRE(run.entries.size() == s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    REQUIRE(run.entries[i].prompt_id == s.prompts()[i].id);
    REQUIRE(run.entries[i].category == s.prompts()[i].category);
  }
}

TEST_CASE("ParityRunner: uses prompt-level min_score override", "[parity][runner]") {
  ParityRunner r;
  ParityPromptSet s;
  auto run = r.run(s, "m", [](const ParityPrompt&) {
    ParityResult res;
    res.baseline_text = "alpha beta";
    res.candidate_text = "alpha beta gamma";
    res.matched_tokens = 2;
    res.baseline_tokens = 2;
    res.candidate_tokens = 3;
    res.score = 2.0 / 3.0;
    res.ok = true;
    return res;
  });
  REQUIRE_FALSE(run.entries.empty());
  for (const auto& e : run.entries) {
    if (e.prompt_id == "code_1") {
      REQUIRE(e.result.ok);
    }
  }
}

TEST_CASE("ParityRunner: empty prompt set yields empty run", "[parity][runner]") {
  ParityRunner r;
  auto empty = ParityPromptSet::empty();
  auto run = r.run(empty, "m", fake_runner_correct);
  REQUIRE(run.entries.empty());
  REQUIRE_FALSE(run.ok);
}
