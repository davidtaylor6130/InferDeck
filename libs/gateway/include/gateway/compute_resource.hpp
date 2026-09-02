#pragma once

#include <cstdint>

namespace inferdeck::gateway {

enum class ComputeResource : std::uint8_t {
    None,
    Cpu,
    Gpu,
};

}
