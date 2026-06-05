#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace inferdeck::messaging {

enum class Role {
    System,
    Developer,
    User,
    Assistant,
    Tool,
};

[[nodiscard]] inline const char* to_string(Role r) noexcept {
    switch (r) {
        case Role::System:    return "system";
        case Role::Developer: return "developer";
        case Role::User:      return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool:      return "tool";
    }
    return "user";
}

[[nodiscard]] inline std::optional<Role> role_from_string(std::string_view s) noexcept {
    if (s == "system")    return Role::System;
    if (s == "developer") return Role::Developer;
    if (s == "user")      return Role::User;
    if (s == "assistant") return Role::Assistant;
    if (s == "tool")      return Role::Tool;
    return std::nullopt;
}

} // namespace inferdeck::messaging
