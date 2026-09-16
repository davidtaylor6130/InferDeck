#pragma once

#include <mutex>

namespace inferdeck::native_runtimes {

inline std::mutex& diffusion_runtime_mutex() {
    static std::mutex mutex;
    return mutex;
}

}
