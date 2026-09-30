#include "context_pool_fit.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

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

struct AvailableMemory { std::size_t free = 0; std::size_t total = 0; };
using MemorySnapshot = std::map<ggml_backend_dev_t, AvailableMemory>;

struct Requirements {
  std::size_t host = 0;
  std::map<ggml_backend_dev_t, std::size_t> devices;
};

foundation::Result<void> add_breakdown(Requirements& requirements,
                                        const llama_context* context) {
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
    int vram_safety_margin_mb,
    const Requirements& reclaimable,
    const MemorySnapshot& available) {
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
    free = available.at(cpu).free;
    total = available.at(cpu).total;
    if (free == 0 && total == 0) {
      return foundation::Err<bool>(foundation::ErrorCode::Internal,
                                   "host memory availability is unknown");
    }
    const auto credited = checked_add(free, reclaimable.host);
    if (!credited) return std::unexpected(credited.error());
    free = std::min(total, *credited);
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
    free = available.at(device).free;
    total = available.at(device).total;
    if (free == 0 && total == 0) {
      return foundation::Err<bool>(foundation::ErrorCode::Internal,
                                   "device memory availability is unknown");
    }
    const auto existing = reclaimable.devices.find(device);
    const std::size_t credit = existing == reclaimable.devices.end() ? 0 : existing->second;
    const auto credited = checked_add(free, credit);
    if (!credited) return std::unexpected(credited.error());
    free = std::min(total, *credited);
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

foundation::Result<std::size_t> context_pool_device_memory_bytes(
    const llama_context* target_context,
    const llama_context* draft_context) try {
  Requirements requirements;
  if (target_context != nullptr) {
    if (const auto added = add_breakdown(requirements, target_context); !added) {
      return std::unexpected(added.error());
    }
  }
  if (draft_context != nullptr) {
    if (const auto added = add_breakdown(requirements, draft_context); !added) {
      return std::unexpected(added.error());
    }
  }

  std::size_t total = 0;
  for (const auto& [device, required] : requirements.devices) {
    (void)device;
    const auto next = checked_add(total, required);
    if (!next) {
      return std::unexpected(next.error());
    }
    total = *next;
  }
  return total;
} catch (const std::exception& error) {
  return foundation::Err<std::size_t>(
      foundation::ErrorCode::Internal,
      std::string("context pool memory snapshot failed: ") + error.what());
} catch (...) {
  return foundation::Err<std::size_t>(
      foundation::ErrorCode::Internal,
      "context pool memory snapshot failed");
}

foundation::Result<int> fit_context_pool(
    const std::filesystem::path& model_path,
    const llama_model_params& model_params,
    const llama_context_params& context_params,
    bool mtp,
    int minimum_capacity,
    int maximum_capacity,
    int vram_safety_margin_mb,
    const model::LifecycleControl& control,
    const llama_context* reclaimable_target,
    const llama_context* reclaimable_draft,
    int* automatic_sequence_capacity) try {
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

  std::vector<float> single_device_split(llama_max_devices(), 0.0f);
  std::size_t gpu_count = 0;
  if (probe_model_params.devices) {
    while (probe_model_params.devices[gpu_count]) ++gpu_count;
  } else {
    for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
      const enum ggml_backend_dev_type type = ggml_backend_dev_type(ggml_backend_dev_get(i));
      if (type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU) ++gpu_count;
    }
  }
  if (!probe_model_params.tensor_split && gpu_count == 1 && !single_device_split.empty()) {
    single_device_split[0] = 1.0f;
    probe_model_params.tensor_split = single_device_split.data();
  }

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

  Requirements reclaimable;
  for (const llama_context* context : {reclaimable_target, reclaimable_draft}) {
    if (context != nullptr) {
      const auto added = add_breakdown(reclaimable, context);
      if (!added) return std::unexpected(added.error());
    }
  }
  for (const auto& [device, bytes] : reclaimable.devices) {
    std::size_t free = 0, total = 0;
    ggml_backend_dev_memory(device, &free, &total);
    const auto recoverable = checked_add(free, bytes);
    if (!recoverable) return std::unexpected(recoverable.error());
    if (static_cast<std::size_t>(vram_safety_margin_mb) * kMib >= std::min(total, *recoverable)) {
      return foundation::Err<int>(foundation::ErrorCode::OutOfMemory,
          "requested reserve cannot fit after context reclamation");
    }
  }
  int selected_actual_capacity = 0;
  int probe_count = 0;
  const auto probe = [&](int capacity, int sequences = 0) -> foundation::Result<bool> {
    if (const auto lifecycle = check_lifecycle(control); !lifecycle) {
      return std::unexpected(lifecycle.error());
    }
    ++probe_count;
    MemorySnapshot available;
    for (std::size_t index = 0; index < ggml_backend_dev_count(); ++index) {
      ggml_backend_dev_t device = ggml_backend_dev_get(index);
      AvailableMemory memory;
      ggml_backend_dev_memory(device, &memory.free, &memory.total);
      available.emplace(device, memory);
    }
    llama_context_params target_params = context_params;
    target_params.n_ctx = static_cast<std::uint32_t>(capacity);
    if (sequences > 0) target_params.n_seq_max = static_cast<std::uint32_t>(sequences);
    ContextPtr target(llama_init_from_model(probe_model.get(), target_params),
                      llama_free);
    if (!target) {
      if (automatic_sequence_capacity && sequences > 1) return false;
      return foundation::Err<bool>(reclaimable_target ? foundation::ErrorCode::ResourceBusy : foundation::ErrorCode::OutOfMemory,
                                   "target context memory probe cannot allocate temporary state");
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
        if (automatic_sequence_capacity && sequences > 1) return false;
        return foundation::Err<bool>(reclaimable_target ? foundation::ErrorCode::ResourceBusy : foundation::ErrorCode::OutOfMemory,
                                     "draft context memory probe cannot allocate temporary state");
      }
      if (const auto added = add_breakdown(requirements, draft.get()); !added) {
        return std::unexpected(added.error());
      }
    }
    if (requirements.host == 0 && requirements.devices.empty()) {
      return foundation::Err<bool>(foundation::ErrorCode::Internal,
          "context pool memory estimate is empty");
    }
    const auto fits = check_available(requirements, capacity, vram_safety_margin_mb, reclaimable, available);
    foundation::LOG_INFO("sequence_capacity_probe", "sequences={} context={} fits={}",
        target_params.n_seq_max, capacity, fits && *fits);
    if (fits && *fits) {
      const std::uint32_t actual = llama_n_ctx(target.get());
      if (actual > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return foundation::Err<bool>(foundation::ErrorCode::InvalidArgument,
                                     "rounded context capacity exceeds supported range");
      }
      selected_actual_capacity = std::max(selected_actual_capacity, static_cast<int>(actual));
    }
    return fits;
  };

  if (automatic_sequence_capacity != nullptr) {
    const int maximum_sequences = static_cast<int>(std::min<std::size_t>(
        llama_max_parallel_sequences(), std::min<std::size_t>(
            std::max(1u, context_params.n_batch),
            std::numeric_limits<int>::max() / minimum_capacity)));
    const auto first = probe(minimum_capacity, 1);
    if (!first) return std::unexpected(first.error());
    if (!*first) return foundation::Err<int>(foundation::ErrorCode::OutOfMemory,
        "one full request context does not fit");
    int full_requests = 1;
    int upper = std::min(2, maximum_sequences + 1);
    while (upper <= maximum_sequences) {
      const auto fits = probe(upper * minimum_capacity, upper);
      if (!fits) return std::unexpected(fits.error());
      if (!*fits) break;
      full_requests = upper;
      upper = std::min(maximum_sequences + 1, upper * 2);
    }
    while (full_requests + 1 < upper) {
      const int candidate = full_requests + (upper - full_requests) / 2;
      const auto fits = probe(candidate * minimum_capacity, candidate);
      if (!fits) return std::unexpected(fits.error());
      if (*fits) full_requests = candidate;
      else upper = candidate;
    }
    const int pool = full_requests * minimum_capacity;
    int sequences = full_requests;
    upper = std::min(maximum_sequences + 1, sequences * 2);
    while (upper <= maximum_sequences) {
      const auto fits = probe(pool, upper);
      if (!fits) return std::unexpected(fits.error());
      if (!*fits) break;
      sequences = upper;
      upper = std::min(maximum_sequences + 1, upper * 2);
    }
    while (sequences + 1 < upper) {
      const int candidate = sequences + (upper - sequences) / 2;
      const auto fits = probe(pool, candidate);
      if (!fits) return std::unexpected(fits.error());
      if (*fits) sequences = candidate;
      else upper = candidate;
    }
    selected_actual_capacity = 0;
    const auto final_fit = probe(pool, sequences);
    if (!final_fit) return std::unexpected(final_fit.error());
    if (!*final_fit) return foundation::Err<int>(foundation::ErrorCode::OutOfMemory,
        "memory availability changed during concurrency fitting");
    *automatic_sequence_capacity = sequences;
    foundation::LOG_INFO("automatic_concurrency_selected",
        "sequences={} full_requests={} context={} request_limit={} probes={}",
        sequences, full_requests, selected_actual_capacity, minimum_capacity, probe_count);
    return selected_actual_capacity;
  }

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
      "selected_capacity={} requested_capacity={} minimum_capacity={} maximum_capacity={} probes={}",
      selected_actual_capacity, selected, minimum_capacity, maximum_capacity, probe_count);
  return selected_actual_capacity;
} catch (const std::exception& error) {
  return foundation::Err<int>(foundation::ErrorCode::Internal,
      std::string("context pool estimation failed: ") + error.what());
}

} // namespace inferdeck::llama_wrapper
