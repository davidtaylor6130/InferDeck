#pragma once

#include <string>
#include <vector>

namespace inferdeck::parity {

struct ParityPrompt {
  std::string id;
  std::string category;
  std::string system;
  std::string user;
  std::string reference;
  double min_score{0.95};
};

class ParityPromptSet {
public:
  ParityPromptSet();
  explicit ParityPromptSet(std::vector<ParityPrompt> prompts);
  static ParityPromptSet empty() { return ParityPromptSet(std::vector<ParityPrompt>{}); }

  const std::vector<ParityPrompt>& prompts() const noexcept { return prompts_; }

  const ParityPrompt* find(const std::string& id) const;

  std::size_t size() const noexcept { return prompts_.size(); }

private:
  std::vector<ParityPrompt> prompts_;
};

}
