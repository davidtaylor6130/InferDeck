#pragma once
#include <hip/hip_runtime.h>
#include <cstddef>
#include <cstdint>

// C ABI for R4D's isolated packed-INT4 prefill kernel. q/out use contiguous
// [seqs,q_len,q_heads,head_dim] BF16. K/V use separate byte cache views with
// explicit page/token/head byte strides. Scale arrays use
// [physical_pages,physical_page_size,kv_heads] uint32: float32 scale bits with
// the low four mantissa bits replaced by the asymmetric zero point. block_table
// entries index 16-token virtual pages: physical_page=entry/virtual_subpages,
// physical_slot=(entry%virtual_subpages)*16 + logical_pos%16.
struct R4DInt4TiledArgs {
    const uint16_t* q;
    const uint8_t* k_cache;
    const uint8_t* v_cache;
    const uint32_t* k_scale_zp;
    const uint32_t* v_scale_zp;
    const int32_t* block_table;
    const int32_t* seq_lens;
    uint16_t* out;
    int32_t seqs, q_len, q_heads, kv_heads, head_dim;
    int32_t virtual_page_size, virtual_subpages, max_blocks;
    int32_t physical_page_size;
    int64_t k_page_stride_bytes, v_page_stride_bytes;
    int64_t k_token_stride_bytes, v_token_stride_bytes;
    int64_t k_head_stride_bytes, v_head_stride_bytes;
    int64_t k_scale_page_stride, v_scale_page_stride;
    int64_t k_scale_token_stride, v_scale_token_stride;
    int64_t k_scale_head_stride, v_scale_head_stride;
    float softmax_scale; // caller passes model_scale / head_dim (vLLM INT4 contract)
};
static_assert(offsetof(R4DInt4TiledArgs, softmax_scale) == 200, "R4D INT4 ABI float offset");
static_assert(sizeof(R4DInt4TiledArgs) == 208, "R4D INT4 Win64 ABI size");

#if defined(_WIN32) && !defined(__HIP_DEVICE_COMPILE__)
#define R4D_INT4_EXPORT __declspec(dllexport)
#else
#define R4D_INT4_EXPORT
#endif

extern "C" R4D_INT4_EXPORT int r4d_int4_tiled_prefill_h256_gqa6(
    const R4DInt4TiledArgs* args, hipStream_t stream);
