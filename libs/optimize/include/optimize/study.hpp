#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "optimize/search_space.hpp"

namespace inferdeck::optimize {

struct StudyResult {
  std::int64_t n_trials{0};
  std::int64_t best_trial{-1};
  double best_score{0.0};
  std::vector<ParamValue> best_params;
  std::vector<TrialResult> all_trials;
  std::int64_t duration_seconds{0};
  std::string error;
  bool ok{false};
};

class Study {
public:
  explicit Study(StudyConfig cfg);

  const StudyConfig& config() const noexcept { return cfg_; }
  const SearchSpace& space() const noexcept { return space_; }
  SearchSpace& space() noexcept { return space_; }

  void add_param_int(const std::string& name, std::int64_t lo, std::int64_t hi);
  void add_param_float(const std::string& name, double lo, double hi, bool log_scale = false);
  void add_param_categorical(const std::string& name, std::vector<std::string> choices);

  using Objective = std::function<double(const std::vector<ParamValue>&)>;

  StudyResult run(Objective objective);

  std::optional<TrialResult> best_trial() const;
  const std::vector<TrialResult>& history() const noexcept { return history_; }

  bool save_json(const std::string& path) const;
  static Study load_json(const std::string& path);

  void reset();

private:
  std::int64_t next_seed() noexcept;

  StudyConfig cfg_;
  SearchSpace space_;
  std::vector<TrialResult> history_;
  std::int64_t seed_counter_{0};
};

}
