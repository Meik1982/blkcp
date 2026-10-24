//! 4-Way AVX2 Parallel ARX Engine for blks-384
//!
//! Interleaves 4 independent 4 KiB chunks across 256-bit AVX2 vector registers,
//! processing 4 G-functions in lockstep in a single CPU core.

#[cfg(target_arch = "x86_64")]
use std::arch::x86_64::*;

use crate::compress::{BLOCK_BYTES, FLAG_CHUNK_END, FLAG_CHUNK_START, IV};

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
#[inline]
unsafe fn rotr64_32(x: __m256i) -> __m256i {
    // Rotating 64-bit words by 32 is a 32-bit swap of dwords in each qword:
    // _MM_SHUFFLE(2, 3, 0, 1) = 0xB1
    _mm256_shuffle_epi32(x, 0xB1)
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
#[inline]
unsafe fn rotr64_24(x: __m256i) -> __m256i {
    let mask = _mm256_setr_epi8(
        3, 4, 5, 6, 7, 0, 1, 2, 11, 12, 13, 14, 15, 8, 9, 10, 3, 4, 5, 6, 7, 0, 1, 2, 11, 12, 13,
        14, 15, 8, 9, 10,
    );
    _mm256_shuffle_epi8(x, mask)
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
#[inline]
unsafe fn rotr64_16(x: __m256i) -> __m256i {
    let mask = _mm256_setr_epi8(
        2, 3, 4, 5, 6, 7, 0, 1, 10, 11, 12, 13, 14, 15, 8, 9, 2, 3, 4, 5, 6, 7, 0, 1, 10, 11, 12,
        13, 14, 15, 8, 9,
    );
    _mm256_shuffle_epi8(x, mask)
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
#[inline]
unsafe fn rotr64_63(x: __m256i) -> __m256i {
    // Rotate right 63 is rotate left 1
    _mm256_or_si256(_mm256_slli_epi64(x, 1), _mm256_srli_epi64(x, 63))
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
#[inline]
unsafe fn g_avx2(
    v: &mut [__m256i; 16],
    a: usize,
    b: usize,
    c: usize,
    d: usize,
    x: __m256i,
    y: __m256i,
) {
    // SAFETY: Caller guarantees AVX2 target feature is active.
    unsafe {
        v[a] = _mm256_add_epi64(_mm256_add_epi64(v[a], v[b]), x);
        v[d] = rotr64_32(_mm256_xor_si256(v[d], v[a]));
        v[c] = _mm256_add_epi64(v[c], v[d]);
        v[b] = rotr64_24(_mm256_xor_si256(v[b], v[c]));
        v[a] = _mm256_add_epi64(_mm256_add_epi64(v[a], v[b]), y);
        v[d] = rotr64_16(_mm256_xor_si256(v[d], v[a]));
        v[c] = _mm256_add_epi64(v[c], v[d]);
        v[b] = rotr64_63(_mm256_xor_si256(v[b], v[c]));
    }
}

/// Hashes 4 full 4096-byte chunks simultaneously using AVX2 vector instructions.
///
/// # Safety
/// Caller must ensure AVX2 target feature is supported on the CPU.
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
pub unsafe fn hash_4_chunks_avx2(
    c0: &[u8; 4096],
    c1: &[u8; 4096],
    c2: &[u8; 4096],
    c3: &[u8; 4096],
) -> [[u8; 48]; 4] {
    let init = crate::compress::initial_cv();
    let mut cv = [
        _mm256_set1_epi64x(init[0] as i64),
        _mm256_set1_epi64x(init[1] as i64),
        _mm256_set1_epi64x(init[2] as i64),
        _mm256_set1_epi64x(init[3] as i64),
        _mm256_set1_epi64x(init[4] as i64),
        _mm256_set1_epi64x(init[5] as i64),
        _mm256_set1_epi64x(init[6] as i64),
        _mm256_set1_epi64x(init[7] as i64),
    ];

    let iv_vec = [
        _mm256_set1_epi64x(IV[0] as i64),
        _mm256_set1_epi64x(IV[1] as i64),
        _mm256_set1_epi64x(IV[2] as i64),
        _mm256_set1_epi64x(IV[3] as i64),
        _mm256_set1_epi64x(IV[4] as i64),
        _mm256_set1_epi64x(IV[5] as i64),
        _mm256_set1_epi64x(IV[6] as i64),
        _mm256_set1_epi64x(IV[7] as i64),
    ];

    let (c0_words, _) = c0.as_chunks::<8>();
    let (c1_words, _) = c1.as_chunks::<8>();
    let (c2_words, _) = c2.as_chunks::<8>();
    let (c3_words, _) = c3.as_chunks::<8>();

    // 4096 bytes / 128 bytes = 32 blocks per chunk
    for blk_idx in 0..32 {
        let word_base = blk_idx * 16;
        let mut m = [_mm256_setzero_si256(); 16];

        for (w, slot) in m.iter_mut().enumerate() {
            let idx = word_base + w;
            let val0 = u64::from_le_bytes(c0_words[idx]);
            let val1 = u64::from_le_bytes(c1_words[idx]);
            let val2 = u64::from_le_bytes(c2_words[idx]);
            let val3 = u64::from_le_bytes(c3_words[idx]);
            *slot = _mm256_set_epi64x(val3 as i64, val2 as i64, val1 as i64, val0 as i64);
        }

        let mut v = [
            cv[0], cv[1], cv[2], cv[3], cv[4], cv[5], cv[6], cv[7], iv_vec[0], iv_vec[1],
            iv_vec[2], iv_vec[3], iv_vec[4], iv_vec[5], iv_vec[6], iv_vec[7],
        ];

        let mut flags = 0u64;
        if blk_idx == 0 {
            flags |= FLAG_CHUNK_START;
        }
        if blk_idx == 31 {
            flags |= FLAG_CHUNK_END;
        }

        let end_bytes = (blk_idx + 1) * BLOCK_BYTES;
        v[12] = _mm256_xor_si256(v[12], _mm256_set1_epi64x(end_bytes as i64));
        v[14] = _mm256_xor_si256(v[14], _mm256_set1_epi64x(flags as i64));

        macro_rules! round_avx2 {
            ($r:expr) => {
                g_avx2(
                    &mut v,
                    0,
                    4,
                    8,
                    12,
                    m[crate::compress::SIGMA[$r][0]],
                    m[crate::compress::SIGMA[$r][1]],
                );
                g_avx2(
                    &mut v,
                    1,
                    5,
                    9,
                    13,
                    m[crate::compress::SIGMA[$r][2]],
                    m[crate::compress::SIGMA[$r][3]],
                );
                g_avx2(
                    &mut v,
                    2,
                    6,
                    10,
                    14,
                    m[crate::compress::SIGMA[$r][4]],
                    m[crate::compress::SIGMA[$r][5]],
                );
                g_avx2(
                    &mut v,
                    3,
                    7,
                    11,
                    15,
                    m[crate::compress::SIGMA[$r][6]],
                    m[crate::compress::SIGMA[$r][7]],
                );

                g_avx2(
                    &mut v,
                    0,
                    5,
                    10,
                    15,
                    m[crate::compress::SIGMA[$r][8]],
                    m[crate::compress::SIGMA[$r][9]],
                );
                g_avx2(
                    &mut v,
                    1,
                    6,
                    11,
                    12,
                    m[crate::compress::SIGMA[$r][10]],
                    m[crate::compress::SIGMA[$r][11]],
                );
                g_avx2(
                    &mut v,
                    2,
                    7,
                    8,
                    13,
                    m[crate::compress::SIGMA[$r][12]],
                    m[crate::compress::SIGMA[$r][13]],
                );
                g_avx2(
                    &mut v,
                    3,
                    4,
                    9,
                    14,
                    m[crate::compress::SIGMA[$r][14]],
                    m[crate::compress::SIGMA[$r][15]],
                );
            };
        }

        // SAFETY: `hash_4_chunks_avx2` is compiled with `#[target_feature(enable = "avx2")]`.
        unsafe {
            round_avx2!(0);
            round_avx2!(1);
            round_avx2!(2);
            round_avx2!(3);
            round_avx2!(4);
            round_avx2!(5);
            round_avx2!(6);
            round_avx2!(7);
            round_avx2!(8);
            round_avx2!(9);
            round_avx2!(10);
            round_avx2!(11);
        }

        for i in 0..8 {
            cv[i] = _mm256_xor_si256(cv[i], _mm256_xor_si256(v[i], v[i + 8]));
        }
    }

    let mut out = [[0u8; 48]; 4];
    for (w, &vec_word) in cv.iter().take(6).enumerate() {
        let lanes = [
            _mm256_extract_epi64(vec_word, 0) as u64,
            _mm256_extract_epi64(vec_word, 1) as u64,
            _mm256_extract_epi64(vec_word, 2) as u64,
            _mm256_extract_epi64(vec_word, 3) as u64,
        ];
        for (lane, &word) in lanes.iter().enumerate() {
            out[lane][w * 8..(w + 1) * 8].copy_from_slice(&word.to_le_bytes());
        }
    }
    out
}
