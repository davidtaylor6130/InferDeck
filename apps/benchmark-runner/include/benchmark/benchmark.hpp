#pragma once

#include <functional>
#include <string>
#include <vector>

#include "optimize/search_space.hpp"

namespace inferdeck::benchmark {

struct Problem {
  std::string id;
  std::string prompt;
  std::string reference;
  double weight{1.0};
};

struct BenchmarkConfig {
  std::string name;
  std::string model;
  int n_problems{0};
};

using ProblemLoader = std::function<std::vector<Problem>(const BenchmarkConfig&)>;
using Scorer = std::function<double(const std::string& output, const Problem& p)>;
using Generator = std::function<std::string(const std::string& prompt)>;

}
