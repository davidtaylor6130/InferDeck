#include "optimize/study.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace inferdeck::optimize {

namespace {
std::int64_t now_seconds() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}
}

Study::Study(StudyConfig cfg) : cfg_(std::move(cfg)), space_(cfg_.search_space) {}

void Study::add_param_int(const std::string& name, std::int64_t lo, std::int64_t hi) {
  space_.add_int(name, lo, hi);
  cfg_.search_space.push_back(space_.specs().back());
}

void Study::add_param_float(const std::string& name, double lo, double hi, bool log_scale) {
  space_.add_float(name, lo, hi, log_scale);
  cfg_.search_space.push_back(space_.specs().back());
}

void Study::add_param_categorical(const std::string& name, std::vector<std::string> choices) {
  space_.add_categorical(name, std::move(choices));
  cfg_.search_space.push_back(space_.specs().back());
}

std::int64_t Study::next_seed() noexcept {
  if (cfg_.seed != 0) return cfg_.seed + seed_counter_++;
  return static_cast<std::int64_t>(now_seconds()) + seed_counter_++;
}

std::optional<TrialResult> Study::best_trial() const {
  if (history_.empty()) return std::nullopt;
  std::size_t best = 0;
  for (std::size_t i = 1; i < history_.size(); ++i) {
    if (cfg_.direction * history_[i].score > cfg_.direction * history_[best].score) best = i;
  }
  return history_[best];
}

void Study::reset() {
  history_.clear();
  seed_counter_ = 0;
}

StudyResult Study::run(Objective objective) {
  StudyResult out;
  const auto start = std::chrono::steady_clock::now();
  if (space_.empty()) {
    out.error = "empty search space";
    out.duration_seconds = 0;
    out.ok = false;
    return out;
  }
  out.best_score = cfg_.direction > 0 ? -1e308 : 1e308;
  for (int i = 0; i < cfg_.n_trials; ++i) {
    TrialResult tr;
    tr.trial_number = i;
    const auto t_start = std::chrono::steady_clock::now();
    try {
      tr.params = space_.sample(next_seed());
      tr.score = objective(tr.params);
      tr.completed = true;
    } catch (const std::exception& e) {
      tr.error = e.what();
      tr.completed = false;
      tr.score = cfg_.direction > 0 ? 0.0 : 1.0;
    } catch (...) {
      tr.error = "unknown exception";
      tr.completed = false;
      tr.score = cfg_.direction > 0 ? 0.0 : 1.0;
    }
    const auto t_end = std::chrono::steady_clock::now();
    tr.duration_seconds = std::chrono::duration<double>(t_end - t_start).count();
    history_.push_back(tr);
    out.all_trials.push_back(tr);
    out.n_trials = static_cast<std::int64_t>(history_.size());
    if (tr.completed && cfg_.direction * tr.score > cfg_.direction * out.best_score) {
      out.best_score = tr.score;
      out.best_trial = i;
      out.best_params = tr.params;
    }
  }
  const auto end = std::chrono::steady_clock::now();
  out.duration_seconds = std::chrono::duration_cast<std::chrono::seconds>(end - start).count();
  out.ok = out.best_trial >= 0;
  return out;
}

namespace {

nlohmann::json param_to_json(const ParamValue& p) {
  nlohmann::json j;
  j["name"] = p.name;
  if (std::holds_alternative<std::int64_t>(p.value)) j["value"] = std::get<std::int64_t>(p.value);
  else if (std::holds_alternative<double>(p.value)) j["value"] = std::get<double>(p.value);
  else j["value"] = std::get<std::string>(p.value);
  return j;
}

ParamValue param_from_json(const nlohmann::json& j) {
  ParamValue p;
  p.name = j.at("name").get<std::string>();
  const auto& v = j.at("value");
  if (v.is_number_integer()) p.value = v.get<std::int64_t>();
  else if (v.is_number_float()) p.value = v.get<double>();
  else p.value = v.get<std::string>();
  return p;
}

}

bool Study::save_json(const std::string& path) const {
  nlohmann::json j;
  j["study_name"] = cfg_.study_name;
  j["n_trials"] = cfg_.n_trials;
  j["seed"] = cfg_.seed;
  j["direction"] = cfg_.direction;

  nlohmann::json space = nlohmann::json::array();
  for (const auto& s : space_.specs()) {
    nlohmann::json sj;
    sj["name"] = s.name;
    sj["type"] = s.type == ParamType::Int ? "int" : s.type == ParamType::Float ? "float" : "categorical";
    if (s.type != ParamType::Categorical) {
      sj["min"] = s.min_v;
      sj["max"] = s.max_v;
      sj["log_scale"] = s.log_scale;
    } else {
      sj["choices"] = s.choices;
    }
    space.push_back(std::move(sj));
  }
  j["search_space"] = std::move(space);

  nlohmann::json trials = nlohmann::json::array();
  for (const auto& t : history_) {
    nlohmann::json tj;
    tj["trial_number"] = t.trial_number;
    tj["score"] = t.score;
    tj["duration_seconds"] = t.duration_seconds;
    tj["completed"] = t.completed;
    tj["error"] = t.error;
    nlohmann::json tp = nlohmann::json::array();
    for (const auto& p : t.params) tp.push_back(param_to_json(p));
    tj["params"] = std::move(tp);
    trials.push_back(std::move(tj));
  }
  j["trials"] = std::move(trials);

  std::ofstream f(path);
  if (!f) return false;
  f << j.dump(2);
  return f.good();
}

Study Study::load_json(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open: " + path);
  nlohmann::json j;
  f >> j;
  StudyConfig cfg;
  cfg.study_name = j.value("study_name", "default");
  cfg.n_trials = j.value("n_trials", 30);
  cfg.seed = j.value("seed", static_cast<std::int64_t>(0));
  cfg.direction = j.value("direction", 1.0);
  if (j.contains("search_space") && j["search_space"].is_array()) {
    for (const auto& sj : j["search_space"]) {
      ParamSpec s;
      s.name = sj.at("name").get<std::string>();
      const auto t = sj.at("type").get<std::string>();
      if (t == "int") {
        s.type = ParamType::Int;
        s.min_v = sj.at("min").get<double>();
        s.max_v = sj.at("max").get<double>();
        s.step = 1.0;
      } else if (t == "float") {
        s.type = ParamType::Float;
        s.min_v = sj.at("min").get<double>();
        s.max_v = sj.at("max").get<double>();
        s.log_scale = sj.value("log_scale", false);
      } else {
        s.type = ParamType::Categorical;
        s.choices = sj.at("choices").get<std::vector<std::string>>();
      }
      cfg.search_space.push_back(std::move(s));
    }
  }
  Study s(std::move(cfg));
  if (j.contains("trials") && j["trials"].is_array()) {
    for (const auto& tj : j["trials"]) {
      TrialResult t;
      t.trial_number = tj.value("trial_number", static_cast<std::int64_t>(0));
      t.score = tj.value("score", 0.0);
      t.duration_seconds = tj.value("duration_seconds", 0.0);
      t.completed = tj.value("completed", false);
      t.error = tj.value("error", std::string{});
      if (tj.contains("params") && tj["params"].is_array()) {
        for (const auto& pj : tj["params"]) t.params.push_back(param_from_json(pj));
      }
      s.history_.push_back(std::move(t));
    }
  }
  return s;
}

}
