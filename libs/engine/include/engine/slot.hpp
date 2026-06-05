#pragma once

#include <mutex>
#include <string>

#include "engine/token_sequence.hpp"

namespace inferdeck::engine {

enum class SlotState {
    Idle,
    Busy,
};

class Slot {
public:
    Slot() = default;
    explicit Slot(int id) : id_(id) {}
    Slot(Slot&& other) noexcept;
    Slot& operator=(Slot&& other) noexcept;
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;

    [[nodiscard]] int id() const noexcept { return id_; }

    [[nodiscard]] SlotState state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    [[nodiscard]] bool is_busy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_ == SlotState::Busy;
    }

    void mark_busy() {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = SlotState::Busy;
    }

    void mark_idle() {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = SlotState::Idle;
    }

    [[nodiscard]] TokenSequence prev_tokens() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return prev_tokens_;
    }

    void set_prev_tokens(TokenSequence tokens) {
        std::lock_guard<std::mutex> lock(mutex_);
        prev_tokens_ = std::move(tokens);
    }

    void append_to_prev(const std::vector<int>& tokens) {
        std::lock_guard<std::mutex> lock(mutex_);
        prev_tokens_.append(tokens);
    }

    void trim_prev_to(std::size_t n) {
        std::lock_guard<std::mutex> lock(mutex_);
        prev_tokens_.truncate(n);
    }

    [[nodiscard]] int sequence_length() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(prev_tokens_.size());
    }

    [[nodiscard]] std::string debug_state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return "slot " + std::to_string(id_) + " " +
               (state_ == SlotState::Busy ? "busy" : "idle") +
               " seq=" + std::to_string(prev_tokens_.size());
    }

    void move_from(Slot&& other) noexcept {
        if (this == &other) return;
        std::lock_guard<std::mutex> lock1(mutex_, std::adopt_lock);
        std::lock_guard<std::mutex> lock2(other.mutex_, std::adopt_lock);
        id_ = other.id_;
        state_ = other.state_;
        prev_tokens_ = std::move(other.prev_tokens_);
        other.id_ = -1;
    }

private:
    int id_{-1};
    mutable std::mutex mutex_;
    SlotState state_{SlotState::Idle};
    TokenSequence prev_tokens_{};
};

inline Slot::Slot(Slot&& other) noexcept {
    move_from(std::move(other));
}

inline Slot& Slot::operator=(Slot&& other) noexcept {
    if (this != &other) {
        move_from(std::move(other));
    }
    return *this;
}

} // namespace inferdeck::engine
