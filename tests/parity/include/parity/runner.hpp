#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "parity/compare.hpp"
#include "parity/prompts.hpp"

namespace inferdeck::parity {

struct ParityRunEntry {
  std::string prompt_id;
  std::string category;
  ParityResult result;
};

struct ParityRun {
  std::string model_name;
  std::string baseline_path;
  std::vector<ParityRunEntry> entries;
  std::int64_t started_unix_ms{};
  std::int64_t finished_unix_ms{};
  double overall_score{0.0};
  std::size_t passing{0};
  std::size_t failing{0};
  bool ok{false};
};

class ParityRunner {
public:
  explicit ParityRunner(ParityOptions opts = {});

  ParityRun run(const ParityPromptSet& prompts,
                const std::string& model_name,
                std::function<ParityResult(const ParityPrompt&)> runner) const;

  const ParityComparator& comparator() const noexcept { return comparator_; }

private:
  ParityComparator comparator_;
};

}
