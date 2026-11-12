#pragma once

#include <string>
#include <vector>

#include "model/provider.hpp"

namespace inferdeck::native_runtimes {

/// <summary>One descriptor per native media/speech runtime linked into this build.</summary>
std::vector<model::ProviderDescriptor> providers();
void register_factories(model::ModelRegistry& registry);
std::vector<std::string> available_runtimes();

}
