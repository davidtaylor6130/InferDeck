#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "optimize/search_space.hpp"

using namespace inferdeck::optimize;

TEST_CASE("SearchSpace: add_int + sample gives int in range", "[optimize][space]") {
  SearchSpace s;
  s.add_int("top_k", 1, 100);
  REQUIRE(s.size() == 1);
  for (int i = 0; i < 50; ++i) {
    auto vals = s.sample(42 + i);
    REQUIRE(vals.size() == 1);
    const auto v = std::get<std::int64_t>(vals[0].value);
    REQUIRE(v >= 1);
    REQUIRE(v <= 100);
  }
}

TEST_CASE("SearchSpace: add_float linear sampling", "[optimize][space]") {
  SearchSpace s;
  s.add_float("temp", 0.0, 2.0, false);
  double min_seen = 1e9, max_seen = -1e9;
  for (int i = 0; i < 200; ++i) {
    auto vals = s.sample(i);
    const auto v = std::get<double>(vals[0].value);
    if (v < min_seen) min_seen = v;
    if (v > max_seen) max_seen = v;
  }
  REQUIRE(min_seen < 0.1);
  REQUIRE(max_seen > 1.9);
}

TEST_CASE("SearchSpace: add_float log sampling spans more orders of magnitude", "[optimize][space]") {
  SearchSpace s;
  s.add_float("lr", 1e-5, 1e-1, true);
  bool saw_small = false, saw_big = false;
  for (int i = 0; i < 200; ++i) {
    auto vals = s.sample(i);
    const auto v = std::get<double>(vals[0].value);
    if (v < 1e-3) saw_small = true;
    if (v > 1e-2) saw_big = true;
  }
  REQUIRE(saw_small);
  REQUIRE(saw_big);
}

TEST_CASE("SearchSpace: add_categorical returns choices", "[optimize][space]") {
  SearchSpace s;
  s.add_categorical("algo", {"greedy", "top_p", "top_k", "min_p"});
  std::set<std::string> seen;
  for (int i = 0; i < 100; ++i) {
    auto vals = s.sample(i);
    seen.insert(std::get<std::string>(vals[0].value));
  }
  REQUIRE(seen.size() == 4);
}

TEST_CASE("SearchSpace: mixed params", "[optimize][space]") {
  SearchSpace s;
  s.add_int("n", 1, 10);
  s.add_float("temp", 0.0, 1.0);
  s.add_categorical("algo", {"a", "b"});
  auto vals = s.sample(1);
  REQUIRE(vals.size() == 3);
  REQUIRE(vals[0].name == "n");
  REQUIRE(vals[1].name == "temp");
  REQUIRE(vals[2].name == "algo");
}

TEST_CASE("SearchSpace: find returns spec by name", "[optimize][space]") {
  SearchSpace s;
  s.add_int("top_k", 1, 50);
  REQUIRE(s.find("top_k").has_value());
  REQUIRE_FALSE(s.find("missing").has_value());
}

TEST_CASE("SearchSpace: empty space yields empty sample", "[optimize][space]") {
  SearchSpace s;
  auto vals = s.sample(0);
  REQUIRE(vals.empty());
}
