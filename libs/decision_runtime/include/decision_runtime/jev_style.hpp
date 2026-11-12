#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "foundation/result.hpp"
#include "model/imodel.hpp"

/// <summary>
/// In-process runtime for Jev-Style decision models (macjev-render-v1,
/// verdict readout). The model does not generate text: each option is followed
/// by a verdict slot, and the option's score is logit(" yes") - logit(" no")
/// at that slot. See docs/decisions.md.
/// </summary>
namespace inferdeck::decision_runtime {

/// <summary>Tokenises plain text with special tokens disabled and no BOS/EOS.</summary>
using Encode = std::function<std::vector<std::int32_t>(std::string_view)>;

struct RenderedQuestion
{
    std::vector<std::int32_t> ids;
    /// <summary>Number of leading tokens that hold the input ("state").</summary>
    std::size_t prefix_tokens{0};
    /// <summary>Position in <c>ids</c> of each option's verdict slot, in option order.</summary>
    std::vector<std::size_t> slots;
};

struct RenderLimits
{
    /// <summary>Whole rendered input.</summary>
    std::size_t max_tokens{25600};
    /// <summary>Question, options, and readout lines.</summary>
    std::size_t head_tokens{2048};
};

/// <summary>Template name of a question kind: "noul", "choice", or "score".</summary>
std::string_view template_type(model::DecisionKind kind);

/// <summary>The option texts the model sees, in probability order.</summary>
std::vector<std::string> option_lines(const model::DecisionQuestion& question);

/// <summary>
/// Renders one question in the layout the model was trained on. Segments are
/// encoded separately and concatenated, as the reference renderer does, so
/// token boundaries match training. Nothing is truncated: an input over a
/// limit is an error.
/// </summary>
foundation::Result<RenderedQuestion> render_question(
    const Encode& encode, std::int32_t verdict_slot_token, std::string_view input,
    const model::DecisionQuestion& question, const RenderLimits& limits = {});

/// <summary>softmax(scores / temperature). Fails on non-finite scores.</summary>
foundation::Result<std::vector<double>> calibrated_probabilities(
    std::span<const double> scores, double temperature);

/// <summary>Option-count bucket used to look up a fitted temperature.</summary>
std::string_view option_bucket(std::size_t option_count);

std::unique_ptr<model::IBackend> make_jev_style_backend(const model::ModelInfo& info);

}
