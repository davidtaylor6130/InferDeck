// R4D prefill. Transposed core: S^T = K.Q^T, one lane owns one query row, ROWS = 16*NWARPS,
// 128-VGPR accumulator. The query is bf16 and the KV cache is fp8 or bf16; the 16-bit operand
// format for both matmuls is f16 (see r4d_dt16.h for why).
//
// Everything that measured far off the optimum was deleted rather than left behind a flag: the
// two-term-e4m3 query and P legs, the single-term fp8 P leg (16% faster, 15x less accurate), the
// paired 8-bit LDS read that only those needed, the S-split, and a layout permutation that CONTIG
// made redundant. What is left is the two families that ship: all-16-bit (bf16 KV) and 16/8 mixed
// (bf16 query, fp8 KV).
//
// The surviving options. Each is a measured step and each stays separable, so any one of them can
// be ablated again after a geometry change:
//
//   LAZY  (always on, and the reason this file exists)
//         The obvious form rescales all 128 accumulator VGPRs by alpha on EVERY m-tile. Here the
//         softmax runs against a REFERENCE max instead of a running one: p = 2^(s - m_ref + SHIFT)
//         with m_ref updated -- and the accumulator rescaled -- only when some row's max exceeds
//         m_ref by more than the P format can absorb (DT16::GROW octaves). Accuracy is unchanged:
//         eager rescaling by alpha and lazy accumulation at a shifted scale produce the SAME
//         relative weighting in an f32 accumulator.
//   F16   f16 rather than bf16 as the 16-bit operand format. gfx1201 has NO bf16 convert, so an
//         f32->bf16 is ~6 VALU of software RTNE; v_cvt_pkrtz_f16_f32 does two f32->f16 in ONE.
//         Both are exact for these operands (fp8 has 3 mantissa bits, bf16 has 7, f16 has 10).
//   MSKIP Causal masking applied only on m-tiles that straddle the limit. Interior tiles pay
//         nothing.
//   FOLDQ scale * k_descale * log2(e) folded into the query ONCE at load.
//   CONTIG The k axis of a WMMA is only ever CONTRACTED, so relabelling it on BOTH operands is
//         free; reading eight contiguous elements at 8*h gives one aligned ds_read_b128 per
//         fragment, deletes 16 address adds, and makes the P fragment lane-local.
//   PF/SGB Prefetch PF fragments before the matching PF WMMAs, with a hard scheduling barrier
//         between the two groups. Without the barrier PF is INERT.
//   DOT2  v_dot2_f32_f16 row sum off the packed f16 P: 4 instructions for 8, and the denominator
//         becomes the p values the PV matmul actually used.
//   BTS/KWIDE/GPRE  the tile-staging set -- see the comments on each define.
#include "../../r4d.h"
#include "r4d_common.h"
#include "r4d_dt16.h"
#include "r4d_int4_tiled.h"

__device__ __forceinline__ void int4_locate(const R4DInt4TiledArgs& a, const int* bt,
                                             int logical_pos, int& page, int& slot) {
    const int entry = bt[logical_pos / a.virtual_page_size];
    page = entry / a.virtual_subpages;
    slot = (entry % a.virtual_subpages) * a.virtual_page_size
         + logical_pos % a.virtual_page_size;
}

__device__ __forceinline__ float int4_scale(uint32_t word) {
    return __builtin_bit_cast(float, word & ~15u);
}

__device__ __forceinline__ uint16_t int4_to_f16(float x) {
    union { __fp16 h; uint16_t u; } r;
    r.h = (__fp16)x;
    return r.u;
}

#define O_F16     1
#define O_MSKIP   2
#define O_FOLDQ   8
#define O_CONTIG  512
#define O_PF2    1024
#define O_PF4    2048
#define O_PF8    3072
#define O_SGB    4096
#define O_DOT2  16384
// BTS: the block table entries for a tile are TILE/BS values at a WAVE-UNIFORM index, so they
// belong in SGPRs. Without this, every staging pass is `global_load_b32` (block table),
// `s_wait_loadcnt 0x0`, fourteen instructions of address arithmetic, `global_load_b64` (the KV),
// `s_wait_loadcnt 0x0` -- TWO serialised memory round trips, inside a barrier-bounded phase with
// nothing else to run. A scalar load hoisted to the top of the tile collapses that to one.
#define O_BTS   262144
// KWIDE: one 16-byte K fetch per thread instead of two 8-byte ones. 768 threads x 16 B is exactly
// a 48-key tile, so the staging loop becomes straight-line: half the global loads, half the block
// table lookups, half the dependency chains.
// The naive assignment (key = tid/16) makes a warp write ONE row at a 32-byte stride, which is a
// 4-way LDS bank conflict: bank = 8*ch mod 32 takes only four values. Interleaving two keys across
// the warp instead -- key = 2*warp + (lane&1), ch = (lane&31)>>1 -- gives eight distinct 4-bank
// groups covering all 32 banks, i.e. the conflict-free minimum, and the global side still reads two
// contiguous 256-byte runs.
#define O_KWIDE 524288
// GPREV: fetch tile ti+1's V into REGISTERS immediately after the second barrier, so its memory
// latency is covered by tile ti's 96 WMMAs and only the LDS stores stay between the barriers.
// V ONLY. The same prefetch for K was built three ways -- as its own bit, issued at the tail of
// the body so its live range missed the m-tile loop, and as a register-free scalar
// `s_prefetch_data` -- and all three LOST. At 240 VGPRs the allocator spills a second prefetch,
// and a spilled prefetch is waited on immediately, which is not a prefetch at all.
#define O_GPREV 1048576
// LDSB: an LDS-scoped barrier instead of __syncthreads(). See lds_barrier() in r4d_common.h --
// the default fence invalidates the vector cache twice per tile for a synchronisation that only
// ever orders LDS.
#define O_LDSB  2097152

// bf16 dword -> 16-bit dword in the target format, optionally scaled. Used once per kernel on Q.
template<class D, int FOLDQ, int F16>
__device__ __forceinline__ uint32_t r4d_qcvt(uint32_t w, float s) {
    if (!FOLDQ) return F16 ? D::from_bf16w(w) : w;
    return D::pk(__builtin_bit_cast(float, w << 16) * s,
                 __builtin_bit_cast(float, w & 0xffff0000u) * s);
}

template<int NWARPS, int TILE, int HEAD_DIM, int GQA, int BS, int KVP, int OPT>
__global__ __launch_bounds__(NWARPS * 32)
void r4d_attn_prefill_kernel(const R4DInt4TiledArgs a)
{
    constexpr int F16   = (OPT & O_F16)   ? 1 : 0;
    constexpr int MSKIP = (OPT & O_MSKIP) ? 1 : 0;
    constexpr int FOLDQ = (OPT & O_FOLDQ) ? 1 : 0;
    constexpr int CTG   = (OPT & O_CONTIG) ? 1 : 0;
    constexpr int PF    = 1 << ((OPT >> 10) & 3);
    constexpr int SGB   = (OPT & O_SGB) ? 1 : 0;
    constexpr int DOT2  = ((OPT & O_DOT2) && (OPT & O_F16)) ? 1 : 0;
    constexpr int BTS   = (OPT & O_BTS)   ? 1 : 0;
    constexpr int GPREV = (OPT & O_GPREV) ? 1 : 0;
    constexpr int LDSB  = (OPT & O_LDSB)  ? 1 : 0;
    constexpr int KWIDE = (OPT & O_KWIDE) ? 1 : 0;
    typedef DT16<F16> D;
    typedef typename D::frag frag16;

    constexpr int ROWS    = NWARPS * 16;
    constexpr int BLOCK_Q = ROWS / GQA;
    constexpr int NKS     = HEAD_DIM / 16;
    constexpr int MT      = TILE / 16;
    constexpr int KSTR    = HEAD_DIM + (CTG ? 8 : 4);
    constexpr int VSTR    = TILE + (CTG ? 8 : 4);
    constexpr int NTHR    = NWARPS * 32;
    constexpr int NB      = TILE / BS;                       // block-table entries per tile
    constexpr int KEL     = KWIDE ? 16 : 8;                  // K elements fetched per thread-pass
    constexpr int KITEMS  = TILE * (HEAD_DIM / KEL);          // K passes needed for one tile
    constexpr int KPASS   = (KITEMS + NTHR - 1) / NTHR;        // passes per thread
    constexpr int KREGS   = KPASS * (KVP ? 2 : 1);             // uint4 of raw K per thread
    constexpr int NTR     = (TILE / 8) * (HEAD_DIM / (KVP ? 32 : 64));
    constexpr int VREGS   = (NTR + NWARPS - 1) / NWARPS;        // uint4 of raw V per thread
    constexpr float PSHIFT = D::SHIFT;
    constexpr float PGROW  = D::GROW;
    static_assert(!KWIDE || KPASS == 1, "the wide K fetch is one straight-line pass");

    __shared__ uint16_t sK[TILE * KSTR];
    __shared__ uint16_t sV16[HEAD_DIM * VSTR];

    const int tid = threadIdx.x, lane = tid & 31, warp = tid >> 5;
    const int c = lane & 15, h = lane >> 4;

    const int qb = blockIdx.x, kvh = blockIdx.y, seq = blockIdx.z;
    const int ctx = a.seq_lens[seq];
    if (ctx <= 0) return;

    const int row = warp * 16 + c;
    const int qi = row / GQA, hi = row - qi * GQA;
    const int qpos = qb * BLOCK_Q + qi;
    const bool live = (qpos < a.q_len);
    const int qrow = live ? qpos : (a.q_len - 1);
    const int qhead = kvh * GQA + hi;

    const int* __restrict__ bt = a.block_table + (size_t)seq * a.max_blocks;

    const float mul   = a.softmax_scale * 1.44269504089f;

    frag16 qf[NKS];
    {
        const uint16_t* qp = (const uint16_t*)a.q
                           + ((size_t)(seq * a.q_len + qrow) * a.q_heads + qhead) * HEAD_DIM;
        #pragma unroll
        for (int t = 0; t < NKS; ++t) {
            // CONTIG reads the same 8 d as ONE run; the legacy map splits them 4+4.
            const uint2 l0 = *(const uint2*)(qp + 16 * t + (CTG ? 8 * h : 4 * h));
            const uint2 h0 = *(const uint2*)(qp + 16 * t + (CTG ? 8 * h + 4 : 8 + 4 * h));
            qf[t] = D::mk(make_uint2(r4d_qcvt<D, FOLDQ, F16>(l0.x, mul),
                                     r4d_qcvt<D, FOLDQ, F16>(l0.y, mul)),
                          make_uint2(r4d_qcvt<D, FOLDQ, F16>(h0.x, mul),
                                     r4d_qcvt<D, FOLDQ, F16>(h0.y, mul)));
        }
    }

    const int klimit = ctx - a.q_len + qpos;

    float m_ref = -3.0e38f;
    float off   = PSHIFT + 3.0e38f;
    float l_i   = 0.0f;
    v8f acc[NKS];
    #pragma unroll
    for (int t = 0; t < NKS; ++t) acc[t] = (v8f){0,0,0,0,0,0,0,0};

    const int qmax_cta   = min(qb * BLOCK_Q + BLOCK_Q - 1, a.q_len - 1);
    const int klimit_cta = ctx - a.q_len + qmax_cta;
    const int ntiles = (min(klimit_cta + 1, ctx) + TILE - 1) / TILE;

    // ---- staging, split into a global FETCH (registers) and an LDS STORE ------------------------
    // Splitting them is what lets GPRE move the fetch out from between the barriers; without GPRE
    // the two run back to back and the generated code is the same shape as it was before GPRE.
    const int vg = lane >> 3, vj = lane & 7;
    uint4 kreg[KREGS], vreg[VREGS];

    auto fetchK = [&](int k0, const int*) {
        #pragma unroll
        for (int p = 0; p < KPASS; ++p) {
            const int i = tid + p * NTHR;
            if (i < KITEMS) {
                const int key = i / (HEAD_DIM / 8);
                const int ch = i - key * (HEAD_DIM / 8);
                const int kk = min(k0 + key, ctx - 1);
                int page, slot;
                int4_locate(a, bt, kk, page, slot);
                const size_t row = (size_t)page * a.k_page_stride_bytes
                    + (size_t)slot * a.k_token_stride_bytes
                    + (size_t)kvh * a.k_head_stride_bytes + ch * 4;
                const uint32_t packed = *(const uint32_t*)(a.k_cache + row);
                const size_t sm = (size_t)page * a.k_scale_page_stride
                    + (size_t)slot * a.k_scale_token_stride
                    + (size_t)kvh * a.k_scale_head_stride;
                const uint32_t meta = a.k_scale_zp[sm];
                const int zp = (int)(meta & 15u);
                const float scale = int4_scale(meta);
                uint16_t* dst = &sK[key * KSTR + ch * 8];
                #pragma unroll
                for (int e = 0; e < 8; e += 2) {
                    const int qa = (packed >> (4 * e)) & 15;
                    const int qb = (packed >> (4 * (e + 1))) & 15;
                    dst[e] = int4_to_f16((qa - zp) * scale);
                    dst[e + 1] = int4_to_f16((qb - zp) * scale);
                }
            }
        }
    };

    auto fetchV = [&](int k0, const int*) {
        #pragma unroll
        for (int r = 0; r < VREGS; ++r) {
            const int idx = warp + r * NWARPS;
            if (idx < NTR) {
                constexpr int DG = HEAD_DIM / 64;
                const int kg = idx / DG, dg = idx - kg * DG;
                const int kk = min(k0 + kg * 8 + vj, ctx - 1);
                const int d0 = dg * 64 + vg * 16;
                int page, slot;
                int4_locate(a, bt, kk, page, slot);
                const size_t row = (size_t)page * a.v_page_stride_bytes
                    + (size_t)slot * a.v_token_stride_bytes
                    + (size_t)kvh * a.v_head_stride_bytes + d0 / 2;
                const uint64_t packed = *(const uint64_t*)(a.v_cache + row);
                const size_t sm = (size_t)page * a.v_scale_page_stride
                    + (size_t)slot * a.v_scale_token_stride
                    + (size_t)kvh * a.v_scale_head_stride;
                const uint32_t meta = a.v_scale_zp[sm];
                const int zp = (int)(meta & 15u);
                const float scale = int4_scale(meta);
                #pragma unroll
                for (int e = 0; e < 16; ++e) {
                    const int qv = (int)((packed >> (4 * e)) & 15u);
                    sV16[(d0 + e) * VSTR + kg * 8 + vj] = int4_to_f16((qv - zp) * scale);
                }
            }
        }
    };

    auto storeK = [&]() {};
    auto storeV = [&]() {};

    // The block table for tile ti is fetched during tile ti-1, so the scalar load has an entire
    // store phase and two barriers to complete instead of stalling the KV address chain.
    int blk[NB];
    if (BTS) {
        #pragma unroll
        for (int i = 0; i < NB; ++i) blk[i] = bt[i];
    }
    if (GPREV) fetchV(0, blk);

    for (int ti = 0; ti < ntiles; ++ti) {
        const int k0 = ti * TILE;
        int blkn[NB];
        if (BTS && ti + 1 < ntiles) {
            const int tb = (ti + 1) * NB;
            #pragma unroll
            for (int i = 0; i < NB; ++i) blkn[i] = bt[tb + i];
        }
        if (LDSB) lds_barrier(); else __syncthreads();
        fetchK(k0, blk);
        if (!GPREV) fetchV(k0, blk);
        storeV();              // V first: its data is already in registers, so it covers K's wait
        storeK();
        if (LDSB) lds_barrier(); else __syncthreads();
        // The next tile's global loads are issued HERE, before the 96 WMMAs below, and the wait
        // that consumes them does not appear until after the next barrier.
        if (GPREV && ti + 1 < ntiles) fetchV(k0 + TILE, blkn);

        #pragma unroll 1
        for (int mt = 0; mt < MT; ++mt) {
            v8f s = (v8f){0,0,0,0,0,0,0,0};
            {
                const uint16_t* krow = &sK[(mt * 16 + c) * KSTR] + (CTG ? 8 * h : 0);
                // PF fragments are LOADED before the matching PF WMMAs are ISSUED, so the wait in
                // front of each WMMA becomes `s_wait_dscnt PF-1` instead of a full drain.
                #pragma unroll
                for (int g = 0; g < NKS; g += PF) {
                    frag16 kb[PF];
                    #pragma unroll
                    for (int j = 0; j < PF; ++j) {
                        const uint16_t* kp = krow + 16 * (g + j);
                        if (CTG) {
                            const uint4 u = *(const uint4*)kp;
                            kb[j] = D::mk(make_uint2(u.x, u.y), make_uint2(u.z, u.w));
                        } else {
                            kb[j] = D::mk(*(const uint2*)(kp + 4 * h),
                                          *(const uint2*)(kp + 8 + 4 * h));
                        }
                    }
                    if (SGB) sched_barrier();
                    #pragma unroll
                    for (int j = 0; j < PF; ++j) s = D::wmma(kb[j], qf[g + j], s);
                }
            }
            float sc[8];
            #pragma unroll
            for (int e = 0; e < 8; ++e) sc[e] = FOLDQ ? s[e] : s[e] * mul;

            const int kbase = k0 + mt * 16 + 8 * h;
            if (!MSKIP || wave_any(kbase + 7 > klimit)) {
                #pragma unroll
                for (int e = 0; e < 8; ++e) sc[e] = (kbase + e <= klimit) ? sc[e] : -INFINITY;
            }

            float smax = sc[0];
            #pragma unroll
            for (int e = 1; e < 8; ++e) smax = fmaxf(smax, sc[e]);
            if (wave_any(smax > m_ref + PGROW)) {
                const float rmax  = fmaxf(smax, swap16(smax));
                const float mnew  = fmaxf(fmaxf(m_ref, rmax), -3.0e38f);
                const float alpha = ex2(m_ref - mnew);
                #pragma unroll
                for (int dt = 0; dt < NKS; ++dt)
                    #pragma unroll
                    for (int e = 0; e < 8; ++e) acc[dt][e] *= alpha;
                l_i *= alpha;
                m_ref = mnew;
                off   = PSHIFT - mnew;
            }

            float pf[8], lsum = 0.0f;
            #pragma unroll
            for (int e = 0; e < 8; ++e) {
                const float p = ex2(sc[e] + off);
                pf[e] = p;
                if (!DOT2) lsum += p;
            }
            if (!DOT2) l_i += lsum;

            {
                uint32_t pw[4];
                #pragma unroll
                for (int e = 0; e < 8; e += 2) pw[e >> 1] = D::pk(pf[e], pf[e + 1]);
                if (DOT2) {
                    const v2hf one = {(__fp16)1.0f, (__fp16)1.0f};
                    #pragma unroll
                    for (int e = 0; e < 4; ++e)
                        l_i = __builtin_amdgcn_fdot2(__builtin_bit_cast(v2hf, pw[e]), one, l_i, false);
                }
                const uint2 lo  = make_uint2(pw[0], pw[1]);
                const uint2 hi2 = make_uint2(pw[2], pw[3]);
                // QK leaves lane (c,h) holding keys 8h+e. Under CONTIG that IS the B fragment, so
                // the cross-half exchange the two-runs-of-four map needed is simply not there.
                frag16 p16;
                if (CTG) {
                    p16 = D::mk(lo, hi2);
                } else {
                    const uint2 y = swap16_u2p((h == 0) ? hi2 : lo);
                    p16 = (h == 0) ? D::mk(lo, y) : D::mk(y, hi2);
                }
                const uint16_t* vbase = &sV16[c * VSTR + mt * 16] + (CTG ? 8 * h : 0);
                #pragma unroll
                for (int g = 0; g < NKS; g += PF) {
                    frag16 vb[PF];
                    #pragma unroll
                    for (int j = 0; j < PF; ++j) {
                        const uint16_t* vp = vbase + (g + j) * 16 * VSTR;
                        if (CTG) {
                            const uint4 u = *(const uint4*)vp;
                            vb[j] = D::mk(make_uint2(u.x, u.y), make_uint2(u.z, u.w));
                        } else {
                            vb[j] = D::mk(*(const uint2*)(vp + 4 * h),
                                          *(const uint2*)(vp + 8 + 4 * h));
                        }
                    }
                    if (SGB) sched_barrier();
                    #pragma unroll
                    for (int j = 0; j < PF; ++j) acc[g + j] = D::wmma(vb[j], p16, acc[g + j]);
                }
            }
        }
        if (BTS) {
            #pragma unroll
            for (int i = 0; i < NB; ++i) blk[i] = blkn[i];
        }
    }

    if (!live) return;
    l_i += swap16(l_i);
    const float inv = (l_i > 0.0f) ? (1.0f / l_i) : 0.0f;
    uint16_t* op = (uint16_t*)a.out
                 + ((size_t)(seq * a.q_len + qpos) * a.q_heads + qhead) * HEAD_DIM;
    #pragma unroll
    for (int dt = 0; dt < NKS; ++dt) {
        uint32_t tw[4];
        #pragma unroll
        for (int e = 0; e < 8; e += 2)
            tw[e >> 1] = (uint32_t)f32_to_bf16(acc[dt][e] * inv)
                       | ((uint32_t)f32_to_bf16(acc[dt][e + 1] * inv) << 16);
        *(uint4*)(op + dt * 16 + 8 * h) = make_uint4(tw[0], tw[1], tw[2], tw[3]);
    }
}

extern "C" R4D_INT4_EXPORT int r4d_int4_tiled_prefill_h256_gqa6(
    const R4DInt4TiledArgs* a, hipStream_t stream) {
    if (!a || !a->q || !a->k_cache || !a->v_cache || !a->k_scale_zp ||
        !a->v_scale_zp || !a->block_table || !a->seq_lens || !a->out ||
        a->seqs <= 0 || a->q_len <= 0 || a->q_heads <= 0 || a->kv_heads <= 0 ||
        a->q_heads != a->kv_heads * 6 || a->head_dim != 256 ||
        a->virtual_page_size != 16 || a->physical_page_size <= 0 ||
        a->physical_page_size % 16 != 0 ||
        a->virtual_subpages != a->physical_page_size / 16 || a->max_blocks <= 0 ||
        a->k_page_stride_bytes <= 0 || a->v_page_stride_bytes <= 0 ||
        a->k_token_stride_bytes <= 0 || a->v_token_stride_bytes <= 0 ||
        a->k_head_stride_bytes <= 0 || a->v_head_stride_bytes <= 0 ||
        a->k_scale_page_stride <= 0 || a->v_scale_page_stride <= 0 ||
        a->k_scale_token_stride <= 0 || a->v_scale_token_stride <= 0 ||
        a->k_scale_head_stride <= 0 || a->v_scale_head_stride <= 0) return -1;
    constexpr int opts = O_F16 | O_MSKIP | O_FOLDQ | O_CONTIG | O_PF4 | O_SGB;
    const dim3 grid((a->q_len + 63) / 64, a->kv_heads, a->seqs);
    hipLaunchKernelGGL((r4d_attn_prefill_kernel<24, 48, 256, 6, 16, 0, opts>),
                       grid, dim3(24 * 32), 0, stream, *a);
    return (int)hipGetLastError();
}


