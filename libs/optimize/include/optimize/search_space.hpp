#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace inferdeck::optimize {

enum class ParamType { Int, Float, Categorical };

struct ParamSpec {
  std::string name;
  ParamType type{ParamType::Float};
  double min_v{0.0};
  double max_v{1.0};
  double step{0.0};
  bool log_scale{false};
  std::vector<std::string> choices;
};

struct ParamValue {
  std::string name;
  std::variant<std::int64_t, double, std::string> value;
};

struct TrialResult {
  std::int64_t trial_number{0};
  std::vector<ParamValue> params;
  double score{0.0};
  double duration_seconds{0.0};
  bool completed{false};
  std::string error;
};

struct StudyConfig {
  std::string study_name{"default"};
  int n_trials{30};
  std::int64_t seed{0};
  double direction{1.0};
  std::vector<ParamSpec> search_space;
};

class SearchSpace {
public:
  SearchSpace() = default;
  explicit SearchSpace(std::vector<ParamSpec> specs) : specs_(std::move(specs)) {}

  const std::vector<ParamSpec>& specs() const noexcept { return specs_; }

  void add_int(const std::string& name, std::int64_t lo, std::int64_t hi);
  void add_float(const std::string& name, double lo, double hi, bool log_scale = false);
  void add_categorical(const std::string& name, std::vector<std::string> choices);

  std::size_t size() const noexcept { return specs_.size(); }
  bool empty() const noexcept { return specs_.empty(); }

  std::optional<ParamSpec> find(const std::string& name) const;

  std::vector<ParamValue> sample(std::int64_t seed) const;

private:
  std::vector<ParamSpec> specs_;
};

}
