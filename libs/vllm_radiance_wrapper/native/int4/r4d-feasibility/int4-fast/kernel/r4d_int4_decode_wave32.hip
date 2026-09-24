#include "r4d_int4_tiled.h"
#include <cmath>

struct Int4DecodeArgs {
    const uint16_t* q; const uint8_t* k; const uint8_t* v;
    const uint32_t* ks; const uint32_t* vs;
    const int32_t* block_table; const int32_t* seq_lens;
    uint16_t* partial_o; float* partial_m; float* partial_l; uint16_t* out;
    int32_t q_heads, kv_heads, head_dim, context, page_size, max_blocks, splits;
    int64_t k_page_stride, v_page_stride, k_token_stride, v_token_stride;
    int64_t k_head_stride, v_head_stride, scale_page_stride, scale_token_stride, scale_head_stride;
    float scale;
};

__device__ __forceinline__ float unpack_scale(uint32_t w) {
    return __builtin_bit_cast(float, w & ~15u);
}

__device__ __forceinline__ int locate(const Int4DecodeArgs& a, int pos, int& slot) {
    const int p = a.block_table[pos / a.page_size];
    slot = pos % a.page_size;
    return p;
}

__device__ __forceinline__ int unpack_nibble(const uint8_t* cache, size_t offset, int dim) {
    const uint8_t packed = cache[offset];
    return (dim & 1) ? int(packed >> 4) : int(packed & 15);
}

// One 32-lane RDNA wave handles a head. Each lane owns eight dimensions
// separated by one wave, reducing K dot products with wave shuffles.
__global__ __launch_bounds__(32)
void int4_decode_wave32_kernel(Int4DecodeArgs a) {
    const int head = blockIdx.x, split = blockIdx.y, lane = threadIdx.x;
    const int ctx = a.seq_lens[0];
    const int chunk = (ctx + a.splits - 1) / a.splits;
    const int begin = split * chunk, end = min(begin + chunk, ctx);
    const int gqa = a.q_heads / a.kv_heads, kvh = head / gqa;
    float m = -INFINITY, l = 0.0f;
    float acc[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float q[8];
    for (int i = 0; i < 8; ++i) {
        const int dim = lane + 32 * i;
        q[i] = __builtin_bit_cast(float, uint32_t(a.q[head * a.head_dim + dim]) << 16);
    }

    for (int pos = begin; pos < end; ++pos) {
        int slot;
        const int page = locate(a, pos, slot);
        const size_t koff = size_t(page) * a.k_page_stride + size_t(slot) * a.k_token_stride + size_t(kvh) * a.k_head_stride;
        const size_t sk = size_t(page) * a.scale_page_stride + size_t(slot) * a.scale_token_stride + size_t(kvh) * a.scale_head_stride;
        const uint32_t km = a.ks[sk];
        const float kscale = unpack_scale(km);
        const int kzp = int(km & 15u);
        float dot = 0.0f;
        for (int i = 0; i < 8; ++i) {
            const int dim = lane + 32 * i;
            const int qk = unpack_nibble(a.k, koff + dim / 2, dim);
            dot = fmaf(q[i], float(qk - kzp) * kscale, dot);
        }
        for (int offset = 16; offset > 0; offset >>= 1)
            dot += __shfl_down(dot, offset, 32);
        const float score = __shfl(dot, 0, 32) * a.scale;
        const float mn = fmaxf(m, score);
        const float alpha = isfinite(m) ? expf(m - mn) : 0.0f;
        const float beta = expf(score - mn);

        const size_t voff = size_t(page) * a.v_page_stride + size_t(slot) * a.v_token_stride + size_t(kvh) * a.v_head_stride;
        const size_t sv = size_t(page) * a.scale_page_stride + size_t(slot) * a.scale_token_stride + size_t(kvh) * a.scale_head_stride;
        const uint32_t vm = a.vs[sv];
        const float vscale = unpack_scale(vm);
        const int vzp = int(vm & 15u);
        for (int i = 0; i < 8; ++i) {
            const int dim = lane + 32 * i;
            const int qv = unpack_nibble(a.v, voff + dim / 2, dim);
            const float vv = float(qv - vzp) * vscale;
            acc[i] = acc[i] * alpha + vv * beta;
        }
        l = l * alpha + beta;
        m = mn;
    }

    for (int i = 0; i < 8; ++i) {
        const int dim = lane + 32 * i;
        const size_t idx = (size_t(head) * a.splits + split) * a.head_dim + dim;
        const uint32_t aw = __builtin_bit_cast(uint32_t, acc[i]);
        a.partial_o[idx] = uint16_t((aw + 0x7fffu + ((aw >> 16) & 1u)) >> 16);
    }
    if (lane == 0) {
        const size_t mi = size_t(head) * a.splits + split;
        a.partial_m[mi] = m;
        a.partial_l[mi] = l;
    }
}

__global__ __launch_bounds__(256)
void int4_decode_combine_kernel(Int4DecodeArgs a) {
    const int head = blockIdx.x, d = threadIdx.x;
    float gm = -INFINITY;
    for (int s = 0; s < a.splits; ++s) gm = fmaxf(gm, a.partial_m[size_t(head) * a.splits + s]);
    float den = 0.0f, num = 0.0f;
    for (int s = 0; s < a.splits; ++s) {
        const size_t row = size_t(head) * a.splits + s;
        const float w = expf(a.partial_m[row] - gm);
        den += w * a.partial_l[row];
        const uint16_t partial_bits = a.partial_o[row * a.head_dim + d];
        num += w * __builtin_bit_cast(float, uint32_t(partial_bits) << 16);
    }
    const float result = den > 0.0f ? num / den : 0.0f;
    const uint32_t rw = __builtin_bit_cast(uint32_t, result);
    a.out[size_t(head) * a.head_dim + d] = uint16_t((rw + 0x7fffu + ((rw >> 16) & 1u)) >> 16);
}

extern "C" __declspec(dllexport) int r4d_int4_decode_splitk(const Int4DecodeArgs* a, hipStream_t stream) {
    if (!a || a->q_heads <= 0 || a->kv_heads <= 0 || a->q_heads % a->kv_heads || a->head_dim != 256 ||
        a->context <= 0 || a->page_size <= 0 || a->splits <= 0 || a->splits > 256) return -1;
    hipLaunchKernelGGL(int4_decode_wave32_kernel, dim3(a->q_heads, a->splits), dim3(32), 0, stream, *a);
    hipError_t e = hipGetLastError(); if (e != hipSuccess) return -100 - (int)e;
    hipLaunchKernelGGL(int4_decode_combine_kernel, dim3(a->q_heads), dim3(256), 0, stream, *a);
    e = hipGetLastError(); return e == hipSuccess ? 0 : -100 - (int)e;
}
