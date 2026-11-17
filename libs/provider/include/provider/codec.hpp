#pragma once

#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "foundation/result.hpp"
#include "inferdeck/provider_abi.h"
#include "model/imodel.hpp"

/// <summary>
/// The JSON-plus-blob wire format of the provider ABI. Compiled into the
/// gateway and into every SDK provider, so both sides agree by construction.
/// Encoders append views of the caller's memory to <c>blobs</c>; decoders copy
/// what they keep, so a decoded value outlives the call.
/// </summary>
namespace inferdeck::provider::codec {

using json = nlohmann::json;
using Blobs = std::span<const inferdeck_blob>;

json encode(const model::ModelInfo& info);
json encode(const model::InferenceRequest& request, int slot, bool stream,
            std::vector<inferdeck_blob>& blobs);
json encode(const model::InferenceDelta& delta);
json encode(const model::InferenceResult& result);
json encode(const model::RequestDemand& demand);
json encode(const model::ChatTemplateMeta& meta);
json encode(const inference::RequestProgress& progress);
json encode(const model::EmbeddingRequest& request, int slot);
json encode(const model::EmbeddingResult& result);
json encode(const model::ImageGenerationRequest& request, int slot,
            std::vector<inferdeck_blob>& blobs);
json encode(const model::ImageGenerationResult& result, std::vector<inferdeck_blob>& blobs);
json encode(const model::AudioGenerationRequest& request, int slot);
json encode(const model::AudioGenerationResult& result, std::vector<inferdeck_blob>& blobs);
json encode(const model::VideoGenerationRequest& request, int slot);
json encode(const model::VideoGenerationResult& result, std::vector<inferdeck_blob>& blobs);
json encode(const model::SpeechRequest& request, int slot, bool stream);
json encode(const model::AudioResult& result, std::vector<inferdeck_blob>& blobs);
json encode(const model::TranscriptionRequest& request, int slot,
            std::vector<inferdeck_blob>& blobs);
json encode(const model::TranscriptionResult& result);
json encode(const model::DecisionRequest& request, int slot);
json encode(const model::DecisionResult& result);

/// <summary>Parses JSON text; ParseError when it is not a JSON object.</summary>
foundation::Result<json> parse(const char* text);

foundation::Result<model::ModelInfo> decode_model_info(const json& value);
foundation::Result<model::InferenceRequest> decode_inference_request(const json& value, Blobs blobs);
foundation::Result<model::InferenceDelta> decode_delta(const json& value);
foundation::Result<model::InferenceResult> decode_inference_result(const json& value);
foundation::Result<model::RequestDemand> decode_demand(const json& value);
foundation::Result<model::ChatTemplateMeta> decode_template_meta(const json& value);
/// <summary>Stores a {"type":"request_progress"} event into <paramref name="progress"/>.</summary>
void apply_progress(const json& value, inference::RequestProgress& progress);
foundation::Result<model::EmbeddingRequest> decode_embedding_request(const json& value);
foundation::Result<model::EmbeddingResult> decode_embedding_result(const json& value);
foundation::Result<model::ImageGenerationRequest> decode_image_request(const json& value, Blobs blobs);
foundation::Result<model::ImageGenerationResult> decode_image_result(const json& value, Blobs blobs);
foundation::Result<model::AudioGenerationRequest> decode_audio_generation_request(const json& value);
foundation::Result<model::AudioGenerationResult> decode_audio_generation_result(const json& value, Blobs blobs);
foundation::Result<model::VideoGenerationRequest> decode_video_request(const json& value);
foundation::Result<model::VideoGenerationResult> decode_video_result(const json& value, Blobs blobs);
foundation::Result<model::SpeechRequest> decode_speech_request(const json& value);
foundation::Result<model::AudioResult> decode_speech_result(const json& value, Blobs blobs);
foundation::Result<model::TranscriptionRequest> decode_transcription_request(const json& value, Blobs blobs);
foundation::Result<model::TranscriptionResult> decode_transcription_result(const json& value);
foundation::Result<model::DecisionRequest> decode_decision_request(const json& value);
foundation::Result<model::DecisionResult> decode_decision_result(const json& value);

/// <summary>
/// Names of the optional generation features a request uses, in the
/// vocabulary of the manifest's "unsupported_options" list.
/// </summary>
std::vector<std::string> generation_options_used(const model::InferenceRequest& request);

std::vector<std::byte> copy_blob(const inferdeck_blob& blob);

}
