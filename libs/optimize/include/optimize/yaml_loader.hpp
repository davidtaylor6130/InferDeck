#pragma once

#include <optional>
#include <string>
#include <vector>

#include "optimize/search_space.hpp"

namespace inferdeck::optimize {

struct SearchSpaceConfig {
  std::vector<ParamSpec> specs;
};

SearchSpaceConfig load_search_space_yaml(const std::string& yaml_text);

std::optional<ParamSpec> parse_param_spec(const std::string& name, const std::string& type_str,
                                          const std::string& range,
                                          const std::vector<std::string>& choices);

}
