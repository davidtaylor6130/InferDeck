#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "sampling.h"

using llama_token = int32_t;
struct llama_context;
struct llama_model;
struct llama_vocab;
struct common_speculative;
struct mtmd_context;
struct mtmd_input_chunk;

namespace inferdeck::llama_wrapper {

// One token produced by the scheduler for a caller to consume.
struct TokenEvent {
    llama_token id{0};
    bool is_done{false};
    bool is_error{false};
    std::string error_msg;
};

namespace detail {

constexpr bool generation_limit_reached(
    int generated_before_sample, int max_tokens) noexcept {
    return max_tokens <= 0 || generated_before_sample >= max_tokens - 1;
}

constexpr bool recurrent_checkpoint_usable(
    std::size_t checkpoint_bytes,
    int checkpoint_pos,
    int common_prefix_tokens,
    int prompt_tokens) noexcept {
    return checkpoint_bytes > 0 &&
           checkpoint_pos > 0 &&
           checkpoint_pos <= common_prefix_tokens &&
           checkpoint_pos <= prompt_tokens;
}

inline int recurrent_checkpoint_capture_pos(
    const std::vector<llama_token>& prompt_tokens,
    const std::vector<llama_token>& stable_prefix_tokens) noexcept {
    const std::size_t common =
        std::min(prompt_tokens.size(), stable_prefix_tokens.size());
    std::size_t i = 0;
    while (i < common && prompt_tokens[i] == stable_prefix_tokens[i]) {
        ++i;
    }
    return static_cast<int>(i);
}

constexpr bool adaptive_mtp_enabled(
    bool available,
    std::size_t runnable_requests,
    int max_active_requests) noexcept {
    return available &&
           runnable_requests > 0 &&
           max_active_requests > 0 &&
           runnable_requests <=
               static_cast<std::size_t>(max_active_requests);
}

constexpr bool adaptive_mtp_request_eligible(
    bool request_eligible,
    bool available,
    std::size_t runnable_requests,
    int max_active_requests) noexcept {
    return request_eligible &&
           adaptive_mtp_enabled(
               available, runnable_requests, max_active_requests);
}

constexpr float generation_tokens_per_second(
    int completion_tokens, float generation_duration_ms) noexcept {
    return completion_tokens > 0 && generation_duration_ms > 0.0f
        ? completion_tokens * 1000.0f / generation_duration_ms
        : 0.0f;
}

}

// One in-flight inference request managed by the scheduler.
// Caller creates this on their stack, fills the input fields, calls submit(),
// then drains out_queue until a TokenEvent with is_done=true arrives.
// The object MUST remain alive until after the done event is consumed.
struct SlotTask {
    // ---- Input (filled by caller before submit) ----
    int slot_id{-1};                          // also the llama sequence ID (0..n_slots-1)
    std::vector<llama_token> prompt_tokens;
    struct MediaChunk {
        int token_start{0};
        int token_count{0};
        int position_count{0};
        std::shared_ptr<mtmd_input_chunk> data;
    };
    std::vector<MediaChunk> media_chunks;
    int prompt_position_count{0};
    std::vector<int> last_prompt_tokens;      // previous call's tokens (KV reuse hint)
    std::shared_ptr<const std::vector<uint8_t>> recurrent_checkpoint;
    std::shared_ptr<const std::vector<uint8_t>> recurrent_draft_checkpoint;
    std::shared_ptr<const std::vector<uint8_t>> recurrent_mtp_checkpoint;
    int checkpoint_pos{0};
    int checkpoint_capture_pos{0};
    common_sampler* sampler{nullptr};         // scheduler takes ownership; freed on completion
    int max_tokens{512};
    std::vector<llama_token> stop_tokens;     // single-token early-stop IDs
    const std::atomic<bool>* ext_cancel{nullptr};  // external cancel (e.g. client disconnect)
    std::atomic<bool> caller_cancel{false};   // set by caller to abort generation early
    std::atomic<bool> caller_stop{false};
    bool mtp_cache_synced{true};

    // ---- State (managed exclusively by scheduler thread) ----
    bool initialized{false};
    int prompt_pos{0};     // next prompt token index to add to batch
    bool prompt_done{false};
    int n_pos{0};          // current KV position (= n_prompt + n_generated so far)
    int n_generated{0};
    int i_batch{-1};       // index of this slot's last token in the current batch (-1 = not present)
    llama_token last_token{0};  // last sampled token (fed back as next generation input)
    std::vector<llama_token> spec_draft;
    std::vector<int> spec_i_batch;
    std::vector<uint8_t> spec_draft_checkpoint;
    int spec_draft_pos_max{-1};
    int n_drafted{0};
    int n_draft_accepted{0};
    bool mtp_eligible{false};
    std::chrono::steady_clock::time_point started_at{};
    bool generation_started{false};
    std::chrono::steady_clock::time_point generation_started_at{};

    // ---- Output (written by scheduler before done event, read by caller after) ----
    int out_cached_prompt_tokens{0};
    std::shared_ptr<const std::vector<uint8_t>> out_recurrent_checkpoint;
    std::shared_ptr<const std::vector<uint8_t>> out_recurrent_draft_checkpoint;
    std::shared_ptr<const std::vector<uint8_t>> out_recurrent_mtp_checkpoint;
    int out_checkpoint_pos{0};
    bool out_mtp_cache_synced{true};
    float out_prompt_duration_ms{0.0f};
    float out_generation_duration_ms{0.0f};
    float out_first_token_duration_ms{0.0f};

    // ---- Async token channel ----
    std::mutex out_mtx;
    std::condition_variable out_cv;
    std::queue<TokenEvent> out_queue;
};

// Central continuous-batching scheduler for one loaded model.
// Owns the inference loop: collects tokens from all active SlotTasks each iteration,
// builds one llama_batch, calls llama_decode once, then samples and distributes results.
// All llama_decode calls happen exclusively on the scheduler's internal thread.
class ContinuousBatchScheduler {
public:
    // ctx, model, vocab: externally owned, must outlive this object.
    ContinuousBatchScheduler(
        llama_context* ctx, llama_model* model, const llama_vocab* vocab, int n_batch);
    ContinuousBatchScheduler(
        llama_context* ctx,
        llama_context* draft_ctx,
        common_speculative* speculative,
        mtmd_context* mtmd,
        llama_model* model,
        const llama_vocab* vocab,
        int n_batch,
        int mtp_max_active_requests,
        common_context_seq_rm_type draft_seq_rm_type);
    ~ContinuousBatchScheduler();

    ContinuousBatchScheduler(const ContinuousBatchScheduler&) = delete;
    ContinuousBatchScheduler& operator=(const ContinuousBatchScheduler&) = delete;

    // Submit a task. Returns immediately; caller blocks on task.out_queue until done.
    // task must remain alive until its done event is consumed.
    void submit(SlotTask* task);

    // Signal the loop to stop and join the thread. Called on model unload.
    void stop();

    llama_context* ctx() const noexcept { return ctx_; }

private:
    void run_loop();
    void init_task(SlotTask* task);
    void push_event(SlotTask* task, TokenEvent ev);
    bool should_cancel(const SlotTask* task) const noexcept;
    void fail_all(std::string error);

    llama_context* ctx_;
    llama_context* draft_ctx_;
    common_speculative* speculative_;
    mtmd_context* mtmd_;
    llama_model* model_;
    const llama_vocab* vocab_;
    int n_batch_;
    int mtp_max_active_requests_;
    common_context_seq_rm_type draft_seq_rm_type_;

    std::mutex sub_mtx_;
    std::condition_variable sub_cv_;
    std::vector<SlotTask*> active_;
    std::string terminal_error_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace inferdeck::llama_wrapper
