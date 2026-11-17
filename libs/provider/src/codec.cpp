#include "provider/codec.hpp"

#include <cstring>
#include <exception>
#include <utility>

namespace inferdeck::inference {

NLOHMANN_JSON_SERIALIZE_ENUM(MessageRole, {
    {MessageRole::User, "user"}, {MessageRole::Developer, "developer"},
    {MessageRole::System, "system"}, {MessageRole::Assistant, "assistant"},
    {MessageRole::Tool, "tool"}, {MessageRole::Function, "function"}})
NLOHMANN_JSON_SERIALIZE_ENUM(ToolChoiceKind, {
    {ToolChoiceKind::Auto, "auto"}, {ToolChoiceKind::None, "none"},
    {ToolChoiceKind::Required, "required"}, {ToolChoiceKind::Function, "function"}})
NLOHMANN_JSON_SERIALIZE_ENUM(StructuredOutputKind, {
    {StructuredOutputKind::Text, "text"}, {StructuredOutputKind::JsonObject, "json_object"},
    {StructuredOutputKind::JsonSchema, "json_schema"}, {StructuredOutputKind::Grammar, "grammar"}})
NLOHMANN_JSON_SERIALIZE_ENUM(ReasoningFormat, {
    {ReasoningFormat::Automatic, "auto"}, {ReasoningFormat::None, "none"},
    {ReasoningFormat::DeepSeek, "deepseek"}, {ReasoningFormat::DeepSeekLegacy, "deepseek_legacy"}})

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FunctionTool, name, description, parameters_schema)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ToolChoice, kind, function_name)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StructuredOutput, kind, name, description, schema, strict)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TopTokenLogprob, token, logprob, bytes)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TokenLogprob, token, logprob, bytes, top_logprobs)

void to_json(nlohmann::json& value, const FunctionCall& call)
{
    value = {{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}};
}

void from_json(const nlohmann::json& value, FunctionCall& call)
{
    call = {value.value("id", ""), value.value("name", ""), value.value("arguments", "")};
}

void to_json(nlohmann::json& value, const ToolCall& call)
{
    value = {{"id", call.id}, {"type", call.type}, {"name", call.function_name},
             {"arguments", call.function_arguments}};
}

void from_json(const nlohmann::json& value, ToolCall& call)
{
    call = {value.value("id", ""), value.value("type", "function"), value.value("name", ""),
            value.value("arguments", "")};
}

void to_json(nlohmann::json& value, const ToolCallDelta& call)
{
    value = {{"index", call.index}, {"id", call.id}, {"type", call.type},
             {"name", call.function_name}, {"arguments", call.function_arguments}};
}

void from_json(const nlohmann::json& value, ToolCallDelta& call)
{
    call = {value.value("index", std::size_t{0}), value.value("id", ""), value.value("type", "function"),
            value.value("name", ""), value.value("arguments", "")};
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(GenerationResult, text, reasoning_text, finish_reason,
    prompt_tokens, cached_prompt_tokens, completion_tokens, reasoning_tokens, duration_ms,
    generation_duration_ms, prompt_duration_ms, first_token_duration_ms, tokens_per_second,
    mtp_drafted_tokens, mtp_accepted_tokens, tool_calls, logprobs, prompt_decode_duration_ms,
    prompt_decode_tokens)

}

namespace inferdeck::model {

NLOHMANN_JSON_SERIALIZE_ENUM(ModelRole, {
    {ModelRole::Conversation, "conversation"}, {ModelRole::Helper, "helper"},
    {ModelRole::Media, "media"}, {ModelRole::Embedding, "embedding"},
    {ModelRole::Maintenance, "maintenance"}})
NLOHMANN_JSON_SERIALIZE_ENUM(ModelCompute, {
    {ModelCompute::VulkanGpu, "vulkan_gpu"}, {ModelCompute::Cpu, "cpu"},
    {ModelCompute::CudaGpu, "cuda_gpu"}, {ModelCompute::RocmGpu, "rocm_gpu"},
    {ModelCompute::Mixed, "mixed"}})
NLOHMANN_JSON_SERIALIZE_ENUM(ResidencyPolicy, {
    {ResidencyPolicy::Managed, "managed"}, {ResidencyPolicy::Always, "always"},
    {ResidencyPolicy::OnDemand, "on_demand"}})

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SamplingConfig, temperature, top_p, top_k, min_p,
    repeat_penalty, repeat_last_n, dry_multiplier, dry_base, dry_allowed_length, dry_penalty_last_n,
    dry_seq_breakers)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ReasoningConfig, supported, efforts, default_effort,
    none_disables, aliases)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ProfileOptimizationInfo, status, measured_at,
    quality_passes, quality_total, single_tokens_per_second, parallel_tokens_per_second,
    schedule_enabled, schedule_window_start, schedule_window_end)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ModelInfo, name, family, runtime, modality, role,
    compute, residency, admission_pool, concurrency_limit, memory_required_mb, eviction_eligible,
    resource_metadata_explicit, capabilities, gguf_path, mmproj_path, n_slots, min_slots,
    continuation_grace_ms, request_queue_timeout_seconds, vram_required_mb, vram_fixed_mb,
    vram_per_slot_mb, context_size, default_max_output_tokens, kv_unified, context_pool_size,
    context_pool_auto, concurrency_auto, n_batch, n_ubatch, cache_type_k, cache_type_v, n_gpu_layers,
    mtp_enabled, mtp_draft_tokens, mtp_p_min, mtp_max_active_requests, speculative_type, draft_model_path,
    has_vision, reasoning_format,
    chat_template_path, prompt_price_per_million, cached_prompt_price_per_million,
    completion_price_per_million, artifacts, sampling, reasoning, optimization)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RequestDemand, prompt_positions, output_tokens,
    required_context, required_sequences, aggregate_context, aggregate_sequences)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ChatTemplateMeta, thinking_start_tag,
    thinking_end_tags, preserved_tokens, supports_thinking)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TranscriptionSegment, id, start_seconds, end_seconds,
    text, tokens, avg_logprob, no_speech_probability)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TranscriptionResult, text, language, duration_seconds,
    inference_ms, segments)
NLOHMANN_JSON_SERIALIZE_ENUM(DecisionKind, {
    {DecisionKind::Predicate, "predicate"}, {DecisionKind::Choice, "choice"},
    {DecisionKind::Score, "score"}})
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DecisionOption, label, description)

void to_json(nlohmann::json& value, const DecisionQuestion& question)
{
    value = {{"type", question.kind}, {"instructions", question.instructions},
             {"options", question.options}};
}

void from_json(const nlohmann::json& value, DecisionQuestion& question)
{
    question.kind = value.value("type", DecisionKind::Predicate);
    question.instructions = value.value("instructions", "");
    question.options = value.value("options", std::vector<DecisionOption>{});
}

}

namespace inferdeck::provider::codec {

namespace {

using foundation::ErrorCode;

template <typename T, typename Decode>
foundation::Result<T> guarded(Decode decode)
{
    try
    {
        return foundation::Ok(decode());
    }
    catch (const std::exception& error)
    {
        return foundation::Err<T>(ErrorCode::ParseError, std::string("malformed provider JSON: ") + error.what());
    }
}

std::size_t add_blob(std::vector<inferdeck_blob>& blobs, const void* data, std::size_t size)
{
    blobs.push_back({static_cast<const uint8_t*>(data), size});
    return blobs.size() - 1;
}

std::size_t add_blob(std::vector<inferdeck_blob>& blobs, const std::vector<std::byte>& bytes)
{
    return add_blob(blobs, bytes.data(), bytes.size());
}

std::vector<std::byte> blob_at(Blobs blobs, std::size_t index)
{
    if (index >= blobs.size())
    {
        throw std::out_of_range("blob index " + std::to_string(index) + " is out of range");
    }
    return copy_blob(blobs[index]);
}

template <typename T>
void put(json& target, const char* key, const std::optional<T>& value)
{
    if (value)
    {
        target[key] = *value;
    }
}

template <typename T>
void take(const json& source, const char* key, std::optional<T>& value)
{
    const json::const_iterator found = source.find(key);
    if (found != source.end() && !found->is_null())
    {
        value = found->get<T>();
    }
}

}

std::vector<std::byte> copy_blob(const inferdeck_blob& blob)
{
    const std::byte* begin = reinterpret_cast<const std::byte*>(blob.data);
    return blob.data ? std::vector<std::byte>(begin, begin + blob.size) : std::vector<std::byte>{};
}

foundation::Result<json> parse(const char* text)
{
    json value = json::parse(text ? text : "", nullptr, false);
    if (value.is_discarded() || !value.is_object())
    {
        return foundation::Err<json>(ErrorCode::ParseError, "provider JSON is not an object");
    }
    return foundation::Ok(std::move(value));
}

json encode(const model::ModelInfo& info)
{
    json value = info;
    value["model_path"] = info.gguf_path;
    return value;
}

foundation::Result<model::ModelInfo> decode_model_info(const json& value)
{
    return guarded<model::ModelInfo>([&] { return value.get<model::ModelInfo>(); });
}

json encode(const model::InferenceRequest& request, int slot, bool stream,
            std::vector<inferdeck_blob>& blobs)
{
    json messages = json::array();
    for (const inference::Message& message : request.messages)
    {
        json content = json::array();
        for (const inference::Content& part : message.content)
        {
            if (const inference::TextContent* text = std::get_if<inference::TextContent>(&part))
            {
                content.push_back({{"type", "text"}, {"text", text->text}});
            }
            else if (const inference::ImageContent* image = std::get_if<inference::ImageContent>(&part))
            {
                content.push_back({{"type", "image"}, {"blob", add_blob(blobs, image->bytes)},
                                   {"media_type", image->media_type}, {"detail", image->detail}});
            }
            else if (const inference::AudioContent* audio = std::get_if<inference::AudioContent>(&part))
            {
                content.push_back({{"type", "audio"}, {"blob", add_blob(blobs, audio->bytes)},
                                   {"media_type", audio->media_type}});
            }
        }
        messages.push_back({{"role", message.role}, {"content", std::move(content)},
                            {"tool_calls", message.tool_calls}, {"reasoning", message.reasoning},
                            {"tool_call_id", message.tool_call_id}, {"name", message.name}});
    }
    const inference::Sampling& sampling = request.sampling;
    json sampled = {{"seed", sampling.seed}};
    put(sampled, "temperature", sampling.temperature);
    put(sampled, "top_p", sampling.top_p);
    put(sampled, "top_k", sampling.top_k);
    put(sampled, "min_p", sampling.min_p);
    put(sampled, "repeat_penalty", sampling.repeat_penalty);
    put(sampled, "repeat_last_n", sampling.repeat_last_n);
    put(sampled, "frequency_penalty", sampling.frequency_penalty);
    put(sampled, "presence_penalty", sampling.presence_penalty);
    put(sampled, "tfs_z", sampling.tfs_z);
    put(sampled, "mirostat", sampling.mirostat);
    put(sampled, "mirostat_eta", sampling.mirostat_eta);
    put(sampled, "mirostat_tau", sampling.mirostat_tau);
    if (!sampling.logit_bias.empty())
    {
        sampled["logit_bias"] = sampling.logit_bias;
    }
    json value = {
        {"slot", slot}, {"stream", stream}, {"prompt", request.prompt},
        {"messages", std::move(messages)}, {"tools", request.tools},
        {"tool_choice", request.tool_choice}, {"output", request.output},
        {"sampling", std::move(sampled)}, {"stop", request.stop},
        {"max_output_tokens", request.max_output_tokens},
        {"parallel_tool_calls", request.parallel_tool_calls},
        {"logprobs", request.logprobs}, {"top_logprobs", request.top_logprobs},
        {"add_generation_prompt", request.add_generation_prompt},
        {"reasoning_format", request.reasoning_format},
        {"progress", request.progress != nullptr},
    };
    put(value, "reasoning_effort", request.reasoning_effort);
    put(value, "enable_reasoning", request.enable_reasoning);
    put(value, "context_window", request.context_window);
    put(value, "prompt_keep_tokens", request.prompt_keep_tokens);
    return value;
}

foundation::Result<model::InferenceRequest> decode_inference_request(const json& value, Blobs blobs)
{
    return guarded<model::InferenceRequest>([&]
    {
        model::InferenceRequest request;
        request.prompt = value.value("prompt", "");
        for (const json& item : value.value("messages", json::array()))
        {
            inference::Message message;
            message.role = item.value("role", inference::MessageRole::User);
            for (const json& part : item.value("content", json::array()))
            {
                const std::string type = part.value("type", "text");
                if (type == "image")
                {
                    message.content.emplace_back(inference::ImageContent{
                        blob_at(blobs, part.at("blob").get<std::size_t>()),
                        part.value("media_type", ""), part.value("detail", "")});
                }
                else if (type == "audio")
                {
                    message.content.emplace_back(inference::AudioContent{
                        blob_at(blobs, part.at("blob").get<std::size_t>()), part.value("media_type", "")});
                }
                else
                {
                    message.content.emplace_back(inference::TextContent{part.value("text", "")});
                }
            }
            message.tool_calls = item.value("tool_calls", std::vector<inference::FunctionCall>{});
            message.reasoning = item.value("reasoning", "");
            message.tool_call_id = item.value("tool_call_id", "");
            message.name = item.value("name", "");
            request.messages.push_back(std::move(message));
        }
        request.tools = value.value("tools", std::vector<inference::FunctionTool>{});
        request.tool_choice = value.value("tool_choice", inference::ToolChoice{});
        request.output = value.value("output", inference::StructuredOutput{});
        const json sampled = value.value("sampling", json::object());
        inference::Sampling& sampling = request.sampling;
        sampling.seed = sampled.value("seed", std::int64_t{-1});
        take(sampled, "temperature", sampling.temperature);
        take(sampled, "top_p", sampling.top_p);
        take(sampled, "top_k", sampling.top_k);
        take(sampled, "min_p", sampling.min_p);
        take(sampled, "repeat_penalty", sampling.repeat_penalty);
        take(sampled, "repeat_last_n", sampling.repeat_last_n);
        take(sampled, "frequency_penalty", sampling.frequency_penalty);
        take(sampled, "presence_penalty", sampling.presence_penalty);
        take(sampled, "tfs_z", sampling.tfs_z);
        take(sampled, "mirostat", sampling.mirostat);
        take(sampled, "mirostat_eta", sampling.mirostat_eta);
        take(sampled, "mirostat_tau", sampling.mirostat_tau);
        sampling.logit_bias = sampled.value("logit_bias", std::vector<std::pair<std::int32_t, float>>{});
        request.stop = value.value("stop", std::vector<std::string>{});
        request.max_output_tokens = value.value("max_output_tokens", model::k_max_tokens_use_context_budget);
        request.parallel_tool_calls = value.value("parallel_tool_calls", true);
        request.logprobs = value.value("logprobs", false);
        request.top_logprobs = value.value("top_logprobs", 0);
        request.add_generation_prompt = value.value("add_generation_prompt", true);
        request.reasoning_format = value.value("reasoning_format", inference::ReasoningFormat::Automatic);
        take(value, "reasoning_effort", request.reasoning_effort);
        take(value, "enable_reasoning", request.enable_reasoning);
        take(value, "context_window", request.context_window);
        take(value, "prompt_keep_tokens", request.prompt_keep_tokens);
        if (value.value("progress", false))
        {
            request.progress = std::make_shared<inference::RequestProgress>();
        }
        return request;
    });
}

json encode(const model::InferenceDelta& delta)
{
    json value = {{"type", "delta"}, {"content", delta.content}, {"reasoning", delta.reasoning_text}};
    if (!delta.tool_calls.empty())
    {
        value["tool_calls"] = delta.tool_calls;
    }
    if (!delta.logprobs.empty())
    {
        value["logprobs"] = delta.logprobs;
    }
    return value;
}

foundation::Result<model::InferenceDelta> decode_delta(const json& value)
{
    return guarded<model::InferenceDelta>([&]
    {
        return model::InferenceDelta{
            value.value("content", ""), value.value("reasoning", ""),
            value.value("tool_calls", std::vector<inference::ToolCallDelta>{}),
            value.value("logprobs", std::vector<inference::TokenLogprob>{})};
    });
}

json encode(const model::InferenceResult& result)
{
    return result;
}

foundation::Result<model::InferenceResult> decode_inference_result(const json& value)
{
    return guarded<model::InferenceResult>([&] { return value.get<model::InferenceResult>(); });
}

json encode(const model::RequestDemand& demand)
{
    return demand;
}

foundation::Result<model::RequestDemand> decode_demand(const json& value)
{
    return guarded<model::RequestDemand>([&] { return value.get<model::RequestDemand>(); });
}

json encode(const model::ChatTemplateMeta& meta)
{
    return meta;
}

foundation::Result<model::ChatTemplateMeta> decode_template_meta(const json& value)
{
    return guarded<model::ChatTemplateMeta>([&] { return value.get<model::ChatTemplateMeta>(); });
}

json encode(const inference::RequestProgress& progress)
{
    return {{"type", "request_progress"}, {"phase", progress.phase.load()},
            {"slot", progress.slot.load()}, {"prompt_tokens", progress.prompt_tokens.load()},
            {"processed_tokens", progress.processed_tokens.load()},
            {"cached_tokens", progress.cached_tokens.load()},
            {"output_tokens", progress.output_tokens.load()},
            {"prompt_ms", progress.prompt_ms.load()}, {"generation_ms", progress.generation_ms.load()}};
}

void apply_progress(const json& value, inference::RequestProgress& progress)
{
    try
    {
        progress.phase.store(value.value("phase", progress.phase.load()));
        progress.prompt_tokens.store(value.value("prompt_tokens", progress.prompt_tokens.load()));
        progress.processed_tokens.store(value.value("processed_tokens", progress.processed_tokens.load()));
        progress.cached_tokens.store(value.value("cached_tokens", progress.cached_tokens.load()));
        progress.output_tokens.store(value.value("output_tokens", progress.output_tokens.load()));
        progress.prompt_ms.store(value.value("prompt_ms", progress.prompt_ms.load()));
        progress.generation_ms.store(value.value("generation_ms", progress.generation_ms.load()));
    }
    catch (const std::exception&)
    {
    }
}

json encode(const model::EmbeddingRequest& request, int slot)
{
    json inputs = json::array();
    for (const model::EmbeddingInput& input : request.inputs)
    {
        if (const model::EmbeddingTextInput* text = std::get_if<model::EmbeddingTextInput>(&input))
        {
            inputs.push_back(text->text);
        }
        else
        {
            inputs.push_back(std::get<model::EmbeddingTokenInput>(input).tokens);
        }
    }
    json value = {{"slot", slot}, {"inputs", std::move(inputs)}};
    put(value, "dimensions", request.dimensions);
    return value;
}

foundation::Result<model::EmbeddingRequest> decode_embedding_request(const json& value)
{
    return guarded<model::EmbeddingRequest>([&]
    {
        model::EmbeddingRequest request;
        for (const json& input : value.value("inputs", json::array()))
        {
            if (input.is_string())
            {
                request.inputs.emplace_back(model::EmbeddingTextInput{input.get<std::string>()});
            }
            else
            {
                request.inputs.emplace_back(model::EmbeddingTokenInput{input.get<std::vector<std::int32_t>>()});
            }
        }
        take(value, "dimensions", request.dimensions);
        return request;
    });
}

json encode(const model::EmbeddingResult& result)
{
    return {{"embeddings", result.embeddings}, {"prompt_tokens", result.prompt_tokens},
            {"duration_ms", result.duration_ms}};
}

foundation::Result<model::EmbeddingResult> decode_embedding_result(const json& value)
{
    return guarded<model::EmbeddingResult>([&]
    {
        return model::EmbeddingResult{value.value("embeddings", std::vector<std::vector<float>>{}),
                                      value.value("prompt_tokens", 0), value.value("duration_ms", 0.0f)};
    });
}

json encode(const model::ImageGenerationRequest& request, int slot, std::vector<inferdeck_blob>& blobs)
{
    for (const std::vector<std::byte>& reference : request.reference_images)
    {
        add_blob(blobs, reference);
    }
    return {{"slot", slot}, {"prompt", request.prompt}, {"negative_prompt", request.negative_prompt},
            {"width", request.width}, {"height", request.height}, {"count", request.count},
            {"steps", request.steps}, {"seed", request.seed}, {"guidance_scale", request.guidance_scale},
            {"reference_image_count", request.reference_images.size()}};
}

foundation::Result<model::ImageGenerationRequest> decode_image_request(const json& value, Blobs blobs)
{
    return guarded<model::ImageGenerationRequest>([&]
    {
        model::ImageGenerationRequest request;
        request.prompt = value.value("prompt", "");
        request.negative_prompt = value.value("negative_prompt", "");
        request.width = value.value("width", request.width);
        request.height = value.value("height", request.height);
        request.count = value.value("count", request.count);
        request.steps = value.value("steps", request.steps);
        request.seed = value.value("seed", request.seed);
        request.guidance_scale = value.value("guidance_scale", request.guidance_scale);
        const std::size_t references = value.value("reference_image_count", std::size_t{0});
        for (std::size_t index = 0; index < references; ++index)
        {
            request.reference_images.push_back(blob_at(blobs, index));
        }
        return request;
    });
}

json encode(const model::ImageGenerationResult& result, std::vector<inferdeck_blob>& blobs)
{
    for (const std::vector<std::byte>& image : result.png_images)
    {
        add_blob(blobs, image);
    }
    return {{"count", result.png_images.size()}, {"duration_ms", result.duration_ms}};
}

foundation::Result<model::ImageGenerationResult> decode_image_result(const json& value, Blobs blobs)
{
    return guarded<model::ImageGenerationResult>([&]
    {
        model::ImageGenerationResult result;
        for (const inferdeck_blob& blob : blobs)
        {
            result.png_images.push_back(copy_blob(blob));
        }
        result.duration_ms = value.value("duration_ms", 0.0f);
        return result;
    });
}

json encode(const model::AudioGenerationRequest& request, int slot)
{
    return {{"slot", slot}, {"prompt", request.prompt}, {"lyrics", request.lyrics},
            {"duration_seconds", request.duration_seconds}, {"seed", request.seed},
            {"steps", request.steps}, {"guidance_scale", request.guidance_scale}};
}

foundation::Result<model::AudioGenerationRequest> decode_audio_generation_request(const json& value)
{
    return guarded<model::AudioGenerationRequest>([&]
    {
        model::AudioGenerationRequest request;
        request.prompt = value.value("prompt", "");
        request.lyrics = value.value("lyrics", "");
        request.duration_seconds = value.value("duration_seconds", request.duration_seconds);
        request.seed = value.value("seed", request.seed);
        request.steps = value.value("steps", request.steps);
        request.guidance_scale = value.value("guidance_scale", request.guidance_scale);
        return request;
    });
}

json encode(const model::AudioGenerationResult& result, std::vector<inferdeck_blob>& blobs)
{
    add_blob(blobs, result.wav_bytes);
    return {{"seed", result.seed}, {"output_audio_seconds", result.output_audio_seconds},
            {"duration_ms", result.duration_ms}};
}

foundation::Result<model::AudioGenerationResult> decode_audio_generation_result(const json& value, Blobs blobs)
{
    return guarded<model::AudioGenerationResult>([&]
    {
        return model::AudioGenerationResult{blobs.empty() ? std::vector<std::byte>{} : copy_blob(blobs[0]),
                                            value.value("seed", std::int64_t{-1}),
                                            value.value("duration_ms", 0.0f),
                                            value.value("output_audio_seconds", 0.0)};
    });
}

json encode(const model::VideoGenerationRequest& request, int slot)
{
    return {{"slot", slot}, {"prompt", request.prompt}, {"negative_prompt", request.negative_prompt},
            {"width", request.width}, {"height", request.height}, {"frames", request.frames},
            {"fps", request.fps}, {"steps", request.steps}, {"seed", request.seed},
            {"guidance_scale", request.guidance_scale}};
}

foundation::Result<model::VideoGenerationRequest> decode_video_request(const json& value)
{
    return guarded<model::VideoGenerationRequest>([&]
    {
        model::VideoGenerationRequest request;
        request.prompt = value.value("prompt", "");
        request.negative_prompt = value.value("negative_prompt", "");
        request.width = value.value("width", request.width);
        request.height = value.value("height", request.height);
        request.frames = value.value("frames", request.frames);
        request.fps = value.value("fps", request.fps);
        request.steps = value.value("steps", request.steps);
        request.seed = value.value("seed", request.seed);
        request.guidance_scale = value.value("guidance_scale", request.guidance_scale);
        return request;
    });
}

json encode(const model::VideoGenerationResult& result, std::vector<inferdeck_blob>& blobs)
{
    add_blob(blobs, result.video_bytes);
    return {{"content_type", result.content_type}, {"output_video_seconds", result.output_video_seconds},
            {"duration_ms", result.duration_ms}};
}

foundation::Result<model::VideoGenerationResult> decode_video_result(const json& value, Blobs blobs)
{
    return guarded<model::VideoGenerationResult>([&]
    {
        model::VideoGenerationResult result;
        result.video_bytes = blobs.empty() ? std::vector<std::byte>{} : copy_blob(blobs[0]);
        result.content_type = value.value("content_type", result.content_type);
        result.duration_ms = value.value("duration_ms", 0.0f);
        result.output_video_seconds = value.value("output_video_seconds", 0.0);
        return result;
    });
}

json encode(const model::SpeechRequest& request, int slot, bool stream)
{
    return {{"slot", slot}, {"input", request.input}, {"voice", request.voice},
            {"format", request.format}, {"speed", request.speed}, {"stream", stream}};
}

foundation::Result<model::SpeechRequest> decode_speech_request(const json& value)
{
    return guarded<model::SpeechRequest>([&]
    {
        return model::SpeechRequest{value.value("input", ""), value.value("voice", ""),
                                    value.value("format", "wav"), value.value("speed", 1.0f)};
    });
}

json encode(const model::AudioResult& result, std::vector<inferdeck_blob>& blobs)
{
    add_blob(blobs, result.bytes);
    return {{"content_type", result.content_type}, {"output_audio_seconds", result.output_audio_seconds},
            {"duration_ms", result.duration_ms}};
}

foundation::Result<model::AudioResult> decode_speech_result(const json& value, Blobs blobs)
{
    return guarded<model::AudioResult>([&]
    {
        return model::AudioResult{blobs.empty() ? std::vector<std::byte>{} : copy_blob(blobs[0]),
                                  value.value("content_type", "audio/wav"),
                                  value.value("duration_ms", 0.0f),
                                  value.value("output_audio_seconds", 0.0)};
    });
}

json encode(const model::TranscriptionRequest& request, int slot, std::vector<inferdeck_blob>& blobs)
{
    return {{"slot", slot}, {"sample_rate", request.sample_rate}, {"language", request.language},
            {"prompt", request.prompt}, {"temperature", request.temperature},
            {"pcm_blob", add_blob(blobs, request.pcm.data(), request.pcm.size() * sizeof(float))}};
}

foundation::Result<model::TranscriptionRequest> decode_transcription_request(const json& value, Blobs blobs)
{
    return guarded<model::TranscriptionRequest>([&]
    {
        model::TranscriptionRequest request;
        const std::vector<std::byte> pcm = blob_at(blobs, value.value("pcm_blob", std::size_t{0}));
        request.pcm.resize(pcm.size() / sizeof(float));
        std::memcpy(request.pcm.data(), pcm.data(), request.pcm.size() * sizeof(float));
        request.sample_rate = value.value("sample_rate", request.sample_rate);
        request.language = value.value("language", "");
        request.prompt = value.value("prompt", "");
        request.temperature = value.value("temperature", 0.0f);
        return request;
    });
}

json encode(const model::TranscriptionResult& result)
{
    return result;
}

foundation::Result<model::TranscriptionResult> decode_transcription_result(const json& value)
{
    return guarded<model::TranscriptionResult>([&] { return value.get<model::TranscriptionResult>(); });
}

json encode(const model::DecisionRequest& request, int slot)
{
    return {{"slot", slot}, {"input", request.input}, {"questions", request.questions}};
}

foundation::Result<model::DecisionRequest> decode_decision_request(const json& value)
{
    return guarded<model::DecisionRequest>([&]
    {
        return model::DecisionRequest{value.value("input", ""),
                                      value.value("questions", std::vector<model::DecisionQuestion>{})};
    });
}

json encode(const model::DecisionResult& result)
{
    json answers = json::array();
    for (const model::DecisionAnswer& answer : result.answers)
    {
        answers.push_back({{"probabilities", answer.probabilities}});
    }
    return {{"answers", std::move(answers)}, {"input_tokens", result.input_tokens},
            {"duration_ms", result.duration_ms}};
}

foundation::Result<model::DecisionResult> decode_decision_result(const json& value)
{
    return guarded<model::DecisionResult>([&]
    {
        model::DecisionResult result;
        for (const json& answer : value.value("answers", json::array()))
        {
            result.answers.push_back({answer.value("probabilities", std::vector<double>{})});
        }
        result.input_tokens = value.value("input_tokens", 0);
        result.duration_ms = value.value("duration_ms", 0.0f);
        return result;
    });
}

std::vector<std::string> generation_options_used(const model::InferenceRequest& request)
{
    std::vector<std::string> used;
    const auto mark = [&used](bool condition, const char* name)
    {
        if (condition)
        {
            used.emplace_back(name);
        }
    };
    const inference::Sampling& sampling = request.sampling;
    mark(request.logprobs || request.top_logprobs > 0, "logprobs");
    mark(!sampling.logit_bias.empty(), "logit_bias");
    mark(sampling.mirostat.has_value() && *sampling.mirostat != 0, "mirostat");
    mark(sampling.tfs_z.has_value(), "tfs_z");
    mark(request.prompt_keep_tokens.has_value(), "prompt_keep_tokens");
    mark(!request.add_generation_prompt, "add_generation_prompt");
    mark(request.reasoning_format != inference::ReasoningFormat::Automatic, "reasoning_format");
    mark(!request.tools.empty(), "tools");
    mark(request.output.kind != inference::StructuredOutputKind::Text, "structured_output");
    bool images = false;
    bool audio = false;
    for (const inference::Message& message : request.messages)
    {
        for (const inference::Content& part : message.content)
        {
            images = images || std::holds_alternative<inference::ImageContent>(part);
            audio = audio || std::holds_alternative<inference::AudioContent>(part);
        }
    }
    mark(images, "image_input");
    mark(audio, "audio_input");
    return used;
}

}
