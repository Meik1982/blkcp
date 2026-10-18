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
