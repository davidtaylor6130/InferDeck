#pragma once

#include "foundation/result.hpp"
#include "model/imodel.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>

namespace inferdeck::gateway {

inline std::size_t transcription_chunk_end(const std::vector<float>& pcm, std::size_t begin,
                                           std::size_t window, std::size_t search, std::size_t step) {
    if (pcm.size() - begin <= window + search) return pcm.size();
    const std::size_t target = begin + window;
    const std::size_t from = target - std::min(search, window);
    std::size_t best = target;
    float best_energy = -1.0f;
    for (std::size_t at = from; at + step <= target; at += step) {
        float energy = 0.0f;
        for (std::size_t i = at; i < at + step; ++i) energy += pcm[i] * pcm[i];
        if (best_energy < 0.0f || energy < best_energy) {
            best_energy = energy;
            best = at + step / 2;
        }
    }
    return best;
}

template <typename Transcribe>
foundation::Result<model::TranscriptionResult> transcribe_in_chunks(
    const model::TranscriptionRequest& request, std::size_t window_seconds,
    const std::function<bool(int)>& progress, Transcribe&& transcribe) {
    const std::size_t rate = static_cast<std::size_t>(request.sample_rate);
    const std::size_t window = window_seconds * rate;
    model::TranscriptionResult merged;
    model::TranscriptionRequest chunk = request;
    std::size_t begin = 0;
    std::size_t done = 0;
    int next_id = 0;
    const std::size_t total = std::max<std::size_t>(1, request.pcm.size());
    while (begin < request.pcm.size()) {
        const std::size_t end = transcription_chunk_end(request.pcm, begin, window, 10 * rate, rate / 50);
        chunk.pcm.assign(request.pcm.begin() + static_cast<std::ptrdiff_t>(begin),
                         request.pcm.begin() + static_cast<std::ptrdiff_t>(end));
        const std::function<bool(int)> scaled = [&](int percent) {
            const std::size_t covered = done + (end - begin) * static_cast<std::size_t>(std::clamp(percent, 0, 100)) / 100;
            return !progress || progress(static_cast<int>(covered * 100 / total));
        };
        auto part = transcribe(chunk, scaled);
        if (!part) return part;
        const float offset = static_cast<float>(begin) / static_cast<float>(rate);
        if (!part->text.empty()) {
            const bool join = !merged.text.empty() && merged.text.back() != ' ' && part->text.front() != ' ';
            if (join) merged.text += ' ';
            merged.text += part->text;
        }
        for (auto segment : part->segments) {
            segment.id = next_id++;
            segment.start_seconds += offset;
            segment.end_seconds += offset;
            merged.segments.push_back(std::move(segment));
        }
        merged.inference_ms += part->inference_ms;
        if (merged.language.empty()) {
            merged.language = part->language;
            if (chunk.language.empty()) chunk.language = part->language;
        }
        done += end - begin;
        begin = end;
        if (progress && !progress(static_cast<int>(done * 100 / total)) && begin < request.pcm.size()) {
            return foundation::Err<model::TranscriptionResult>(foundation::ErrorCode::Cancelled,
                                                                "transcription cancelled");
        }
    }
    merged.duration_seconds = static_cast<float>(request.pcm.size()) / static_cast<float>(rate);
    return foundation::Ok(std::move(merged));
}

}
