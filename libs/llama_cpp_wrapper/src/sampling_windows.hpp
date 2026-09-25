#pragma once

#include "sampling.h"

namespace inferdeck::llama_wrapper::detail {

inline void resolve_sampling_windows(common_params_sampling& sampling, int context_size) {
  if (sampling.penalty_last_n == -1) sampling.penalty_last_n = context_size;
  if (sampling.dry_penalty_last_n == -1) sampling.dry_penalty_last_n = context_size;
}

}
