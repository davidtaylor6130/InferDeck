#include "llama_cpp_wrapper/continuous_batch_scheduler.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <thread>

#include "llama.h"
#include "foundation/logging.hpp"

namespace inferdeck::llama_wrapper {

using inferdeck::foundation::LOG_ERROR;
using inferdeck::foundation::LOG_INFO;
using inferdeck::foundation::LOG_WARN;

ContinuousBatchScheduler::ContinuousBatchScheduler(
    llama_context* ctx, llama_model* model, const llama_vocab* vocab, int n_batch)
    : ctx_(ctx), model_(model), vocab_(vocab), n_batch_(n_batch) {
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
    {
        std::lock_guard lk(sub_mtx_);
        active_.push_back(task);
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

// Called on scheduler thread the first time a task is seen.
// Performs KV cache reuse detection and trims the cache for this slot's sequence.
// Operates only on this slot's sequence ID, never clears the whole context.
void ContinuousBatchScheduler::init_task(SlotTask* task) {
    auto* mem = llama_get_memory(ctx_);
    const int seq_id = task->slot_id;

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
        // Clear this slot's KV entries and decode from scratch
        if (mem) llama_memory_seq_rm(mem, seq_id, 0, -1);
        task->prompt_pos = 0;
        task->n_pos = 0;
        task->out_cached_prompt_tokens = 0;
        return;
    }

    // Check against what is actually in the cache
    const auto pos_max = llama_memory_seq_pos_max(mem, seq_id);
    if (n_past > pos_max) {
        // Cache has fewer tokens than the common prefix; use what is there
        task->prompt_pos = n_past;
        task->n_pos = n_past;
        task->out_cached_prompt_tokens = n_past;
        LOG_INFO("scheduler_kv_reuse",
                 "slot={} cached_tokens={} prompt_tokens={}",
                 seq_id, n_past, (int)task->prompt_tokens.size());
        return;
    }

    // Trim the cache to keep positions 0..n_past-1
    if (llama_memory_seq_rm(mem, seq_id, n_past, -1)) {
        task->prompt_pos = n_past;
        task->n_pos = n_past;
        task->out_cached_prompt_tokens = n_past;
        LOG_INFO("scheduler_kv_reuse",
                 "slot={} cached_tokens={} prompt_tokens={}",
                 seq_id, n_past, (int)task->prompt_tokens.size());
        return;
    }

    // seq_rm failed (e.g. SWA cache): clear this slot and start fresh
    LOG_WARN("scheduler_kv_clear",
             "slot={} seq_rm_failed clearing slot sequence", seq_id);
    llama_memory_seq_rm(mem, seq_id, 0, -1);
    task->prompt_pos = 0;
    task->n_pos = 0;
    task->out_cached_prompt_tokens = 0;
}

void ContinuousBatchScheduler::run_loop() {
    // Allocate a reusable batch. Size is n_batch_ (covers all slots' tokens
    // in one iteration; typical n_batch=512 is ample for a handful of slots).
    llama_batch batch = llama_batch_init(n_batch_, 0, 1);

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

        // ---- Build batch ----
        batch.n_tokens = 0;
        std::vector<SlotTask*> cancelled;

        for (auto* t : tasks) {
            if (should_cancel(t)) {
                cancelled.push_back(t);
                continue;
            }

            if (!t->prompt_done) {
                // Prompt phase: add as many tokens as fit in the remaining batch space
                const int remaining = static_cast<int>(t->prompt_tokens.size()) - t->prompt_pos;
                if (remaining <= 0) {
                    // Empty prompt or already fully processed; transition in post-decode
                    continue;
                }
                const int space = n_batch_ - batch.n_tokens;
                if (space <= 0) continue; // batch full this iteration; retry next

                const int chunk = std::min(remaining, space);
                const bool is_last = (t->prompt_pos + chunk >= static_cast<int>(t->prompt_tokens.size()));

                for (int i = 0; i < chunk; ++i) {
                    const int bi = batch.n_tokens++;
                    batch.token[bi] = t->prompt_tokens[t->prompt_pos + i];
                    batch.pos[bi] = t->prompt_pos + i;
                    batch.n_seq_id[bi] = 1;
                    batch.seq_id[bi][0] = t->slot_id;
                    // Request logits only for the last token (used for sampling)
                    batch.logits[bi] = (is_last && i == chunk - 1) ? 1 : 0;
                    if (is_last && i == chunk - 1) t->i_batch = bi;
                }
                t->prompt_pos += chunk;
                if (is_last) {
                    t->n_pos = static_cast<int>(t->prompt_tokens.size());
                }
            } else {
                // Generation phase: add the last sampled token
                if (batch.n_tokens >= n_batch_) continue; // batch full
                const int bi = batch.n_tokens++;
                batch.token[bi] = t->last_token;
                batch.pos[bi] = t->n_pos;
                batch.n_seq_id[bi] = 1;
                batch.seq_id[bi][0] = t->slot_id;
                batch.logits[bi] = 1;
                t->i_batch = bi;
                t->n_pos++;
            }
        }

        // Signal and remove cancelled tasks
        for (auto* t : cancelled) {
            if (t->sampler) { common_sampler_free(t->sampler); t->sampler = nullptr; }
            TokenEvent ev; ev.is_done = true;
            push_event(t, ev);
        }
        if (!cancelled.empty()) {
            auto* mem = llama_get_memory(ctx_);
            std::lock_guard lk(sub_mtx_);
            for (auto* t : cancelled) {
                if (mem) llama_memory_seq_rm(mem, t->slot_id, 0, -1);
                active_.erase(std::remove(active_.begin(), active_.end(), t), active_.end());
            }
        }

        if (batch.n_tokens == 0) {
            std::this_thread::yield();
            continue;
        }

        // ---- Decode ----
        const int rc = llama_decode(ctx_, batch);
        if (rc != 0) {
            LOG_ERROR("scheduler_decode_failed", "llama_decode rc={} batch_tokens={}", rc, batch.n_tokens);
            TokenEvent ev;
            ev.is_done = true;
            ev.is_error = true;
            ev.error_msg = "llama_decode failed (rc=" + std::to_string(rc) + ")";
            std::lock_guard lk(sub_mtx_);
            for (auto* t : active_) {
                if (t->sampler) { common_sampler_free(t->sampler); t->sampler = nullptr; }
                push_event(t, ev);
            }
            active_.clear();
            break;
        }

        // ---- Sample and push tokens ----
        std::vector<SlotTask*> completed;

        for (auto* t : tasks) {
            // Skip cancelled (already handled) or tasks not in this batch
            if (std::find(cancelled.begin(), cancelled.end(), t) != cancelled.end()) continue;
            if (t->i_batch < 0) continue;

            const bool just_finished_prompt =
                (!t->prompt_done &&
                 t->prompt_pos >= static_cast<int>(t->prompt_tokens.size()));

            // Sample the next token for this slot
            const llama_token id = common_sampler_sample(t->sampler, ctx_, t->i_batch);
            common_sampler_accept(t->sampler, id, true);
            t->i_batch = -1;

            if (just_finished_prompt) {
                // Transition to generation
                t->prompt_done = true;
                // Take a recurrent-model checkpoint (no-op for transformer models)
                {
                    const size_t sz = llama_state_seq_get_size_ext(
                        ctx_, t->slot_id, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
                    if (sz > 0) {
                        t->out_recurrent_checkpoint.resize(sz);
                        const size_t written = llama_state_seq_get_data_ext(
                            ctx_, t->out_recurrent_checkpoint.data(), sz,
                            t->slot_id, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
                        if (written == sz) {
                            t->out_checkpoint_pos = t->n_pos;
                        } else {
                            t->out_recurrent_checkpoint.clear();
                        }
                    }
                }
            }

            // Check stop conditions
            bool stop = llama_vocab_is_eog(vocab_, id);
            if (!stop) {
                for (auto s : t->stop_tokens) {
                    if (s == id) { stop = true; break; }
                }
            }

            const bool at_max = (t->n_generated >= t->max_tokens);

            if (stop || at_max) {
                if (!stop) {
                    // Emit this last token (max_tokens reached, not EOS)
                    TokenEvent ev; ev.id = id;
                    push_event(t, ev);
                }
                if (t->sampler) { common_sampler_free(t->sampler); t->sampler = nullptr; }
                TokenEvent done; done.is_done = true;
                push_event(t, done);
                completed.push_back(t);
            } else {
                TokenEvent ev; ev.id = id;
                push_event(t, ev);
                t->last_token = id;
                t->n_generated++;
            }
        }

        // Remove completed slots and clear their KV entries
        if (!completed.empty()) {
            auto* mem = llama_get_memory(ctx_);
            std::lock_guard lk(sub_mtx_);
            for (auto* t : completed) {
                if (mem) llama_memory_seq_rm(mem, t->slot_id, 0, -1);
                active_.erase(std::remove(active_.begin(), active_.end(), t), active_.end());
            }
        }
    }

    // Drain: any tasks still active when stop_ fires get an error event
    {
        std::lock_guard lk(sub_mtx_);
        for (auto* t : active_) {
            if (t->sampler) { common_sampler_free(t->sampler); t->sampler = nullptr; }
            TokenEvent ev; ev.is_done = true; ev.is_error = true;
            ev.error_msg = "scheduler stopped";
            push_event(t, ev);
        }
        active_.clear();
    }

    llama_batch_free(batch);
}

} // namespace inferdeck::llama_wrapper
