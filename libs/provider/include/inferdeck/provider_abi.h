/*
 * InferDeck provider ABI, versions 1 and 2.
 *
 * A provider is a shared library (DLL / .so / .dylib) that runs models for
 * InferDeck inside the gateway process. This header is the whole contract:
 * plain C, no InferDeck types, no C++ runtime shared across the boundary.
 * Any language that can export a C function can implement it.
 *
 * Read docs/providers.md first. It explains every operation and JSON payload.
 *
 * Rules that keep the boundary safe:
 *   - All strings are UTF-8, NUL-terminated JSON.
 *   - Pointers passed to a function are valid only until that function
 *     returns. Copy what you need to keep.
 *   - Results travel through inferdeck_call.reply. The host copies them
 *     before reply returns, so no memory is ever freed across the boundary.
 *   - Never let an exception or panic cross the boundary. Return a status.
 */
#ifndef INFERDECK_PROVIDER_ABI_H
#define INFERDECK_PROVIDER_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INFERDECK_PROVIDER_ABI_VERSION 1u

/* Name of the single symbol a provider library exports. */
#define INFERDECK_PROVIDER_ENTRY_POINT "inferdeck_provider_v1"

#if defined(_WIN32)
#define INFERDECK_PROVIDER_EXPORT __declspec(dllexport)
#else
#define INFERDECK_PROVIDER_EXPORT __attribute__((visibility("default")))
#endif

/* Return value of every provider function. Zero is success. */
typedef enum inferdeck_status {
    INFERDECK_OK = 0,
    INFERDECK_INVALID_ARGUMENT = 1,
    INFERDECK_CONTEXT_LENGTH_EXCEEDED = 2,
    INFERDECK_NOT_FOUND = 3,
    INFERDECK_NOT_LOADED = 4,
    INFERDECK_ALREADY_EXISTS = 5,
    INFERDECK_IO_ERROR = 6,
    INFERDECK_PARSE_ERROR = 7,
    INFERDECK_OUT_OF_MEMORY = 8,
    INFERDECK_RESOURCE_BUSY = 9,
    INFERDECK_TIMEOUT = 10,
    INFERDECK_CANCELLED = 11,
    INFERDECK_UNAVAILABLE = 12,
    INFERDECK_INTERNAL = 13,
    /* v2 only: this optional operation is not implemented; the host applies
     * its documented default instead of failing. */
    INFERDECK_NOT_IMPLEMENTED = 14
} inferdeck_status;

/* Binary payload (image, audio, video, PCM samples). Never JSON-encoded. */
typedef struct inferdeck_blob {
    const uint8_t* data;
    size_t size;
} inferdeck_blob;

/*
 * Host callbacks for one call. Pass `context` back unchanged.
 * The callbacks may be invoked from the thread that received the call only.
 */
typedef struct inferdeck_call {
    void* context;

    /* Nonzero when the host wants this call to stop. Poll it between steps
     * and return INFERDECK_CANCELLED. */
    int32_t (*cancelled)(void* context);

    /* Sends one streaming event (text delta, progress, audio chunk).
     * Returns nonzero when the host wants the call to stop. */
    int32_t (*emit)(void* context, const char* event_json,
                    const inferdeck_blob* blobs, size_t blob_count);

    /* Sets the final result, or {"message":"..."} when returning an error.
     * Call it at most once, before returning. */
    void (*reply)(void* context, const char* result_json,
                  const inferdeck_blob* blobs, size_t blob_count);
} inferdeck_call;

/* The provider's function table. Returned by the entry point; must stay
 * valid for the life of the process. */
typedef struct inferdeck_provider {
    /* Must equal INFERDECK_PROVIDER_ABI_VERSION. */
    uint32_t abi_version;

    /* Static JSON describing the provider. See docs/providers.md. */
    const char* manifest_json;

    /* Creates one instance for one configured model. Must not load weights. */
    int32_t (*create)(const char* model_json, const inferdeck_call* call,
                      void** instance);

    /* Destroys an instance. The host unloads it first. */
    void (*destroy)(void* instance);

    /* Loads / releases model weights. Never concurrent with invoke. */
    int32_t (*load)(void* instance, const inferdeck_call* call);
    int32_t (*unload)(void* instance, const inferdeck_call* call);

    /* Megabytes of GPU memory currently held, or a negative value to let the
     * host use the model's configured vram_required_mb. */
    int32_t (*vram_usage_mb)(void* instance);

    /* Runs one operation ("chat", "embed", "image", "audio_generation",
     * "video", "speech", "transcribe"). May run concurrently on up to
     * n_slots threads, each with a distinct "slot" in the request. */
    int32_t (*invoke)(void* instance, const char* operation,
                      const char* request_json,
                      const inferdeck_blob* blobs, size_t blob_count,
                      const inferdeck_call* call);
} inferdeck_provider;

/* Signature of the exported entry point. */
typedef const inferdeck_provider* (*inferdeck_provider_entry)(void);

#ifdef INFERDECK_PROVIDER_BUILD
INFERDECK_PROVIDER_EXPORT const inferdeck_provider* inferdeck_provider_v1(void);
#endif

/* ------------------------------------------------------------------------
 * Version 2. Mirrors the gateway's backend interfaces one for one, so a v2
 * provider is indistinguishable from a built-in runtime. Every struct starts
 * with struct_size, the number of bytes the writer filled in. Members are
 * only ever appended. A reader must not touch a member unless
 * struct_size >= offsetof(member) + sizeof(member); INFERDECK_STRUCT_HAS
 * does that check. The host treats absent provider table members as NULL.
 * ---------------------------------------------------------------------- */

#define INFERDECK_STRUCT_HAS(ptr, type, member)     ((ptr)->struct_size >= offsetof(type, member) + sizeof((ptr)->member))

#define INFERDECK_PROVIDER_ABI_VERSION_2 2u
#define INFERDECK_PROVIDER_ENTRY_POINT_V2 "inferdeck_provider_v2"

/* Returned by cancelled(). */
#define INFERDECK_CALL_RUNNING 0
#define INFERDECK_CALL_CANCELLED 1
#define INFERDECK_CALL_DEADLINE 2

typedef struct inferdeck_call_v2 {
    uint32_t struct_size;
    void* context;

    /* INFERDECK_CALL_RUNNING, _CANCELLED, or _DEADLINE. Return
     * INFERDECK_CANCELLED or INFERDECK_TIMEOUT respectively. Any thread. */
    int32_t (*cancelled)(void* context);

    /* One streaming event. Any thread, never concurrently for one call, never
     * after the call returned. Nonzero return: stop the call. */
    int32_t (*emit)(void* context, const char* event_json,
                    const inferdeck_blob* blobs, size_t blob_count);

    /* Final result, or {"message":"..."} with an error status. At most once. */
    void (*reply)(void* context, const char* result_json,
                  const inferdeck_blob* blobs, size_t blob_count);

    /* Milliseconds before the call's deadline, or -1 without one. Any thread. */
    int64_t (*remaining_ms)(void* context);
} inferdeck_call_v2;

/* Process-wide host services handed to init(). Valid until shutdown(). */
typedef struct inferdeck_host_v2 {
    uint32_t struct_size;
    /* {"abi_version":2,"gateway":{...},"models":[model_json...]} */
    const char* host_json;
    void* context;
    /* level: 0 trace .. 5 fatal. Any thread, any time before shutdown(). */
    void (*log)(void* context, int32_t level, const char* event, const char* message);
} inferdeck_host_v2;

/* Status queries. Answered without blocking: the scheduler calls them while
 * holding its global lock. Return INFERDECK_QUERY_DEFAULT for anything not
 * implemented and the host applies the documented default. */
typedef enum inferdeck_query {
    INFERDECK_QUERY_LOADED = 1,
    INFERDECK_QUERY_HEALTHY = 2,
    INFERDECK_QUERY_VRAM_USAGE_MB = 3,
    INFERDECK_QUERY_N_SLOTS = 4,
    INFERDECK_QUERY_N_FREE_SLOTS = 5,
    INFERDECK_QUERY_SLOT_BUSY = 6,
    INFERDECK_QUERY_CONTEXT_POOL_CAPACITY = 7,
    INFERDECK_QUERY_MIN_SLOTS = 8,
    INFERDECK_QUERY_CAN_RESIZE_SLOTS = 9,
    INFERDECK_QUERY_CAN_RECLAIM_IDLE_CONTEXT = 10,
    INFERDECK_QUERY_ESTIMATE_VRAM_MB = 11,
    INFERDECK_QUERY_ADDITIONAL_VRAM_RESERVE_MB = 12,
    INFERDECK_QUERY_LIVE_VRAM_ACCOUNTING_COMPLETE = 13,
    INFERDECK_QUERY_RECLAIMABLE_IDLE_CONTEXT_MB = 14
} inferdeck_query;

#define INFERDECK_QUERY_DEFAULT INT64_MIN

/* Optional members may be NULL; the host then uses the default documented in
 * docs/providers.md. */
typedef struct inferdeck_provider_v2 {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* manifest_json;

    /* Optional. Called on the gateway's startup thread before any create, once
     * per gateway (re)start. shutdown() runs once at process exit. */
    int32_t (*init)(const inferdeck_host_v2* host, const inferdeck_call_v2* call);
    void (*shutdown)(void);

    int32_t (*create)(const char* model_json, const inferdeck_call_v2* call,
                      void** instance);
    void (*destroy)(void* instance);
    int32_t (*load)(void* instance, const inferdeck_call_v2* call);
    int32_t (*unload)(void* instance, const inferdeck_call_v2* call);

    /* Optional, non-blocking. arg is the slot for SLOT_BUSY and the slot count
     * for ESTIMATE_VRAM_MB. */
    int64_t (*query)(void* instance, int32_t query, int64_t arg);

    /* Optional, non-blocking, all three or none. Without them the host keeps
     * n_slots fixed slots itself. */
    int32_t (*acquire_slot)(void* instance, int32_t* slot);
    int32_t (*release_slot)(void* instance, int32_t slot);
    void (*request_cancel)(void* instance);

    /* Optional. */
    int32_t (*reset_all_slots)(void* instance, const inferdeck_call_v2* call);
    int32_t (*resize_slots)(void* instance, int32_t slots, const inferdeck_call_v2* call);
    /* Replies {"reclaimed": true|false}. */
    int32_t (*reclaim_idle_context)(void* instance, int32_t additional_reserve_mb,
                                    const inferdeck_call_v2* call);

    /* Runs one operation; see docs/providers.md for the list. May run
     * concurrently, one call per acquired slot. */
    int32_t (*invoke)(void* instance, const char* operation,
                      const char* request_json,
                      const inferdeck_blob* blobs, size_t blob_count,
                      const inferdeck_call_v2* call);

    /* Optional. A process-level operation that needs no model instance, such
     * as "quantize" or "devices" for llama_cpp. Called after init. */
    int32_t (*provider_invoke)(const char* operation, const char* request_json,
                               const inferdeck_blob* blobs, size_t blob_count,
                               const inferdeck_call_v2* call);
} inferdeck_provider_v2;

/* Smallest provider table the host accepts: everything up to and including
 * invoke. This value is frozen; later members (provider_invoke and any added
 * after it) are optional and recognised only when struct_size covers them. */
#define INFERDECK_PROVIDER_V2_MIN_SIZE offsetof(inferdeck_provider_v2, provider_invoke)

typedef const inferdeck_provider_v2* (*inferdeck_provider_entry_v2)(void);

#ifdef INFERDECK_PROVIDER_BUILD_V2
INFERDECK_PROVIDER_EXPORT const inferdeck_provider_v2* inferdeck_provider_v2(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
