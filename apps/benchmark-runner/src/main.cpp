#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include "optimize/search_space.hpp"
#include "optimize/study.hpp"
#include "optimize/yaml_loader.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Args {
  std::string model = "qwen3.6-27b";
  std::string suite = "humaneval";
  std::string config = "config/bench-search-spaces.yaml";
  std::string output = "build/optimization.json";
  int trials = 30;
  int seed = 0;
  bool dry_run = false;
  bool verbose = false;
  bool show_help = false;
  bool show_version = false;
};

void print_help() {
  std::cout
    << "inferdeck-bench - sampler profile optimizer\n"
    << "\n"
    << "Usage: inferdeck-bench [options]\n"
    << "\n"
    << "Options:\n"
    << "  --model <name>        Model identifier (default qwen3.6-27b)\n"
    << "  --suite <name>        Benchmark suite (default humaneval)\n"
    << "  --config <path>       Search space YAML (default config/bench-search-spaces.yaml)\n"
    << "  --trials <n>          Number of trials (default 30)\n"
    << "  --seed <n>            Random seed (default time-based)\n"
    << "  --output <path>       Output JSON (default build/optimization.json)\n"
    << "  --dry-run             Use synthetic scoring (no real inference)\n"
    << "  --verbose             Print per-trial detail\n"
    << "  -h, --help            Show this help\n"
    << "  -v, --version         Show version\n";
}

Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    if (s == "-h" || s == "--help") a.show_help = true;
    else if (s == "-v" || s == "--version") a.show_version = true;
    else if (s == "--model" && i + 1 < argc) a.model = argv[++i];
    else if (s == "--suite" && i + 1 < argc) a.suite = argv[++i];
    else if (s == "--config" && i + 1 < argc) a.config = argv[++i];
    else if (s == "--trials" && i + 1 < argc) a.trials = std::stoi(argv[++i]);
    else if (s == "--seed" && i + 1 < argc) a.seed = std::stoi(argv[++i]);
    else if (s == "--output" && i + 1 < argc) a.output = argv[++i];
    else if (s == "--dry-run") a.dry_run = true;
    else if (s == "--verbose") a.verbose = true;
  }
  return a;
}

double synthetic_score(const inferdeck::optimize::ParamValue& p) {
  if (std::holds_alternative<double>(p.value)) {
    const double v = std::get<double>(p.value);
    if (p.name == "temperature") return 1.0 - std::abs(v - 0.7);
    if (p.name == "top_p") return 1.0 - std::abs(v - 0.9);
    if (p.name == "min_p") return 1.0 - std::abs(v - 0.05);
    if (p.name == "top_k") return 1.0 - std::abs(v - 40.0) / 100.0;
    if (p.name == "repeat_penalty") return 1.0 - std::abs(v - 1.1);
    return 0.5;
  }
  if (std::holds_alternative<std::int64_t>(p.value)) {
    const auto v = std::get<std::int64_t>(p.value);
    if (p.name == "top_k") return 1.0 - std::abs(static_cast<double>(v) - 40.0) / 100.0;
    if (p.name == "seed") return 0.5;
  }
  if (std::holds_alternative<std::string>(p.value)) {
    const auto& s = std::get<std::string>(p.value);
    if (s == "greedy" || s == "top_p") return 0.8;
    return 0.5;
  }
  return 0.5;
}

double score_params(const std::vector<inferdeck::optimize::ParamValue>& params) {
  double total = 0.0;
  std::size_t n = 0;
  for (const auto& p : params) {
    total += synthetic_score(p);
    ++n;
  }
  return n > 0 ? total / static_cast<double>(n) : 0.0;
}

}

int main(int argc, char** argv) {
  using namespace inferdeck::optimize;
  const Args args = parse_args(argc, argv);
  if (args.show_help) { print_help(); return 0; }
  if (args.show_version) { std::cout << "inferdeck-bench 0.3.0\n"; return 0; }

  std::cout << "inferdeck-bench 0.3.0\n"
            << "  model=" << args.model
            << " suite=" << args.suite
            << " trials=" << args.trials
            << " config=" << args.config
            << (args.dry_run ? " dry-run=true" : "")
            << "\n";

  if (!fs::exists(args.config)) {
    std::cerr << "config not found: " << args.config << "\n";
    return 2;
  }
  std::ifstream cf(args.config);
  std::stringstream buf;
  buf << cf.rdbuf();
  const auto ss_cfg = load_search_space_yaml(buf.str());
  if (ss_cfg.specs.empty()) {
    std::cerr << "no params found in search space\n";
    return 2;
  }

  StudyConfig scfg;
  scfg.study_name = args.model + ":" + args.suite;
  scfg.n_trials = args.trials;
  scfg.seed = args.seed;
  scfg.direction = 1.0;
  scfg.search_space = ss_cfg.specs;

  Study study(std::move(scfg));
  const auto t_start = std::chrono::steady_clock::now();
  auto result = study.run([&](const std::vector<ParamValue>& params) {
    const double s = args.dry_run ? score_params(params) : 0.0;
    if (args.verbose) {
      std::cout << "  trial: score=" << s << " params=";
      for (const auto& p : params) {
        if (std::holds_alternative<double>(p.value)) std::cout << p.name << "=" << std::get<double>(p.value) << " ";
        else if (std::holds_alternative<std::int64_t>(p.value)) std::cout << p.name << "=" << std::get<std::int64_t>(p.value) << " ";
        else std::cout << p.name << "=" << std::get<std::string>(p.value) << " ";
      }
      std::cout << "\n";
    }
    return s;
  });
  const auto t_end = std::chrono::steady_clock::now();
  const auto dur = std::chrono::duration_cast<std::chrono::seconds>(t_end - t_start).count();

  if (!result.ok) {
    std::cerr << "study failed: " << result.error << "\n";
    return 1;
  }

  std::cout << "\nBest trial: " << result.best_trial
            << "  score=" << result.best_score
            << "  trials=" << result.n_trials
            << "  duration=" << dur << "s\n";
  for (const auto& p : result.best_params) {
    if (std::holds_alternative<double>(p.value)) std::cout << "  " << p.name << " = " << std::get<double>(p.value) << "\n";
    else if (std::holds_alternative<std::int64_t>(p.value)) std::cout << "  " << p.name << " = " << std::get<std::int64_t>(p.value) << "\n";
    else std::cout << "  " << p.name << " = " << std::get<std::string>(p.value) << "\n";
  }

  json out;
  out["model"] = args.model;
  out["suite"] = args.suite;
  out["n_trials"] = result.n_trials;
  out["best_trial"] = result.best_trial;
  out["best_score"] = result.best_score;
  out["duration_seconds"] = dur;
  out["dry_run"] = args.dry_run;
  json params = json::array();
  for (const auto& p : result.best_params) {
    json pj;
    pj["name"] = p.name;
    if (std::holds_alternative<std::int64_t>(p.value)) pj["value"] = std::get<std::int64_t>(p.value);
    else if (std::holds_alternative<double>(p.value)) pj["value"] = std::get<double>(p.value);
    else pj["value"] = std::get<std::string>(p.value);
    params.push_back(pj);
  }
  out["best_params"] = params;
  fs::create_directories(fs::path(args.output).parent_path());
  std::ofstream f(args.output);
  f << out.dump(2);
  std::cout << "Saved to " << args.output << "\n";
  return 0;
}
