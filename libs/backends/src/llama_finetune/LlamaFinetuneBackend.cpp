/// @file LlamaFinetuneBackend.cpp
/// @brief llama.cpp full fine-tuning implementation.

#include "llama_finetune/LlamaFinetuneBackend.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <random>
#include <iomanip>
#include <sstream>

namespace inferdeck::backends {

std::string LlamaFinetuneBackend::GetBackendName() const { return "llama_finetune"; }

BackendStatus LlamaFinetuneBackend::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool LlamaFinetuneBackend::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = BackendStatus::INITIALIZING;

    // Check if training directory exists and has data
    // llama_training_ctx* ctx = llama_training_init();

    spdlog::info("LlamaFinetuneBackend: initialized [max_concurrent: {}]", max_concurrent_jobs_);
    status_ = BackendStatus::READY;
    return true;
}

void LlamaFinetuneBackend::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Cancel all active jobs
    for (auto& [id, job] : jobs_) {
        if (job.status == TrainingJobStatus::RUNNING || job.status == TrainingJobStatus::QUEUED) {
            job.status = TrainingJobStatus::CANCELLED;
            spdlog::warn("LlamaFinetuneBackend: cancelled pending job '{}'", id);
        }
    }
    jobs_.clear();
    active_training_ = 0;
    status_ = BackendStatus::UNINITIALIZED;
    spdlog::info("LlamaFinetuneBackend: shut down");
}

nlohmann::json LlamaFinetuneBackend::GetInfo() const {
    nlohmann::json info;
    info["name"] = "llama_finetune";
    info["backend"] = "llama.cpp";
    info["engine"] = "finetune";
    info["full_finetuning_only"] = true; // no LoRA support
    info["max_concurrent_jobs"] = max_concurrent_jobs_;
    info["active_jobs"] = active_training_;
    info["total_jobs"] = static_cast<int>(jobs_.size());
    info["status"] = static_cast<int>(status_);
    return info;
}

nlohmann::json LlamaFinetuneBackend::GetVRAMUsage() const {
    nlohmann::json vram;
    vram["backend"] = "llama_finetune";
    vram["allocated_bytes"] = 0;
    vram["peak_bytes"] = 0;
    vram["exclusive_lock"] = (active_training_ > 0);
    return vram;
}

bool LlamaFinetuneBackend::IsReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_ == BackendStatus::READY && active_training_ < max_concurrent_jobs_;
}

std::string LlamaFinetuneBackend::SubmitJob(const nlohmann::json& request) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string job_id = GenerateJobId();

    TrainingJob job;
    job.id = job_id;
    job.model_path = request.value("model_path", "");
    job.train_data_path = request.value("train_data_path", "");
    job.epochs = request.value("epochs", 3);
    job.learning_rate = request.value("learning_rate", 0.0001f);
    job.batch_size = request.value("batch_size", 1);
    job.max_steps = request.value("max_steps", 1000);
    job.lora = false;
    job.status = TrainingJobStatus::PENDING;
    job.current_step = 0;
    job.current_loss = 0.0f;

    // Store parameters
    job.parameters["epochs"] = job.epochs;
    job.parameters["learning_rate"] = job.learning_rate;
    job.parameters["batch_size"] = job.batch_size;
    job.parameters["max_steps"] = job.max_steps;
    job.parameters["learning_rate_schedule"] = request.value("learning_rate_schedule", "cosine");
    job.parameters["warmup_steps"] = request.value("warmup_steps", 0);
    job.parameters["adam_beta1"] = request.value("adam_beta1", 0.9);
    job.parameters["adam_beta2"] = request.value("adam_beta2", 0.999);
    job.parameters["adam_eps"] = request.value("adam_eps", 1e-8);

    // If training data is provided as inline format (ChatML JSONL), store it
    if (request.contains("data")) {
        job.metadata["data_format"] = "chatml_jsonl";
        job.metadata["data"] = request["data"];
    }

    jobs_[job_id] = job;

    // Check if we can start training immediately
    if (active_training_ < max_concurrent_jobs_) {
        jobs_[job_id].status = TrainingJobStatus::QUEUED;
        active_training_++;
        spdlog::info("LlamaFinetuneBackend: job '{}' submitted and queued [model: {}, epochs: {}]",
                     job_id, job.model_path, job.epochs);
    } else {
        jobs_[job_id].status = TrainingJobStatus::PENDING;
        spdlog::info("LlamaFinetuneBackend: job '{}' submitted (waiting for capacity)", job_id);
    }

    return job_id;
}

TrainingJob LlamaFinetuneBackend::GetJobStatus(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
        TrainingJob empty;
        empty.id = job_id;
        empty.status = TrainingJobStatus::FAILED;
        empty.metadata["error"] = "job not found";
        return empty;
    }

    return it->second;
}

std::vector<TrainingJob> LlamaFinetuneBackend::ListJobs() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<TrainingJob> jobs;
    jobs.reserve(jobs_.size());
    for (const auto& [id, job] : jobs_) {
        jobs.push_back(job);
    }
    return jobs;
}

void LlamaFinetuneBackend::CancelJob(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
        spdlog::warn("LlamaFinetuneBackend: cancel requested for non-existent job '{}'", job_id);
        return;
    }

    if (it->second.status == TrainingJobStatus::RUNNING) {
        // Signal to stop training
        it->second.status = TrainingJobStatus::CANCELLED;
        active_training_--;
        spdlog::info("LlamaFinetuneBackend: job '{}' cancelled (was running)", job_id);
    } else if (it->second.status == TrainingJobStatus::QUEUED) {
        it->second.status = TrainingJobStatus::CANCELLED;
        spdlog::info("LlamaFinetuneBackend: job '{}' cancelled (was queued)", job_id);
    } else {
        spdlog::info("LlamaFinetuneBackend: job '{}' cancelled (status: {})", job_id,
                     static_cast<int>(it->second.status));
    }
}

nlohmann::json LlamaFinetuneBackend::GetMetrics() const {
    nlohmann::json metrics;
    metrics["total_jobs"] = static_cast<int>(jobs_.size());
    metrics["active_jobs"] = active_training_;
    metrics["completed_jobs"] = 0;
    metrics["failed_jobs"] = 0;

    nlohmann::json by_status = nlohmann::json::object();
    by_status["pending"] = 0;
    by_status["queued"] = 0;
    by_status["running"] = 0;
    by_status["succeeded"] = 0;
    by_status["failed"] = 0;
    by_status["cancelled"] = 0;
    metrics["by_status"] = by_status;

    return metrics;
}

nlohmann::json LlamaFinetuneBackend::GetTrainingConfig() const {
    return training_config_;
}

std::string LlamaFinetuneBackend::GenerateJobId() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(1000, 9999);

    std::ostringstream ss;
    ss << std::setfill('0') << std::setw(8) << time << "_"
       << std::setfill('0') << std::setw(4) << dist(gen);
    return ss.str();
}

void LlamaFinetuneBackend::SetCheckpointDir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    checkpoint_dir_ = dir;
}

std::string LlamaFinetuneBackend::GetCheckpointDir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return checkpoint_dir_;
}

} // namespace inferdeck::backends
