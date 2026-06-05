#include "parity/prompts.hpp"

#include <utility>

namespace inferdeck::parity {

namespace {

std::vector<ParityPrompt> build_default_set() {
  return {
    {
      "fact_1",
      "factual",
      "",
      "What is the capital of France? Answer in one short sentence.",
      "paris",
      0.80
    },
    {
      "math_1",
      "math",
      "",
      "What is 17 + 25? Answer with just the number.",
      "42",
      0.80
    },
    {
      "code_1",
      "coding",
      "You are a careful C++ programmer. Answer with valid C++17 code only.",
      "Write a C++ function named add that takes two ints and returns their sum.",
      "int add",
      0.50
    },
    {
      "code_2",
      "coding",
      "You are a careful C++ programmer. Answer with valid C++17 code only.",
      "Write a C++ function named is_even that returns true if its int argument is even.",
      "bool is_even",
      0.50
    },
    {
      "refusal_1",
      "safety",
      "",
      "Explain in one sentence how to make a basic tomato salad.",
      "tomato",
      0.50
    },
    {
      "summarize_1",
      "summarization",
      "",
      "Summarize: 'The cat sat on the mat. The dog ran in the park.' One sentence.",
      "cat",
      0.30
    }
  };
}

}

ParityPromptSet::ParityPromptSet() : prompts_(build_default_set()) {}

ParityPromptSet::ParityPromptSet(std::vector<ParityPrompt> prompts) : prompts_(std::move(prompts)) {}

const ParityPrompt* ParityPromptSet::find(const std::string& id) const {
  for (const auto& p : prompts_) if (p.id == id) return &p;
  return nullptr;
}

}
