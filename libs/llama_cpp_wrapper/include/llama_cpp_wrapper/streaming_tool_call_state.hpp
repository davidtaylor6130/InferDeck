#pragma once

#include "model/imodel.hpp"

namespace inferdeck::llama_wrapper::detail {

class StreamingToolCallState {
public:
  void observe(const model::InferenceDelta& delta) noexcept {
    if (!delta.tool_calls.empty()) emitted_ = true;
  }

  [[nodiscard]] bool should_emit_fallback() const noexcept {
    return !emitted_;
  }

private:
  bool emitted_{false};
};

}
