/*
 * InferDeck provider ABI, version 1.
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
    INFERDECK_INTERNAL = 13
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

#ifdef __cplusplus
}
#endif

#endif
