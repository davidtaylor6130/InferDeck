#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace inferdeck::foundation {

enum class ErrorCode {
    Ok = 0,
    InvalidArgument,
    ContextLengthExceeded,
    NotFound,
    NotLoaded,
    AlreadyExists,
    IoError,
    ParseError,
    OutOfMemory,
    ResourceBusy,
    Timeout,
    Cancelled,
    Unavailable,
    Internal,
};

struct Error {
    ErrorCode code{ErrorCode::Ok};
    std::string message{};
    std::string field{};

    constexpr Error() = default;
    constexpr Error(ErrorCode c, std::string m, std::string f = {})
        : code(c), message(std::move(m)), field(std::move(f)) {}

    [[nodiscard]] constexpr bool ok() const noexcept { return code == ErrorCode::Ok; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return ok(); }
};

template <typename T>
using Result = std::expected<T, Error>;

template <typename T = void>
[[nodiscard]] constexpr auto Ok(T&& value) {
    return std::expected<std::decay_t<T>, Error>(std::forward<T>(value));
}

[[nodiscard]] constexpr auto Ok() {
    return std::expected<void, Error>{};
}

template <typename T = void>
[[nodiscard]] constexpr auto Err(ErrorCode code, std::string message,
                                 std::string field = {}) {
    if constexpr (std::is_void_v<T>) {
        return std::expected<void, Error>(
            std::unexpect,
            Error{code, std::move(message), std::move(field)});
    } else {
        return std::expected<T, Error>(
            std::unexpect,
            Error{code, std::move(message), std::move(field)});
    }
}

} // namespace inferdeck::foundation
