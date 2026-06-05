#include "parity/runner.hpp"

#include <chrono>
#include <functional>
#include <numeric>

namespace inferdeck::parity {

namespace {
std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
}

ParityRunner::ParityRunner(ParityOptions opts) : comparator_(std::move(opts)) {}

ParityRun ParityRunner::run(const ParityPromptSet& prompts,
                            const std::string& model_name,
                            std::function<ParityResult(const ParityPrompt&)> runner) const {
  ParityRun run;
  run.model_name = model_name;
  run.started_unix_ms = now_ms();
  for (const auto& prompt : prompts.prompts()) {
    auto cmp_opts = comparator_.options();
    cmp_opts.min_score = prompt.min_score;
    ParityComparator per(cmp_opts);
    ParityResult r = runner(prompt);
    if (r.baseline_text.empty() && !prompt.reference.empty()) {
      r.baseline_text = prompt.reference;
      r = per.compare(prompt.reference, r.candidate_text);
      r.ok = r.score >= prompt.min_score;
    }
    ParityRunEntry entry{prompt.id, prompt.category, r};
    run.entries.push_back(std::move(entry));
  }
  run.finished_unix_ms = now_ms();
  if (run.entries.empty()) {
    run.ok = false;
    return run;
  }
  double total = 0.0;
  for (const auto& e : run.entries) {
    total += e.result.score;
    if (e.result.ok) ++run.passing; else ++run.failing;
  }
  run.overall_score = total / static_cast<double>(run.entries.size());
  run.ok = run.failing == 0;
  return run;
}

}
