#include <catch2/catch_test_macros.hpp>

#include "gateway/gpu_backend.hpp"

using inferdeck::gateway::compiled_gpu_backend;
using inferdeck::gateway::gpu_backend_diagnostics;
using inferdeck::gateway::validate_model_compute;
using inferdeck::model::ModelCompute;
using inferdeck::model::ModelInfo;

namespace {

ModelInfo info_with(inferdeck::model::ModelCompute compute) {
    ModelInfo info{};
    info.name = "probe";
    info.compute = compute;
    return info;
}

}

TEST_CASE("compiled backend is a known Vulkan/HIP selector value") {
    const auto backend = compiled_gpu_backend();
    CHECK((backend == "VULKAN" || backend == "HIP"));
}

TEST_CASE("backend diagnostics report compiled backend and device list") {
    const auto diag = gpu_backend_diagnostics();
    CHECK(diag.at("compiledBackend").get<std::string>() == compiled_gpu_backend());
    CHECK(diag.contains("deviceCount"));
    CHECK(diag.contains("devices"));
    CHECK(diag.at("devices").is_array());
}

TEST_CASE("model compute validation matches the compiled backend") {
    const bool is_hip = compiled_gpu_backend() == "HIP";
    CHECK_FALSE(validate_model_compute(info_with(ModelCompute::Cpu)).has_value());
    CHECK_FALSE(validate_model_compute(info_with(ModelCompute::Mixed)).has_value());
    CHECK(validate_model_compute(info_with(ModelCompute::VulkanGpu)).has_value() == is_hip);
    CHECK(validate_model_compute(info_with(ModelCompute::RocmGpu)).has_value() == !is_hip);
    CHECK(validate_model_compute(info_with(ModelCompute::CudaGpu)).has_value());
}

TEST_CASE("ggml backend selection does not override another runtime", "[radiance]")
{
    ModelInfo info = info_with(ModelCompute::RocmGpu);
    info.runtime = "vllm_radiance";
    CHECK_FALSE(validate_model_compute(info).has_value());
}
