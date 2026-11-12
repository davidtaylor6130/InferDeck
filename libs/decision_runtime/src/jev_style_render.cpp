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
    const std::size_t option_count =
        question.kind == model::DecisionKind::Predicate ? 2 : question.options.size();
    const std::size_t maximum = question.kind == model::DecisionKind::Score ? 10 : 255;
    if (question.instructions.empty() || option_count < 2 || option_count > maximum)
    {
        return foundation::Err<RenderedQuestion>(
            ErrorCode::InvalidArgument,
            "a question needs instructions and 2 to " + std::to_string(maximum) + " options");
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
    rendered.ids = encode("State:\n");
    append(rendered.ids, encode(input));
    append(rendered.ids, encode("\n\n"));
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
