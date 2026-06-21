#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "sampling.h"

using llama_token = int32_t;
struct llama_context;
struct llama_model;
struct llama_vocab;

namespace inferdeck::llama_wrapper {

// One token produced by the scheduler for a caller to consume.
struct TokenEvent {
    llama_token id{0};
    bool is_done{false};
    bool is_error{false};
    std::string error_msg;
};

// One in-flight inference request managed by the scheduler.
// Caller creates this on their stack, fills the input fields, calls submit(),
// then drains out_queue until a TokenEvent with is_done=true arrives.
// The object MUST remain alive until after the done event is consumed.
struct SlotTask {
    // ---- Input (filled by caller before submit) ----
    int slot_id{-1};                          // also the llama sequence ID (0..n_slots-1)
    std::vector<llama_token> prompt_tokens;
    std::vector<int> last_prompt_tokens;      // previous call's tokens (KV reuse hint)
    common_sampler* sampler{nullptr};         // scheduler takes ownership; freed on completion
    int max_tokens{512};
    std::vector<llama_token> stop_tokens;     // single-token early-stop IDs
    const std::atomic<bool>* ext_cancel{nullptr};  // external cancel (e.g. client disconnect)
    std::atomic<bool> caller_cancel{false};   // set by caller to abort generation early

    // ---- State (managed exclusively by scheduler thread) ----
    bool initialized{false};
    int prompt_pos{0};     // next prompt token index to add to batch
    bool prompt_done{false};
    int n_pos{0};          // current KV position (= n_prompt + n_generated so far)
    int n_generated{0};
    int i_batch{-1};       // index of this slot's last token in the current batch (-1 = not present)
    llama_token last_token{0};  // last sampled token (fed back as next generation input)

    // ---- Output (written by scheduler before done event, read by caller after) ----
    int out_cached_prompt_tokens{0};
    std::vector<uint8_t> out_recurrent_checkpoint;
    int out_checkpoint_pos{0};

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

    llama_context* ctx_;
    llama_model* model_;
    const llama_vocab* vocab_;
    int n_batch_;

    std::mutex sub_mtx_;
    std::condition_variable sub_cv_;
    std::vector<SlotTask*> active_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace inferdeck::llama_wrapper
