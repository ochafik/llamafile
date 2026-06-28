// Phase 4a: generic (scalar, no-SIMD) CPU kernels for the ik_llama.cpp
// i-quants IQ2_K / IQ3_K / IQ4_K.
//
// Ported from ik_llama.cpp's ggml/src/iqk/iqk_quantize.cpp:
//   - dequantize_row_iq2_k / _iq3_k / _iq4_k  (scalar reference, verbatim)
//   - vec_dot_iqN_k_q8_k                       (ik_llama only has a SIMD path
//                                               here, so we implement a generic
//                                               dequant-and-dot reference)
//
// This file deliberately uses NO SIMD intrinsics and is built without -march
// so it works on every target cosmocc produces. Correctness over speed; the
// fast SIMD Dequantizer path is a separate later phase (4b).

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "ggml-impl.h" // GGML_FP16_TO_FP32, GGML_UNUSED

#define GGML_COMMON_DECL_C
#define GGML_COMMON_IMPL_C
#include "ggml-common.h" // block_iq{2,3,4}_k, block_q8_K, iq{2,3}nl_values, iq4k_values

#include "iqk_k_types.h"

// ---------------------------------------------------------------------------
// dequantize (to_float)
// ---------------------------------------------------------------------------

void dequantize_row_iq2_k(const void * vx, float * y, int64_t k) {
    assert(k % QK_K == 0);
    const block_iq2_k * x = (const block_iq2_k *)vx;
    const int nb = k / QK_K;

    for (int i = 0; i < nb; i++) {
        const float d = GGML_FP16_TO_FP32(x[i].d);
        const uint8_t * qs = x[i].qs;

        uint16_t extra = x[i].extra;

        int shift = 0;
        for (int ib32 = 0; ib32 < QK_K/32; ++ib32) {
            float dl1 = d * ((x[i].scales[ib32] & 0xf) - 8);
            float dl2 = d * ((x[i].scales[ib32] >>  4) - 8);
            const int8_t * values1 = extra & 1 ? iq2nl_values + 4 : iq2nl_values;
            const int8_t * values2 = extra & 2 ? iq2nl_values + 4 : iq2nl_values;
            extra >>= 2;
            for (int j = 0; j < 16; ++j) {
                y[j+ 0] = dl1 * values1[(qs[j+ 0] >> shift) & 3];
                y[j+16] = dl2 * values2[(qs[j+16] >> shift) & 3];
            }
            y += 32;
            shift += 2;
            if (shift == 8) { qs += 32; shift = 0; }
        }
    }
}

void dequantize_row_iq3_k(const void * vx, float * y, int64_t k) {
    assert(k % QK_K == 0);
    const block_iq3_k * x = (const block_iq3_k *)vx;
    const int nb = k / QK_K;

    for (int i = 0; i < nb; i++) {
        const float d = GGML_FP16_TO_FP32(x[i].d);
        const uint8_t * qs = x[i].qs;
        const uint8_t * qh = x[i].qh;

        uint16_t sh = x[i].scales_h;
        uint16_t extra = x[i].extra;

        for (int ib32 = 0; ib32 < QK_K/32; ++ib32) {
            float dl1 = d * ((2*(x[i].scales_l[ib32] & 0xf) + 1) * ((sh & 1) ? -1 : 1));
            float dl2 = d * ((2*(x[i].scales_l[ib32] >>  4) + 1) * ((sh & 2) ? -1 : 1));
            sh >>= 2;
            const int8_t * values1 = extra & 1 ? iq3nl_values + 8 : iq3nl_values;
            const int8_t * values2 = extra & 2 ? iq3nl_values + 8 : iq3nl_values;
            extra >>= 2;
            int shift_l = 2*(ib32%4);
            int shift_h = ib32%8;
            for (int j = 0; j < 16; ++j) {
                y[j+ 0] = dl1 * values1[((qs[j+ 0] >> shift_l) & 3) | (((qh[j+ 0] >> shift_h) & 1) << 2)];
                y[j+16] = dl2 * values2[((qs[j+16] >> shift_l) & 3) | (((qh[j+16] >> shift_h) & 1) << 2)];
            }
            y += 32;
            if (shift_l == 6) qs += 32;
        }
    }
}

void dequantize_row_iq4_k(const void * vx, float * y, int64_t k) {
    assert(k % QK_K == 0);
    const block_iq4_k * x = (const block_iq4_k *)vx;
    const int nb = k / QK_K;

    for (int i = 0; i < nb; i++) {
        const uint8_t * qs = x[i].qs;
        const float d = GGML_FP16_TO_FP32(x[i].d);

        uint16_t extra = x[i].extra;

        for (int ib = 0; ib < QK_K/32; ++ib) {
            const uint8_t sh = x[i].scales_h[ib/2] >> 4*(ib%2);
            const float dl1 = d * (((x[i].scales_l[ib] & 0xf) | ((sh << 4) & 0x30)) - 32);
            const float dl2 = d * (((x[i].scales_l[ib] >>  4) | ((sh << 2) & 0x30)) - 32);
            const int8_t * values1 = extra & 1 ? iq4k_values + 16 : iq4k_values;
            const int8_t * values2 = extra & 2 ? iq4k_values + 16 : iq4k_values;
            extra >>= 2;
            for (int j = 0; j < 16; ++j) {
                y[j+ 0] = dl1 * values1[qs[j] & 0xf];
                y[j+16] = dl2 * values2[qs[j] >>  4];
            }
            y  += 32;
            qs += 16;
        }
    }
}

// ---------------------------------------------------------------------------
// vec_dot (generic dequant-and-dot reference; vec_dot_type == GGML_TYPE_Q8_K)
// ---------------------------------------------------------------------------

namespace {
template <typename Dequant>
inline void vec_dot_k_q8_k(Dequant dequant, int n, float * s, const void * vy) {
    assert(n % QK_K == 0);
    const int nb = n / QK_K;
    const block_q8_K * y = (const block_q8_K *)vy;

    float xb[QK_K];
    float sumf = 0;
    for (int i = 0; i < nb; ++i) {
        dequant(i, xb);
        const int8_t * q8 = y[i].qs;
        float bd = y[i].d;
        float bsum = 0;
        for (int j = 0; j < QK_K; ++j) {
            bsum += xb[j] * q8[j];
        }
        sumf += bd * bsum;
    }
    *s = sumf;
}
} // namespace

void vec_dot_iq2_k_q8_k(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by, int nrc) {
    assert(nrc == 1);
    GGML_UNUSED(nrc); GGML_UNUSED(bx); GGML_UNUSED(by); GGML_UNUSED(bs);
    const block_iq2_k * x = (const block_iq2_k *)vx;
    vec_dot_k_q8_k([x](int i, float * xb) { dequantize_row_iq2_k(x + i, xb, QK_K); }, n, s, vy);
}

void vec_dot_iq3_k_q8_k(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by, int nrc) {
    assert(nrc == 1);
    GGML_UNUSED(nrc); GGML_UNUSED(bx); GGML_UNUSED(by); GGML_UNUSED(bs);
    const block_iq3_k * x = (const block_iq3_k *)vx;
    vec_dot_k_q8_k([x](int i, float * xb) { dequantize_row_iq3_k(x + i, xb, QK_K); }, n, s, vy);
}

void vec_dot_iq4_k_q8_k(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by, int nrc) {
    assert(nrc == 1);
    GGML_UNUSED(nrc); GGML_UNUSED(bx); GGML_UNUSED(by); GGML_UNUSED(bs);
    const block_iq4_k * x = (const block_iq4_k *)vx;
    vec_dot_k_q8_k([x](int i, float * xb) { dequantize_row_iq4_k(x + i, xb, QK_K); }, n, s, vy);
}
