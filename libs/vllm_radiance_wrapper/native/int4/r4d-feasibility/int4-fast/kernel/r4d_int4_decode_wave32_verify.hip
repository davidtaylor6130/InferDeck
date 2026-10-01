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

// The pinned layout keeps every head row at a 132-byte multiple and every token
// row at a 1056-byte multiple, so each lane's 4-byte chunk is aligned.
__device__ __forceinline__ uint32_t load_dword(const void* p) {
    return *reinterpret_cast<const uint32_t*>(p);
}

// One 32-lane RDNA wave handles a head. Each lane owns eight consecutive
// dimensions, so one 32-bit load per lane covers the whole 256-dimension head
// instead of eight byte loads. Nibble order and the accumulation order are
// unchanged from the scalar form, so results are bit-identical.
__global__ __launch_bounds__(32)
void int4_decode_wave32_kernel(Int4DecodeArgs a, int query_tokens) {
    const int head = blockIdx.z * a.q_heads + blockIdx.x, split = blockIdx.y, lane = threadIdx.x;
    const int ctx = max(0, a.seq_lens[0] - query_tokens + 1 + int(blockIdx.z));
    const int chunk = (ctx + a.splits - 1) / a.splits;
    const int begin = split * chunk, end = min(begin + chunk, ctx);
    const int gqa = a.q_heads / a.kv_heads, kvh = (head % a.q_heads) / gqa;
    const int dim = lane * 8;
    float m = -INFINITY, l = 0.0f;
    float acc[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float q[8];
    for (int i = 0; i < 8; ++i) {
        q[i] = __builtin_bit_cast(float, uint32_t(a.q[head * a.head_dim + dim + i]) << 16);
    }

    for (int pos = begin; pos < end; ++pos) {
        int slot;
        const int page = locate(a, pos, slot);
        const size_t koff = size_t(page) * a.k_page_stride + size_t(slot) * a.k_token_stride + size_t(kvh) * a.k_head_stride;
        const size_t voff = size_t(page) * a.v_page_stride + size_t(slot) * a.v_token_stride + size_t(kvh) * a.v_head_stride;
        const size_t soff = size_t(page) * a.scale_page_stride + size_t(slot) * a.scale_token_stride + size_t(kvh) * a.scale_head_stride;
        const uint32_t km = a.ks[soff], vm = a.vs[soff];
        const float kscale = unpack_scale(km), vscale = unpack_scale(vm);
        const float kzp = float(int(km & 15u)), vzp = float(int(vm & 15u));
        const uint32_t kw = load_dword(a.k + koff + dim / 2);
        const uint32_t vw = load_dword(a.v + voff + dim / 2);
        float dot = 0.0f;
        float vv[8];
        for (int i = 0; i < 8; ++i) {
            const int qk = int((kw >> (4 * (2 * (i >> 1) + (i & 1)))) & 15u);
            const int qv = int((vw >> (4 * (2 * (i >> 1) + (i & 1)))) & 15u);
            dot = fmaf(q[i], (qk - kzp) * kscale, dot);
            vv[i] = (qv - vzp) * vscale;
        }
        for (int offset = 16; offset > 0; offset >>= 1)
            dot += __shfl_down(dot, offset, 32);
        const float score = __shfl(dot, 0, 32) * a.scale;
        const float mn = fmaxf(m, score);
        const float alpha = isfinite(m) ? expf(m - mn) : 0.0f;
        const float beta = expf(score - mn);
        for (int i = 0; i < 8; ++i) acc[i] = acc[i] * alpha + vv[i] * beta;
        l = l * alpha + beta;
        m = mn;
    }

    for (int i = 0; i < 8; ++i) {
        const size_t idx = (size_t(head) * a.splits + split) * a.head_dim + dim + i;
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
    const int head = blockIdx.z * a.q_heads + blockIdx.x, d = threadIdx.x;
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

static int launch_grouped_decode(const Int4DecodeArgs* pArgs, hipStream_t stream);

extern "C" __declspec(dllexport) int r4d_int4_decode_splitk(const Int4DecodeArgs* a, hipStream_t stream) {
    if (!a || a->q_heads <= 0 || a->kv_heads <= 0 || a->q_heads % a->kv_heads || a->head_dim != 256 ||
        a->context <= 0 || a->page_size <= 0 || a->splits <= 0 || a->splits > 256) return -1;
    if (a->q_heads == 24 && a->kv_heads == 4)
    {
        return launch_grouped_decode(a, stream);
    }
    hipLaunchKernelGGL(int4_decode_wave32_kernel, dim3(a->q_heads, a->splits), dim3(32), 0, stream, *a, 1);
    hipError_t e = hipGetLastError(); if (e != hipSuccess) return -100 - (int)e;
    hipLaunchKernelGGL(int4_decode_combine_kernel, dim3(a->q_heads), dim3(256), 0, stream, *a);
    e = hipGetLastError(); return e == hipSuccess ? 0 : -100 - (int)e;
}

template<int QueryTokens, int HeadGroup = 1>
__global__ __launch_bounds__(32)
void int4_verify_wave32_kernel(Int4DecodeArgs a)
{
    const int head = blockIdx.x * HeadGroup, split = blockIdx.y, lane = threadIdx.x;
    const int ctx = max(0, a.seq_lens[0]);
    const int chunk = (ctx + a.splits - 1) / a.splits;
    const int begin = split * chunk, end = min(begin + chunk, ctx);
    const int kvh = head / (a.q_heads / a.kv_heads), dim = lane * 8;
    float q[QueryTokens * HeadGroup][8], acc[QueryTokens * HeadGroup][8];
    float m[QueryTokens * HeadGroup], l[QueryTokens * HeadGroup];
    #pragma unroll
    for (int query = 0; query < QueryTokens * HeadGroup; ++query)
    {
        m[query] = -INFINITY;
        l[query] = 0.0f;
        #pragma unroll
        for (int i = 0; i < 8; ++i)
        {
            q[query][i] = __builtin_bit_cast(float, uint32_t(a.q[((query / HeadGroup) * a.q_heads + head + query % HeadGroup) * a.head_dim + dim + i]) << 16);
            acc[query][i] = 0.0f;
        }
    }
    for (int pos = begin; pos < end; ++pos)
    {
        int slot;
        const int page = locate(a, pos, slot);
        const size_t koff = size_t(page) * a.k_page_stride + size_t(slot) * a.k_token_stride + size_t(kvh) * a.k_head_stride;
        const size_t voff = size_t(page) * a.v_page_stride + size_t(slot) * a.v_token_stride + size_t(kvh) * a.v_head_stride;
        const size_t soff = size_t(page) * a.scale_page_stride + size_t(slot) * a.scale_token_stride + size_t(kvh) * a.scale_head_stride;
        const uint32_t km = a.ks[soff], vm = a.vs[soff];
        const float kscale = unpack_scale(km), vscale = unpack_scale(vm);
        const float kzp = float(int(km & 15u)), vzp = float(int(vm & 15u));
        const uint32_t kw = load_dword(a.k + koff + dim / 2), vw = load_dword(a.v + voff + dim / 2);
        float kk[8], vv[8];
        #pragma unroll
        for (int i = 0; i < 8; ++i)
        {
            kk[i] = (int((kw >> (4 * i)) & 15u) - kzp) * kscale;
            vv[i] = (int((vw >> (4 * i)) & 15u) - vzp) * vscale;
        }
        #pragma unroll
        for (int query = 0; query < QueryTokens * HeadGroup; ++query)
        {
            if (pos < ctx - QueryTokens + query / HeadGroup + 1)
            {
                float dot = 0.0f;
                #pragma unroll
                for (int i = 0; i < 8; ++i) dot = fmaf(q[query][i], kk[i], dot);
                for (int offset = 16; offset > 0; offset >>= 1) dot += __shfl_down(dot, offset, 32);
                const float reduced_dot = __shfl(dot, 0, 32);
                const float score = reduced_dot * a.scale;
                const float mn = fmaxf(m[query], score);
                const float alpha = isfinite(m[query]) ? expf(m[query] - mn) : 0.0f;
                const float beta = QueryTokens == 1 ? expf(fmaf(reduced_dot, a.scale, -mn)) : expf(score - mn);
                #pragma unroll
                for (int i = 0; i < 8; ++i) acc[query][i] = QueryTokens == 1 ? fmaf(acc[query][i], alpha, vv[i] * beta) : acc[query][i] * alpha + vv[i] * beta;
                l[query] = l[query] * alpha + beta;
                m[query] = mn;
            }
        }
    }
    #pragma unroll
    for (int query = 0; query < QueryTokens * HeadGroup; ++query)
    {
        const size_t row = (size_t(query / HeadGroup) * a.q_heads + head + query % HeadGroup) * a.splits + split;
        #pragma unroll
        for (int i = 0; i < 8; ++i)
        {
            const uint32_t bits = __builtin_bit_cast(uint32_t, acc[query][i]);
            a.partial_o[row * a.head_dim + dim + i] = uint16_t((bits + 0x7fffu + ((bits >> 16) & 1u)) >> 16);
        }
        if (lane == 0)
        {
            a.partial_m[row] = m[query];
            a.partial_l[row] = l[query];
        }
    }
}

extern "C" __declspec(dllexport) int r4d_int4_verify_splitk(const Int4DecodeArgs* pArgs, int32_t query_tokens, hipStream_t stream)
{
    if (!pArgs || pArgs->q_heads <= 0 || pArgs->kv_heads <= 0 || pArgs->q_heads % pArgs->kv_heads || pArgs->head_dim != 256 ||
        pArgs->context <= 0 || pArgs->page_size <= 0 || pArgs->splits <= 0 || pArgs->splits > 256 || query_tokens < 1 || query_tokens > 4)
    {
        return -1;
    }
    if (query_tokens == 4)
    {
        hipLaunchKernelGGL((int4_verify_wave32_kernel<4>), dim3(pArgs->q_heads, pArgs->splits), dim3(32), 0, stream, *pArgs);
    }
    else if (query_tokens == 3)
    {
        hipLaunchKernelGGL((int4_verify_wave32_kernel<3>), dim3(pArgs->q_heads, pArgs->splits), dim3(32), 0, stream, *pArgs);
    }
    else if (query_tokens == 2)
    {
        hipLaunchKernelGGL((int4_verify_wave32_kernel<2>), dim3(pArgs->q_heads, pArgs->splits), dim3(32), 0, stream, *pArgs);
    }
    else
    {
        return r4d_int4_decode_splitk(pArgs, stream);
    }
    hipError_t eError = hipGetLastError();
    if (eError != hipSuccess)
    {
        return -100 - int(eError);
    }
    hipLaunchKernelGGL(int4_decode_combine_kernel, dim3(pArgs->q_heads, 1, query_tokens), dim3(256), 0, stream, *pArgs);
    eError = hipGetLastError();
    return eError == hipSuccess ? 0 : -100 - int(eError);
}

static int launch_grouped_decode(const Int4DecodeArgs* pArgs, hipStream_t stream)
{
    hipLaunchKernelGGL((int4_verify_wave32_kernel<1, 3>), dim3(pArgs->q_heads / 3, pArgs->splits), dim3(32), 0, stream, *pArgs);
    hipError_t eError = hipGetLastError();
    if (eError != hipSuccess)
    {
        return -100 - int(eError);
    }
    hipLaunchKernelGGL(int4_decode_combine_kernel, dim3(pArgs->q_heads), dim3(256), 0, stream, *pArgs);
    eError = hipGetLastError();
    return eError == hipSuccess ? 0 : -100 - int(eError);
}
