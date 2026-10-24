//! Official Known-Good Test Vectors for blks-384
//!
//! Locks in exact deterministic cryptographic outputs for standard inputs,
//! chunk boundaries, and tree transitions.

use blks_core::{hash, BlksHasher, Digest};

struct TestVector {
    name: &'static str,
    input: Vec<u8>,
    expected_b64: &'static str,
}

#[test]
fn test_official_known_good_vectors() {
    let vectors = [
        TestVector {
            name: "empty (0 bytes)",
            input: b"".to_vec(),
            expected_b64: "FZ2NCFBU+4oQxXfNYWX5yShDYw/6uD/LPSp1wDlpXBuxKuNhQNw36+DfWX//Vu6h",
        },
        TestVector {
            name: "single_byte_a (1 byte)",
            input: b"a".to_vec(),
            expected_b64: "LEgdlGC2A2FvOSCT1WeSwk/VjET4PZ1z5t7bmL+7hWrJXy8G1Rmlu9h49gvCk5mI",
        },
        TestVector {
            name: "abc (3 bytes)",
            input: b"abc".to_vec(),
            expected_b64: "48J0Cn0lGfzJhFKUdnbi1wrXF1SOevynMi73ZxF5lleXvoHvcR0bjF8KMAiLlKOH",
        },
        TestVector {
            name: "quick_brown_fox (43 bytes)",
            input: b"The quick brown fox jumps over the lazy dog".to_vec(),
            expected_b64: "fLm4TI9xjaLm7jjLJioInotHz8FrvDh09LZBoyhM3Ogg22uDK84CJDxgCB9D0cE+",
        },
        TestVector {
            name: "block_128 (exact single 128-byte ARX block)",
            input: (0u8..128).collect(),
            expected_b64: "AFHnf/R12Ge2Ca6gFuj/+lSbz+FOfFAHHVqwULCAJI1wJMJx/a8fIBxlqj3qt4dE",
        },
        TestVector {
            name: "chunk_4096 (exact single 4096-byte chunk)",
            input: b"blks".repeat(1024),
            expected_b64: "jTcWxn7hP/ZqvAydsL8mJ10jv1pyZkkmnBKAiyoA7N6MaOcNrdHyneZ2KjsAOH5A",
        },
        TestVector {
            name: "chunks_8192 (exact 2 chunks, Merkle-root parent)",
            input: b"blks".repeat(2048),
            expected_b64: "tJiNKSn8/atYVnOXMno6JCy3FxXN1UTs77vkCk31UZaEWiykksK5KU/oVI8UrHXF",
        },
        TestVector {
            name: "chunks_16384 (exact 4 chunks, multi-level Merkle tree)",
            input: (0..16384).map(|i| (i % 256) as u8).collect(),
            expected_b64: "HdYLa7zvAvi0aSJHp2jPlHPLncpIDPO2eh1wkOoeXvd1gVS9aCTlX4wNcWn7KfbJ",
        },
    ];

    for vec in &vectors {
        // 1. One-shot slice hash verification
        let computed_slice = hash(&vec.input);
        assert_eq!(
            computed_slice.to_base64(),
            vec.expected_b64,
            "Known-Good mismatch (slice) on vector: {}",
            vec.name
        );
        assert_eq!(computed_slice.to_base64().len(), 64);
        assert!(!computed_slice.to_base64().contains('='));

        // 2. Incremental streaming verification (1 byte at a time)
        let mut hasher_byte = BlksHasher::new();
        for &b in &vec.input {
            hasher_byte.update(&[b]);
        }
        let computed_stream_byte = Digest(hasher_byte.finalize());
        assert_eq!(
            computed_stream_byte.to_base64(),
            vec.expected_b64,
            "Known-Good mismatch (1-byte stream) on vector: {}",
            vec.name
        );

        // 3. Incremental streaming verification (chunked)
        let mut hasher_chunked = BlksHasher::new();
        for chunk in vec.input.chunks(73) {
            hasher_chunked.update(chunk);
        }
        let computed_stream_chunked = Digest(hasher_chunked.finalize());
        assert_eq!(
            computed_stream_chunked.to_base64(),
            vec.expected_b64,
            "Known-Good mismatch (chunked stream) on vector: {}",
            vec.name
        );
    }
}
