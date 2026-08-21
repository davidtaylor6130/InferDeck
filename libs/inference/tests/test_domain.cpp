#include <inference/domain.hpp>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

using inferdeck::inference::GenerationRequest;
using inferdeck::inference::RequestOutcome;

static_assert(std::is_default_constructible_v<GenerationRequest>);
static_assert(std::is_copy_constructible_v<RequestOutcome>);

TEST_CASE() {
    GenerationRequest request;
    request.messages.emplace_back(
        inferdeck::inference::MessageRole::Developer, std::string{});
    request.tools.emplace_back();
    CHECK(request.messages.size() == 1);
    CHECK(request.tools.size() == 1);

    inferdeck::inference::OutputEvent output =
        inferdeck::inference::UsageOutput{10, 4, 2};
    CHECK(std::holds_alternative<inferdeck::inference::UsageOutput>(output));
}
