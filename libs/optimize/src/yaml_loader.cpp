#include "optimize/yaml_loader.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace inferdeck::optimize {

namespace {

std::string trim(const std::string& s) {
  std::size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

}

std::optional<ParamSpec> parse_param_spec(const std::string& name, const std::string& type_str,
                                          const std::string& range,
                                          const std::vector<std::string>& choices) {
  ParamSpec s;
  s.name = name;
  const auto t = [](const std::string& v) {
    std::string lower;
    lower.reserve(v.size());
    for (const char c : v) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower;
  }(type_str);
  if (t == "int") {
    s.type = ParamType::Int;
    auto dash = range.find("..");
    if (dash == std::string::npos) throw std::runtime_error("int range requires lo..hi");
    s.min_v = std::stod(trim(range.substr(0, dash)));
    s.max_v = std::stod(trim(range.substr(dash + 2)));
    s.step = 1.0;
  } else if (t == "float" || t == "log_float" || t == "logfloat") {
    s.type = ParamType::Float;
    s.log_scale = (t != "float");
    auto dash = range.find("..");
    if (dash == std::string::npos) throw std::runtime_error("float range requires lo..hi");
    s.min_v = std::stod(trim(range.substr(0, dash)));
    s.max_v = std::stod(trim(range.substr(dash + 2)));
  } else if (t == "categorical") {
    if (choices.empty()) throw std::runtime_error("categorical requires choices");
    s.type = ParamType::Categorical;
    s.choices = choices;
  } else {
    return std::nullopt;
  }
  return s;
}

SearchSpaceConfig load_search_space_yaml(const std::string& yaml_text) {
  SearchSpaceConfig out;
  YAML::Node root = YAML::Load(yaml_text);
  if (!root["params"] || !root["params"].IsMap()) return out;
  for (auto it = root["params"].begin(); it != root["params"].end(); ++it) {
    const std::string name = it->first.as<std::string>();
    const auto& node = it->second;
    std::string type_str = node["type"] ? node["type"].as<std::string>() : "float";
    std::string range;
    if (node["range"]) range = node["range"].as<std::string>();
    std::vector<std::string> choices;
    if (node["choices"] && node["choices"].IsSequence()) {
      for (const auto& c : node["choices"]) choices.push_back(c.as<std::string>());
    }
    auto spec = parse_param_spec(name, type_str, range, choices);
    if (spec) out.specs.push_back(*spec);
  }
  return out;
}

}
