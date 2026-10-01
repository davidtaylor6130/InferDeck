#include "ggml.h"
#include "ggml-backend.h"
#include "ggml-alloc.h"
#include "ggml-vulkan.h"

#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

extern "C" GGML_BACKEND_API int inferdeck_ggml_vk_release_idle_cache();

int main()
{
    if (ggml_backend_vk_get_device_count() == 0)
    {
        std::cout << "SKIP: no Vulkan device\n";
        return 0;
    }
    try
    {
        for (int nRound = 0; nRound < 2; ++nRound)
        {
            {
                const std::unique_ptr<ggml_backend, decltype(&ggml_backend_free)> pBackend(
                    ggml_backend_vk_init(0), ggml_backend_free);
                if (!pBackend)
                {
                    throw std::runtime_error("Vulkan backend initialization failed");
                }
                const std::unique_ptr<ggml_context, decltype(&ggml_free)> pContext(
                    ggml_init({1024 * 1024, nullptr, true}), ggml_free);
                ggml_tensor* const pLeft = ggml_new_tensor_1d(pContext.get(), GGML_TYPE_F32, 256);
                ggml_tensor* const pRight = ggml_new_tensor_1d(pContext.get(), GGML_TYPE_F32, 256);
                ggml_tensor* const pSum = ggml_add(pContext.get(), pLeft, pRight);
                ggml_cgraph* const pGraph = ggml_new_graph(pContext.get());
                ggml_build_forward_expand(pGraph, pSum);
                const std::unique_ptr<ggml_backend_buffer, decltype(&ggml_backend_buffer_free)> pBuffer(
                    ggml_backend_alloc_ctx_tensors(pContext.get(), pBackend.get()), ggml_backend_buffer_free);
                if (!pBuffer)
                {
                    throw std::runtime_error("Vulkan tensor allocation failed");
                }
                std::array<float, 256> aLeft{};
                std::array<float, 256> aRight{};
                std::array<float, 256> aOutput{};
                for (size_t nIndex = 0; nIndex < aLeft.size(); ++nIndex)
                {
                    aLeft[nIndex] = static_cast<float>(nIndex);
                    aRight[nIndex] = static_cast<float>(nRound) + 0.5f;
                }
                ggml_backend_tensor_set(pLeft, aLeft.data(), 0, sizeof(aLeft));
                ggml_backend_tensor_set(pRight, aRight.data(), 0, sizeof(aRight));
                if (inferdeck_ggml_vk_release_idle_cache() != 0)
                {
                    throw std::runtime_error("Cleanup released an owned Vulkan device");
                }
                if (ggml_backend_graph_compute(pBackend.get(), pGraph) != GGML_STATUS_SUCCESS)
                {
                    throw std::runtime_error("Vulkan graph computation failed");
                }
                ggml_backend_tensor_get(pSum, aOutput.data(), 0, sizeof(aOutput));
                for (size_t nIndex = 0; nIndex < aOutput.size(); ++nIndex)
                {
                    if (aOutput[nIndex] != aLeft[nIndex] + aRight[nIndex])
                    {
                        throw std::runtime_error("Vulkan output changed after cache cleanup");
                    }
                }
            }
            if (inferdeck_ggml_vk_release_idle_cache() < 1)
            {
                throw std::runtime_error("Idle Vulkan cache was not released");
            }
        }
        if (inferdeck_ggml_vk_release_idle_cache() != 0)
        {
            throw std::runtime_error("Idle cleanup is not idempotent");
        }
        std::cout << "PASS: owned-device guard, numerical parity, idle cleanup\n";
        return 0;
    }
    catch (const std::exception& oError)
    {
        std::cerr << oError.what() << '\n';
        return 1;
    }
}
