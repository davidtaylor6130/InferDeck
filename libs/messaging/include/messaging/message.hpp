#pragma once

#include <optional>
#include <string>
#include <vector>

#include "messaging/content.hpp"
#include "messaging/role.hpp"

namespace inferdeck::messaging {

struct Message {
    Role role{Role::User};
    std::string name{};
    std::vector<Content> content{};
    std::optional<std::string> tool_call_id{};
    std::optional<std::string> refusal{};
    std::optional<std::string> reasoning{};

    [[nodiscard]] bool empty() const noexcept { return content.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return content.size(); }
};

struct Conversation {
    std::string model{};
    std::vector<Message> messages{};
    std::optional<double> temperature{};
    std::optional<double> top_p{};
    std::optional<int> top_k{};
    std::optional<int> max_tokens{};
    std::optional<bool> stream{};
    std::optional<bool> thinking_mode{};
    std::optional<std::string> reasoning_effort{};

    [[nodiscard]] bool is_empty() const noexcept { return messages.empty(); }
    [[nodiscard]] std::size_t message_count() const noexcept { return messages.size(); }
};

} // namespace inferdeck::messaging
