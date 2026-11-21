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

namespace {

using decision_runtime::render_question_v2;
using decision_runtime::render_v2_block_tokens;

// Blocks must tile [0, ids) contiguously, stay within the block size, and split exactly at the state end.
void check_tiling(const decision_runtime::RenderedQuestion& rendered) {
    std::size_t at = 0;
    bool split_at_state = false;
    for (const decision_runtime::TokenBlock& block : rendered.blocks) {
        CHECK(block.start == at);
        CHECK(block.stop > block.start);
        CHECK(block.stop - block.start <= render_v2_block_tokens);
        split_at_state = split_at_state || block.stop == rendered.prefix_tokens;
        at = block.stop;
    }
    CHECK(at == rendered.ids.size());
    CHECK(split_at_state);
    for (const std::size_t slot : rendered.slots) {
        CHECK(rendered.ids[slot] == slot_token);
        CHECK(slot >= rendered.prefix_tokens);
    }
}

model::DecisionQuestion two_options(std::string first, std::string second) {
    model::DecisionQuestion question;
    question.kind = model::DecisionKind::Choice;
    question.instructions = "Q";
    question.options = {{std::move(first), ""}, {std::move(second), ""}};
    return question;
}

}

TEST_CASE("Render v2 short form puts every option on a slot line in one question block", "[decision][render][v2]") {
    model::DecisionQuestion choice;
    choice.kind = model::DecisionKind::Choice;
    choice.instructions = "Which team?";
    choice.options = {{"billing", "payments and refunds"}, {"support", ""}};
    const auto rendered = render_question_v2(bytes, slot_token, "My card was charged twice.", choice, 32768);
    REQUIRE(rendered);
    CHECK(text_of(rendered->ids) ==
          "State:\nMy card was charged twice.\n\n"
          "Question [choice]: Which team?\nOptions:\n"
          "- billing: payments and refunds ->\n- support ->\n");
    CHECK(rendered->prefix_tokens == std::string("State:\nMy card was charged twice.\n\n").size());
    REQUIRE(rendered->blocks.size() == 2);
    CHECK(rendered->blocks[1] == decision_runtime::TokenBlock{rendered->prefix_tokens, rendered->ids.size()});
    REQUIRE(rendered->slots.size() == 2);
    check_tiling(*rendered);

    model::DecisionQuestion predicate;
    predicate.instructions = "Is the customer angry?";
    const auto yes_no = render_question_v2(bytes, slot_token, "", predicate, 32768);
    REQUIRE(yes_no);
    CHECK(text_of(yes_no->ids) ==
          "State:\n\n\nQuestion [noul]: Is the customer angry?\nOptions:\n"
          "- false: no, the statement does not hold ->\n- true: yes, the statement holds ->\n");
    check_tiling(*yes_no);
}

TEST_CASE("Render v2 cuts a long state into 2048-token blocks", "[decision][render][v2]") {
    const std::string input(5000, 'x');
    const auto rendered = render_question_v2(bytes, slot_token, input, two_options("a", "b"), 32768);
    REQUIRE(rendered);
    const std::size_t state = std::string("State:\n\n\n").size() + input.size();
    CHECK(rendered->prefix_tokens == state);
    REQUIRE(rendered->blocks.size() == 4);
    CHECK(rendered->blocks[0] == decision_runtime::TokenBlock{0, 2048});
    CHECK(rendered->blocks[1] == decision_runtime::TokenBlock{2048, 4096});
    CHECK(rendered->blocks[2] == decision_runtime::TokenBlock{4096, state});
    check_tiling(*rendered);
}

TEST_CASE("Render v2 switches to the catalogue form only past one block", "[decision][render][v2]") {
    // head + "- " + option + " ->" + "\n" twice: exactly 2048 tokens stays in the short form.
    const std::size_t head = std::string("Question [choice]: Q\nOptions:\n").size();
    const std::size_t fill = render_v2_block_tokens - head - 8;
    const auto fits = render_question_v2(bytes, slot_token, "s", two_options("a", std::string(fill - 1, 'b')), 32768);
    REQUIRE(fits);
    CHECK(fits->ids.size() - fits->prefix_tokens == render_v2_block_tokens);
    CHECK(fits->blocks.size() == 2);
    check_tiling(*fits);

    const std::string long_option(fill, 'b');
    const auto overflow = render_question_v2(bytes, slot_token, "s", two_options("a", long_option), 32768);
    REQUIRE(overflow);
    const std::string catalogue = "Question [choice]: Q\nOptions:\nOption 1: a\nOption 2: " + long_option + "\n";
    const std::string rubric = "Judge each numbered option in the complete catalogue above:\nOption 1 ->\nOption 2 ->\n";
    CHECK(text_of(overflow->ids) == "State:\ns\n\n" + catalogue + rubric);
    const std::size_t catalogue_end = overflow->prefix_tokens + catalogue.size();
    REQUIRE(overflow->blocks.size() == 4);
    CHECK(overflow->blocks[1] == decision_runtime::TokenBlock{overflow->prefix_tokens, overflow->prefix_tokens + 2048});
    CHECK(overflow->blocks[2] == decision_runtime::TokenBlock{overflow->prefix_tokens + 2048, catalogue_end});
    CHECK(overflow->blocks[3] == decision_runtime::TokenBlock{catalogue_end, overflow->ids.size()});
    REQUIRE(overflow->slots.size() == 2);
    CHECK(overflow->slots[0] > catalogue_end);
    check_tiling(*overflow);
}

TEST_CASE("Render v2 rejects inputs over the total limit without truncating", "[decision][render][v2]") {
    const auto too_long = render_question_v2(bytes, slot_token, std::string(500, 'x'), two_options("a", "b"), 200);
    REQUIRE_FALSE(too_long);
    CHECK(too_long.error().code == foundation::ErrorCode::ContextLengthExceeded);
    model::DecisionQuestion one;
    one.kind = model::DecisionKind::Choice;
    one.instructions = "Pick";
    one.options = {{"a", ""}};
    CHECK(render_question_v2(bytes, slot_token, "x", one, 32768).error().code ==
          foundation::ErrorCode::InvalidArgument);
}
