#pragma once

#include <string>

namespace inferdeck::gateway {

std::string mask_config_secrets(std::string text);
std::string restore_config_secrets(std::string submitted,
                                   const std::string& current);

}
