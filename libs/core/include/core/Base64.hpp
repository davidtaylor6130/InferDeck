#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace inferdeck::core {

std::vector<uint8_t> Base64Decode(const std::string& input);

} // namespace inferdeck::core
