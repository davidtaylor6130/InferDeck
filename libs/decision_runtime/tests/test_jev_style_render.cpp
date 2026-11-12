#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "decision_runtime/jev_style.hpp"

using namespace inferdeck;
using decision_runtime::render_question;

namespace {

constexpr std::int32_t slot_token = 1000;

// One token per byte, so decoding the ids reproduces the rendered text exactly.
std::vector<std::int32_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

std::string text_of(const std::vector<std::int32_t>& ids) {
    std::string text;
    for (const std::int32_t id : ids) text += id == slot_token ? std::string(" ->") : std::string(1, static_cast<char>(id));
    return text;
}

}

TEST_CASE("Jev-Style questions render in the macjev-render-v1 layout", "[decision][render]") {
    model::DecisionQuestion choice;
    choice.kind = model::DecisionKind::Choice;
    choice.instructions = "Which team?";
    choice.options = {{"billing", "payments and refunds"}, {"support", ""}};
    const auto rendered = render_question(bytes, slot_token, "My card was charged twice.", choice);
    REQUIRE(rendered);
    CHECK(text_of(rendered->ids) ==
          "State:\nMy card was charged twice.\n\n"
          "Question [choice]: Which team?\nOptions:\n"
          "- billing: payments and refunds\n- support\n"
          "Judge each option:\n"
          "billing: payments and refunds ->\nsupport ->\n");
    CHECK(rendered->prefix_tokens == std::string("State:\nMy card was charged twice.\n\n").size());
    REQUIRE(rendered->slots.size() == 2);
    for (const std::size_t slot : rendered->slots) {
        CHECK(rendered->ids[slot] == slot_token);
        CHECK(slot >= rendered->prefix_tokens);
    }

    model::DecisionQuestion predicate;
    predicate.instructions = "Is the customer angry?";
    const auto yes_no = render_question(bytes, slot_token, "x", predicate);
    REQUIRE(yes_no);
    CHECK(text_of(yes_no->ids).ends_with(
        "Question [noul]: Is the customer angry?\nOptions:\n"
        "- false: no, the statement does not hold\n- true: yes, the statement holds\n"
        "Judge each option:\n"
        "false: no, the statement does not hold ->\ntrue: yes, the statement holds ->\n"));

    model::DecisionQuestion score;
    score.kind = model::DecisionKind::Score;
    score.instructions = "How severe?";
    score.options = {{"Cosmetic", ""}, {"Blocking", "cannot work"}};
    CHECK(decision_runtime::option_lines(score) ==
          std::vector<std::string>{"level 0: Cosmetic", "level 1: Blocking: cannot work"});
}

TEST_CASE("Jev-Style rendering rejects oversized and malformed questions without truncating",
          "[decision][render]") {
    model::DecisionQuestion question;
    question.kind = model::DecisionKind::Choice;
    question.instructions = "Pick";
    question.options = {{"a", ""}};
    CHECK(render_question(bytes, slot_token, "x", question).error().code ==
          foundation::ErrorCode::InvalidArgument);
    question.options = {{"a", ""}, {"b", ""}};
    const auto too_long_head = render_question(bytes, slot_token, "x", question, {25600, 20});
    REQUIRE_FALSE(too_long_head);
    CHECK(too_long_head.error().code == foundation::ErrorCode::ContextLengthExceeded);
    const auto too_long_input = render_question(bytes, slot_token, std::string(500, 'x'), question, {200, 2048});
    REQUIRE_FALSE(too_long_input);
    CHECK(too_long_input.error().code == foundation::ErrorCode::ContextLengthExceeded);
    question.kind = model::DecisionKind::Score;
    question.options.assign(11, {"level", ""});
    CHECK_FALSE(render_question(bytes, slot_token, "x", question));
}

TEST_CASE("Decision scores become calibrated probabilities", "[decision][calibration]") {
    const std::vector<double> scores{2.0, 0.0};
    const auto plain = decision_runtime::calibrated_probabilities(scores, 1.0);
    REQUIRE(plain);
    CHECK((*plain)[0] == Catch::Approx(0.880797).epsilon(1e-5));
    CHECK((*plain)[0] + (*plain)[1] == Catch::Approx(1.0));
    const auto softened = decision_runtime::calibrated_probabilities(scores, 2.0);
    REQUIRE(softened);
    CHECK((*softened)[0] == Catch::Approx(0.731059).epsilon(1e-5));
    const std::vector<double> broken{1.0, std::nan("")};
    CHECK_FALSE(decision_runtime::calibrated_probabilities(broken, 1.0));
    CHECK(decision_runtime::option_bucket(2) == "2");
    CHECK(decision_runtime::option_bucket(5) == "3-5");
    CHECK(decision_runtime::option_bucket(21) == "21+");
}
