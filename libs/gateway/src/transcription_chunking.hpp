#pragma once

#include "foundation/result.hpp"
#include "model/imodel.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <numeric>
#include <string>
#include <vector>

namespace inferdeck::gateway {

inline bool transcription_budget_admits(std::size_t in_flight, std::size_t request, std::size_t limit) {
    return in_flight == 0 || in_flight + request <= limit;
}

inline std::size_t transcription_chunk_end(const std::vector<float>& pcm, std::size_t begin,
                                           std::size_t window, std::size_t search, std::size_t frame,
                                           std::size_t smooth_frames) {
    if (pcm.size() - begin <= window + search) return pcm.size();
    const std::size_t target = begin + window;
    const std::size_t from = target - std::min(search, window);
    const std::size_t frames = (target - from) / frame;
    if (frames <= smooth_frames) return target;
    std::vector<float> energy(frames, 0.0f);
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t i = from + f * frame; i < from + (f + 1) * frame; ++i) energy[f] += pcm[i] * pcm[i];
    }
    float running = 0.0f;
    for (std::size_t f = 0; f < smooth_frames; ++f) running += energy[f];
    float best_energy = running;
    std::size_t best_start = 0;
    for (std::size_t f = smooth_frames; f < frames; ++f) {
        running += energy[f] - energy[f - smooth_frames];
        if (running < best_energy) {
            best_energy = running;
            best_start = f - smooth_frames + 1;
        }
    }
    return from + (best_start + smooth_frames / 2) * frame;
}

inline std::string transcription_prompt_tail(const std::string& text, std::size_t bytes) {
    if (text.size() <= bytes) return text;
    std::size_t start = text.size() - bytes;
    while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) ++start;
    return text.substr(start);
}

template <typename Transcribe>
foundation::Result<model::TranscriptionResult> transcribe_in_chunks(
    const model::TranscriptionRequest& request, std::size_t window_seconds,
    const std::function<bool(int)>& progress, Transcribe&& transcribe) {
    const std::size_t rate = static_cast<std::size_t>(request.sample_rate);
    const std::size_t window = window_seconds * rate;
    const std::size_t frame = std::max<std::size_t>(1, rate / 50);
    model::TranscriptionResult merged;
    model::TranscriptionRequest chunk;
    chunk.sample_rate = request.sample_rate;
    chunk.language = request.language;
    chunk.temperature = request.temperature;
    std::size_t begin = 0;
    std::size_t done = 0;
    int next_id = 0;
    const std::size_t total = std::max<std::size_t>(1, request.pcm.size());
    while (begin < request.pcm.size()) {
        const std::size_t end = transcription_chunk_end(request.pcm, begin, window, 10 * rate, frame, 12);
        const std::size_t span = end - begin;
        const auto first = request.pcm.begin() + static_cast<std::ptrdiff_t>(begin);
        const auto last = request.pcm.begin() + static_cast<std::ptrdiff_t>(end);
        const float peak = std::accumulate(first, last, 0.0f,
            [](float maximum, float sample) { return std::max(maximum, std::abs(sample)); });
        if (peak >= 0.0001f) {
            chunk.pcm.assign(first, last);
            chunk.prompt = request.prompt;
            const std::string tail = transcription_prompt_tail(merged.text, 200);
            if (!tail.empty()) chunk.prompt += (chunk.prompt.empty() ? "" : " ") + tail;
            const std::function<bool(int)> scaled = [&](int percent) {
                const std::size_t covered = done + span * static_cast<std::size_t>(std::clamp(percent, 0, 100)) / 100;
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
                if (chunk.language.empty() || chunk.language == "auto") chunk.language = part->language;
            }
        }
        done += span;
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
