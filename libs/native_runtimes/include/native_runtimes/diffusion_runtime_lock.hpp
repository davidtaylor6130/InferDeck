#pragma once

#include <mutex>
#include <string_view>

#include <stable-diffusion.h>

#include "foundation/logging.hpp"

namespace inferdeck::native_runtimes {

inline std::mutex& diffusion_runtime_mutex() {
    static std::mutex mutex;
    return mutex;
}

/// <summary>
/// Routes stable-diffusion.cpp messages into the gateway log. Warnings and
/// errors keep their level; everything else is logged at debug. Without this
/// the runtime's own failure reasons are discarded. Call before creating a
/// context; repeat calls are harmless.
/// </summary>
inline void install_diffusion_logging() {
    sd_set_log_callback([](sd_log_level_t level, const char* text, void*) {
        std::string_view message = text ? text : "";
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
            message.remove_suffix(1);
        }
        if (level == SD_LOG_ERROR) foundation::LOG_ERROR("diffusion_runtime", "{}", message);
        else if (level == SD_LOG_WARN) foundation::LOG_WARN("diffusion_runtime", "{}", message);
        else foundation::LOG_DEBUG("diffusion_runtime", "{}", message);
    }, nullptr);
}

}
