/**
 * @file conversions.h
 * @brief High-performance SIMD-accelerated data transformation routines.
 *
 * Implements hardware-vectorized AVX2/SSSE3 byte-swapping (swab) for endianness
 * conversions on raw storage, audio, and network streams.
 */

#ifndef DD_CONVERSIONS_H
#define DD_CONVERSIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "idx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Swap adjacent byte pairs (swab) with preservation of odd trailing bytes across block boundaries.
 *
 * Uses AVX2 256-bit shuffle instructions with 4x unrolling (128 bytes/iteration)
 * where hardware allows, falling back dynamically to SSSE3 or portable scalar swapping.
 *
 * @param buf Data buffer to mutate in place.
 * @param nread In/out pointer to byte length.
 * @param saved_byte In/out tracker for odd-boundary byte carry-over (-1 when empty).
 * @return Pointer to adjusted buffer base.
 */
char *dd_swab_buffer (char *buf, idx_t *nread, int *saved_byte);

#ifdef __cplusplus
}
#endif

#endif /* DD_CONVERSIONS_H */
