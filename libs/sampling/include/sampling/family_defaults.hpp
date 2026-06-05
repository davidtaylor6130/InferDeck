#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "sampling/SamplerParams.hpp"

namespace inferdeck::sampling {

[[nodiscard]] SamplerParams defaults_for_family(std::string_view family);

[[nodiscard]] SamplerParams apply_user_overrides(
    SamplerParams base,
    const SamplerParams& overrides);

[[nodiscard]] bool is_known_family(std::string_view family);

} // namespace inferdeck::sampling
