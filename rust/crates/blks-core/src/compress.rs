//! 64-Bit ARX Compression Engine for blks (384-Bit Tree-Hash)
//!
//! Based on the 64-bit BLAKE2b ARX permutation with 12 full cryptographic rounds,
//! personalized for blks-384, operating on 128-byte message blocks and 512-bit
//! internal states truncated to 384-bit (48-byte) chaining values.

/// Message block size for the 64-bit ARX compression engine in bytes (16 x 64-bit words)
pub const BLOCK_BYTES: usize = 128;

/// Output digest size in bytes (384 bits = 48 bytes)
pub const DIGEST_BYTES: usize = 48;

/// Number of 64-bit words in the internal chaining value (512-bit state)
pub const CV_WORDS: usize = 8;

/// Flag indicating the first block of a chunk
pub const FLAG_CHUNK_START: u64 = 1 << 0;

/// Flag indicating the final block of a chunk
pub const FLAG_CHUNK_END: u64 = 1 << 1;

/// Flag indicating a parent node merging two child hashes
pub const FLAG_PARENT: u64 = 1 << 2;

/// Flag indicating the final root node of the Merkle tree
pub const FLAG_ROOT: u64 = 1 << 3;

/// Standard 64-bit initialization vectors (fractional parts of square roots of first 8 primes)
pub const IV: [u64; 8] = [
    0x6a09e667f3bcc908,
    0xbb67ae8584caa73b,
    0x3c6ef372fe94f82b,
    0xa54ff53a5f1d36f1,
    0x510e527fade682d1,
    0x9b05688c2b3e6c1f,
    0x1f83d9abfb41bd6b,
    0x5be0cd19137e2179,
];

/// blks-384 personalization constants: "BLKS_384"
pub const PERSONALIZATION: [u8; 8] = *b"BLKS_384";

/// Compute the initial chaining value for blks-384.
/// Parameter block: digest_len = 48, key_len = 0, fanout = 2, max_depth = 255.
pub fn initial_cv() -> [u64; 8] {
    let mut cv = IV;
    // Parameter word 0: digest_len (48 = 0x30), fanout=2, depth=255
    let p0: u64 = 0x30 | (2 << 16) | (255 << 24);
    cv[0] ^= p0;
    // Personalization word in cv[6]
    let pers = u64::from_le_bytes(PERSONALIZATION);
    cv[6] ^= pers;
    cv
}

/// BLAKE2b Sigma permutations for 12 rounds
pub const SIGMA: [[usize; 16]; 12] = [
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
    [14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3],
    [11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4],
    [7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8],
    [9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13],
    [2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9],
    [12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11],
    [13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10],
    [6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5],
    [10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0],
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
    [14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3],
];

#[inline(always)]
fn g(v: &mut [u64; 16], a: usize, b: usize, c: usize, d: usize, x: u64, y: u64) {
    v[a] = v[a].wrapping_add(v[b]).wrapping_add(x);
    v[d] = (v[d] ^ v[a]).rotate_right(32);
    v[c] = v[c].wrapping_add(v[d]);
    v[b] = (v[b] ^ v[c]).rotate_right(24);
    v[a] = v[a].wrapping_add(v[b]).wrapping_add(y);
    v[d] = (v[d] ^ v[a]).rotate_right(16);
    v[c] = v[c].wrapping_add(v[d]);
    v[b] = (v[b] ^ v[c]).rotate_right(63);
}

/// Core ARX compression function
#[inline(always)]
pub fn compress(
    cv: &[u64; 8],
    block: &[u8; BLOCK_BYTES],
    byte_offset: u64,
    flags: u64,
) -> [u64; 8] {
    let mut m = [0u64; 16];
    let (chunks, _) = block.as_chunks::<8>();
    for (chunk, slot) in chunks.iter().zip(m.iter_mut()) {
        *slot = u64::from_le_bytes(*chunk);
    }

    let mut v = [0u64; 16];
    v[0..8].copy_from_slice(cv);
    v[8..16].copy_from_slice(&IV);

    // Feed counter and domain flags
    v[12] ^= byte_offset;
    v[13] ^= 0; // high word of 128-bit counter
    v[14] ^= flags;

    macro_rules! round {
        ($r:expr) => {
            g(&mut v, 0, 4, 8, 12, m[SIGMA[$r][0]], m[SIGMA[$r][1]]);
            g(&mut v, 1, 5, 9, 13, m[SIGMA[$r][2]], m[SIGMA[$r][3]]);
            g(&mut v, 2, 6, 10, 14, m[SIGMA[$r][4]], m[SIGMA[$r][5]]);
            g(&mut v, 3, 7, 11, 15, m[SIGMA[$r][6]], m[SIGMA[$r][7]]);

            g(&mut v, 0, 5, 10, 15, m[SIGMA[$r][8]], m[SIGMA[$r][9]]);
            g(&mut v, 1, 6, 11, 12, m[SIGMA[$r][10]], m[SIGMA[$r][11]]);
            g(&mut v, 2, 7, 8, 13, m[SIGMA[$r][12]], m[SIGMA[$r][13]]);
            g(&mut v, 3, 4, 9, 14, m[SIGMA[$r][14]], m[SIGMA[$r][15]]);
        };
    }

    round!(0);
    round!(1);
    round!(2);
    round!(3);
    round!(4);
    round!(5);
    round!(6);
    round!(7);
    round!(8);
    round!(9);
    round!(10);
    round!(11);

    let mut out_cv = [0u64; 8];
    for i in 0..8 {
        out_cv[i] = cv[i] ^ v[i] ^ v[i + 8];
    }
    out_cv
}

/// Compress a parent node combining two 48-byte (384-bit) child hashes.
/// Input: left (48 bytes) and right (48 bytes) = 96 bytes.
/// Fits cleanly into the 128-byte block without overflow!
#[inline]
pub fn compress_parent(left: &[u8; 48], right: &[u8; 48], is_root: bool) -> [u8; 48] {
    let mut block = [0u8; BLOCK_BYTES];
    block[0..48].copy_from_slice(left);
    block[48..96].copy_from_slice(right);
    // Remaining 32 bytes (96..128) are zeroed padding.

    let flags = FLAG_PARENT | if is_root { FLAG_ROOT } else { 0 };
    let parent_cv = initial_cv();
    let out_cv = compress(&parent_cv, &block, 96, flags);

    let mut out = [0u8; 48];
    for i in 0..6 {
        out[i * 8..(i + 1) * 8].copy_from_slice(&out_cv[i].to_le_bytes());
    }
    out
}
