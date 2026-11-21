// Reference v2 provider built with the provider SDK. Copy this file to start a
// C++ runtime: implement the backend interfaces, then INFERDECK_PROVIDER_SDK.
//
// Runtime "echo". Text echoes the last user message one word per streamed
// delta, with reasoning when enabled, a tool call when the message starts with
// "call ", per-token logprobs when requested, request progress, and one
// "[image N bytes]" note per image input. Embeddings and decisions are
// deterministic. See docs/providers.md.
#include <algorithm>
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "model/imodel.hpp"
#include "provider/sdk.hpp"

namespace
{

using namespace inferdeck;
using foundation::ErrorCode;

class EchoBackend final : public model::IModel, public model::IEmbeddingBackend, public model::IDecisionBackend
{
public:
    explicit EchoBackend(model::ModelInfo info) : info_(std::move(info)), busy_(std::max(1, info_.n_slots), false)
    {
        meta_.supports_thinking = true;
        meta_.thinking_start_tag = "<think>";
        meta_.thinking_end_tags = {"</think>"};
    }

    const model::ModelInfo& info() const override { return info_; }

    foundation::Result<void> load() override
    {
        if (info_.artifacts.contains("fail_load"))
        {
            return foundation::Err<void>(ErrorCode::Unavailable, "echo provider was asked to fail its load");
        }
        loaded_ = true;
        cancel_all_ = false;
        return foundation::Ok();
    }

    foundation::Result<void> unload() override
    {
        std::lock_guard lock(mutex_);
        loaded_ = false;
        std::fill(busy_.begin(), busy_.end(), false);
        return foundation::Ok();
    }

    bool is_loaded() const override { return loaded_; }
    int vram_usage_mb() const override { return loaded_ ? info_.vram_required_mb : 0; }
    int n_slots() const override { std::lock_guard lock(mutex_); return static_cast<int>(busy_.size()); }
    int n_free_slots() const override
    {
        std::lock_guard lock(mutex_);
        return static_cast<int>(std::count(busy_.begin(), busy_.end(), false));
    }
    int min_slots() const override { return 1; }
    int context_pool_capacity() const override { return n_slots() * info_.context_size; }
    bool can_resize_slots() const override { return true; }
    int estimate_vram_mb(int slots) const override { return info_.vram_required_mb + 10 * slots; }
    int additional_vram_reserve_mb() const override { return 7; }
    bool live_vram_accounting_complete() const override { return true; }

    foundation::Result<void> resize_slots(int slots) override
    {
        std::lock_guard lock(mutex_);
        if (slots < 1 || std::count(busy_.begin(), busy_.end(), true) > 0)
        {
            return foundation::Err<void>(ErrorCode::ResourceBusy, "echo resizes only while idle");
        }
        busy_.assign(static_cast<std::size_t>(slots), false);
        return foundation::Ok();
    }

    foundation::Result<int> acquire_slot() override
    {
        std::lock_guard lock(mutex_);
        if (!loaded_)
        {
            return foundation::Err<int>(ErrorCode::NotLoaded, "backend is not loaded");
        }
        const auto free_slot = std::find(busy_.begin(), busy_.end(), false);
        if (free_slot == busy_.end())
        {
            return foundation::Err<int>(ErrorCode::Unavailable, "no free slots");
        }
        *free_slot = true;
        return foundation::Ok(static_cast<int>(free_slot - busy_.begin()));
    }

    foundation::Result<void> release_slot(int slot) override
    {
        std::lock_guard lock(mutex_);
        if (slot < 0 || slot >= static_cast<int>(busy_.size()))
        {
            return foundation::Err<void>(ErrorCode::InvalidArgument, "invalid slot");
        }
        busy_[static_cast<std::size_t>(slot)] = false;
        return foundation::Ok();
    }

    bool slot_busy(int slot) const override
    {
        std::lock_guard lock(mutex_);
        return slot >= 0 && slot < static_cast<int>(busy_.size()) && busy_[static_cast<std::size_t>(slot)];
    }

    void request_cancel() override { cancel_all_ = true; }

    foundation::Result<model::RequestDemand> estimate_request_demand(
        const model::InferenceRequest& request) const override
    {
        const int words = static_cast<int>(words_of(last_user_text(request)).size());
        return model::RequestDemand{words, words, words * 2, 1, words * 2, 1};
    }

    const model::ChatTemplateMeta& chat_template_meta() const override { return meta_; }

    foundation::Result<model::InferenceResult> predict(int slot, const model::InferenceRequest& request) override
    {
        return predict_stream(slot, request, {}, nullptr);
    }

    foundation::Result<model::InferenceResult> predict_cancellable(
        int slot, const model::InferenceRequest& request, const std::atomic<bool>* cancel) override
    {
        return predict_stream(slot, request, {}, cancel);
    }

    foundation::Result<model::InferenceResult> predict_stream(
        int, const model::InferenceRequest& request, const TokenCallback& callback,
        const std::atomic<bool>* cancel) override
    {
        if (info_.artifacts.contains("device_lost"))
        {
            return foundation::Err<model::InferenceResult>(ErrorCode::Internal, "vk::DeviceLostError: ErrorDeviceLost");
        }
        const std::string text = last_user_text(request);
        const std::vector<std::string> words = words_of(text);
        model::InferenceResult result;
        result.prompt_tokens = static_cast<int>(words.size());
        if (request.progress)
        {
            request.progress->phase.store(3);
            request.progress->prompt_tokens.store(result.prompt_tokens);
            request.progress->processed_tokens.store(result.prompt_tokens);
        }
        const auto stopped = [&] { return cancel_all_.load() || (cancel && cancel->load()); };
        if (request.enable_reasoning.value_or(false))
        {
            result.reasoning_text = "thinking";
            result.reasoning_tokens = 1;
            if (callback && !callback({"", "thinking", {}, {}}))
            {
                return foundation::Err<model::InferenceResult>(ErrorCode::Cancelled, "chat cancelled");
            }
        }
        if (text.rfind("call ", 0) == 0 && !request.tools.empty())
        {
            const std::string arguments = "{\"text\":\"" + text.substr(5) + "\"}";
            result.tool_calls.push_back({"call_0", "function", request.tools.front().name, arguments});
            result.finish_reason = "tool_calls";
            if (callback && !callback({"", "", {{0, "call_0", "function", request.tools.front().name, arguments}}, {}}))
            {
                return foundation::Err<model::InferenceResult>(ErrorCode::Cancelled, "chat cancelled");
            }
            return foundation::Ok(std::move(result));
        }
        for (const std::string& word : words)
        {
            if (stopped())
            {
                return foundation::Err<model::InferenceResult>(ErrorCode::Cancelled, "chat cancelled");
            }
            model::InferenceDelta delta;
            delta.content = (result.text.empty() ? "" : " ") + word;
            if (request.logprobs)
            {
                delta.logprobs.push_back({delta.content, -0.5f, {}, {{delta.content, -0.5f, {}}}});
                result.logprobs.push_back(delta.logprobs.back());
            }
            result.text += delta.content;
            ++result.completion_tokens;
            if (request.progress)
            {
                request.progress->output_tokens.store(result.completion_tokens);
            }
            if (callback && !callback(delta))
            {
                return foundation::Err<model::InferenceResult>(ErrorCode::Cancelled, "chat cancelled");
            }
        }
        return foundation::Ok(std::move(result));
    }

    foundation::Result<model::EmbeddingResult> embed(
        int, const model::EmbeddingRequest& request, const std::function<bool()>&) override
    {
        model::EmbeddingResult result;
        for (const model::EmbeddingInput& input : request.inputs)
        {
            const float size = static_cast<float>(std::holds_alternative<model::EmbeddingTextInput>(input)
                ? std::get<model::EmbeddingTextInput>(input).text.size()
                : std::get<model::EmbeddingTokenInput>(input).tokens.size());
            result.embeddings.push_back({size, 1.0f});
            result.prompt_tokens += static_cast<int>(size);
        }
        return foundation::Ok(std::move(result));
    }

    foundation::Result<model::DecisionResult> decide(
        int, const model::DecisionRequest& request, const std::function<bool()>&) override
    {
        model::DecisionResult result;
        for (const model::DecisionQuestion& question : request.questions)
        {
            const std::size_t options = question.kind == model::DecisionKind::Predicate ? 2 : question.options.size();
            result.answers.push_back({std::vector<double>(options, 1.0 / static_cast<double>(options))});
        }
        return foundation::Ok(std::move(result));
    }

private:
    static std::string last_user_text(const model::InferenceRequest& request)
    {
        std::string text;
        for (const inference::Message& message : request.messages)
        {
            if (message.role != inference::MessageRole::User)
            {
                continue;
            }
            text.clear();
            for (const inference::Content& part : message.content)
            {
                if (const auto* piece = std::get_if<inference::TextContent>(&part))
                {
                    text += piece->text;
                }
                else if (const auto* image = std::get_if<inference::ImageContent>(&part))
                {
                    text += " [image " + std::to_string(image->bytes.size()) + " bytes]";
                }
            }
        }
        return text;
    }

    static std::vector<std::string> words_of(const std::string& text)
    {
        std::istringstream stream(text);
        std::vector<std::string> words;
        for (std::string word; stream >> word;)
        {
            words.push_back(word);
        }
        return words;
    }

    model::ModelInfo info_;
    model::ChatTemplateMeta meta_;
    mutable std::mutex mutex_;
    std::atomic<bool> loaded_{false};
    std::atomic<bool> cancel_all_{false};
    std::vector<bool> busy_;
};

inferdeck::provider::sdk::Definition define()
{
    return {R"({
        "runtime": "echo",
        "version": "2.0.0",
        "modalities": {
            "text": ["chat_completions", "responses"],
            "embedding": ["embeddings"],
            "decision": ["decisions"]
        },
        "model_source": "none",
        "vision": true,
        "reasoning": true,
        "unsupported_options": ["mirostat"]
    })",
            [](const inferdeck::model::ModelInfo& info, const nlohmann::json&)
            {
                return std::make_unique<EchoBackend>(info);
            }};
}

}

INFERDECK_PROVIDER_SDK(define)
