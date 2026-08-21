#include <inference/domain.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

}

TEST_CASE("model and runtime sources contain no HTTP protocol dependency") {
    const auto root = std::filesystem::path{INFERDECK_SOURCE_DIR};
    const std::array directories{
        root / "libs/inference/include",
        root / "libs/model/include",
        root / "libs/model/src",
        root / "libs/llama_cpp_wrapper/include",
        root / "libs/llama_cpp_wrapper/src",
    };
    const std::array forbidden{
        "httplib",
        "gateway/routes",
        "openai_body_json",
        "chat.completion",
        "stream_options",
    };

    for (const auto& directory : directories) {
        REQUIRE(std::filesystem::exists(directory));
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (!entry.is_regular_file()) continue;
            const auto extension = entry.path().extension();
            if (extension != ".hpp" && extension != ".cpp") continue;
            const auto source = read_file(entry.path());
            for (const auto term : forbidden) {
                INFO(entry.path().string());
                INFO(term);
                CHECK(source.find(term) == std::string::npos);
            }
        }
    }
}
