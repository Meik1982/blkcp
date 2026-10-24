/// Swaps adjacent bytes in `buf` (16-bit endianness conversion).
///
/// Implemented entirely in Safe Rust using `as_chunks_mut::<2>()`.
/// Modern LLVM auto-vectorizes this loop directly into SIMD byte-shuffle
/// instructions (`vpshufb` on x86_64 AVX2/SSSE3, `tbl`/`rev16` on ARM64 Neon)
/// achieving identical hardware throughput without requiring unsafe intrinsic blocks.
#[inline]
pub fn swab_buffer(buf: &mut [u8]) {
    let (chunks, _) = buf.as_chunks_mut::<2>();
    for pair in chunks {
        pair.swap(0, 1);
    }
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
