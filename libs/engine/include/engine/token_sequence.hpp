#pragma once

#include <cstddef>
#include <vector>

namespace inferdeck::engine {

class TokenSequence {
public:
    TokenSequence() = default;
    explicit TokenSequence(std::vector<int> tokens) : tokens_(std::move(tokens)) {}

    [[nodiscard]] const std::vector<int>& tokens() const noexcept { return tokens_; }
    [[nodiscard]] std::size_t size() const noexcept { return tokens_.size(); }
    [[nodiscard]] bool empty() const noexcept { return tokens_.empty(); }
    [[nodiscard]] int at(std::size_t i) const { return tokens_.at(i); }

    void append(int token) { tokens_.push_back(token); }
    void append(const std::vector<int>& more) {
        tokens_.insert(tokens_.end(), more.begin(), more.end());
    }
    void truncate(std::size_t n) {
        if (n < tokens_.size()) tokens_.resize(n);
    }
    void clear() { tokens_.clear(); }

    [[nodiscard]] int lcp_with(const TokenSequence& other) const noexcept {
        const std::size_t n = std::min(tokens_.size(), other.tokens_.size());
        std::size_t i = 0;
        while (i < n && tokens_[i] == other.tokens_[i]) ++i;
        return static_cast<int>(i);
    }

    [[nodiscard]] bool operator==(const TokenSequence& other) const noexcept {
        return tokens_ == other.tokens_;
    }

private:
    std::vector<int> tokens_;
};

} // namespace inferdeck::engine
