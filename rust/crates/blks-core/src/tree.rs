//! High-Performance Merkle Tree Engine for blks-384
//!
//! Supports both parallel tree reduction (via rayon) for in-memory slices and memory-mapped files,
//! as well as streaming reduction for sequential I/O (stdin, pipes).

use crate::compress::{
    compress, compress_parent, initial_cv, BLOCK_BYTES, FLAG_CHUNK_END, FLAG_CHUNK_START, FLAG_ROOT,
};
use rayon::prelude::*;
use std::io::{self, Read};

/// Standard chunk size for leaf nodes: 4096 bytes (4 KiB, matching Linux page size)
pub const CHUNK_SIZE: usize = 4096;

/// Subtree chunk count for coarse-grained multithreading: 64 chunks = 256 KiB
pub const SUBTREE_CHUNKS: usize = 64;

/// Subtree size in bytes (256 KiB, matching CPU L2 cache size)
pub const SUBTREE_BYTES: usize = CHUNK_SIZE * SUBTREE_CHUNKS;

/// Hash a single chunk (up to 4096 bytes).
/// If `is_root` is true, this chunk is the entire input and receives FLAG_ROOT.
pub fn hash_chunk(chunk: &[u8], is_root: bool) -> [u8; 48] {
    let len = chunk.len();
    assert!(len <= CHUNK_SIZE);

    let num_blocks = if len == 0 {
        1
    } else {
        len.div_ceil(BLOCK_BYTES)
    };

    let mut cv = initial_cv();
    let mut block = [0u8; BLOCK_BYTES];

    for i in 0..num_blocks {
        let start = i * BLOCK_BYTES;
        let end = (start + BLOCK_BYTES).min(len);
        let block_len = if start < len { end - start } else { 0 };

        block.fill(0);
        if block_len > 0 {
            block[..block_len].copy_from_slice(&chunk[start..end]);
        }

        let mut flags = 0u64;
        if i == 0 {
            flags |= FLAG_CHUNK_START;
        }
        if i == num_blocks - 1 {
            flags |= FLAG_CHUNK_END;
            if is_root {
                flags |= FLAG_ROOT;
            }
        }

        cv = compress(&cv, &block, end as u64, flags);
    }

    let mut out = [0u8; 48];
    for i in 0..6 {
        out[i * 8..(i + 1) * 8].copy_from_slice(&cv[i].to_le_bytes());
    }
    out
}

#[cfg(target_arch = "x86_64")]
fn hash_chunks_accelerated(slice: &[u8]) -> Vec<[u8; 48]> {
    let num_full_chunks = slice.len() / CHUNK_SIZE;
    let mut leaves = Vec::with_capacity(slice.len().div_ceil(CHUNK_SIZE));

    let mut i = 0;
    if is_x86_feature_detected!("avx2") {
        while i + 4 <= num_full_chunks {
            let offset = i * CHUNK_SIZE;
            let c0: &[u8; 4096] = slice[offset..offset + 4096].try_into().unwrap();
            let c1: &[u8; 4096] = slice[offset + 4096..offset + 8192].try_into().unwrap();
            let c2: &[u8; 4096] = slice[offset + 8192..offset + 12288].try_into().unwrap();
            let c3: &[u8; 4096] = slice[offset + 12288..offset + 16384].try_into().unwrap();

            let four_hashes = unsafe { crate::simd_avx2::hash_4_chunks_avx2(c0, c1, c2, c3) };
            leaves.extend_from_slice(&four_hashes);
            i += 4;
        }
    }

    while i < num_full_chunks {
        let offset = i * CHUNK_SIZE;
        leaves.push(hash_chunk(&slice[offset..offset + CHUNK_SIZE], false));
        i += 1;
    }

    let tail_offset = num_full_chunks * CHUNK_SIZE;
    if tail_offset < slice.len() {
        leaves.push(hash_chunk(&slice[tail_offset..], false));
    }

    leaves
}

#[cfg(not(target_arch = "x86_64"))]
fn hash_chunks_accelerated(slice: &[u8]) -> Vec<[u8; 48]> {
    slice
        .chunks(CHUNK_SIZE)
        .map(|chunk| hash_chunk(chunk, false))
        .collect()
}

/// Level-by-level binary Merkle tree reduction.
/// Guarantees bit-exact identical tree structures across multithreaded and streaming paths.
pub fn reduce_nodes(mut current_level: Vec<[u8; 48]>, is_entire_tree_root: bool) -> [u8; 48] {
    assert!(!current_level.is_empty(), "cannot reduce empty tree");
    if current_level.len() == 1 {
        return current_level[0];
    }

    while current_level.len() > 1 {
        let is_root_level = is_entire_tree_root && (current_level.len() == 2);
        let has_orphan = current_level.len() % 2 == 1;
        let orphan = if has_orphan {
            current_level.pop()
        } else {
            None
        };

        // For large trees (>= 512 nodes per level), parallelize the layer reduction with Rayon
        let mut next_level: Vec<[u8; 48]> = if current_level.len() >= 512 {
            current_level
                .par_chunks_exact(2)
                .map(|pair| compress_parent(&pair[0], &pair[1], is_root_level))
                .collect()
        } else {
            let (pairs, _) = current_level.as_chunks::<2>();
            pairs
                .iter()
                .map(|pair| compress_parent(&pair[0], &pair[1], is_root_level))
                .collect()
        };

        if let Some(orph) = orphan {
            next_level.push(orph);
        }
        current_level = next_level;
    }

    current_level[0]
}

/// Compute blks-384 hash over an in-memory slice using Rayon multithreading and AVX2 vectorization.
pub fn hash_slice_parallel(data: &[u8]) -> [u8; 48] {
    if data.len() <= CHUNK_SIZE {
        return hash_chunk(data, true);
    }

    // For inputs >= 2 subtrees (>= 512 KiB), use coarse-grained 256 KiB slicing:
    // Each thread processes 64 chunks in its local L2 cache and reduces its own subtree,
    // reducing Rayon scheduling and allocation overhead by a factor of 64.
    if data.len() >= SUBTREE_BYTES * 2 {
        let subroots: Vec<[u8; 48]> = data
            .par_chunks(SUBTREE_BYTES)
            .map(|slice| {
                let leaves = hash_chunks_accelerated(slice);
                reduce_nodes(leaves, false)
            })
            .collect();

        reduce_nodes(subroots, true)
    } else {
        let leaves = hash_chunks_accelerated(data);
        reduce_nodes(leaves, true)
    }
}

/// Incremental streaming hasher for blks-384 (e.g. stdin or network streams).
/// Produces bit-exact identical hashes to `hash_slice_parallel`.
pub struct BlksHasher {
    buffer: Vec<u8>,
    leaf_hashes: Vec<[u8; 48]>,
    total_bytes: u64,
}

impl Default for BlksHasher {
    fn default() -> Self {
        Self::new()
    }
}

impl BlksHasher {
    /// Creates a new streaming hasher with an empty internal chunk buffer and leaf stack.
    pub fn new() -> Self {
        Self {
            buffer: Vec::with_capacity(CHUNK_SIZE),
            leaf_hashes: Vec::new(),
            total_bytes: 0,
        }
    }

    /// Feeds a slice of data into the incremental hasher.
    ///
    /// Chunks are buffered in memory up to 4 KiB and hashed automatically as soon as a full
    /// chunk boundary is reached and further data arrives.
    /// Automatically utilizes 4-Way AVX2 vector instructions for multi-chunk buffers.
    pub fn update(&mut self, mut input: &[u8]) {
        self.total_bytes += input.len() as u64;

        // 1. If buffer has a pending complete chunk and more data arrives, hash it
        if self.buffer.len() == CHUNK_SIZE && !input.is_empty() {
            let leaf_hash = hash_chunk(&self.buffer, false);
            self.leaf_hashes.push(leaf_hash);
            self.buffer.clear();
        }

        // 2. If buffer has a partial chunk, fill it up first
        if !self.buffer.is_empty() {
            let available = CHUNK_SIZE - self.buffer.len();
            let to_take = available.min(input.len());
            self.buffer.extend_from_slice(&input[..to_take]);
            input = &input[to_take..];

            if self.buffer.len() == CHUNK_SIZE && !input.is_empty() {
                let leaf_hash = hash_chunk(&self.buffer, false);
                self.leaf_hashes.push(leaf_hash);
                self.buffer.clear();
            } else {
                return;
            }
        }

        // 3. Buffer is now empty: Bulk-process full chunks directly from input with AVX2
        if input.len() > CHUNK_SIZE {
            let hashable_len = if input.len().is_multiple_of(CHUNK_SIZE) {
                input.len() - CHUNK_SIZE
            } else {
                (input.len() / CHUNK_SIZE) * CHUNK_SIZE
            };

            if hashable_len > 0 {
                let leaves = hash_chunks_accelerated(&input[..hashable_len]);
                self.leaf_hashes.extend_from_slice(&leaves);
                input = &input[hashable_len..];
            }
        }

        // 4. Retain any trailing bytes (<= 4096) in the buffer
        if !input.is_empty() {
            self.buffer.extend_from_slice(input);
        }
    }

    /// Finalizes the stream and returns the 384-bit (48-byte) Merkle root hash.
    ///
    /// Consumes the hasher and guarantees bit-exact identical output to one-shot parallel slice hashing.
    pub fn finalize(mut self) -> [u8; 48] {
        if self.leaf_hashes.is_empty() {
            // Entire input fits in single chunk buffer (<= 4096 bytes)
            return hash_chunk(&self.buffer, true);
        }

        if !self.buffer.is_empty() {
            let final_leaf = hash_chunk(&self.buffer, false);
            self.leaf_hashes.push(final_leaf);
        }

        reduce_nodes(self.leaf_hashes, true)
    }
}

/// Helper function to hash an `io::Read` stream to completion
pub fn hash_reader<R: Read>(mut reader: R) -> io::Result<[u8; 48]> {
    let mut hasher = BlksHasher::new();
    let mut buf = vec![0u8; 128 * 1024]; // 128 KiB I/O buffer for high-throughput pipe reads
    loop {
        let n = reader.read(&mut buf)?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(hasher.finalize())
}
