#[inline]
pub fn swab_buffer(buf: &mut [u8]) {
    #[cfg(target_arch = "x86_64")]
    {
        if is_x86_feature_detected!("avx2") {
            // SAFETY: `is_x86_feature_detected!("avx2")` dynamically guarantees AVX2 hardware support at runtime.
            unsafe {
                swab_avx2(buf);
                return;
            }
        }
    }

    swab_scalar(buf);
}

#[inline]
pub fn swab_scalar(buf: &mut [u8]) {
    let (chunks, _) = buf.as_chunks_mut::<2>();
    for pair in chunks {
        pair.swap(0, 1);
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn swab_avx2(buf: &mut [u8]) {
    use std::arch::x86_64::*;

    #[rustfmt::skip]
    let mask = _mm256_setr_epi8(
        1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14,
        1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14,
    );

    let (chunks_32, remainder) = buf.as_chunks_mut::<32>();
    for chunk in chunks_32 {
        // SAFETY: `chunk` has an exact length of 32 bytes guaranteed by `as_chunks_mut::<32>()`.
        // `_mm256_loadu_si256` and `_mm256_storeu_si256` are unaligned memory safe.
        let v = _mm256_loadu_si256(chunk.as_ptr() as *const __m256i);
        let swapped = _mm256_shuffle_epi8(v, mask);
        _mm256_storeu_si256(chunk.as_mut_ptr() as *mut __m256i, swapped);
    }

    swab_scalar(remainder);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_swab_even_length() {
        let mut data = *b"123456";
        swab_buffer(&mut data);
        assert_eq!(&data, b"214365");
    }

    #[test]
    fn test_swab_odd_length() {
        let mut data = *b"12345";
        swab_buffer(&mut data);
        assert_eq!(&data, b"21435");
    }
}
