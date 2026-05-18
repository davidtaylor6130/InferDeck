/// @file LlamaFinetuneBackend.hpp
/// @brief Full fine-tuning backend using llama.cpp finetune.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "backends/ITrainingBackend.hpp"

namespace inferdeck::backends {

class LlamaFinetuneBackend : public ITrainingBackend {
public:
    LlamaFinetuneBackend() = default;
    ~LlamaFinetuneBackend() override = default;

    std::string GetBackendName() const override;
    BackendStatus GetStatus() const override;
    bool Initialize() override;
    void Shutdown() override;
    nlohmann::json GetInfo() const override;
    nlohmann::json GetVRAMUsage() const override;
    bool IsReady() const override;

    std::string SubmitJob(const nlohmann::json& request) override;
    TrainingJob GetJobStatus(const std::string& job_id) override;
    std::vector<TrainingJob> ListJobs() override;
    void CancelJob(const std::string& job_id) override;
    nlohmann::json GetMetrics() const override;
    nlohmann::json GetTrainingConfig() const override;

    // Configuration
    void SetCheckpointDir(const std::string& dir);
    std::string GetCheckpointDir() const;

private:
    std::string GenerateJobId();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, TrainingJob> jobs_;
    int active_training_{0};
    std::string checkpoint_dir_;
    BackendStatus status_{BackendStatus::UNINITIALIZED};
    int max_concurrent_jobs_{1};
    nlohmann::json training_config_;
};

} // namespace inferdeck::backends
