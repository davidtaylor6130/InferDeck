#pragma once

#include <mutex>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <stable-diffusion.h>

#include "foundation/logging.hpp"

namespace inferdeck::native_runtimes {

#ifdef _WIN32
/// <summary>
/// One lock per process shared by every copy of this code: stable-diffusion.cpp
/// and LTX ship as separate provider DLLs, each with its own statics, but must
/// still take turns on the GPU. A named kernel mutex keyed by process id is
/// the same object in every module.
/// </summary>
class DiffusionRuntimeMutex {
public:
    DiffusionRuntimeMutex()
        : handle_(CreateMutexW(nullptr, FALSE,
                               (L"Local\\InferDeck.diffusion." + std::to_wstring(GetCurrentProcessId())).c_str())) {}
    DiffusionRuntimeMutex(const DiffusionRuntimeMutex&) = delete;
    DiffusionRuntimeMutex& operator=(const DiffusionRuntimeMutex&) = delete;
    ~DiffusionRuntimeMutex() { if (handle_) CloseHandle(handle_); }
    void lock() { if (handle_) WaitForSingleObject(handle_, INFINITE); }
    void unlock() { if (handle_) ReleaseMutex(handle_); }

private:
    HANDLE handle_;
};

inline DiffusionRuntimeMutex& diffusion_runtime_mutex() {
    static DiffusionRuntimeMutex mutex;
    return mutex;
}
#else
inline std::mutex& diffusion_runtime_mutex() {
    static std::mutex mutex;
    return mutex;
}
#endif

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
