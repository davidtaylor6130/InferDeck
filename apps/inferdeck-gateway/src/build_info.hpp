#pragma once

#include "inferdeck_build_info.hpp"

#include <string>

namespace inferdeck::app {

inline std::string build_identity()
{
    return "inferdeck-gateway " + std::string(build_version) +
        " revision=" + std::string(build_revision) +
        " dirty=" + (build_dirty ? "true" : "false");
}

}
