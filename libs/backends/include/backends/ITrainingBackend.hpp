/// @file ITrainingBackend.hpp
/// @brief Model fine-tuning backend interface.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>
#include "backends/ITextBackend.hpp"

namespace inferdeck::backends {

enum class TrainingJobStatus {
    PENDING,
    QUEUED,
    RUNNING,
    SUCCEEDED,
    FAILED,
    CANCELLED
};

struct TrainingCheckpoint {
    std::string path;
    int step;
    float loss;
    double validation_loss;
    nlohmann::json metrics;
};

struct TrainingJob {
    std::string id;
    std::string model_path;
    std::string train_data_path;
    int epochs;
    float learning_rate;
    int batch_size;
    int max_steps;
    bool lora;
    TrainingJobStatus status;
    int current_step;
    float current_loss;
    std::vector<TrainingCheckpoint> checkpoints;
    nlohmann::json parameters;
    nlohmann::json metadata;
};

class ITrainingBackend : public ITextBackend {
public:
    ~ITrainingBackend() override = default;

    virtual std::string SubmitJob(const nlohmann::json& request) = 0;
    virtual TrainingJob GetJobStatus(const std::string& job_id) = 0;
    virtual std::vector<TrainingJob> ListJobs() = 0;
    virtual void CancelJob(const std::string& job_id) = 0;
    virtual nlohmann::json GetMetrics() const = 0;
    virtual nlohmann::json GetTrainingConfig() const = 0;
};

} // namespace inferdeck::backends
