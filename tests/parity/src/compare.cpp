#include "parity/compare.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace inferdeck::parity {

namespace {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool is_word_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

std::string strip_think(const std::string& text) {
  static const std::regex think_re(R"(\<think\>.*?\<\/think\>)", std::regex::icase | std::regex::optimize);
  static const std::regex think_re2(R"(\<think\>.*)", std::regex::icase | std::regex::optimize);
  std::string out = std::regex_replace(text, think_re, "");
  out = std::regex_replace(out, think_re2, "");
  return out;
}

std::string collapse_whitespace(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  bool last_was_space = true;
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!last_was_space) {
        out.push_back(' ');
        last_was_space = true;
      }
    } else {
      out.push_back(c);
      last_was_space = false;
    }
  }
  if (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

}

ParityComparator::ParityComparator(ParityOptions opts) : options_(std::move(opts)) {}

std::string ParityComparator::normalize(const std::string& text) const {
  std::string out = text;
  if (options_.strip_think_blocks) out = strip_think(out);
  if (options_.normalize_whitespace) out = collapse_whitespace(out);
  if (options_.case_insensitive) out = to_lower(out);
  return out;
}

std::vector<std::string> ParityComparator::tokenize(const std::string& text) const {
  std::vector<std::string> toks;
  std::string cur;
  for (const char c : text) {
    if (is_word_char(c)) {
      cur.push_back(c);
    } else {
      if (!cur.empty()) {
        toks.push_back(std::move(cur));
        cur.clear();
      }
    }
  }
  if (!cur.empty()) toks.push_back(std::move(cur));
  return toks;
}

ParityResult ParityComparator::compare(const std::string& baseline, const std::string& candidate) const {
  ParityResult r;
  r.baseline_text = baseline;
  r.candidate_text = candidate;
  const std::string a = normalize(baseline);
  const std::string b = normalize(candidate);
  const auto a_toks = tokenize(a);
  const auto b_toks = tokenize(b);
  r.baseline_tokens = a_toks.size();
  r.candidate_tokens = b_toks.size();

  if (a_toks.empty() && b_toks.empty()) {
    r.score = 1.0;
    r.matched_tokens = 0;
    r.ok = true;
    return r;
  }
  if (a_toks.empty() || b_toks.empty()) {
    r.score = 0.0;
    r.matched_tokens = 0;
    r.ok = false;
    return r;
  }

  const std::size_t n = std::max(a_toks.size(), b_toks.size());
  std::vector<std::vector<std::size_t>> lcs(a_toks.size() + 1,
                                            std::vector<std::size_t>(b_toks.size() + 1, 0));
  for (std::size_t i = 1; i <= a_toks.size(); ++i) {
    for (std::size_t j = 1; j <= b_toks.size(); ++j) {
      if (a_toks[i - 1] == b_toks[j - 1]) {
        lcs[i][j] = lcs[i - 1][j - 1] + 1;
      } else {
        lcs[i][j] = std::max(lcs[i - 1][j], lcs[i][j - 1]);
      }
    }
  }
  r.matched_tokens = lcs[a_toks.size()][b_toks.size()];
  const double denom = static_cast<double>(n);
  r.score = denom > 0.0 ? static_cast<double>(r.matched_tokens) / denom : 0.0;
  r.ok = r.score >= options_.min_score;
  return r;
}

}
