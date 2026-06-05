#include <catch2/catch_test_macros.hpp>

#include "optimize/yaml_loader.hpp"

using namespace inferdeck::optimize;

TEST_CASE("YAML loader: int range", "[optimize][yaml]") {
  const std::string text = R"(
params:
  top_k:
    type: int
    range: "1..100"
)";
  auto cfg = load_search_space_yaml(text);
  REQUIRE(cfg.specs.size() == 1);
  REQUIRE(cfg.specs[0].name == "top_k");
  REQUIRE(cfg.specs[0].type == ParamType::Int);
  REQUIRE(cfg.specs[0].min_v == 1.0);
  REQUIRE(cfg.specs[0].max_v == 100.0);
}

TEST_CASE("YAML loader: float linear", "[optimize][yaml]") {
  const std::string text = R"(
params:
  temperature:
    type: float
    range: "0.0..2.0"
)";
  auto cfg = load_search_space_yaml(text);
  REQUIRE(cfg.specs.size() == 1);
  REQUIRE(cfg.specs[0].type == ParamType::Float);
  REQUIRE(cfg.specs[0].log_scale == false);
}

TEST_CASE("YAML loader: log_float", "[optimize][yaml]") {
  const std::string text = R"(
params:
  learning_rate:
    type: log_float
    range: "0.00001..0.1"
)";
  auto cfg = load_search_space_yaml(text);
  REQUIRE(cfg.specs.size() == 1);
  REQUIRE(cfg.specs[0].log_scale == true);
}

TEST_CASE("YAML loader: categorical", "[optimize][yaml]") {
  const std::string text = R"(
params:
  algorithm:
    type: categorical
    choices: ["greedy", "top_p", "top_k"]
)";
  auto cfg = load_search_space_yaml(text);
  REQUIRE(cfg.specs.size() == 1);
  REQUIRE(cfg.specs[0].type == ParamType::Categorical);
  REQUIRE(cfg.specs[0].choices.size() == 3);
}

TEST_CASE("YAML loader: multiple params", "[optimize][yaml]") {
  const std::string text = R"(
params:
  top_k:
    type: int
    range: "1..50"
  temperature:
    type: float
    range: "0.0..1.5"
  algorithm:
    type: categorical
    choices: ["a", "b"]
)";
  auto cfg = load_search_space_yaml(text);
  REQUIRE(cfg.specs.size() == 3);
  REQUIRE(cfg.specs[0].name == "top_k");
  REQUIRE(cfg.specs[1].name == "temperature");
  REQUIRE(cfg.specs[2].name == "algorithm");
}

TEST_CASE("YAML loader: empty params yields empty", "[optimize][yaml]") {
  auto cfg = load_search_space_yaml("params: {}");
  REQUIRE(cfg.specs.empty());
}

TEST_CASE("YAML loader: missing params key yields empty", "[optimize][yaml]") {
  auto cfg = load_search_space_yaml("foo: bar");
  REQUIRE(cfg.specs.empty());
}
