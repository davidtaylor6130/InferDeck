#include "llama_cpp_wrapper/continuous_batch_scheduler.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <thread>

#include "llama.h"
#include "llama-ext.h"
#include "mtmd-helper.h"
#include "speculative.h"
#include "foundation/logging.hpp"

namespace inferdeck::llama_wrapper {

using inferdeck::foundation::LOG_ERROR;
using inferdeck::foundation::LOG_DEBUG;
using inferdeck::foundation::LOG_INFO;
using inferdeck::foundation::LOG_WARN;

ContinuousBatchScheduler::ContinuousBatchScheduler(
    llama_context* ctx,
    llama_model* model,
    const llama_vocab* vocab,
    int n_batch)
    : ContinuousBatchScheduler(
          ctx,
          nullptr,
          nullptr,
          nullptr,
          model,
          vocab,
          n_batch,
          1,
          COMMON_CONTEXT_SEQ_RM_TYPE_NO) {}

ContinuousBatchScheduler::ContinuousBatchScheduler(
    llama_context* ctx,
    llama_context* draft_ctx,
    common_speculative* speculative,
    mtmd_context* mtmd,
    llama_model* model,
    const llama_vocab* vocab,
    int n_batch,
    int mtp_max_active_requests,
    common_context_seq_rm_type draft_seq_rm_type)
    : ctx_(ctx),
      draft_ctx_(draft_ctx),
      speculative_(speculative),
      mtmd_(mtmd),
      model_(model),
      vocab_(vocab),
      n_batch_(n_batch),
      mtp_max_active_requests_(std::max(1, mtp_max_active_requests)),
      draft_seq_rm_type_(draft_seq_rm_type) {
    thread_ = std::thread([this] { run_loop(); });
}

ContinuousBatchScheduler::~ContinuousBatchScheduler() {
    stop();
}

void ContinuousBatchScheduler::stop() {
    stop_.store(true);
    sub_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void ContinuousBatchScheduler::submit(SlotTask* task) {
    std::string error;
    {
        std::lock_guard lk(sub_mtx_);
        if (!terminal_error_.empty()) {
            error = terminal_error_;
        } else if (stop_.load()) {
            error = "scheduler stopped";
        } else {
            active_.push_back(task);
        }
    }
    if (!error.empty()) {
        if (task->sampler) {
            common_sampler_free(task->sampler);
            task->sampler = nullptr;
        }
        TokenEvent ev;
        ev.is_done = true;
        ev.is_error = true;
        ev.error_msg = std::move(error);
        push_event(task, std::move(ev));
        return;
    }
    sub_cv_.notify_one();
}

bool ContinuousBatchScheduler::should_cancel(const SlotTask* task) const noexcept {
    if (task->caller_cancel.load()) return true;
    if (task->ext_cancel && task->ext_cancel->load()) return true;
    return false;
}

void ContinuousBatchScheduler::push_event(SlotTask* task, TokenEvent ev) {
    {
        std::lock_guard lk(task->out_mtx);
        task->out_queue.push(std::move(ev));
    }
    task->out_cv.notify_one();
}

void ContinuousBatchScheduler::fail_all(std::string error) {
    std::vector<SlotTask*> tasks;
    {
        std::lock_guard lk(sub_mtx_);
        if (terminal_error_.empty()) terminal_error_ = std::move(error);
        tasks.swap(active_);
    }
    for (auto* task : tasks) {
        if (task->sampler) {
            common_sampler_free(task->sampler);
            task->sampler = nullptr;
        }
        TokenEvent ev;
        ev.is_done = true;
        ev.is_error = true;
        ev.error_msg = terminal_error_;
        push_event(task, std::move(ev));
    }
}

// Called on scheduler thread the first time a task is seen.
// Performs KV cache reuse detection and trims the cache for this slot's sequence.
// Operates only on this slot's sequence ID, never clears the whole context.
void ContinuousBatchScheduler::init_task(SlotTask* task) {
    auto* mem = llama_get_memory(ctx_);
    auto* draft_mem = draft_ctx_ ? llama_get_memory(draft_ctx_) : nullptr;
    const int seq_id = task->slot_id;
    const bool has_media = !task->media_chunks.empty();
    task->mtp_eligible = speculative_ != nullptr && !has_media;
    task->out_mtp_cache_synced = true;
    const auto clear_sequence = [&] {
        if (mem) llama_memory_seq_rm(mem, seq_id, 0, -1);
        if (draft_mem) llama_memory_seq_rm(draft_mem, seq_id, 0, -1);
    };

    if (has_media) {
        clear_sequence();
        task->prompt_pos = 0;
        task->n_pos = 0;
        task->out_cached_prompt_tokens = 0;
        task->out_mtp_cache_synced = false;
        return;
    }

    if (speculative_ && !task->mtp_cache_synced) {
        clear_sequence();
        task->prompt_pos = 0;
        task->n_pos = 0;
        task->out_cached_prompt_tokens = 0;
        LOG_INFO("scheduler_mtp_resync",
                 "slot={} prompt_tokens={}",
                 seq_id, static_cast<int>(task->prompt_tokens.size()));
        return;
    }

    // Determine how many leading tokens are already in the KV cache
    int n_past = 0;
    if (!task->last_prompt_tokens.empty() && !task->prompt_tokens.empty()) {
        const std::size_t n_common = std::min(
            task->last_prompt_tokens.size(), task->prompt_tokens.size());
        std::size_t i = 0;
        while (i < n_common &&
               task->last_prompt_tokens[i] == task->prompt_tokens[i]) {
            ++i;
        }
        n_past = static_cast<int>(i);
        // Keep at least one uncached token so we get fresh logits
        if (n_past >= static_cast<int>(task->prompt_tokens.size()) && n_past > 0) {
            --n_past;
        }
    }

    if (n_past <= 0 || !mem) {
        clear_sequence();
        task->prompt_pos = 0;
        task->n_pos = 0;
        task->out_cached_prompt_tokens = 0;
        return;
    }

    // Only reuse what is *physically* present in this slot's KV cache. The
    // cached token arrays (last_prompt_tokens) can claim a longer common
    // prefix than the cache actually holds — after a prior turn evicted
    // entries, or with a SWA cache. Skipping past the real cache contents
    // would decode the tail against an empty KV and silently drop the earlier
    // context (system prompt, tool definitions, history) — see issue #43.
    const int pos_max = static_cast<int>(llama_memory_seq_pos_max(mem, seq_id));
    int physical_tokens = std::max(0, pos_max + 1);
    if (draft_mem) {
        const int draft_pos_max = static_cast<int>(
            llama_memory_seq_pos_max(draft_mem, seq_id));
        physical_tokens = std::min(
            physical_tokens, std::max(0, draft_pos_max + 1));
    }
    LOG_DEBUG("scheduler_cache_probe",
              "slot={} common_tokens={} physical_tokens={} checkpoint_bytes={} draft_checkpoint_bytes={} mtp_checkpoint_bytes={} checkpoint_pos={} checkpoint_capture_pos={} prompt_tokens={}",
              seq_id,
              n_past,
              physical_tokens,
              task->recurrent_checkpoint ? task->recurrent_checkpoint->size() : 0,
              task->recurrent_draft_checkpoint ? task->recurrent_draft_checkpoint->size() : 0,
              task->recurrent_mtp_checkpoint ? task->recurrent_mtp_checkpoint->size() : 0,
              task->checkpoint_pos,
              task->checkpoint_capture_pos,
              static_cast<int>(task->prompt_tokens.size()));
    const bool checkpoint_usable =
        detail::recurrent_checkpoint_usable(
            task->recurrent_checkpoint ? task->recurrent_checkpoint->size() : 0,
            task->checkpoint_pos,
            n_past,
            static_cast<int>(task->prompt_tokens.size())) &&
        (!draft_mem ||
         (detail::recurrent_checkpoint_usable(
              task->recurrent_draft_checkpoint
                  ? task->recurrent_draft_checkpoint->size()
                  : 0,
              task->checkpoint_pos,
              n_past,
              static_cast<int>(task->prompt_tokens.size())) &&
          detail::recurrent_checkpoint_usable(
              task->recurrent_mtp_checkpoint
                  ? task->recurrent_mtp_checkpoint->size()
                  : 0,
              task->checkpoint_pos,
              n_past,
              static_cast<int>(task->prompt_tokens.size()))));
    if (task->checkpoint_pos == task->checkpoint_capture_pos &&
        checkpoint_usable) {
        task->out_recurrent_checkpoint = task->recurrent_checkpoint;
        task->out_recurrent_draft_checkpoint = task->recurrent_draft_checkpoint;
        task->out_recurrent_mtp_checkpoint = task->recurrent_mtp_checkpoint;
        task->out_checkpoint_pos = task->checkpoint_pos;
    }
    n_past = std::min(n_past, physical_tokens);
    if (n_past <= 0) {
        clear_sequence();
        task->prompt_pos = 0;
        task->n_pos = 0;
        task->out_cached_prompt_tokens = 0;
        return;
    }

    if (n_past >= physical_tokens) {
        task->prompt_pos = n_past;
        task->n_pos = n_past;
        task->out_cached_prompt_tokens = n_past;
        LOG_INFO("scheduler_kv_extend",
                 "slot={} cached_tokens={} prompt_tokens={}",
                 seq_id, n_past, (int)task->prompt_tokens.size());
        return;
    }

    const bool target_trimmed =
        llama_memory_seq_rm(mem, seq_id, n_past, -1);
    const bool draft_trimmed =
        !draft_mem || llama_memory_seq_rm(draft_mem, seq_id, n_past, -1);
    if (!target_trimmed || !draft_trimmed) {
        if (checkpoint_usable) {
            const size_t target_restored = llama_state_seq_set_data_ext(
                ctx_,
                task->recurrent_checkpoint->data(),
                task->recurrent_checkpoint->size(),
                seq_id,
                LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
            bool restored = target_restored == task->recurrent_checkpoint->size();
            if (restored && draft_mem) {
                const size_t draft_restored = llama_state_seq_set_data_ext(
                    draft_ctx_,
                    task->recurrent_draft_checkpoint->data(),
                    task->recurrent_draft_checkpoint->size(),
                    seq_id,
                    LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
                common_speculative_set_state(
                    speculative_, seq_id, *task->recurrent_mtp_checkpoint);
                restored =
                    draft_restored == task->recurrent_draft_checkpoint->size();
            }
            if (restored) {
                const bool target_checkpoint_trimmed = llama_memory_seq_rm(
                    mem, seq_id, task->checkpoint_pos, -1);
                const bool draft_checkpoint_trimmed =
                    !draft_mem || llama_memory_seq_rm(
                        draft_mem, seq_id, task->checkpoint_pos, -1);
                if (target_checkpoint_trimmed && draft_checkpoint_trimmed) {
                    task->prompt_pos = task->checkpoint_pos;
                    task->n_pos = task->checkpoint_pos;
                    task->out_cached_prompt_tokens = task->checkpoint_pos;
                    LOG_INFO("scheduler_checkpoint_restore",
                             "slot={} checkpoint_tokens={} common_tokens={} prompt_tokens={}",
                             seq_id,
                             task->checkpoint_pos,
                             n_past,
                             (int)task->prompt_tokens.size());
                    return;
                }
            }
        }
        clear_sequence();
        task->prompt_pos = 0;
        task->n_pos = 0;
        task->out_cached_prompt_tokens = 0;
        if (draft_mem) {
            LOG_INFO("scheduler_mtp_cache_rebuild",
                     "slot={} common_tokens={} prompt_tokens={}",
                     seq_id, n_past, (int)task->prompt_tokens.size());
        } else {
            LOG_WARN("scheduler_kv_clear",
                     "slot={} seq_rm_failed clearing slot sequence", seq_id);
        }
        return;
    }
    task->prompt_pos = n_past;
    task->n_pos = n_past;
    task->out_cached_prompt_tokens = n_past;
    LOG_INFO("scheduler_kv_reuse",
             "slot={} cached_tokens={} prompt_tokens={}",
             seq_id, n_past, (int)task->prompt_tokens.size());
}

void ContinuousBatchScheduler::run_loop() {
    // Allocate a reusable batch. Size is n_batch_ (covers all slots' tokens
    // in one iteration; typical n_batch=512 is ample for a handful of slots).
    llama_batch batch = llama_batch_init(n_batch_, 0, 1);

    try {
      while (!stop_.load()) {
        // ---- Wait for work ----
        std::vector<SlotTask*> tasks;
        {
            std::unique_lock lk(sub_mtx_);
            if (active_.empty()) {
                sub_cv_.wait(lk, [this] {
                    return stop_.load() || !active_.empty();
                });
                if (stop_.load()) break;
            }
            tasks = active_;
        }
        if (tasks.empty()) continue;

        // ---- Initialize newly submitted tasks ----
        for (auto* t : tasks) {
            if (!t->initialized) {
                init_task(t);
                t->initialized = true;
            }
        }

        std::vector<SlotTask*> runnable;
        runnable.reserve(tasks.size());
        for (auto* t : tasks) {
            t->spec_draft.clear();
            t->spec_i_batch.clear();
            t->spec_draft_checkpoint.clear();
            t->spec_draft_pos_max = -1;
            if (!should_cancel(t) && !t->caller_stop.load()) {
                runnable.push_back(t);
            }
            if (speculative_) {
                common_speculative_get_draft_params(
                    speculative_, t->slot_id).drafting = false;
            }
        }

        const bool has_media_request = std::any_of(
            runnable.begin(), runnable.end(), [](const SlotTask* task) {
                return !task->media_chunks.empty();
            });
        const bool mtp_window = !has_media_request && detail::adaptive_mtp_enabled(
            speculative_ != nullptr,
            runnable.size(),
            mtp_max_active_requests_);
        for (auto* t : runnable) {
            t->mtp_eligible = detail::adaptive_mtp_request_eligible(
                t->mtp_eligible,
                speculative_ != nullptr,
                runnable.size(),
                mtp_max_active_requests_);
            if (!mtp_window) {
                t->out_mtp_cache_synced = false;
            }
        }

        std::vector<SlotTask*> drafting;
        if (mtp_window) {
            for (auto* t : runnable) {
                if (!t->mtp_eligible || !t->prompt_done || !draft_ctx_) continue;
                auto* draft_mem = llama_get_memory(draft_ctx_);
                t->spec_draft_pos_max = draft_mem
                    ? static_cast<int>(
                        llama_memory_seq_pos_max(draft_mem, t->slot_id))
                    : -1;
                if (draft_seq_rm_type_ == COMMON_CONTEXT_SEQ_RM_TYPE_FULL) {
                    const auto flags = LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY;
                    const size_t size = llama_state_seq_get_size_ext(
                        draft_ctx_, t->slot_id, flags);
                    t->spec_draft_checkpoint.resize(size);
                    if (size > 0) {
                        const size_t written = llama_state_seq_get_data_ext(
                            draft_ctx_,
                            t->spec_draft_checkpoint.data(),
                            size,
                            t->slot_id,
                            flags);
                        if (written != size) {
                            throw std::runtime_error(
                                "failed to capture MTP draft checkpoint");
                        }
                    }
                }
                common_speculative_get_draft_params(
                    speculative_, t->slot_id) = {
                        true,
                        -1,
                        t->n_pos,
                        t->last_token,
                        &t->prompt_tokens,
                        &t->spec_draft,
                    };
                drafting.push_back(t);
            }
            common_speculative_draft(speculative_);
        }

        for (auto* t : drafting) {
            auto* draft_mem = llama_get_memory(draft_ctx_);
            bool restored = true;
            if (draft_seq_rm_type_ == COMMON_CONTEXT_SEQ_RM_TYPE_FULL &&
                !t->spec_draft_checkpoint.empty()) {
                const auto flags = LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY;
                const size_t restored_bytes = llama_state_seq_set_data_ext(
                    draft_ctx_,
                    t->spec_draft_checkpoint.data(),
                    t->spec_draft_checkpoint.size(),
                    t->slot_id,
                    flags);
                restored =
                    restored_bytes == t->spec_draft_checkpoint.size();
            }
            if (!restored ||
                (draft_mem &&
                 !llama_memory_seq_rm(
                     draft_mem,
                     t->slot_id,
                     t->spec_draft_pos_max + 1,
                     -1))) {
                throw std::runtime_error(
                    "failed to restore MTP draft context after drafting");
            }
            t->n_drafted += static_cast<int>(t->spec_draft.size());
        }

        // ---- Build batch ----
        batch.n_tokens = 0;
        bool process_mtp = false;
        std::vector<SlotTask*> cancelled;
        std::vector<SlotTask*> stopped;
        std::vector<SlotTask*> checkpoint_ready;

        for (auto* t : tasks) {
            if (should_cancel(t)) {
                cancelled.push_back(t);
                continue;
            }
            if (t->caller_stop.load()) {
                stopped.push_back(t);
                continue;
            }

            if (!t->prompt_done) {
                while (t->prompt_pos < static_cast<int>(t->prompt_tokens.size()) &&
                       t->prompt_tokens[t->prompt_pos] == LLAMA_TOKEN_NULL) {
                    const auto media = std::find_if(
                        t->media_chunks.begin(), t->media_chunks.end(),
                        [t](const SlotTask::MediaChunk& chunk) {
                            return chunk.token_start == t->prompt_pos;
                        });
                    if (media == t->media_chunks.end() || !media->data || !mtmd_) {
                        throw std::runtime_error("invalid multimodal prompt chunk");
                    }
                    llama_pos new_position = t->n_pos;
                    const auto media_started = std::chrono::steady_clock::now();
                    LOG_INFO("vision_chunk_eval_begin",
                             "slot={} token_start={} tokens={} positions={}",
                             t->slot_id,
                             media->token_start,
                             media->token_count,
                             media->position_count);
                    const int rc = mtmd_helper_eval_chunk_single(
                        mtmd_, ctx_, media->data.get(), t->n_pos, t->slot_id,
                        n_batch_, true, &new_position);
                    if (rc != 0) {
                        throw std::runtime_error(
                            "multimodal chunk evaluation failed (rc=" +
                            std::to_string(rc) + ")");
                    }
                    LOG_INFO("vision_chunk_eval_complete",
                             "slot={} tokens={} positions={} duration_ms={:.3f}",
                             t->slot_id,
                             media->token_count,
                             media->position_count,
                             std::chrono::duration<float, std::milli>(
                                 std::chrono::steady_clock::now() - media_started).count());
                    t->prompt_pos += media->token_count;
                    t->n_pos = new_position;
                    t->out_mtp_cache_synced = false;
                }
                const int remaining = static_cast<int>(t->prompt_tokens.size()) - t->prompt_pos;
                if (remaining <= 0) {
                    // Empty prompt or already fully processed; transition in post-decode
                    continue;
                }
                const int space = n_batch_ - batch.n_tokens;
                if (space <= 0) continue; // batch full this iteration; retry next

                int chunk = std::min(remaining, space);
                const auto next_media = std::find_if(
                    t->media_chunks.begin(), t->media_chunks.end(),
                    [t](const SlotTask::MediaChunk& media) {
                        return media.token_start > t->prompt_pos;
                    });
                if (next_media != t->media_chunks.end()) {
                    chunk = std::min(
                        chunk, next_media->token_start - t->prompt_pos);
                }
                if (t->checkpoint_capture_pos > t->prompt_pos) {
                    chunk = std::min(
                        chunk, t->checkpoint_capture_pos - t->prompt_pos);
                }
                const bool is_last = (t->prompt_pos + chunk >= static_cast<int>(t->prompt_tokens.size()));

                for (int i = 0; i < chunk; ++i) {
                    const int bi = batch.n_tokens++;
                    batch.token[bi] = t->prompt_tokens[t->prompt_pos + i];
                    batch.pos[bi] = t->n_pos + i;
                    batch.n_seq_id[bi] = 1;
                    batch.seq_id[bi][0] = t->slot_id;
                    // Request logits only for the last token (used for sampling)
                    batch.logits[bi] = (is_last && i == chunk - 1) ? 1 : 0;
                    process_mtp = process_mtp || t->mtp_eligible;
                    if (is_last && i == chunk - 1) t->i_batch = bi;
                }
                t->prompt_pos += chunk;
                t->n_pos += chunk;
                if (t->prompt_pos == t->checkpoint_capture_pos) {
                    checkpoint_ready.push_back(t);
                }
                if (is_last) {
                    t->n_pos = t->prompt_position_count > 0
                        ? t->prompt_position_count
                        : t->n_pos;
                }
            } else {
                const int required = 1 + static_cast<int>(t->spec_draft.size());
                if (batch.n_tokens + required > n_batch_) continue;
                if (!t->spec_draft.empty()) {
                    for (int index = 0; index < required; ++index) {
                        const int bi = batch.n_tokens++;
                        batch.token[bi] = index == 0
                            ? t->last_token
                            : t->spec_draft[static_cast<std::size_t>(index - 1)];
                        batch.pos[bi] = t->n_pos + index;
                        batch.n_seq_id[bi] = 1;
                        batch.seq_id[bi][0] = t->slot_id;
                        batch.logits[bi] = 1;
                        process_mtp = process_mtp || t->mtp_eligible;
                        t->spec_i_batch.push_back(bi);
                    }
                    t->i_batch = -1;
                } else {
                    const int bi = batch.n_tokens++;
                    batch.token[bi] = t->last_token;
                    batch.pos[bi] = t->n_pos;
                    batch.n_seq_id[bi] = 1;
                    batch.seq_id[bi][0] = t->slot_id;
                    batch.logits[bi] = 1;
                    process_mtp = process_mtp || t->mtp_eligible;
                    t->i_batch = bi;
                    t->n_pos++;
                }
            }
        }

        if (!cancelled.empty() || !stopped.empty()) {
            auto* mem = llama_get_memory(ctx_);
            std::lock_guard lk(sub_mtx_);
            for (auto* t : cancelled) {
                if (mem) llama_memory_seq_rm(mem, t->slot_id, 0, -1);
                if (draft_ctx_) {
                    if (auto* draft_mem = llama_get_memory(draft_ctx_)) {
                        llama_memory_seq_rm(draft_mem, t->slot_id, 0, -1);
                    }
                }
                active_.erase(std::remove(active_.begin(), active_.end(), t), active_.end());
            }
            for (auto* t : stopped) {
                active_.erase(std::remove(active_.begin(), active_.end(), t), active_.end());
            }
        }
        for (auto* t : cancelled) {
            if (t->generation_started) {
                t->out_generation_duration_ms =
                    std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() -
                        t->generation_started_at).count();
            }
            if (t->sampler) { common_sampler_free(t->sampler); t->sampler = nullptr; }
            TokenEvent ev; ev.is_done = true;
            push_event(t, ev);
        }
        for (auto* t : stopped) {
            if (t->generation_started) {
                t->out_generation_duration_ms =
                    std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() -
                        t->generation_started_at).count();
            }
            if (t->sampler) { common_sampler_free(t->sampler); t->sampler = nullptr; }
            TokenEvent ev; ev.is_done = true;
            push_event(t, ev);
        }

        if (batch.n_tokens == 0) {
            std::this_thread::yield();
            continue;
        }

        // ---- Decode ----
        const int rc = llama_decode(ctx_, batch);
        if (rc != 0) {
            LOG_ERROR("scheduler_decode_failed", "llama_decode rc={} batch_tokens={}", rc, batch.n_tokens);
            fail_all("llama_decode failed (rc=" + std::to_string(rc) + ")");
            break;
        }
        if (speculative_ && process_mtp &&
            !common_speculative_process(speculative_, batch)) {
            fail_all("MTP draft context decode failed");
            break;
        }

        for (auto* t : checkpoint_ready) {
            const auto capture_context = [t](llama_context* context) {
                const size_t size = llama_state_seq_get_size_ext(
                    context,
                    t->slot_id,
                    LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
                if (size == 0) {
                    return std::shared_ptr<const std::vector<uint8_t>>{};
                }
                auto data = std::make_shared<std::vector<uint8_t>>(size);
                const size_t written = llama_state_seq_get_data_ext(
                    context,
                    data->data(),
                    size,
                    t->slot_id,
                    LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
                return written == size
                    ? std::shared_ptr<const std::vector<uint8_t>>(std::move(data))
                    : std::shared_ptr<const std::vector<uint8_t>>{};
            };
            auto target_checkpoint = capture_context(ctx_);
            if (!target_checkpoint) {
                continue;
            }
            std::shared_ptr<const std::vector<uint8_t>> draft_checkpoint;
            std::shared_ptr<const std::vector<uint8_t>> mtp_checkpoint;
            if (draft_ctx_) {
                draft_checkpoint = capture_context(draft_ctx_);
                auto data = std::make_shared<std::vector<uint8_t>>();
                if (common_speculative_get_state(
                        speculative_, t->slot_id, *data)) {
                    mtp_checkpoint = std::move(data);
                }
                if (!draft_checkpoint || !mtp_checkpoint) {
                    continue;
                }
            }
            t->out_recurrent_checkpoint = std::move(target_checkpoint);
            t->out_recurrent_draft_checkpoint = std::move(draft_checkpoint);
            t->out_recurrent_mtp_checkpoint = std::move(mtp_checkpoint);
            t->out_checkpoint_pos = t->checkpoint_capture_pos;
        }

        // ---- Sample and push tokens ----
        std::vector<SlotTask*> completed;

        for (auto* t : tasks) {
            // Skip cancelled (already handled) or tasks not in this batch
            if (std::find(cancelled.begin(), cancelled.end(), t) != cancelled.end()) continue;
            if (std::find(stopped.begin(), stopped.end(), t) != stopped.end()) continue;
            if (t->i_batch < 0) continue;

            const bool just_finished_prompt =
                (!t->prompt_done &&
                 t->prompt_pos >= static_cast<int>(t->prompt_tokens.size()));
            if (just_finished_prompt && !t->generation_started) {
                t->generation_started = true;
                t->generation_started_at =
                    std::chrono::steady_clock::now();
            }

            // Sample the next token for this slot
            const llama_token id = common_sampler_sample(t->sampler, ctx_, t->i_batch);
            common_sampler_accept(t->sampler, id, true);
            t->i_batch = -1;

            if (just_finished_prompt) {
                // Transition to generation
                t->prompt_done = true;
                if (speculative_ && t->mtp_eligible) {
                    common_speculative_begin(
                        speculative_, t->slot_id, t->prompt_tokens);
                }
            }

            // Check stop conditions
            bool stop = llama_vocab_is_eog(vocab_, id);
            if (!stop) {
                for (auto s : t->stop_tokens) {
                    if (s == id) { stop = true; break; }
                }
            }

            const bool at_max =
                detail::generation_limit_reached(t->n_generated, t->max_tokens);

            if (stop || at_max) {
                if (!stop) {
                    // Emit this last token (max_tokens reached, not EOS)
                    TokenEvent ev; ev.id = id;
                    push_event(t, ev);
                }
                if (t->sampler) { common_sampler_free(t->sampler); t->sampler = nullptr; }
                completed.push_back(t);
            } else {
                TokenEvent ev; ev.id = id;
                push_event(t, ev);
                t->last_token = id;
                t->n_generated++;
            }
        }

        for (auto* t : tasks) {
            if (t->spec_i_batch.empty()) continue;
            if (!speculative_ ||
                t->spec_i_batch.size() != t->spec_draft.size() + 1) {
                throw std::runtime_error(
                    "invalid MTP verification batch");
            }

            const auto accepted = common_sampler_sample_and_accept_n(
                t->sampler, ctx_, t->spec_i_batch, t->spec_draft);
            if (accepted.empty()) {
                throw std::runtime_error(
                    "MTP verification returned no target token");
            }

            const int accepted_drafts =
                static_cast<int>(accepted.size()) - 1;
            common_speculative_accept(
                speculative_,
                t->slot_id,
                static_cast<uint16_t>(accepted_drafts));
            t->n_draft_accepted += accepted_drafts;
            t->n_pos += static_cast<int>(accepted.size());

            auto* target_mem = llama_get_memory(ctx_);
            auto* draft_mem = draft_ctx_
                ? llama_get_memory(draft_ctx_) : nullptr;
            if ((target_mem &&
                 !llama_memory_seq_rm(
                     target_mem, t->slot_id, t->n_pos, -1)) ||
                (draft_mem &&
                 !llama_memory_seq_rm(
                     draft_mem, t->slot_id, t->n_pos, -1))) {
                throw std::runtime_error(
                    "failed to roll back rejected MTP draft tokens");
            }

            bool task_completed = false;
            for (const llama_token id : accepted) {
                bool stop = llama_vocab_is_eog(vocab_, id);
                if (!stop) {
                    for (const auto stop_id : t->stop_tokens) {
                        if (stop_id == id) {
                            stop = true;
                            break;
                        }
                    }
                }
                if (stop) {
                    task_completed = true;
                    break;
                }

                TokenEvent event;
                event.id = id;
                push_event(t, event);
                ++t->n_generated;
                if (t->n_generated >= t->max_tokens) {
                    task_completed = true;
                    break;
                }
                t->last_token = id;
            }

            t->spec_i_batch.clear();
            t->spec_draft.clear();
            if (task_completed) {
                if (t->sampler) {
                    common_sampler_free(t->sampler);
                    t->sampler = nullptr;
                }
                completed.push_back(t);
            }
        }

        // Remove completed slots from the active set, but keep their KV cache
        // intact so the next request on this slot can reuse the shared prefix
        // (init_task trims any divergent suffix). Wiping here forces a full
        // re-decode every turn and, with init_task's prefix assumptions, drops
        // conversation context — the root cause of issue #43.
        if (!completed.empty()) {
            {
                std::lock_guard lk(sub_mtx_);
                for (auto* t : completed) {
                    active_.erase(std::remove(active_.begin(), active_.end(), t), active_.end());
                }
            }
            for (auto* t : completed) {
                if (t->generation_started) {
                    t->out_generation_duration_ms =
                        std::chrono::duration<float, std::milli>(
                            std::chrono::steady_clock::now() -
                            t->generation_started_at).count();
                }
                if (t->n_drafted > 0) {
                    LOG_INFO(
                        "scheduler_mtp_stats",
                        "slot={} drafted={} accepted={} acceptance_pct={:.2f}",
                        t->slot_id,
                        t->n_drafted,
                        t->n_draft_accepted,
                        100.0 * t->n_draft_accepted /
                            static_cast<double>(t->n_drafted));
                }
                TokenEvent done; done.is_done = true;
                push_event(t, done);
            }
        }
      }
    } catch (const std::exception& e) {
        LOG_ERROR("scheduler_exception", "error={}", e.what());
        fail_all(std::string("scheduler exception: ") + e.what());
    } catch (...) {
        LOG_ERROR("scheduler_exception", "error=unknown");
        fail_all("scheduler exception: unknown error");
    }

    fail_all(stop_.load() ? "scheduler stopped" : "scheduler terminated");

    llama_batch_free(batch);
}

} // namespace inferdeck::llama_wrapper
