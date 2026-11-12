#include "decision_runtime/jev_style.hpp"

#include <algorithm>
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

/// <summary>Context size the reference scorer uses: the 25,600-token input limit plus headroom.</summary>
constexpr int context_tokens = 32768;

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
/// Scores Jev-Style questions on one llama.cpp context. Every question is
/// decoded from scratch with logits requested only at its verdict slots,
/// which is the reference scorer's exact (unshared) path.
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
        llama_context_params context_params = llama_context_default_params();
        context_params.n_ctx = context_tokens;
        context_params.n_batch = context_tokens;
        context_params.n_ubatch = 1024;
        context_params.n_seq_max = 1;
        context_params.n_outputs_max = 256;
        context_params.kv_unified = true;
        context_params.no_perf = true;
        context_params.n_threads = static_cast<int>(std::max(1U, std::thread::hardware_concurrency() / 2));
        context_params.n_threads_batch = context_params.n_threads;
        context_ = llama_init_from_model(model_, context_params);
        if (!context_)
        {
            release();
            return foundation::Err<void>(ErrorCode::OutOfMemory, "decision context could not be created");
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
            foundation::Result<RenderedQuestion> one =
                render_question(encoder, slot_token_, request.input, question, limits_);
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
        model::DecisionResult result;
        for (std::size_t index = 0; index < rendered.size(); ++index)
        {
            if (cancelled && cancelled())
            {
                return foundation::Err<model::DecisionResult>(ErrorCode::Cancelled, "decision request cancelled");
            }
            foundation::Result<std::vector<double>> scores = score(rendered[index]);
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
    foundation::Result<void> read_readout_config()
    {
        const auto configured = info_.artifacts.find("readout_config");
        const std::filesystem::path path = configured != info_.artifacts.end()
            ? std::filesystem::path(configured->second)
            : std::filesystem::path(info_.gguf_path).parent_path() / "readout_config.json";
        std::ifstream file(path);
        const json config = json::parse(file, nullptr, false);
        if (!config.is_object() || config.value("format", "") != "macjev-readout-v1" ||
            config.value("template", "") != "macjev-render-v1" || config.value("readout", "") != "verdict")
        {
            return foundation::Err<void>(
                ErrorCode::InvalidArgument,
                "readout config is missing or is not macjev-readout-v1 / macjev-render-v1: " + path.string());
        }
        try
        {
            const json& tokens = config.at("slot_tokens");
            yes_token_ = tokens.at("yes").at("id").get<std::int32_t>();
            no_token_ = tokens.at("no").at("id").get<std::int32_t>();
            slot_token_ = tokens.at("verdict_slot").at("id").get<std::int32_t>();
            temperatures_ = config.at("temperatures");
            if (config.contains("budgets"))
            {
                limits_.max_tokens = config["budgets"].value("max_len", limits_.max_tokens);
                limits_.head_tokens = config["budgets"].value("head_max", limits_.head_tokens);
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
    /// Fitted temperature for free-form typed questions ("typed" family) of
    /// this kind and option count, or the global temperature when that group
    /// was not fitted. This is the category the reference server applies.
    /// </summary>
    double temperature(model::DecisionKind kind, std::size_t option_count) const
    {
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

    /// <summary>logit(" yes") - logit(" no") at every verdict slot of one rendered question.</summary>
    foundation::Result<std::vector<double>> score(const RenderedQuestion& question)
    {
        llama_memory_clear(llama_get_memory(context_), true);
        const int count = static_cast<int>(question.ids.size());
        llama_batch batch = llama_batch_init(count, 0, 1);
        batch.n_tokens = count;
        for (int index = 0; index < count; ++index)
        {
            batch.token[index] = question.ids[static_cast<std::size_t>(index)];
            batch.pos[index] = index;
            batch.n_seq_id[index] = 1;
            batch.seq_id[index][0] = 0;
            batch.logits[index] = 0;
        }
        for (const std::size_t slot : question.slots)
        {
            batch.logits[slot] = 1;
        }
        const int status = llama_decode(context_, batch);
        llama_batch_free(batch);
        if (status != 0)
        {
            return foundation::Err<std::vector<double>>(
                ErrorCode::Internal, "llama_decode failed with code " + std::to_string(status));
        }
        std::vector<double> scores;
        for (const std::size_t slot : question.slots)
        {
            const float* logits = llama_get_logits_ith(context_, static_cast<int>(slot));
            if (!logits)
            {
                return foundation::Err<std::vector<double>>(ErrorCode::Internal, "no logits at a verdict slot");
            }
            scores.push_back(static_cast<double>(logits[yes_token_]) - static_cast<double>(logits[no_token_]));
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
