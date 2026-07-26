#include "optimize/profile_optimizer.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>

namespace inferdeck::optimize {

namespace {

double cache_memory_factor(const std::string& type) {
    if (type == "q4_0") return 0.5;
    if (type == "q8_0") return 1.0;
    if (type == "f16") return 2.0;
    throw std::invalid_argument("unsupported KV cache precision: " + type);
}

double cache_quality(const std::string& type) {
    if (type == "q4_0") return 0.94;
    if (type == "q8_0") return 0.985;
    if (type == "f16") return 1.0;
    throw std::invalid_argument("unsupported KV cache precision: " + type);
}

double cache_speed(const std::string& type) {
    if (type == "q4_0") return 1.0;
    if (type == "q8_0") return 0.93;
    if (type == "f16") return 0.82;
    throw std::invalid_argument("unsupported KV cache precision: " + type);
}

template <typename Value>
void add_unique(std::vector<Value>& values, Value value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

}

ProfileRecommendation recommend_profile(const ProfileInput& input) {
    if (input.model.empty()) throw std::invalid_argument("model is required");
    if (input.total_vram_mb <= 0.0) throw std::invalid_argument("GPU VRAM capacity is required");
    if (input.model_file_mb <= 0.0 && input.configured_vram_mb <= 0.0) {
        throw std::invalid_argument("model size or configured VRAM is required");
    }
    if (input.context_per_slot < 512) throw std::invalid_argument("context per slot must be at least 512");
    if (input.slots < 1 || input.min_slots < 1 || input.min_slots > input.slots) {
        throw std::invalid_argument("invalid slot bounds");
    }
    if (input.n_batch < 1 || input.n_ubatch < 1 || input.n_ubatch > input.n_batch) {
        throw std::invalid_argument("invalid batch sizes");
    }

    const double current_cache_factor =
        (cache_memory_factor(input.cache_type_k) +
         cache_memory_factor(input.cache_type_v)) / 2.0;
    const double model_resident_mb = input.model_file_mb > 0.0
        ? input.model_file_mb * 1.06
        : input.configured_vram_mb * 0.78;
    const double configured_mb = std::max(input.configured_vram_mb, model_resident_mb);
    const double current_kv_mb = std::max(
        256.0, configured_mb - model_resident_mb);
    const double current_context_total =
        static_cast<double>(input.context_per_slot) * input.slots;

    std::vector<int> contexts;
    add_unique(contexts, input.context_per_slot);
    if (input.context_per_slot > 100000) add_unique(contexts, 100000);
    if (input.context_per_slot > 65536) add_unique(contexts, 65536);
    if (input.context_per_slot > 32768) add_unique(contexts, 32768);

    std::vector<int> slots;
    add_unique(slots, input.slots);
    if (input.slots > input.min_slots) add_unique(slots, input.min_slots);
    if (input.slots > 4 && input.min_slots <= 4) add_unique(slots, 4);
    if (input.slots > 2 && input.min_slots <= 2) add_unique(slots, 2);

    using CachePair = std::pair<std::string, std::string>;
    std::vector<CachePair> caches;
    add_unique(caches, CachePair{"q8_0", "q8_0"});
    add_unique(caches, CachePair{input.cache_type_k, input.cache_type_v});
    add_unique(caches, CachePair{"q4_0", "q8_0"});
    add_unique(caches, CachePair{"q4_0", "q4_0"});
    add_unique(caches, CachePair{"f16", "f16"});

    std::vector<int> batches;
    add_unique(batches, input.n_batch);
    add_unique(batches, 2048);
    add_unique(batches, 1024);
    add_unique(batches, 512);

    ProfileRecommendation result;
    for (const int context : contexts) {
        for (const int slot_count : slots) {
            for (const auto& [cache_k, cache_v] : caches) {
                for (const int batch : batches) {
                    ProfileCandidate candidate;
                    candidate.context_per_slot = context;
                    candidate.slots = slot_count;
                    candidate.n_batch = batch;
                    candidate.n_ubatch = std::min(batch, std::max(1, input.n_ubatch));
                    candidate.cache_type_k = cache_k;
                    candidate.cache_type_v = cache_v;

                    const double candidate_cache_factor =
                        (cache_memory_factor(cache_k) +
                         cache_memory_factor(cache_v)) / 2.0;
                    const double context_ratio =
                        (static_cast<double>(context) * slot_count) /
                        current_context_total;
                    const double batch_delta =
                        std::max(0, batch - input.n_batch) / 512.0 * 48.0;
                    candidate.estimated_vram_mb =
                        model_resident_mb +
                        current_kv_mb * context_ratio *
                            (candidate_cache_factor / current_cache_factor) +
                        batch_delta;
                    candidate.reserve_vram_mb =
                        input.total_vram_mb - candidate.estimated_vram_mb;
                    candidate.fits =
                        candidate.estimated_vram_mb <= input.total_vram_mb * 0.88;

                    const double kv_quality =
                        (cache_quality(cache_k) + cache_quality(cache_v)) / 2.0;
                    const double context_quality =
                        0.85 + 0.15 * std::sqrt(
                            std::min(1.0, static_cast<double>(context) /
                                                   input.context_per_slot));
                    candidate.quality_score =
                        std::clamp(kv_quality * 0.60 + context_quality * 0.40,
                                   0.0, 1.0);
                    const double cache_speed_score =
                        (cache_speed(cache_k) + cache_speed(cache_v)) / 2.0;
                    const double batch_score =
                        std::clamp(static_cast<double>(batch) / 2048.0,
                                   0.25, 1.0);
                    candidate.speed_score =
                        std::clamp(cache_speed_score * 0.65 +
                                       batch_score * 0.35,
                                   0.0, 1.0);
                    candidate.parallelism_score =
                        std::clamp(static_cast<double>(slot_count) /
                                       input.slots,
                                   0.0, 1.0);
                    candidate.headroom_score =
                        std::clamp(candidate.reserve_vram_mb /
                                       (input.total_vram_mb * 0.25),
                                   0.0, 1.0);
                    candidate.overall_score =
                        result.quality_weight * candidate.quality_score +
                        result.speed_weight * candidate.speed_score +
                        result.parallelism_weight *
                            candidate.parallelism_score +
                        result.headroom_weight * candidate.headroom_score;
                    if (!candidate.fits) candidate.overall_score *= 0.25;

                    candidate.reasons.push_back(
                        "Keeps " + std::to_string(context) +
                        " context tokens per slot across " +
                        std::to_string(slot_count) + " slot(s)");
                    candidate.reasons.push_back(
                        "Uses " + cache_k + "/" + cache_v +
                        " KV cache precision");
                    candidate.reasons.push_back(
                        "Reserves about " +
                        std::to_string(static_cast<int>(
                            std::max(0.0, candidate.reserve_vram_mb))) +
                        " MB of VRAM");
                    result.candidates.push_back(std::move(candidate));
                }
            }
        }
    }

    std::sort(result.candidates.begin(), result.candidates.end(),
              [](const ProfileCandidate& left,
                 const ProfileCandidate& right) {
                  if (left.fits != right.fits) return left.fits > right.fits;
                  if (std::abs(left.overall_score - right.overall_score) >
                      1e-9) {
                      return left.overall_score > right.overall_score;
                  }
                  if (std::abs(left.quality_score - right.quality_score) >
                      1e-9) {
                      return left.quality_score > right.quality_score;
                  }
                  return std::tie(left.context_per_slot, left.slots) >
                         std::tie(right.context_per_slot, right.slots);
              });
    if (result.candidates.empty() || !result.candidates.front().fits) {
        throw std::runtime_error(
            "no candidate preserves the required 12 percent VRAM reserve");
    }
    if (result.candidates.size() > 8) result.candidates.resize(8);
    result.recommended = result.candidates.front();
    result.notes = {
        "This is a hardware and profile estimate, not a measured quality benchmark.",
        "The model-file quantization is fixed by the selected GGUF artifact and is not changed.",
        "Normal request seeds remain random; measured benchmark trials should use a fixed seed.",
        "Apply only stages the fields in Model Details. Saving the active profile remains explicit."
    };
    if (input.observed_tokens_per_second > 0.0) {
        result.notes.push_back(
            "Persisted baseline throughput is " +
            std::to_string(input.observed_tokens_per_second) +
            " output tokens per second; candidate speed scores are estimates.");
    }
    return result;
}

}
