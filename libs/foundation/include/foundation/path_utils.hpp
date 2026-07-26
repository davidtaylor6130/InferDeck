#pragma once

#include <cstdlib>
#include <cwchar>
#include <filesystem>

namespace inferdeck::foundation {

inline bool path_component_equal(const std::filesystem::path& left,
                                 const std::filesystem::path& right) {
#ifdef _WIN32
    return ::_wcsicmp(left.native().c_str(), right.native().c_str()) == 0;
#else
    return left == right;
#endif
}

inline bool is_path_within(const std::filesystem::path& root,
                           const std::filesystem::path& candidate) {
    if (root.empty() || candidate.empty()) return false;
    const auto normalized_root = root.lexically_normal();
    const auto normalized_candidate = candidate.lexically_normal();
    if (normalized_root.is_absolute() != normalized_candidate.is_absolute()) return false;

    auto root_part = normalized_root.begin();
    auto candidate_part = normalized_candidate.begin();
    while (root_part != normalized_root.end()) {
        if (candidate_part == normalized_candidate.end() ||
            !path_component_equal(*root_part, *candidate_part)) {
            return false;
        }
        ++root_part;
        ++candidate_part;
    }
    return true;
}

inline std::filesystem::path expand_user_path(const std::filesystem::path& path) {
    if (path.empty()) return path;
    auto component = path.begin();
    if (component == path.end() || *component != "~") return path;

    std::filesystem::path user_directory;
#ifdef _WIN32
    const auto environment_path = [](const wchar_t* name) {
        wchar_t* value = nullptr;
        std::size_t length = 0;
        if (::_wdupenv_s(&value, &length, name) != 0 || !value) {
            return std::filesystem::path{};
        }
        const std::filesystem::path result =
            length > 1 ? std::filesystem::path(value) : std::filesystem::path{};
        std::free(value);
        return result;
    };
    user_directory = environment_path(L"USERPROFILE");
    if (user_directory.empty()) {
        user_directory = environment_path(L"HOME");
    }
#else
    if (const auto* value = std::getenv("HOME"); value && *value) {
        user_directory = value;
    }
#endif
    if (user_directory.empty()) return path;

    ++component;
    for (; component != path.end(); ++component) {
        user_directory /= *component;
    }
    return user_directory;
}

}
