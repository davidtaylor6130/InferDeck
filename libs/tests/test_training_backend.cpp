/// @file test_training_backend.cpp
/// @brief Tests for fine-tuning backend interface and mock implementation.

#include <catch2/catch_test_macros.hpp>
#include "backends/ITrainingBackend.hpp"
#include "backends/ITextBackend.hpp"
#include <memory>

namespace {

using namespace inferdeck::backends;

class MockTrainingBackend : public ITrainingBackend {
public:
    std::string GetBackendName() const override { return "mock_finetune"; }
    BackendStatus GetStatus() const override { return status_; }
    bool Initialize() override { status_ = BackendStatus::READY; return true; }
    void Shutdown() override { status_ = BackendStatus::UNINITIALIZED; }
    nlohmann::json GetInfo() const override { return nlohmann::json{{"name", "mock_finetune"}}; }
    nlohmann::json GetVRAMUsage() const override { return nlohmann::json{{"used", 8ULL << 30}}; }
    bool IsReady() const override { return status_ == BackendStatus::READY; }

    std::string SubmitJob(const nlohmann::json& request) override {
        std::string job_id = "ft_" + std::to_string(job_counter_++);
        jobs_[job_id] = {job_id, request.value("model_path", ""),
                         request.value("train_data_path", ""),
                         request.value("epochs", 3),
                         request.value("learning_rate", 0.0001f),
                         request.value("batch_size", 1),
                         request.value("max_steps", 1000),
                         false,
                         0, 0.0f, {}, request};
        return job_id;
    }

    TrainingJob GetJobStatus(const std::string& job_id) override {
        auto it = jobs_.find(job_id);
        if (it != jobs_.end()) return it->second;
        return {job_id, "", "", 0, 0.0f, 0, false, 0, 0.0f, {}, {}};
    }

    std::vector<TrainingJob> ListJobs() override {
        std::vector<TrainingJob> result;
        for (const auto& [_, job] : jobs_) {
            result.push_back(job);
        }
        return result;
    }

    void CancelJob(const std::string& job_id) override {
        auto it = jobs_.find(job_id);
        if (it != jobs_.end()) {
            it->second.status = TrainingJobStatus::CANCELLED;
        }
    }

    nlohmann::json GetMetrics() const override {
        return nlohmann::json{{"total_jobs", (int)jobs_.size()},
                              {"succeeded", 0}, {"failed", 0}};
    }

    nlohmann::json GetTrainingConfig() const override {
        return nlohmann::json{{"default_epochs", 3},
                              {"default_learning_rate", 0.0001},
                              {"default_batch_size", 1}};
    }

    BackendStatus status_ = BackendStatus::UNINITIALIZED;
    int job_counter_ = 0;
    std::unordered_map<std::string, TrainingJob> jobs_;
};

} // namespace

TEST_CASE("Training: Mock backend name and status", "[training][mock]") {
    MockTrainingBackend backend;
    REQUIRE(backend.GetBackendName() == "mock_finetune");
    REQUIRE(!backend.IsReady());
}

TEST_CASE("Training: Initialize and shutdown", "[training][mock]") {
    MockTrainingBackend backend;
    REQUIRE(backend.Initialize());
    REQUIRE(backend.IsReady());
    backend.Shutdown();
    REQUIRE(!backend.IsReady());
}

TEST_CASE("Training: SubmitJob creates job", "[training][mock]") {
    MockTrainingBackend backend;
    backend.Initialize();

    nlohmann::json request;
    request["model_path"] = "llama.gguf";
    request["train_data_path"] = "train.jsonl";
    request["epochs"] = 5;
    request["learning_rate"] = 0.001f;

    auto job_id = backend.SubmitJob(request);
    REQUIRE(job_id.substr(0, 3) == "ft_");

    auto job = backend.GetJobStatus(job_id);
    REQUIRE(job.model_path == "llama.gguf");
    REQUIRE(job.train_data_path == "train.jsonl");
    REQUIRE(job.epochs == 5);
    REQUIRE(job.learning_rate == 0.001f);
    REQUIRE(job.status == TrainingJobStatus::PENDING);
}

TEST_CASE("Training: GetJobStatus returns correct status", "[training][mock]") {
    MockTrainingBackend backend;
    backend.Initialize();

    nlohmann::json request;
    request["model_path"] = "model.gguf";
    request["train_data_path"] = "data.jsonl";
    auto job_id = backend.SubmitJob(request);

    auto status = backend.GetJobStatus(job_id);
    REQUIRE(status.id == job_id);
    REQUIRE(status.status == TrainingJobStatus::PENDING);
    REQUIRE(status.current_step == 0);
}

TEST_CASE("Training: GetJobStatus for unknown job", "[training][mock]") {
    MockTrainingBackend backend;
    backend.Initialize();

    auto status = backend.GetJobStatus("ft_nonexistent");
    REQUIRE(status.id == "ft_nonexistent");
    REQUIRE(status.status == TrainingJobStatus::PENDING);
}

TEST_CASE("Training: ListJobs returns all jobs", "[training][mock]") {
    MockTrainingBackend backend;
    backend.Initialize();

    nlohmann::json r1; r1["model_path"] = "a.gguf"; r1["train_data_path"] = "a.jsonl";
    nlohmann::json r2; r2["model_path"] = "b.gguf"; r2["train_data_path"] = "b.jsonl";

    backend.SubmitJob(r1);
    backend.SubmitJob(r2);

    auto jobs = backend.ListJobs();
    REQUIRE(jobs.size() == 2);
}

TEST_CASE("Training: CancelJob updates status", "[training][mock]") {
    MockTrainingBackend backend;
    backend.Initialize();

    nlohmann::json request;
    request["model_path"] = "model.gguf";
    request["train_data_path"] = "data.jsonl";
    auto job_id = backend.SubmitJob(request);

    backend.CancelJob(job_id);
    auto status = backend.GetJobStatus(job_id);
    REQUIRE(status.status == TrainingJobStatus::CANCELLED);
}

TEST_CASE("Training: CancelJob for unknown job is safe", "[training][mock]") {
    MockTrainingBackend backend;
    backend.Initialize();
    REQUIRE_NOTHROW(backend.CancelJob("ft_unknown"));
}

TEST_CASE("Training: GetMetrics returns valid JSON", "[training][mock]") {
    MockTrainingBackend backend;
    backend.Initialize();

    nlohmann::json r; r["model_path"] = "m.gguf"; r["train_data_path"] = "d.jsonl";
    backend.SubmitJob(r);

    auto metrics = backend.GetMetrics();
    REQUIRE(metrics.is_object());
    REQUIRE(metrics.contains("total_jobs"));
    REQUIRE(metrics["total_jobs"].get<int>() == 1);
}

TEST_CASE("Training: GetTrainingConfig returns defaults", "[training][mock]") {
    MockTrainingBackend backend;
    auto config = backend.GetTrainingConfig();
    REQUIRE(config.is_object());
    REQUIRE(config.contains("default_epochs"));
    REQUIRE(config["default_epochs"].get<int>() == 3);
    REQUIRE(config.contains("default_learning_rate"));
}

TEST_CASE("Training: TrainingJobStatus enum values", "[training][enum]") {
    REQUIRE(static_cast<int>(TrainingJobStatus::PENDING) == static_cast<int>(0));
    REQUIRE(static_cast<int>(TrainingJobStatus::QUEUED) == static_cast<int>(1));
    REQUIRE(static_cast<int>(TrainingJobStatus::RUNNING) == static_cast<int>(2));
    REQUIRE(static_cast<int>(TrainingJobStatus::SUCCEEDED) == static_cast<int>(3));
    REQUIRE(static_cast<int>(TrainingJobStatus::FAILED) == static_cast<int>(4));
    REQUIRE(static_cast<int>(TrainingJobStatus::CANCELLED) == static_cast<int>(5));
}

TEST_CASE("Training: TrainingCheckpoint fields", "[training][struct]") {
    TrainingCheckpoint checkpoint;
    checkpoint.path = "checkpoints/step_100.gguf";
    checkpoint.step = 100;
    checkpoint.loss = 0.5f;
    checkpoint.validation_loss = 0.6f;

    REQUIRE(checkpoint.path == "checkpoints/step_100.gguf");
    REQUIRE(checkpoint.step == 100);
    REQUIRE(checkpoint.loss == 0.5f);
    REQUIRE(checkpoint.validation_loss == 0.6f);
}

TEST_CASE("Training: Cast to ITextBackend interface", "[training][mock]") {
    std::unique_ptr<ITextBackend> base = std::make_unique<MockTrainingBackend>();
    REQUIRE(base->Initialize());
    REQUIRE(base->IsReady());
    base->Shutdown();
}

TEST_CASE("Training: Cast to ITrainingBackend interface", "[training][mock]") {
    std::unique_ptr<ITrainingBackend> train = std::make_unique<MockTrainingBackend>();
    REQUIRE(train->GetBackendName() == "mock_finetune");

    nlohmann::json r; r["model_path"] = "m.gguf"; r["train_data_path"] = "d.jsonl";
    auto job_id = train->SubmitJob(r);
    auto status = train->GetJobStatus(job_id);
    REQUIRE(!status.id.empty());
}
