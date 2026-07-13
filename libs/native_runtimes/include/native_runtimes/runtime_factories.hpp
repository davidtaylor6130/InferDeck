#pragma once

#include <string>
#include <vector>

#include "model/model_registry.hpp"

namespace inferdeck::native_runtimes {

void register_factories(model::ModelRegistry& registry);
std::vector<std::string> available_runtimes();

}
