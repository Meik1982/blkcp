/**
 * @file conversions.c
 * @brief High-performance SIMD-accelerated data transformation routines.
 *
 * Implements hardware-vectorized AVX2/SSSE3 byte-swapping (swab) for endianness
 * conversions on raw storage, audio, and network streams.
 */

#include <config.h>
#include <stdbool.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
# include <immintrin.h>
#endif

#include "conversions.h"

static void
swab_scalar (char *buf, idx_t len)
{
  for (idx_t i = 0; i + 2 <= len; i += 2)
    {
      char tmp = buf[i];
      buf[i] = buf[i + 1];
      buf[i + 1] = tmp;
    }
}

#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("avx2")))
static void
swab_avx2 (char *buf, idx_t len)
{
  const __m256i mask = _mm256_setr_epi8 (
      1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14,
      1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14
  );

  idx_t i = 0;
  /* 4x unrolled loop: 128 bytes per iteration */
  for (; i + 128 <= len; i += 128)
    {
      __m256i v0 = _mm256_loadu_si256 ((const __m256i *) (buf + i));
      __m256i v1 = _mm256_loadu_si256 ((const __m256i *) (buf + i + 32));
      __m256i v2 = _mm256_loadu_si256 ((const __m256i *) (buf + i + 64));
      __m256i v3 = _mm256_loadu_si256 ((const __m256i *) (buf + i + 96));

      v0 = _mm256_shuffle_epi8 (v0, mask);
      v1 = _mm256_shuffle_epi8 (v1, mask);
      v2 = _mm256_shuffle_epi8 (v2, mask);
      v3 = _mm256_shuffle_epi8 (v3, mask);

      _mm256_storeu_si256 ((__m256i *) (buf + i), v0);
      _mm256_storeu_si256 ((__m256i *) (buf + i + 32), v1);
      _mm256_storeu_si256 ((__m256i *) (buf + i + 64), v2);
      _mm256_storeu_si256 ((__m256i *) (buf + i + 96), v3);
    }

  for (; i + 32 <= len; i += 32)
    {
      __m256i v = _mm256_loadu_si256 ((const __m256i *) (buf + i));
      v = _mm256_shuffle_epi8 (v, mask);
      _mm256_storeu_si256 ((__m256i *) (buf + i), v);
    }

  /* 16-byte tail with SSSE3 */
  if (i + 16 <= len)
    {
      __m128i m128 = _mm_setr_epi8 (1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
      __m128i v = _mm_loadu_si128 ((const __m128i *) (buf + i));
      v = _mm_shuffle_epi8 (v, m128);
      _mm_storeu_si128 ((__m128i *) (buf + i), v);
      i += 16;
    }

  /* Tail scalar swap */
  swab_scalar (buf + i, len - i);
}
#endif

char *
dd_swab_buffer (char *buf, idx_t *nread, int *saved_byte)
{
  if (*nread == 0)
    return buf;

  int prev_saved = *saved_byte;

  /* Fast path: Standard transfer without odd carry-over byte from previous read */
  if (prev_saved < 0)
    {
      if (*nread & 1)
        {
          *saved_byte = (unsigned char) buf[--*nread];
        }
      else
        {
          *saved_byte = -1;
        }

#if defined(__x86_64__) || defined(_M_X64)
      if (__builtin_cpu_supports ("avx2"))
        swab_avx2 (buf, *nread);
      else
        swab_scalar (buf, *nread);
#else
      swab_scalar (buf, *nread);
#endif
      return buf;
    }

  /* Robust fallback path: Carry-over byte from a previous odd block exists */
  if (*nread & 1)
    *saved_byte = -1;
  else
    {
      unsigned char c = buf[--*nread];
      *saved_byte = c;
    }

  for (idx_t i = *nread; 1 < i; i -= 2)
    buf[i] = buf[i - 2];

  buf[1] = (char) prev_saved;
  ++*nread;
  return buf;
}
