// Phase 4a: scalar CPU support for ik_llama.cpp IQ2_K / IQ3_K / IQ4_K
// Declarations for the generic (no-SIMD) dequant + vec_dot reference kernels
// implemented in llamafile/iqk_quantize_k.cpp.
//
// C linkage so these can be referenced from ggml.c (C) and ggml-cpu.c (C).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// to_float (ggml_to_float_t-compatible): x points at packed block_iqN_k rows,
// y receives k dequantized floats.
void dequantize_row_iq2_k(const void * x, float * y, int64_t k);
void dequantize_row_iq3_k(const void * x, float * y, int64_t k);
void dequantize_row_iq4_k(const void * x, float * y, int64_t k);

// vec_dot (ggml_vec_dot_t-compatible): dot of one iqN_k row (vx) against one
// q8_K row (vy), result in *s.
void vec_dot_iq2_k_q8_k(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by, int nrc);
void vec_dot_iq3_k_q8_k(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by, int nrc);
void vec_dot_iq4_k_q8_k(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by, int nrc);

#ifdef __cplusplus
}
#endif
