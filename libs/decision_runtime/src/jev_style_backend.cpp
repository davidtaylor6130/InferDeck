// Ports the Jev-Style scorers jev-score (0.8B v3) and jev-score-v2 (2B v4)
// onto in-process libllama (chaoliangUNSW, Apache-2.0).

#include "decision_runtime/jev_style.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "foundation/logging.hpp"
#include "llama.h"
#include "model/fixed_backend.hpp"

namespace inferdeck::decision_runtime {

namespace {

using foundation::ErrorCode;
using json = nlohmann::json;

/// <summary>Context size the reference scorers use: the largest input limit (25,600 or 32,768 tokens) fits.</summary>
constexpr int context_tokens = 32768;

/// <summary>Most verdict slots render v2 can put in one block: every slot line is at least 4 tokens.</summary>
constexpr int v2_outputs_max = 512;

/// <summary>Input layout and scoring protocol named by readout_config.json.</summary>
enum class Layout
{
    /// <summary>macjev-render-v1: one causal decode per question (0.8B v3).</summary>
    RenderV1,
    /// <summary>macjev-render-v2-long-options: block-causal decodes on a shared state (2B v4).</summary>
    RenderV2,
};

model::ModelInfo decision_info(model::ModelInfo info)
{
    info.runtime = "jev_style";
    info.modality = "decision";
    info.capabilities = {"decisions"};
    // ponytail: one context, one request at a time. Add sequences per slot if decisions queue up.
    info.n_slots = 1;
    info.min_slots = 1;
    return info;
}

/// <summary>
/// Scores Jev-Style questions on one llama.cpp context. Render v1 decodes every
/// question from scratch in one causal batch. Render v2 uses a NON_CAUSAL context
/// with n_batch = n_ubatch = 2048 and one llama_decode per renderer block, which
/// gives block-causal attention: the state blocks go into sequence 0 once per
/// request, and each question is decoded into sequence 1 on a copy of it.
/// </summary>
class JevStyleBackend final : public model::FixedBackend, public model::IDecisionBackend
{
public:
    explicit JevStyleBackend(model::ModelInfo info) : FixedBackend(decision_info(std::move(info))) {}

    ~JevStyleBackend() override
    {
        std::lock_guard lock(mutex_);
        release();
    }

    foundation::Result<void> load() override
    {
        std::lock_guard lock(mutex_);
        set_loaded(false);
        release();
        const foundation::Result<void> configured = read_readout_config();
        if (!configured)
        {
            return configured;
        }
        llama_model_params model_params = llama_model_default_params();
        model_params.n_gpu_layers = info_.n_gpu_layers.value_or(999);
        model_ = llama_model_load_from_file(info_.gguf_path.c_str(), model_params);
        if (!model_)
        {
            return foundation::Err<void>(ErrorCode::Unavailable,
                                         "decision model could not be loaded: " + info_.gguf_path);
        }
        vocab_ = llama_model_get_vocab(model_);
        for (const auto& [text, expected] : {std::pair<const char*, std::int32_t>{" yes", yes_token_},
                                             {" no", no_token_}, {" ->", slot_token_}})
        {
            const std::vector<std::int32_t> actual = encode(text);
            if (actual != std::vector<std::int32_t>{expected})
            {
                release();
                return foundation::Err<void>(
                    ErrorCode::InvalidArgument,
                    std::string("tokenizer does not match readout_config.json for \"") + text + "\"");
            }
        }
        // The 0.8B and 2B GGUFs share a tokenizer, so only the name tells a wrong pairing apart.
        if (layout_ == Layout::RenderV2)
        {
            std::array<char, 256> name{};
            llama_model_meta_val_str(model_, "general.name", name.data(), name.size());
            if (model_name_ != name.data())
            {
                release();
                return foundation::Err<void>(
                    ErrorCode::InvalidArgument,
                    "GGUF general.name is \"" + std::string(name.data()) + "\" but readout_config.json is for \"" +
                        model_name_ + "\"; other models give plausible but wrong probabilities");
            }
        }
        llama_context_params context_params = llama_context_default_params();
        context_params.n_ctx = context_tokens;
        context_params.kv_unified = true;
        context_params.no_perf = true;
        context_params.n_threads = static_cast<int>(std::max(1U, std::thread::hardware_concurrency() / 2));
        context_params.n_threads_batch = context_params.n_threads;
        if (layout_ == Layout::RenderV1)
        {
            context_params.n_batch = context_tokens;
            context_params.n_ubatch = 1024;
            context_params.n_seq_max = 1;
            context_params.n_outputs_max = 256;
        }
        else
        {
            context_params.n_batch = static_cast<std::uint32_t>(render_v2_block_tokens);
            context_params.n_ubatch = static_cast<std::uint32_t>(render_v2_block_tokens);
            context_params.n_seq_max = 2;
            context_params.n_outputs_max = v2_outputs_max;
            context_params.attention_type = LLAMA_ATTENTION_TYPE_NON_CAUSAL;
            // As the reference: flash attention auto on a GPU, off on the CPU fallback (precision).
            context_params.flash_attn_type = model_params.n_gpu_layers > 0 && llama_supports_gpu_offload()
                ? LLAMA_FLASH_ATTN_TYPE_AUTO : LLAMA_FLASH_ATTN_TYPE_DISABLED;
        }
        context_ = llama_init_from_model(model_, context_params);
        if (!context_)
        {
            release();
            return foundation::Err<void>(ErrorCode::OutOfMemory, "decision context could not be created");
        }
        if (layout_ == Layout::RenderV2 &&
            (llama_n_batch(context_) != render_v2_block_tokens || llama_n_ubatch(context_) != render_v2_block_tokens))
        {
            release();
            return foundation::Err<void>(ErrorCode::Internal,
                                         "decision context did not keep n_batch = n_ubatch = 2048");
        }
        set_loaded(true);
        return foundation::Ok();
    }

    foundation::Result<void> unload() override
    {
        std::lock_guard lock(mutex_);
        set_loaded(false);
        release();
        return foundation::Ok();
    }

    foundation::Result<model::DecisionResult> decide(
        int, const model::DecisionRequest& request,
        const std::function<bool()>& cancelled) override
    {
        std::lock_guard lock(mutex_);
        if (!context_)
        {
            return foundation::Err<model::DecisionResult>(ErrorCode::NotLoaded, "decision model is not loaded");
        }
        const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        const Encode encoder = [this](std::string_view text) { return encode(text); };
        // Render everything first so a budget error fails the request before any scoring.
        std::vector<RenderedQuestion> rendered;
        for (const model::DecisionQuestion& question : request.questions)
        {
            foundation::Result<RenderedQuestion> one = layout_ == Layout::RenderV1
                ? render_question(encoder, slot_token_, request.input, question, limits_)
                : render_question_v2(encoder, slot_token_, request.input, question, limits_.max_tokens);
            if (!one)
            {
                return foundation::Err<model::DecisionResult>(one.error().code, one.error().message);
            }
            if (one->slots.size() > 256)
            {
                return foundation::Err<model::DecisionResult>(ErrorCode::InvalidArgument,
                                                              "a question may have at most 256 options");
            }
            rendered.push_back(std::move(*one));
        }
        if (layout_ == Layout::RenderV2 && !rendered.empty())
        {
            const foundation::Result<void> state = decode_state(rendered.front());
            if (!state)
            {
                return foundation::Err<model::DecisionResult>(state.error().code, state.error().message);
            }
        }
        model::DecisionResult result;
        for (std::size_t index = 0; index < rendered.size(); ++index)
        {
            if (cancelled && cancelled())
            {
                return foundation::Err<model::DecisionResult>(ErrorCode::Cancelled, "decision request cancelled");
            }
            foundation::Result<std::vector<double>> scores =
                layout_ == Layout::RenderV1 ? score(rendered[index]) : score_on_state(rendered[index]);
            if (!scores)
            {
                return foundation::Err<model::DecisionResult>(scores.error().code, scores.error().message);
            }
            foundation::Result<std::vector<double>> probabilities = calibrated_probabilities(
                *scores, temperature(request.questions[index].kind, scores->size()));
            if (!probabilities)
            {
                return foundation::Err<model::DecisionResult>(probabilities.error().code,
                                                              probabilities.error().message);
            }
            result.answers.push_back({std::move(*probabilities)});
            result.input_tokens += static_cast<int>(rendered[index].ids.size());
        }
        result.duration_ms = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return foundation::Ok(std::move(result));
    }

private:
    /// <summary>
    /// Reads readout_config.json and selects the layout from its format,
    /// template, and readout fields. Anything else fails the load.
    /// </summary>
    foundation::Result<void> read_readout_config()
    {
        const auto configured = info_.artifacts.find("readout_config");
        const std::filesystem::path path = configured != info_.artifacts.end()
            ? std::filesystem::path(configured->second)
            : std::filesystem::path(info_.gguf_path).parent_path() / "readout_config.json";
        std::ifstream file(path);
        const json config = json::parse(file, nullptr, false);
        if (!config.is_object())
        {
            return foundation::Err<void>(ErrorCode::InvalidArgument,
                                         "readout config is missing or is not JSON: " + path.string());
        }
        const std::string format = config.value("format", "");
        const std::string layout_name = config.value("template", "");
        if (format == "macjev-readout-v1" && layout_name == "macjev-render-v1" &&
            config.value("readout", "") == "verdict")
        {
            layout_ = Layout::RenderV1;
        }
        else if (format == "macjev-readout-v2" && layout_name == "macjev-render-v2-long-options" &&
                 config.value("readout", "") == "verdict" && config.value("layout", "") == "sb" &&
                 config.value("block_size", 0) == static_cast<int>(render_v2_block_tokens) &&
                 config.value("total_context_limit", 0) == context_tokens)
        {
            layout_ = Layout::RenderV2;
        }
        else
        {
            return foundation::Err<void>(
                ErrorCode::InvalidArgument,
                "unsupported decision template \"" + layout_name + "\" (format \"" + format +
                    "\") in " + path.string() + "; supported: macjev-render-v1, macjev-render-v2-long-options");
        }
        try
        {
            const json& tokens = config.at("slot_tokens");
            yes_token_ = tokens.at("yes").at("id").get<std::int32_t>();
            no_token_ = tokens.at("no").at("id").get<std::int32_t>();
            slot_token_ = tokens.at("verdict_slot").at("id").get<std::int32_t>();
            temperatures_ = config.at("temperatures");
            model_name_ = config.value("model_name", "");
            limits_ = {};
            if (config.contains("budgets"))
            {
                limits_.max_tokens = config["budgets"].value("max_len", limits_.max_tokens);
                limits_.head_tokens = config["budgets"].value("head_max", limits_.head_tokens);
            }
            if (layout_ == Layout::RenderV2)
            {
                limits_.max_tokens = std::min<std::size_t>(limits_.max_tokens, context_tokens);
                for (const char* type : {"choice", "noul", "score"})
                {
                    const double value = temperatures_.at("by_qtype").at(type).get<double>();
                    if (!std::isfinite(value) || value <= 0.0)
                    {
                        return foundation::Err<void>(ErrorCode::InvalidArgument,
                                                     std::string("readout config temperature for ") + type +
                                                         " must be finite and positive");
                    }
                }
            }
        }
        catch (const json::exception& error)
        {
            return foundation::Err<void>(ErrorCode::InvalidArgument,
                                         std::string("readout config is incomplete: ") + error.what());
        }
        return foundation::Ok();
    }

    /// <summary>
    /// Render v2: the fitted temperature of the question type
    /// (temperatures.by_qtype), unclamped as the reference applies it.
    /// Render v1: the temperature of free-form typed questions ("typed"
    /// family) of this kind and option count, or the global temperature when
    /// that group was not fitted, clamped to temperatures.clamp. Both are what
    /// the reference runtimes apply.
    /// </summary>
    double temperature(model::DecisionKind kind, std::size_t option_count) const
    {
        if (layout_ == Layout::RenderV2)
        {
            return temperatures_["by_qtype"][std::string(template_type(kind))].get<double>();
        }
        const std::string group = "typed|" + std::string(template_type(kind)) + "|" +
                                  std::string(option_bucket(option_count));
        double value = temperatures_.value("global", 1.0);
        const json groups = temperatures_.value("groups", json::object());
        if (groups.contains(group) && groups[group].contains("T"))
        {
            value = groups[group]["T"].get<double>();
        }
        const std::vector<double> clamp = temperatures_.value("clamp", std::vector<double>{0.3, 5.0});
        return clamp.size() == 2 ? std::clamp(value, clamp[0], clamp[1]) : value;
    }

    std::vector<std::int32_t> encode(std::string_view text) const
    {
        std::vector<std::int32_t> tokens(text.size() + 8);
        const int count = llama_tokenize(vocab_, text.data(), static_cast<int>(text.size()),
                                         tokens.data(), static_cast<int>(tokens.size()), false, false);
        tokens.resize(static_cast<std::size_t>(std::max(0, count)));
        return tokens;
    }

    /// <summary>
    /// Decodes ids[block.start, block.stop) at those positions into one sequence
    /// with one llama_decode, requesting logits only at <paramref name="slots"/>
    /// inside the block, and appends logit(" yes") - logit(" no") for each of
    /// them to <paramref name="scores"/> (read now: the next decode overwrites them).
    /// </summary>
    foundation::Result<void> decode(const std::vector<std::int32_t>& ids, TokenBlock block, llama_seq_id sequence,
                                    const std::vector<std::size_t>& slots, std::vector<double>& scores)
    {
        const int count = static_cast<int>(block.stop - block.start);
        llama_batch batch = llama_batch_init(count, 0, 1);
        batch.n_tokens = count;
        for (int index = 0; index < count; ++index)
        {
            batch.token[index] = ids[block.start + static_cast<std::size_t>(index)];
            batch.pos[index] = static_cast<llama_pos>(block.start) + index;
            batch.n_seq_id[index] = 1;
            batch.seq_id[index][0] = sequence;
            batch.logits[index] = 0;
        }
        std::vector<int> outputs;
        for (const std::size_t slot : slots)
        {
            if (slot >= block.start && slot < block.stop)
            {
                outputs.push_back(static_cast<int>(slot - block.start));
                batch.logits[outputs.back()] = 1;
            }
        }
        const int status = llama_decode(context_, batch);
        llama_batch_free(batch);
        if (status != 0)
        {
            return foundation::Err<void>(ErrorCode::Internal,
                                         "llama_decode failed with code " + std::to_string(status));
        }
        for (const int output : outputs)
        {
            const float* logits = llama_get_logits_ith(context_, output);
            if (!logits)
            {
                return foundation::Err<void>(ErrorCode::Internal, "no logits at a verdict slot");
            }
            scores.push_back(static_cast<double>(logits[yes_token_]) - static_cast<double>(logits[no_token_]));
        }
        return foundation::Ok();
    }

    /// <summary>Render v1: logit(" yes") - logit(" no") at every verdict slot, decoded from scratch.</summary>
    foundation::Result<std::vector<double>> score(const RenderedQuestion& question)
    {
        llama_memory_clear(llama_get_memory(context_), true);
        std::vector<double> scores;
        const foundation::Result<void> decoded =
            decode(question.ids, {0, question.ids.size()}, 0, question.slots, scores);
        if (!decoded)
        {
            return foundation::Err<std::vector<double>>(decoded.error().code, decoded.error().message);
        }
        return foundation::Ok(std::move(scores));
    }

    /// <summary>
    /// Render v2: clears the memory and decodes the state blocks into sequence 0,
    /// one llama_decode per block. Every question of a request shares this state.
    /// </summary>
    foundation::Result<void> decode_state(const RenderedQuestion& question)
    {
        llama_memory_clear(llama_get_memory(context_), true);
        std::vector<double> unused;
        for (const TokenBlock& block : question.blocks)
        {
            if (block.stop > question.prefix_tokens)
            {
                break;
            }
            const foundation::Result<void> decoded = decode(question.ids, block, 0, {}, unused);
            if (!decoded)
            {
                llama_memory_clear(llama_get_memory(context_), true);
                return decoded;
            }
        }
        return foundation::Ok();
    }

    /// <summary>
    /// Render v2: copies the state (sequence 0) to sequence 1, decodes the
    /// question blocks into it one llama_decode each, reads the slot scores,
    /// and removes sequence 1 again.
    /// </summary>
    foundation::Result<std::vector<double>> score_on_state(const RenderedQuestion& question)
    {
        llama_memory_t memory = llama_get_memory(context_);
        llama_memory_seq_rm(memory, 1, -1, -1);
        llama_memory_seq_cp(memory, 0, 1, -1, -1);
        std::vector<double> scores;
        for (const TokenBlock& block : question.blocks)
        {
            if (block.start < question.prefix_tokens)
            {
                continue;
            }
            const foundation::Result<void> decoded = decode(question.ids, block, 1, question.slots, scores);
            if (!decoded)
            {
                llama_memory_seq_rm(memory, 1, -1, -1);
                return foundation::Err<std::vector<double>>(decoded.error().code, decoded.error().message);
            }
        }
        llama_memory_seq_rm(memory, 1, -1, -1);
        if (scores.size() != question.slots.size())
        {
            return foundation::Err<std::vector<double>>(ErrorCode::Internal, "a verdict slot was not decoded");
        }
        return foundation::Ok(std::move(scores));
    }

    void release() noexcept
    {
        if (context_) llama_free(context_);
        if (model_) llama_model_free(model_);
        context_ = nullptr;
        model_ = nullptr;
        vocab_ = nullptr;
    }

    std::mutex mutex_;
    llama_model* model_{nullptr};
    llama_context* context_{nullptr};
    const llama_vocab* vocab_{nullptr};
    Layout layout_{Layout::RenderV1};
    std::string model_name_;
    std::int32_t yes_token_{-1};
    std::int32_t no_token_{-1};
    std::int32_t slot_token_{-1};
    json temperatures_ = json::object();
    RenderLimits limits_{};
};

}

std::unique_ptr<model::IBackend> make_jev_style_backend(const model::ModelInfo& info)
{
    return std::make_unique<JevStyleBackend>(info);
}

}
