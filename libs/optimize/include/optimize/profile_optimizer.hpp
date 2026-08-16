#pragma once

#include <string>
#include <vector>

namespace inferdeck::optimize {

struct ProfileInput {
    std::string model;
    double total_vram_mb{0.0};
    double model_file_mb{0.0};
    double configured_vram_mb{0.0};
    double observed_tokens_per_second{0.0};
    int context_per_slot{0};
    int slots{0};
    int min_slots{1};
    int n_batch{512};
    int n_ubatch{512};
    std::string cache_type_k{"q8_0"};
    std::string cache_type_v{"q8_0"};
    std::string flash_attention{"auto"};
};

struct ProfileCandidate {
    int context_per_slot{0};
    int slots{0};
    int n_batch{0};
    int n_ubatch{0};
    std::string cache_type_k;
    std::string cache_type_v;
    std::string flash_attention{"auto"};
    int mtp_max_active_requests{1};
    double estimated_vram_mb{0.0};
    double reserve_vram_mb{0.0};
    double quality_score{0.0};
    double speed_score{0.0};
    double parallelism_score{0.0};
    double headroom_score{0.0};
    double overall_score{0.0};
    bool fits{false};
    std::vector<std::string> reasons;
};

struct ProfileRecommendation {
    ProfileCandidate recommended;
    std::vector<ProfileCandidate> candidates;
    double quality_weight{0.60};
    double speed_weight{0.15};
    double parallelism_weight{0.15};
    double headroom_weight{0.10};
    bool measured{false};
    std::vector<std::string> notes;
};

ProfileRecommendation recommend_profile(const ProfileInput& input);

}
