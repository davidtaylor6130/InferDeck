#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "parity/compare.hpp"

using namespace inferdeck::parity;

TEST_CASE("ParityComparator: identical strings score 1.0", "[parity][compare]") {
  ParityComparator c;
  auto r = c.compare("hello world", "hello world");
  REQUIRE(r.score == 1.0);
  REQUIRE(r.ok);
  REQUIRE(r.baseline_tokens == 2);
  REQUIRE(r.candidate_tokens == 2);
  REQUIRE(r.matched_tokens == 2);
}

TEST_CASE("ParityComparator: completely different scores 0.0", "[parity][compare]") {
  ParityComparator c;
  auto r = c.compare("alpha beta gamma", "x y z");
  REQUIRE(r.score == 0.0);
  REQUIRE_FALSE(r.ok);
}

TEST_CASE("ParityComparator: empty both sides scores 1.0", "[parity][compare]") {
  ParityComparator c;
  auto r = c.compare("", "");
  REQUIRE(r.score == 1.0);
  REQUIRE(r.ok);
}

TEST_CASE("ParityComparator: one side empty scores 0.0", "[parity][compare]") {
  ParityComparator c;
  auto r = c.compare("hello", "");
  REQUIRE(r.score == 0.0);
  REQUIRE_FALSE(r.ok);
}

TEST_CASE("ParityComparator: token-level LCS handles reorder", "[parity][compare]") {
  ParityComparator c;
  const std::string a = "the quick brown fox jumps over the lazy dog";
  const std::string b = "the quick brown fox jumps over the lazy dog";
  auto r = c.compare(a, b);
  REQUIRE(r.matched_tokens == 9);
  REQUIRE(r.score == 1.0);
}

TEST_CASE("ParityComparator: partial match gives fractional score", "[parity][compare]") {
  ParityComparator c;
  const std::string a = "the capital of france is paris";
  const std::string b = "the capital of france";
  auto r = c.compare(a, b);
  REQUIRE(r.matched_tokens == 4);
  REQUIRE(r.baseline_tokens == 6);
  REQUIRE(r.candidate_tokens == 4);
  REQUIRE_THAT(r.score, Catch::Matchers::WithinAbs(4.0 / 6.0, 1e-9));
  REQUIRE_FALSE(r.ok);
}

TEST_CASE("ParityComparator: strip_think_blocks removes thinking", "[parity][compare]") {
  ParityOptions opts;
  opts.strip_think_blocks = true;
  ParityComparator c(opts);
  const std::string a = "<think>step 1 step 2 step 3</think>the answer is 42";
  const std::string b = "the answer is 42";
  auto r = c.compare(a, b);
  REQUIRE(r.score == 1.0);
  REQUIRE(r.ok);
}

TEST_CASE("ParityComparator: strip_think_blocks unclosed", "[parity][compare]") {
  ParityOptions opts;
  opts.strip_think_blocks = true;
  ParityComparator c(opts);
  const std::string a = "<think>the answer might be 7\nor maybe 8";
  const std::string b = "the answer is 8";
  auto r = c.compare(a, b);
  REQUIRE(r.matched_tokens >= 1);
}

TEST_CASE("ParityComparator: normalize_whitespace collapses runs", "[parity][compare]") {
  ParityOptions opts;
  opts.normalize_whitespace = true;
  ParityComparator c(opts);
  const std::string a = "hello\n\n  world\t\t  foo";
  const std::string b = "hello world foo";
  auto r = c.compare(a, b);
  REQUIRE(r.score == 1.0);
  REQUIRE(r.ok);
}

TEST_CASE("ParityComparator: case_insensitive makes case not count", "[parity][compare]") {
  ParityOptions opts;
  opts.case_insensitive = true;
  ParityComparator c(opts);
  auto r = c.compare("Hello World", "HELLO world");
  REQUIRE(r.score == 1.0);
  REQUIRE(r.ok);
}

TEST_CASE("ParityComparator: min_score threshold ok flag", "[parity][compare]") {
  ParityOptions opts;
  opts.min_score = 0.50;
  ParityComparator c(opts);
  auto r = c.compare("the answer is forty two", "the answer is 42");
  REQUIRE(r.ok);
  auto r2 = c.compare("foo bar baz", "completely unrelated text");
  REQUIRE_FALSE(r2.ok);
}

TEST_CASE("ParityComparator: tokenize splits on punctuation", "[parity][compare]") {
  ParityComparator c;
  const auto t = c.tokenize("Hello, world! It's me.");
  REQUIRE(t == std::vector<std::string>{"Hello", "world", "It", "s", "me"});
}

TEST_CASE("ParityComparator: long-output quality", "[parity][compare]") {
  ParityComparator c;
  const std::string a = "int add(int a, int b) { return a + b; }";
  const std::string b = "int add(int a, int b) { return a + b; }";
  auto r = c.compare(a, b);
  REQUIRE(r.matched_tokens == 9);
  REQUIRE(r.score == 1.0);
}
