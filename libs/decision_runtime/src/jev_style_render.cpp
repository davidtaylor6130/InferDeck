// Ports the input layouts and verdict readout of the Jev-Style decision runtimes
// (Jev-Style-0.8B-Decision-v3 and Jev-Style-2B-Decision-v4, chaoliangUNSW, Apache-2.0).

#include "decision_runtime/jev_style.hpp"

#include <algorithm>
#include <cmath>

namespace inferdeck::decision_runtime {

namespace {

void append(std::vector<std::int32_t>& target, const std::vector<std::int32_t>& tokens)
{
    target.insert(target.end(), tokens.begin(), tokens.end());
}

std::string described(const std::string& label, const std::string& description)
{
    return description.empty() ? label : label + ": " + description;
}

/// <summary>Rejects a question without instructions or with an option count its kind does not allow.</summary>
foundation::Result<void> check_question(const model::DecisionQuestion& question)
{
    const std::size_t option_count =
        question.kind == model::DecisionKind::Predicate ? 2 : question.options.size();
    const std::size_t maximum = question.kind == model::DecisionKind::Score ? 10 : 255;
    if (question.instructions.empty() || option_count < 2 || option_count > maximum)
    {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "a question needs instructions and 2 to " + std::to_string(maximum) + " options");
    }
    return foundation::Ok();
}

/// <summary>The state prefix both layouts share: "State:\n" + input + "\n\n", each part encoded alone.</summary>
std::vector<std::int32_t> state_prefix(const Encode& encode, std::string_view input)
{
    std::vector<std::int32_t> ids = encode("State:\n");
    append(ids, encode(input));
    append(ids, encode("\n\n"));
    return ids;
}

/// <summary>Cuts [start, stop) into consecutive blocks of at most render_v2_block_tokens.</summary>
void append_blocks(std::vector<TokenBlock>& blocks, std::size_t start, std::size_t stop)
{
    for (std::size_t at = start; at < stop; at += render_v2_block_tokens)
    {
        blocks.push_back({at, std::min(at + render_v2_block_tokens, stop)});
    }
}

}

std::string_view template_type(model::DecisionKind kind)
{
    switch (kind)
    {
    case model::DecisionKind::Choice: return "choice";
    case model::DecisionKind::Score: return "score";
    default: return "noul";
    }
}

std::vector<std::string> option_lines(const model::DecisionQuestion& question)
{
    std::vector<std::string> lines;
    if (question.kind == model::DecisionKind::Predicate)
    {
        return {"false: no, the statement does not hold", "true: yes, the statement holds"};
    }
    for (std::size_t index = 0; index < question.options.size(); ++index)
    {
        const model::DecisionOption& option = question.options[index];
        lines.push_back(question.kind == model::DecisionKind::Score
            ? "level " + std::to_string(index) + ": " + described(option.label, option.description)
            : described(option.label, option.description));
    }
    return lines;
}

foundation::Result<RenderedQuestion> render_question(
    const Encode& encode, std::int32_t verdict_slot_token, std::string_view input,
    const model::DecisionQuestion& question, const RenderLimits& limits)
{
    using foundation::ErrorCode;
    if (const foundation::Result<void> valid = check_question(question); !valid)
    {
        return foundation::Err<RenderedQuestion>(valid.error().code, valid.error().message);
    }

    std::vector<std::vector<std::int32_t>> options;
    for (const std::string& line : option_lines(question))
    {
        options.push_back(encode(line));
    }
    const std::vector<std::int32_t> newline = encode("\n");
    const std::vector<std::int32_t> dash = encode("- ");

    std::vector<std::int32_t> head = encode(
        "Question [" + std::string(template_type(question.kind)) + "]: " +
        question.instructions + "\nOptions:\n");
    for (const std::vector<std::int32_t>& option : options)
    {
        append(head, dash);
        append(head, option);
        append(head, newline);
    }
    append(head, encode("Judge each option:\n"));
    std::vector<std::size_t> head_slots;
    for (const std::vector<std::int32_t>& option : options)
    {
        append(head, option);
        head.push_back(verdict_slot_token);
        head_slots.push_back(head.size() - 1);
        append(head, newline);
    }
    if (head.size() > limits.head_tokens)
    {
        return foundation::Err<RenderedQuestion>(
            ErrorCode::ContextLengthExceeded,
            "question and options need " + std::to_string(head.size()) + " tokens; the limit is " +
                std::to_string(limits.head_tokens) + ". Shorten them or split the options.");
    }

    RenderedQuestion rendered;
    rendered.ids = state_prefix(encode, input);
    rendered.prefix_tokens = rendered.ids.size();
    append(rendered.ids, head);
    if (rendered.ids.size() > limits.max_tokens)
    {
        return foundation::Err<RenderedQuestion>(
            ErrorCode::ContextLengthExceeded,
            "input needs " + std::to_string(rendered.ids.size()) + " tokens; the limit is " +
                std::to_string(limits.max_tokens) + ". Shorten the input.");
    }
    for (const std::size_t slot : head_slots)
    {
        rendered.slots.push_back(rendered.prefix_tokens + slot);
    }
    return foundation::Ok(std::move(rendered));
}

foundation::Result<RenderedQuestion> render_question_v2(
    const Encode& encode, std::int32_t verdict_slot_token, std::string_view input,
    const model::DecisionQuestion& question, std::size_t max_tokens)
{
    if (const foundation::Result<void> valid = check_question(question); !valid)
    {
        return foundation::Err<RenderedQuestion>(valid.error().code, valid.error().message);
    }
    std::vector<std::vector<std::int32_t>> options;
    for (const std::string& line : option_lines(question))
    {
        options.push_back(encode(line));
    }
    const std::vector<std::int32_t> newline = encode("\n");
    const std::vector<std::int32_t> head = encode(
        "Question [" + std::string(template_type(question.kind)) + "]: " +
        question.instructions + "\nOptions:\n");

    RenderedQuestion rendered;
    rendered.ids = state_prefix(encode, input);
    rendered.prefix_tokens = rendered.ids.size();
    append_blocks(rendered.blocks, 0, rendered.prefix_tokens);

    std::vector<std::int32_t> short_form = head;
    std::vector<std::size_t> short_slots;
    const std::vector<std::int32_t> dash = encode("- ");
    for (const std::vector<std::int32_t>& option : options)
    {
        append(short_form, dash);
        append(short_form, option);
        short_form.push_back(verdict_slot_token);
        short_slots.push_back(short_form.size() - 1);
        append(short_form, newline);
    }
    if (short_form.size() <= render_v2_block_tokens)
    {
        for (const std::size_t slot : short_slots)
        {
            rendered.slots.push_back(rendered.prefix_tokens + slot);
        }
        append(rendered.ids, short_form);
        rendered.blocks.push_back({rendered.prefix_tokens, rendered.ids.size()});
    }
    else
    {
        append(rendered.ids, head);
        for (std::size_t index = 0; index < options.size(); ++index)
        {
            append(rendered.ids, encode("Option " + std::to_string(index + 1) + ": "));
            append(rendered.ids, options[index]);
            append(rendered.ids, newline);
        }
        const std::size_t catalogue_end = rendered.ids.size();
        append_blocks(rendered.blocks, rendered.prefix_tokens, catalogue_end);
        append(rendered.ids, encode("Judge each numbered option in the complete catalogue above:\n"));
        for (std::size_t index = 0; index < options.size(); ++index)
        {
            append(rendered.ids, encode("Option " + std::to_string(index + 1)));
            rendered.ids.push_back(verdict_slot_token);
            rendered.slots.push_back(rendered.ids.size() - 1);
            append(rendered.ids, newline);
        }
        append_blocks(rendered.blocks, catalogue_end, rendered.ids.size());
    }
    if (rendered.ids.size() > max_tokens)
    {
        return foundation::Err<RenderedQuestion>(
            foundation::ErrorCode::ContextLengthExceeded,
            "input needs " + std::to_string(rendered.ids.size()) + " tokens (state " +
                std::to_string(rendered.prefix_tokens) + "); the limit is " + std::to_string(max_tokens) +
                ". Shorten the input, the question, or the options.");
    }
    return foundation::Ok(std::move(rendered));
}

foundation::Result<std::vector<double>> calibrated_probabilities(
    std::span<const double> scores, double temperature)
{
    std::vector<double> probabilities(scores.size());
    double highest = -INFINITY;
    for (std::size_t index = 0; index < scores.size(); ++index)
    {
        probabilities[index] = scores[index] / temperature;
        highest = std::max(highest, probabilities[index]);
    }
    double total = 0.0;
    for (double& value : probabilities)
    {
        value = std::exp(value - highest);
        total += value;
    }
    if (scores.empty() || !std::isfinite(total) || total <= 0.0)
    {
        return foundation::Err<std::vector<double>>(foundation::ErrorCode::Internal,
                                                    "decision scores are not finite");
    }
    for (double& value : probabilities)
    {
        value /= total;
    }
    return foundation::Ok(std::move(probabilities));
}

std::string_view option_bucket(std::size_t option_count)
{
    return option_count <= 2 ? "2" : option_count <= 5 ? "3-5" : option_count <= 10 ? "6-10"
         : option_count <= 20 ? "11-20" : "21+";
}

}
