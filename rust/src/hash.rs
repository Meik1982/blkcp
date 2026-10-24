use crate::args::HashAlgo;
use sha2::Digest;

pub enum InFlightHasher {
    None,
    Sha256(sha2::Sha256),
    Blake3(Box<blake3::Hasher>),
    Blks(blks_core::BlksHasher),
}

impl InFlightHasher {
    pub fn new(algo: HashAlgo) -> Self {
        match algo {
            HashAlgo::None => InFlightHasher::None,
            HashAlgo::Sha256 => InFlightHasher::Sha256(sha2::Sha256::new()),
            HashAlgo::Blake3 => InFlightHasher::Blake3(Box::new(blake3::Hasher::new())),
            HashAlgo::Blks => InFlightHasher::Blks(blks_core::BlksHasher::new()),
        }
    }

    #[inline]
    pub fn update(&mut self, chunk: &[u8]) {
        match self {
            InFlightHasher::None => {}
            InFlightHasher::Sha256(h) => h.update(chunk),
            InFlightHasher::Blake3(h) => {
                h.update(chunk);
            }
            InFlightHasher::Blks(h) => h.update(chunk),
        }
    }

    pub fn finalize_string(self) -> Option<(String, &'static str)> {
        match self {
            InFlightHasher::None => None,
            InFlightHasher::Sha256(h) => {
                let res = h.finalize();
                let mut s = String::with_capacity(64);
                for b in res {
                    s.push_str(&format!("{:02x}", b));
                }
                Some((s, "sha256"))
            }
            InFlightHasher::Blake3(h) => {
                let res = h.finalize();
                Some((res.to_hex().to_string(), "blake3"))
            }
            InFlightHasher::Blks(h) => {
                let hash = h.finalize();
                Some((blks_core::encode_base64(&hash), "blks"))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use blks_core::tree::{hash_chunk, reduce_nodes, CHUNK_SIZE, SUBTREE_BYTES};
    use blks_core::BlksHasher;

    #[test]
    fn test_blks_chunk_merkle_combiner() {
        let total_size = 16 * 1024 * 1024; // 16 MB = 64 subtrees (256 KB each)
        let data: Vec<u8> = (0..total_size).map(|i| (i ^ (i >> 8)) as u8).collect();

        // 1. Canonical Sequential Hash
        let mut seq_hasher = BlksHasher::new();
        seq_hasher.update(&data);
        let expected_hash = seq_hasher.finalize();

        // 2. Parallel Inter-Chunk Hashing (e.g. 4 chunks of 4 MB each)
        let num_chunks = 4;
        let chunk_size = 4 * 1024 * 1024;
        let mut chunk_subroots: Vec<Vec<[u8; 48]>> = vec![Vec::new(); num_chunks];

        for (chunk_idx, chunk_data) in data.chunks(chunk_size).enumerate() {
            // Each worker computes its subroots for its 256 KB slices
            let mut subroots = Vec::new();
            for slice in chunk_data.chunks(SUBTREE_BYTES) {
                let leaves: Vec<[u8; 48]> = slice
                    .chunks(CHUNK_SIZE)
                    .map(|c| hash_chunk(c, false))
                    .collect();
                subroots.push(reduce_nodes(leaves, false));
            }
            chunk_subroots[chunk_idx] = subroots;
        }

        // Merkle Combiner: Flatten subroots in deterministic chunk order
        let all_subroots: Vec<[u8; 48]> = chunk_subroots.into_iter().flatten().collect();
        let combined_hash = reduce_nodes(all_subroots, true);

        assert_eq!(
            expected_hash, combined_hash,
            "Inter-chunk Merkle combiner failed!"
        );
    }
}
