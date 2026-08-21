#include "model/backend_coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>
#include <utility>

namespace inferdeck::model {
foundation::Result<InferenceResult> BackendCoordinator::predict(
    const std::string& name, int lease_id, const InferenceRequest& req) {
    IModel* inst = nullptr;
    int backend_slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<InferenceResult>(foundation::ErrorCode::NotFound,
                                                      "model not loaded: " + name);
        }
        inst = dynamic_cast<IModel*>(it->second.get());
        if (!inst || !it->second->info().supports("chat_completions")) {
            return foundation::Err<InferenceResult>(foundation::ErrorCode::InvalidArgument,
                                                      "backend does not support text generation: " + name);
        }
        auto slot = backend_slot_for_lease_locked(name, lease_id);
        if (!slot) {
            return foundation::Err<InferenceResult>(slot.error().code, slot.error().message);
        }
        backend_slot = *slot;
    }
    // Safe to call unlocked: the caller holds a slot, so unload() drains before
    // the instance can be destroyed.
    return inst->predict(backend_slot, req);
}

foundation::Result<InferenceResult> BackendCoordinator::predict_stream(
    const std::string& name, int lease_id, const InferenceRequest& req,
    const IModel::TokenCallback& callback, const std::atomic<bool>* cancel) {
    IModel* inst = nullptr;
    int backend_slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<InferenceResult>(foundation::ErrorCode::NotFound,
                                                      "model not loaded: " + name);
        }
        inst = dynamic_cast<IModel*>(it->second.get());
        if (!inst || !it->second->info().supports("chat_completions")) {
            return foundation::Err<InferenceResult>(foundation::ErrorCode::InvalidArgument,
                                                      "backend does not support text generation: " + name);
        }
        auto slot = backend_slot_for_lease_locked(name, lease_id);
        if (!slot) {
            return foundation::Err<InferenceResult>(slot.error().code, slot.error().message);
        }
        backend_slot = *slot;
    }
    return inst->predict_stream(backend_slot, req, callback, cancel);
}

foundation::Result<EmbeddingResult> BackendCoordinator::embed(
    const std::string& name, int lease_id, const EmbeddingRequest& request,
    const std::function<bool()>& cancelled) {
    IEmbeddingBackend* backend = nullptr;
    int backend_slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<EmbeddingResult>(foundation::ErrorCode::NotFound,
                                                      "model not loaded: " + name);
        }
        backend = dynamic_cast<IEmbeddingBackend*>(it->second.get());
        if (!backend || !it->second->info().supports("embeddings")) {
            return foundation::Err<EmbeddingResult>(foundation::ErrorCode::InvalidArgument,
                                                      "backend does not support embeddings: " + name);
        }
        auto slot = backend_slot_for_lease_locked(name, lease_id);
        if (!slot) {
            return foundation::Err<EmbeddingResult>(slot.error().code, slot.error().message);
        }
        backend_slot = *slot;
    }
    return backend->embed(backend_slot, request, cancelled);
}

foundation::Result<ImageGenerationResult> BackendCoordinator::generate_images(
    const std::string& name, int lease_id, const ImageGenerationRequest& request,
    const std::function<bool(int)>& progress) {
    IImageBackend* backend = nullptr;
    int backend_slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<ImageGenerationResult>(foundation::ErrorCode::NotFound,
                                                           "model not loaded: " + name);
        }
        backend = dynamic_cast<IImageBackend*>(it->second.get());
        if (!backend || !it->second->info().supports("image_generation")) {
            return foundation::Err<ImageGenerationResult>(foundation::ErrorCode::InvalidArgument,
                                                           "backend does not support image generation: " + name);
        }
        auto slot = backend_slot_for_lease_locked(name, lease_id);
        if (!slot) {
            return foundation::Err<ImageGenerationResult>(slot.error().code, slot.error().message);
        }
        backend_slot = *slot;
    }
    return backend->generate_images(backend_slot, request, progress);
}

foundation::Result<AudioResult> BackendCoordinator::synthesize(
    const std::string& name, int lease_id, const SpeechRequest& request,
    const std::function<bool(const std::byte*, std::size_t)>& stream) {
    ISpeechBackend* backend = nullptr;
    int backend_slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<AudioResult>(foundation::ErrorCode::NotFound,
                                                 "model not loaded: " + name);
        }
        backend = dynamic_cast<ISpeechBackend*>(it->second.get());
        if (!backend || !it->second->info().supports("audio_speech")) {
            return foundation::Err<AudioResult>(foundation::ErrorCode::InvalidArgument,
                                                 "backend does not support speech synthesis: " + name);
        }
        auto slot = backend_slot_for_lease_locked(name, lease_id);
        if (!slot) {
            return foundation::Err<AudioResult>(slot.error().code, slot.error().message);
        }
        backend_slot = *slot;
    }
    return backend->synthesize(backend_slot, request, stream);
}

foundation::Result<void> BackendCoordinator::validate_speech_request(
    const std::string& name, const SpeechRequest& request) {
    auto created = registry_.create_result(name);
    if (!created) {
        return foundation::Err<void>(created.error().code,
                                     created.error().message);
    }
    auto* backend = dynamic_cast<ISpeechBackend*>(created->get());
    if (!backend || !(*created)->info().supports("audio_speech")) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "backend does not support speech synthesis: " + name);
    }
    return backend->validate_speech_request(request);
}

foundation::Result<TranscriptionResult> BackendCoordinator::transcribe(
    const std::string& name, int lease_id, const TranscriptionRequest& request,
    const std::function<bool(int)>& progress) {
    ITranscriptionBackend* backend = nullptr;
    int backend_slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<TranscriptionResult>(foundation::ErrorCode::NotFound,
                                                         "model not loaded: " + name);
        }
        backend = dynamic_cast<ITranscriptionBackend*>(it->second.get());
        if (!backend || !it->second->info().supports("audio_transcription")) {
            return foundation::Err<TranscriptionResult>(foundation::ErrorCode::InvalidArgument,
                                                         "backend does not support transcription: " + name);
        }
        auto slot = backend_slot_for_lease_locked(name, lease_id);
        if (!slot) {
            return foundation::Err<TranscriptionResult>(slot.error().code, slot.error().message);
        }
        backend_slot = *slot;
    }
    return backend->transcribe(backend_slot, request, progress);
}

void BackendCoordinator::drain_active(std::chrono::milliseconds timeout) {
    auto deadline = clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    while (active_requests_ > 0 && clock::now() < deadline) {
        cv_.wait_for(lock, std::chrono::milliseconds{50});
    }
}

int BackendCoordinator::active_request_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_requests_;
}

int BackendCoordinator::active_request_count(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto active = active_requests_by_model_.find(name);
    return active == active_requests_by_model_.end() ? 0 : active->second;
}

}
