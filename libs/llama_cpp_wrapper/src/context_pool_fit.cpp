#include "context_pool_fit.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>

#include "foundation/logging.hpp"
#include "ggml-backend.h"
#include "llama-ext.h"

namespace inferdeck::llama_wrapper {
namespace {

constexpr std::size_t kMib = 1024ULL * 1024ULL;
constexpr std::size_t kGib = 1024ULL * kMib;

using ModelPtr = std::unique_ptr<llama_model, decltype(&llama_model_free)>;
using ContextPtr = std::unique_ptr<llama_context, decltype(&llama_free)>;

foundation::Result<void> check_lifecycle(
    const model::LifecycleControl& control) {
  if (control.is_cancelled()) {
    return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                 "context pool fitting cancelled");
  }
  if (control.is_expired()) {
    return foundation::Err<void>(foundation::ErrorCode::Timeout,
                                 "context pool fitting deadline expired");
  }
  return foundation::Ok();
}

foundation::Result<std::size_t> checked_add(std::size_t left,
                                             std::size_t right) {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return foundation::Err<std::size_t>(
        foundation::ErrorCode::OutOfMemory,
        "context pool memory estimate overflowed");
  }
  return left + right;
}

struct Requirements {
  std::size_t host = 0;
  std::map<ggml_backend_dev_t, std::size_t> devices;
};

foundation::Result<void> add_breakdown(Requirements& requirements,
                                        llama_context* context) {
  const llama_memory_breakdown breakdown = llama_get_memory_breakdown(context);
  for (const auto& [buffer_type, data] : breakdown) {
    const auto combined = checked_add(data.context, data.compute);
    if (!combined) {
      return std::unexpected(combined.error());
    }
    if (*combined == 0) {
      continue;
    }

    if (ggml_backend_buft_is_host(buffer_type)) {
      const auto total = checked_add(requirements.host, *combined);
      if (!total) {
        return std::unexpected(total.error());
      }
      requirements.host = *total;
      continue;
    }

    ggml_backend_dev_t device = ggml_backend_buft_get_device(buffer_type);
    if (device == nullptr) {
      return foundation::Err<void>(
          foundation::ErrorCode::Internal,
          "context pool estimate contains an unmapped non-host buffer");
    }
    const auto total = checked_add(requirements.devices[device], *combined);
    if (!total) {
      return std::unexpected(total.error());
    }
    requirements.devices[device] = *total;
  }
  return foundation::Ok();
}

foundation::Result<bool> check_available(
    const Requirements& requirements,
    int capacity,
    int vram_safety_margin_mb) {
  bool fits = true;
  if (requirements.host > 0) {
    ggml_backend_dev_t cpu =
        ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
    if (cpu == nullptr) {
      return foundation::Err<bool>(foundation::ErrorCode::Internal,
                                   "host memory device is unavailable");
    }
    std::size_t free = 0;
    std::size_t total = 0;
    ggml_backend_dev_memory(cpu, &free, &total);
    if (free == 0 && total == 0) {
      return foundation::Err<bool>(foundation::ErrorCode::Internal,
                                   "host memory availability is unknown");
    }
    constexpr std::size_t margin = 2ULL * kGib;
    const bool device_fits = free >= margin &&
                             requirements.host <= free - margin;
    fits = fits && device_fits;
    foundation::LOG_INFO(
        "context_pool_fit_probe",
        "capacity={} device=host required_mb={} free_mb={} margin_mb={} fits={}",
        capacity, requirements.host / kMib, free / kMib, margin / kMib,
        device_fits);
  }

  const std::size_t margin = static_cast<std::size_t>(vram_safety_margin_mb) * kMib;
  for (const auto& [device, required] : requirements.devices) {
    std::size_t free = 0;
    std::size_t total = 0;
    ggml_backend_dev_memory(device, &free, &total);
    if (free == 0 && total == 0) {
      return foundation::Err<bool>(foundation::ErrorCode::Internal,
                                   "device memory availability is unknown");
    }
    const bool device_fits = free >= margin && required <= free - margin;
    fits = fits && device_fits;
    foundation::LOG_INFO(
        "context_pool_fit_probe",
        "capacity={} device={} required_mb={} free_mb={} margin_mb={} fits={}",
        capacity, ggml_backend_dev_name(device), required / kMib, free / kMib,
        margin / kMib, device_fits);
  }
  return fits;
}

} // namespace

foundation::Result<int> fit_context_pool(
    const std::filesystem::path& model_path,
    const llama_model_params& model_params,
    const llama_context_params& context_params,
    bool mtp,
    int minimum_capacity,
    int maximum_capacity,
    int vram_safety_margin_mb,
    const model::LifecycleControl& control) try {
  if (model_path.empty() || minimum_capacity <= 0 ||
      maximum_capacity < minimum_capacity || vram_safety_margin_mb < 0) {
    return foundation::Err<int>(foundation::ErrorCode::InvalidArgument,
                                "invalid context pool fitting bounds");
  }
  if (static_cast<std::uint64_t>(maximum_capacity) >
      std::numeric_limits<std::uint32_t>::max()) {
    return foundation::Err<int>(foundation::ErrorCode::InvalidArgument,
                                "context pool capacity exceeds llama limits");
  }
  if (const auto lifecycle = check_lifecycle(control); !lifecycle) {
    return std::unexpected(lifecycle.error());
  }

  llama_model_params probe_model_params = model_params;
  probe_model_params.no_alloc = true;
  probe_model_params.load_mode = LLAMA_LOAD_MODE_NONE;
  probe_model_params.progress_callback = [](float, void* user_data) {
    const auto* lifecycle =
        static_cast<const model::LifecycleControl*>(user_data);
    return !lifecycle->is_cancelled() && !lifecycle->is_expired();
  };
  probe_model_params.progress_callback_user_data =
      const_cast<model::LifecycleControl*>(&control);

  const std::string native_path = model_path.string();
  ModelPtr probe_model(
      llama_model_load_from_file(native_path.c_str(), probe_model_params),
      llama_model_free);
  if (!probe_model) {
    if (const auto lifecycle = check_lifecycle(control); !lifecycle) {
      return std::unexpected(lifecycle.error());
    }
    return foundation::Err<int>(foundation::ErrorCode::Internal,
                                "context pool metadata model load failed");
  }

  int probe_count = 0;
  const auto probe = [&](int capacity) -> foundation::Result<bool> {
    if (const auto lifecycle = check_lifecycle(control); !lifecycle) {
      return std::unexpected(lifecycle.error());
    }
    ++probe_count;
    llama_context_params target_params = context_params;
    target_params.n_ctx = static_cast<std::uint32_t>(capacity);
    ContextPtr target(llama_init_from_model(probe_model.get(), target_params),
                      llama_free);
    if (!target) {
      return foundation::Err<bool>(foundation::ErrorCode::Internal,
                                   "target context memory probe failed");
    }

    Requirements requirements;
    if (const auto added = add_breakdown(requirements, target.get()); !added) {
      return std::unexpected(added.error());
    }

    ContextPtr draft(nullptr, llama_free);
    if (mtp) {
      llama_context_params draft_params = target_params;
      draft_params.ctx_type = LLAMA_CONTEXT_TYPE_MTP;
      draft_params.n_rs_seq = 0;
      draft_params.n_ubatch = std::min<std::uint32_t>(draft_params.n_ubatch, 512);
      draft.reset(llama_init_from_model(probe_model.get(), draft_params));
      if (!draft) {
        return foundation::Err<bool>(foundation::ErrorCode::Internal,
                                     "draft context memory probe failed");
      }
      if (const auto added = add_breakdown(requirements, draft.get()); !added) {
        return std::unexpected(added.error());
      }
    }
    if (requirements.host == 0 && requirements.devices.empty()) {
      return foundation::Err<bool>(foundation::ErrorCode::Internal,
          "context pool memory estimate is empty");
    }
    return check_available(requirements, capacity, vram_safety_margin_mb);
  };

  const auto minimum_fits = probe(minimum_capacity);
  if (!minimum_fits) {
    return std::unexpected(minimum_fits.error());
  }
  if (!*minimum_fits) {
    return foundation::Err<int>(foundation::ErrorCode::OutOfMemory,
                                "minimum shared context pool does not fit");
  }

  int selected = minimum_capacity;
  if (maximum_capacity != minimum_capacity) {
    const auto maximum_fits = probe(maximum_capacity);
    if (!maximum_fits) {
      return std::unexpected(maximum_fits.error());
    }
    if (*maximum_fits) {
      selected = maximum_capacity;
    } else {
      int first_failure = maximum_capacity;
      for (int binary_probe = 0;
           binary_probe < 12 && selected + 1 < first_failure;
           ++binary_probe) {
        if (const auto lifecycle = check_lifecycle(control); !lifecycle) {
          return std::unexpected(lifecycle.error());
        }
        const int midpoint = selected + (first_failure - selected) / 2;
        const auto midpoint_fits = probe(midpoint);
        if (!midpoint_fits) {
          return std::unexpected(midpoint_fits.error());
        }
        if (*midpoint_fits) {
          selected = midpoint;
        } else {
          first_failure = midpoint;
        }
      }
    }
  }

  foundation::LOG_INFO(
      "context_pool_fit_selected",
      "selected_capacity={} minimum_capacity={} maximum_capacity={} probes={}",
      selected, minimum_capacity, maximum_capacity, probe_count);
  return selected;
} catch (const std::exception& error) {
  return foundation::Err<int>(foundation::ErrorCode::Internal,
      std::string("context pool estimation failed: ") + error.what());
}

} // namespace inferdeck::llama_wrapper
