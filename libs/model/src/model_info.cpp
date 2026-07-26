#include "model/model_info.hpp"

#include <algorithm>

namespace inferdeck::model {

bool ModelInfo::supports(const std::string& capability) const {
    return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
}

} // namespace inferdeck::model
