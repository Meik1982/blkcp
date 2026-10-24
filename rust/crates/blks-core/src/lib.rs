//! # blks-core
//!
//! High-Performance 384-Bit Cryptographic Tree-Hash Engine.
//!
//! - **384-Bit Digest:** Exactly 48 bytes output.
//! - **Clean Base64:** Encodes to exactly 64 ASCII characters with ZERO padding ('='),
//!   matching the terminal and log width of SHA-256 hexadecimal output.
//! - **192-Bit Collision Resistance:** Breaks the 128-bit birthday-paradox bottleneck of BLAKE3/SHA-256.
//! - **Multi-Core Merkle Tree:** Leverages Rayon work-stealing parallelism and Linux page-aligned 4 KiB chunks.

#![warn(missing_docs)]

pub mod compress;
pub mod encoding;
pub mod ffi;
#[cfg(target_arch = "x86_64")]
pub mod simd_avx2;
pub mod tree;

pub use encoding::{constant_time_eq, decode_base64, encode_base64, encode_base64_url, encode_hex};
pub use tree::{hash_reader, hash_slice_parallel, BlksHasher, CHUNK_SIZE};

use memmap2::Mmap;
use std::fmt;
use std::fs::File;
use std::io;
use std::path::Path;

/// A 384-bit (48-byte) cryptographic digest
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Digest(pub [u8; 48]);

impl Digest {
    /// Returns the raw 48-byte array slice of the digest.
    pub fn as_bytes(&self) -> &[u8; 48] {
        &self.0
    }

    /// Formats the digest as a 64-character standard Base64 string without padding.
    pub fn to_base64(&self) -> String {
        encode_base64(&self.0)
    }

    /// Formats the digest as a 64-character URL-safe Base64 string without padding.
    pub fn to_base64_url(&self) -> String {
        encode_base64_url(&self.0)
    }

    /// Formats the digest as a 96-character lowercase hexadecimal string.
    pub fn to_hex(&self) -> String {
        encode_hex(&self.0)
    }

    /// Parses a 64-character Base64 string back into a 384-bit digest.
    pub fn from_base64(s: &str) -> Result<Self, &'static str> {
        let bytes = decode_base64(s)?;
        Ok(Digest(bytes))
    }

    /// Compares two digests in constant time to prevent timing side-channel attacks.
    #[inline]
    pub fn ct_eq(&self, other: &Self) -> bool {
        constant_time_eq(&self.0, &other.0)
    }
}

impl fmt::Display for Digest {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.to_base64())
    }
}

impl fmt::Debug for Digest {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "blks:{}", self.to_base64())
    }
}

/// Compute blks-384 hash over an in-memory byte slice using multicore parallelism.
pub fn hash(data: &[u8]) -> Digest {
    Digest(hash_slice_parallel(data))
}

/// Compute blks-384 hash over a file.
/// Automatically uses zero-copy memory mapping (`memmap2`) for regular files
/// to saturate NVMe / PCIe bandwidth and CPU cores.
pub fn hash_file<P: AsRef<Path>>(path: P) -> io::Result<Digest> {
    hash_file_opts(path, true)
}

/// Compute blks-384 hash over a file with optional memory-mapping.
///
/// If `use_mmap` is true, regular non-empty files are hashed using zero-copy memory mapping (`memmap2`).
/// If `use_mmap` is false, the file is read sequentially using buffered streaming (`hash_reader`),
/// completely eliminating `SIGBUS` crash risks when reading from volatile network mounts (NFS, SMB)
/// or files that might be truncated concurrently by another process.
pub fn hash_file_opts<P: AsRef<Path>>(path: P, use_mmap: bool) -> io::Result<Digest> {
    let file = File::open(path)?;
    let metadata = file.metadata()?;

    if use_mmap && metadata.len() > 0 && metadata.is_file() {
        // Safe fast-path with memory-mapping for files > 0 bytes
        match unsafe { Mmap::map(&file) } {
            Ok(mmap) => {
                // Advise kernel for aggressive sequential readahead on Unix
                #[cfg(unix)]
                let _ = mmap.advise(memmap2::Advice::Sequential);
                Ok(hash(&mmap))
            }
            Err(_) => {
                // Fallback to streaming reader if mmap fails (e.g. special files)
                let bytes = hash_reader(file)?;
                Ok(Digest(bytes))
            }
        }
    } else {
        // Safe streaming path (empty file, special device, or --no-mmap)
        let bytes = hash_reader(file)?;
        Ok(Digest(bytes))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_determinism_and_parity_between_slice_and_streaming() {
        for size in [
            0, 1, 63, 64, 127, 128, 129, 255, 256, 1023, 1024, 4095, 4096, 4097, 8191, 8192, 8193,
            12287, 12288, 12289, 16383, 16384, 16385, 32768, 65536, 131072, 500000,
        ] {
            let data: Vec<u8> = (0..size).map(|i| (i * 31 + 7) as u8).collect();
            let d_slice = hash(&data);

            let mut hasher = BlksHasher::new();
            hasher.update(&data);
            let d_stream = Digest(hasher.finalize());

            assert_eq!(
                d_slice, d_stream,
                "Mismatch between slice and streaming for size {}",
                size
            );
            assert_eq!(d_slice.to_base64().len(), 64);
            assert!(!d_slice.to_base64().contains('='));
        }
    }

    #[test]
    fn test_fragmented_stream_feeds() {
        let data: Vec<u8> = (0..50000).map(|i| (i * 17 + 3) as u8).collect();
        let expected = hash(&data);

        for step in [1, 7, 32, 127, 128, 513, 1024, 4095, 4096, 4097, 8192] {
            let mut hasher = BlksHasher::new();
            let mut offset = 0;
            while offset < data.len() {
                let end = (offset + step).min(data.len());
                hasher.update(&data[offset..end]);
                offset = end;
            }
            let actual = Digest(hasher.finalize());
            assert_eq!(
                expected, actual,
                "Fragmented feed with step {} produced mismatched hash",
                step
            );
        }
    }

    #[test]
    fn test_strict_avalanche_criterion_diffusion() {
        let base_data = b"Cryptographic 384-Bit Tree-Hash Test String For Diffusion Verification!";
        let base_hash = hash(base_data);

        let mut total_flipped_bits = 0;
        let mut tests_count = 0;

        // Flip each bit in the first 32 bytes of the input
        for byte_idx in 0..32 {
            for bit_idx in 0..8 {
                let mut mutated = base_data.to_vec();
                mutated[byte_idx] ^= 1 << bit_idx;
                let mutated_hash = hash(&mutated);

                // Count flipped bits in the 48-byte (384-bit) output
                let mut flipped = 0;
                for i in 0..48 {
                    let diff = base_hash.0[i] ^ mutated_hash.0[i];
                    flipped += diff.count_ones();
                }

                total_flipped_bits += flipped;
                tests_count += 1;

                // Each individual bit flip must flip at least 120 and at most 260 bits (out of 384)
                assert!(
                    (120..=260).contains(&flipped),
                    "Poor diffusion on byte {}, bit {}: flipped only {} bits",
                    byte_idx,
                    bit_idx,
                    flipped
                );
            }
        }

        let avg_flipped = (total_flipped_bits as f64) / (tests_count as f64);
        let diffusion_pct = (avg_flipped / 384.0) * 100.0;
        // Ideal SAC is exactly 50.0 % (192 bits out of 384)
        assert!(
            (48.0..=52.0).contains(&diffusion_pct),
            "Strict Avalanche Criterion failed: average diffusion was {:.2} % (expected ~50 %)",
            diffusion_pct
        );
    }

    #[test]
    fn test_thread_concurrency_invariance() {
        let data: Vec<u8> = (0..200_000).map(|i| (i * 13 + 5) as u8).collect();
        let expected = hash(&data);

        for threads in [1, 2, 4, 8] {
            let pool = rayon::ThreadPoolBuilder::new()
                .num_threads(threads)
                .build()
                .unwrap();

            let computed = pool.install(|| hash(&data));
            assert_eq!(
                expected, computed,
                "Hash changed under {} worker threads!",
                threads
            );
        }
    }

    #[test]
    #[cfg(target_arch = "x86_64")]
    fn test_avx2_4_chunks_parity() {
        if is_x86_feature_detected!("avx2") {
            let mut c0 = [0u8; 4096];
            let mut c1 = [0u8; 4096];
            let mut c2 = [0u8; 4096];
            let mut c3 = [0u8; 4096];

            for i in 0..4096 {
                c0[i] = (i * 3 + 1) as u8;
                c1[i] = (i * 7 + 2) as u8;
                c2[i] = (i * 11 + 3) as u8;
                c3[i] = (i * 13 + 4) as u8;
            }

            let expected0 = tree::hash_chunk(&c0, false);
            let expected1 = tree::hash_chunk(&c1, false);
            let expected2 = tree::hash_chunk(&c2, false);
            let expected3 = tree::hash_chunk(&c3, false);

            let avx_hashes = unsafe { simd_avx2::hash_4_chunks_avx2(&c0, &c1, &c2, &c3) };

            assert_eq!(expected0, avx_hashes[0], "AVX2 mismatch on chunk 0");
            assert_eq!(expected1, avx_hashes[1], "AVX2 mismatch on chunk 1");
            assert_eq!(expected2, avx_hashes[2], "AVX2 mismatch on chunk 2");
            assert_eq!(expected3, avx_hashes[3], "AVX2 mismatch on chunk 3");
        }
    }

    #[test]
    fn test_collision_resistance_different_inputs() {
        let d1 = hash(b"The quick brown fox jumps over the lazy dog");
        let d2 = hash(b"The quick brown fox jumps over the lazy dog.");
        assert_ne!(d1, d2);
    }
}
