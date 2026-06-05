#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace inferdeck::parity {

struct ParityResult {
  double score{0.0};
  std::size_t baseline_tokens{0};
  std::size_t candidate_tokens{0};
  std::size_t matched_tokens{0};
  std::int64_t baseline_duration_ms{0};
  std::int64_t candidate_duration_ms{0};
  std::string baseline_text;
  std::string candidate_text;
  bool ok{false};
};

struct ParityOptions {
  double min_score{0.95};
  bool strip_think_blocks{true};
  bool normalize_whitespace{true};
  bool case_insensitive{false};
};

class ParityComparator {
public:
  explicit ParityComparator(ParityOptions opts = {});

  ParityResult compare(const std::string& baseline, const std::string& candidate) const;

  double score(const std::string& baseline, const std::string& candidate) const {
    return compare(baseline, candidate).score;
  }

  std::string normalize(const std::string& text) const;

  std::vector<std::string> tokenize(const std::string& text) const;

  const ParityOptions& options() const noexcept { return options_; }

private:
  ParityOptions options_;
};

}
