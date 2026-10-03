#include "ggml-vulkan.cpp"

extern "C" GGML_BACKEND_API int inferdeck_ggml_vk_release_idle_cache()
{
    int nReleasedDevices = 0;
    try
    {
        for (vk_device& pDevice : vk_instance.devices)
        {
            if (!pDevice)
            {
                continue;
            }

            const long nIdleOwners = 2 + (pDevice->sync_staging ? 1 : 0);
            if (pDevice.use_count() != nIdleOwners)
            {
                continue;
            }

            std::scoped_lock oLock(pDevice->mutex, pDevice->compile_mutex);
            if (pDevice.use_count() != nIdleOwners)
            {
                continue;
            }
            pDevice->device.waitIdle();
            const size_t nStagingBytes = pDevice->sync_staging ? pDevice->sync_staging->size : 0;
            ggml_vk_destroy_buffer(pDevice->sync_staging);

            size_t nReleasedPipelines = 0;
            for (const vk_pipeline_ref& pReference : pDevice->all_pipelines)
            {
                const vk_pipeline pPipeline = pReference.lock();
                if (!pPipeline || !pPipeline->compiled)
                {
                    continue;
                }
                pDevice->device.destroyPipeline(pPipeline->pipeline);
                pDevice->device.destroyPipelineLayout(pPipeline->layout);
                pDevice->device.destroyShaderModule(pPipeline->shader_module);
                pPipeline->pipeline = nullptr;
                pPipeline->layout = nullptr;
                pPipeline->shader_module = nullptr;
                pPipeline->compiled = false;
                ++nReleasedPipelines;
            }
            pDevice->all_pipelines.clear();
            if (nStagingBytes != 0 || nReleasedPipelines != 0)
            {
                ++nReleasedDevices;
                GGML_LOG_INFO("event=vulkan_idle_cache_released device=%zu staging_bytes=%zu pipelines=%zu\n",
                    pDevice->idx, nStagingBytes, nReleasedPipelines);
            }
        }
        return nReleasedDevices;
    }
    catch (const std::exception& oError)
    {
        GGML_LOG_ERROR("event=vulkan_idle_cache_release_failed error=%s\n", oError.what());
        return -1;
    }
}
