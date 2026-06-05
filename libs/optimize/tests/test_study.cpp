#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "optimize/study.hpp"

using namespace inferdeck::optimize;

namespace {

std::filesystem::path unique_tmp(const std::string& tag) {
  static int counter = 0;
  const auto p = std::filesystem::temp_directory_path() /
                 ("optimize_" + tag + "_" + std::to_string(++counter) + ".json");
  std::error_code ec;
  std::filesystem::remove(p, ec);
  return p;
}

}

TEST_CASE("Study: simple quadratic objective is maximized", "[optimize][study]") {
  StudyConfig cfg;
  cfg.n_trials = 30;
  cfg.seed = 42;
  cfg.direction = 1.0;
  cfg.search_space = {ParamSpec{"x", ParamType::Float, -5.0, 5.0, 0.0, false, {}}};
  Study s(std::move(cfg));
  auto result = s.run([](const std::vector<ParamValue>& p) {
    const double x = std::get<double>(p[0].value);
    return -(x * x) + 4.0;
  });
  REQUIRE(result.ok);
  REQUIRE(result.n_trials == 30);
  REQUIRE(result.best_score > 3.5);
}

TEST_CASE("Study: minimize direction inverts score selection", "[optimize][study]") {
  StudyConfig cfg;
  cfg.n_trials = 20;
  cfg.seed = 1;
  cfg.direction = -1.0;
  cfg.search_space = {ParamSpec{"x", ParamType::Float, 0.0, 10.0, 0.0, false, {}}};
  Study s(std::move(cfg));
  auto result = s.run([](const std::vector<ParamValue>& p) {
    const double x = std::get<double>(p[0].value);
    return (x - 7.0) * (x - 7.0);
  });
  REQUIRE(result.ok);
  REQUIRE(result.best_score < 1.0);
}

TEST_CASE("Study: exception in objective marks trial incomplete", "[optimize][study]") {
  StudyConfig cfg;
  cfg.n_trials = 3;
  cfg.seed = 1;
  cfg.search_space = {ParamSpec{"x", ParamType::Float, 0.0, 1.0, 0.0, false, {}}};
  Study s(std::move(cfg));
  int count = 0;
  auto result = s.run([&count](const std::vector<ParamValue>&) {
    if (++count == 2) throw std::runtime_error("synthetic");
    return static_cast<double>(count);
  });
  REQUIRE(result.n_trials == 3);
  REQUIRE_FALSE(s.history()[1].completed);
  REQUIRE(s.history()[1].error == "synthetic");
  REQUIRE(s.history()[0].completed);
  REQUIRE(s.history()[2].completed);
}

TEST_CASE("Study: empty search space returns error result", "[optimize][study]") {
  StudyConfig cfg;
  cfg.n_trials = 5;
  Study s(std::move(cfg));
  auto result = s.run([](const auto&) { return 1.0; });
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.error == "empty search space");
}

TEST_CASE("Study: history reflects trials in order", "[optimize][study]") {
  StudyConfig cfg;
  cfg.n_trials = 5;
  cfg.seed = 1;
  cfg.search_space = {ParamSpec{"x", ParamType::Float, 0.0, 1.0, 0.0, false, {}}};
  Study s(std::move(cfg));
  s.run([](const auto&) { return 0.5; });
  REQUIRE(s.history().size() == 5);
  for (int i = 0; i < 5; ++i) {
    REQUIRE(s.history()[i].trial_number == i);
  }
}

TEST_CASE("Study: best_trial helper returns highest score", "[optimize][study]") {
  StudyConfig cfg;
  cfg.n_trials = 10;
  cfg.seed = 1;
  cfg.direction = 1.0;
  cfg.search_space = {ParamSpec{"x", ParamType::Float, 0.0, 1.0, 0.0, false, {}}};
  Study s(std::move(cfg));
  int n = 0;
  s.run([&n](const auto&) { return static_cast<double>(n++) / 10.0; });
  auto best = s.best_trial();
  REQUIRE(best.has_value());
  REQUIRE_THAT(best->score, Catch::Matchers::WithinAbs(0.9, 1e-9));
}

TEST_CASE("Study: save/load round-trip", "[optimize][study]") {
  StudyConfig cfg;
  cfg.study_name = "round-trip";
  cfg.n_trials = 3;
  cfg.seed = 7;
  cfg.direction = 1.0;
  cfg.search_space = {
    ParamSpec{"x", ParamType::Float, 0.0, 1.0, 0.0, false, {}},
    ParamSpec{"algo", ParamType::Categorical, 0.0, 0.0, 0.0, false, {"a", "b", "c"}}
  };
  Study s(std::move(cfg));
  s.run([](const auto&) { return 0.5; });
  const auto path = unique_tmp("rt");
  REQUIRE(s.save_json(path.string()));
  REQUIRE(std::filesystem::exists(path));

  auto loaded = Study::load_json(path.string());
  REQUIRE(loaded.config().study_name == "round-trip");
  REQUIRE(loaded.config().n_trials == 3);
  REQUIRE(loaded.config().seed == 7);
  REQUIRE(loaded.space().size() == 2);
  REQUIRE(loaded.history().size() == 3);
  std::filesystem::remove(path);
}

TEST_CASE("Study: reset clears history but keeps config", "[optimize][study]") {
  StudyConfig cfg;
  cfg.n_trials = 3;
  cfg.seed = 1;
  cfg.search_space = {ParamSpec{"x", ParamType::Float, 0.0, 1.0, 0.0, false, {}}};
  Study s(std::move(cfg));
  s.run([](const auto&) { return 1.0; });
  REQUIRE(s.history().size() == 3);
  s.reset();
  REQUIRE(s.history().empty());
  REQUIRE(s.space().size() == 1);
}

TEST_CASE("Study: add_param_int/float/categorical", "[optimize][study]") {
  StudyConfig cfg;
  cfg.n_trials = 1;
  Study s(std::move(cfg));
  s.add_param_int("n", 1, 5);
  s.add_param_float("lr", 0.001, 0.1, true);
  s.add_param_categorical("a", {"x", "y"});
  REQUIRE(s.space().size() == 3);
  s.run([](const auto&) { return 1.0; });
  REQUIRE(s.history().size() == 1);
}
