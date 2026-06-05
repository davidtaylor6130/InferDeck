#include "optimize/search_space.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>
#include <stdexcept>

namespace inferdeck::optimize {

namespace {

std::int64_t iround(double v) { return static_cast<std::int64_t>(std::llround(v)); }

}

void SearchSpace::add_int(const std::string& name, std::int64_t lo, std::int64_t hi) {
  ParamSpec s;
  s.name = name;
  s.type = ParamType::Int;
  s.min_v = static_cast<double>(lo);
  s.max_v = static_cast<double>(hi);
  s.step = 1.0;
  specs_.push_back(std::move(s));
}

void SearchSpace::add_float(const std::string& name, double lo, double hi, bool log_scale) {
  if (lo >= hi) throw std::invalid_argument("add_float: lo must be < hi");
  ParamSpec s;
  s.name = name;
  s.type = ParamType::Float;
  s.min_v = lo;
  s.max_v = hi;
  s.log_scale = log_scale;
  specs_.push_back(std::move(s));
}

void SearchSpace::add_categorical(const std::string& name, std::vector<std::string> choices) {
  if (choices.empty()) throw std::invalid_argument("add_categorical: empty choices");
  ParamSpec s;
  s.name = name;
  s.type = ParamType::Categorical;
  s.choices = std::move(choices);
  specs_.push_back(std::move(s));
}

std::optional<ParamSpec> SearchSpace::find(const std::string& name) const {
  for (const auto& s : specs_) if (s.name == name) return s;
  return std::nullopt;
}

std::vector<ParamValue> SearchSpace::sample(std::int64_t seed) const {
  std::vector<ParamValue> out;
  out.reserve(specs_.size());
  std::mt19937_64 rng(static_cast<std::uint64_t>(seed));
  for (const auto& s : specs_) {
    ParamValue v;
    v.name = s.name;
    switch (s.type) {
      case ParamType::Int: {
        const auto lo = static_cast<std::int64_t>(s.min_v);
        const auto hi = static_cast<std::int64_t>(s.max_v);
        std::uniform_int_distribution<std::int64_t> d(lo, hi);
        v.value = d(rng);
        break;
      }
      case ParamType::Float: {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        const double t = u(rng);
        double x;
        if (s.log_scale) {
          const double log_lo = std::log(s.min_v);
          const double log_hi = std::log(s.max_v);
          x = std::exp(log_lo + t * (log_hi - log_lo));
        } else {
          x = s.min_v + t * (s.max_v - s.min_v);
        }
        v.value = x;
        break;
      }
      case ParamType::Categorical: {
        std::uniform_int_distribution<std::size_t> d(0, s.choices.size() - 1);
        v.value = s.choices[d(rng)];
        break;
      }
    }
    out.push_back(std::move(v));
  }
  return out;
}

}
