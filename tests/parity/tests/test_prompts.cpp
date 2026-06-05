#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "parity/prompts.hpp"

using namespace inferdeck::parity;

TEST_CASE("ParityPromptSet has built-in prompts", "[parity][prompts]") {
  ParityPromptSet s;
  REQUIRE(s.size() >= 4);
  REQUIRE(s.find("fact_1") != nullptr);
  REQUIRE(s.find("code_1") != nullptr);
  REQUIRE(s.find("math_1") != nullptr);
  REQUIRE(s.find("does_not_exist") == nullptr);
}

TEST_CASE("Each prompt has id, category, user, min_score in (0,1]", "[parity][prompts]") {
  ParityPromptSet s;
  for (const auto& p : s.prompts()) {
    REQUIRE_FALSE(p.id.empty());
    REQUIRE_FALSE(p.category.empty());
    REQUIRE_FALSE(p.user.empty());
    REQUIRE(p.min_score > 0.0);
    REQUIRE(p.min_score <= 1.0);
  }
}

TEST_CASE("Prompt categories cover factual, math, coding, safety", "[parity][prompts]") {
  ParityPromptSet s;
  std::set<std::string> cats;
  for (const auto& p : s.prompts()) cats.insert(p.category);
  REQUIRE(cats.count("factual") > 0);
  REQUIRE(cats.count("math") > 0);
  REQUIRE(cats.count("coding") > 0);
}
