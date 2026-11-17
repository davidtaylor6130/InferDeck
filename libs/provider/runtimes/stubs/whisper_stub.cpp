// Test double for the whisper.cpp C API: one two-token segment, "en".
#include <whisper.h>

namespace {
int g_context{};
}

extern "C" {
whisper_context_params whisper_context_default_params(void) { return {}; }
whisper_full_params whisper_full_default_params(whisper_sampling_strategy) { return {}; }
whisper_context* whisper_init_from_file_with_params(const char*, whisper_context_params) {
    return reinterpret_cast<whisper_context*>(&g_context);
}
void whisper_free(whisper_context*) {}
int whisper_is_multilingual(whisper_context*) { return 1; }
int whisper_lang_id(const char*) { return 0; }
const char* whisper_lang_str(int) { return "en"; }
int whisper_full(whisper_context* context, whisper_full_params params, const float*, int) {
    if (params.progress_callback) params.progress_callback(context, nullptr, 50, params.progress_callback_user_data);
    if (params.abort_callback && params.abort_callback(params.abort_callback_user_data)) return -1;
    return 0;
}
int whisper_full_n_segments(whisper_context*) { return 1; }
int whisper_full_lang_id(whisper_context*) { return 0; }
int64_t whisper_full_get_segment_t0(whisper_context*, int) { return 0; }
int64_t whisper_full_get_segment_t1(whisper_context*, int) { return 100; }
const char* whisper_full_get_segment_text(whisper_context*, int) { return " hello"; }
int whisper_full_n_tokens(whisper_context*, int) { return 2; }
whisper_token whisper_full_get_token_id(whisper_context*, int, int token) { return 100 + token; }
float whisper_full_get_token_p(whisper_context*, int, int) { return 0.5f; }
float whisper_full_get_segment_no_speech_prob(whisper_context*, int) { return 0.1f; }
}
