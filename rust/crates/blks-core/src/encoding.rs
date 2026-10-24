//! Zero-dependency Base64 and Hex encoding for 384-bit (48-byte) digests.
//!
//! Because 48 bytes is an exact multiple of 3 (48 / 3 = 16),
//! Base64 produces exactly 64 ASCII characters with ZERO padding ('=').

const BASE64_STANDARD: &[u8; 64] =
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

const BASE64_URL_SAFE: &[u8; 64] =
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

const HEX_CHARS: &[u8; 16] = b"0123456789abcdef";

/// Compile-time precomputed reverse lookup table for Base64 (supporting both standard and URL-safe)
const fn build_rev_table() -> [u8; 256] {
    let mut table = [255u8; 256];
    let mut i = 0;
    while i < 64 {
        table[BASE64_STANDARD[i] as usize] = i as u8;
        table[BASE64_URL_SAFE[i] as usize] = i as u8;
        i += 1;
    }
    table
}

const REV_TABLE: [u8; 256] = build_rev_table();

/// Encode a 48-byte digest into exactly 64 standard Base64 characters.
#[inline]
pub fn encode_base64(bytes: &[u8; 48]) -> String {
    encode_b64_table(bytes, BASE64_STANDARD)
}

/// Encode a 48-byte digest into exactly 64 URL-safe Base64 characters.
#[inline]
pub fn encode_base64_url(bytes: &[u8; 48]) -> String {
    encode_b64_table(bytes, BASE64_URL_SAFE)
}

#[inline]
fn encode_b64_table(bytes: &[u8; 48], table: &[u8; 64]) -> String {
    // Stack-allocated 64-byte array to avoid intermediate heap reallocations
    let mut out = [0u8; 64];
    for i in 0..16 {
        let b0 = bytes[i * 3] as usize;
        let b1 = bytes[i * 3 + 1] as usize;
        let b2 = bytes[i * 3 + 2] as usize;

        out[i * 4] = table[b0 >> 2];
        out[i * 4 + 1] = table[((b0 & 0x03) << 4) | (b1 >> 4)];
        out[i * 4 + 2] = table[((b1 & 0x0F) << 2) | (b2 >> 6)];
        out[i * 4 + 3] = table[b2 & 0x3F];
    }
    // Safety: table only contains valid ASCII characters (A-Z, a-z, 0-9, +, /, -, _)
    unsafe { String::from_utf8_unchecked(out.to_vec()) }
}

/// Encode a 48-byte digest into 96 hexadecimal characters.
#[inline]
pub fn encode_hex(bytes: &[u8; 48]) -> String {
    let mut out = [0u8; 96];
    for (i, &b) in bytes.iter().enumerate() {
        out[i * 2] = HEX_CHARS[(b >> 4) as usize];
        out[i * 2 + 1] = HEX_CHARS[(b & 0x0F) as usize];
    }
    unsafe { String::from_utf8_unchecked(out.to_vec()) }
}

/// Decode a 64-character Base64 string into a 48-byte digest.
pub fn decode_base64(s: &str) -> Result<[u8; 48], &'static str> {
    if s.len() != 64 {
        return Err("invalid blks-384 Base64 string: must be exactly 64 characters");
    }

    let bytes_in = s.as_bytes();
    let mut out = [0u8; 48];

    for i in 0..16 {
        let c0 = REV_TABLE[bytes_in[i * 4] as usize];
        let c1 = REV_TABLE[bytes_in[i * 4 + 1] as usize];
        let c2 = REV_TABLE[bytes_in[i * 4 + 2] as usize];
        let c3 = REV_TABLE[bytes_in[i * 4 + 3] as usize];

        if c0 == 255 || c1 == 255 || c2 == 255 || c3 == 255 {
            return Err("invalid character in Base64 string");
        }

        out[i * 3] = (c0 << 2) | (c1 >> 4);
        out[i * 3 + 1] = ((c1 & 0x0F) << 4) | (c2 >> 2);
        out[i * 3 + 2] = ((c2 & 0x03) << 6) | c3;
    }

    Ok(out)
}

/// Compare two byte slices in constant time, preventing timing side-channel attacks.
///
/// Returns true if and only if both slices have the same length and identical contents.
/// Operates in constant time regardless of where or whether bytes differ.
#[inline]
pub fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0u8;
    for (&x, &y) in a.iter().zip(b.iter()) {
        diff |= x ^ y;
    }
    // Convert to bool without branching
    std::hint::black_box(diff) == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_constant_time_eq() {
        let a = b"0123456789abcdef0123456789abcdef0123456789abcdef";
        let b = b"0123456789abcdef0123456789abcdef0123456789abcdef";
        let c = b"0123456789abcdef0123456789abcdef0123456789abcdeg";
        let d = b"0123";

        assert!(constant_time_eq(a, b));
        assert!(!constant_time_eq(a, c));
        assert!(!constant_time_eq(a, d));
    }

    #[test]
    fn test_base64_roundtrip_and_exact_64_len() {
        let mut sample = [0u8; 48];
        for (i, byte) in sample.iter_mut().enumerate() {
            *byte = (i * 7 + 13) as u8;
        }

        let b64 = encode_base64(&sample);
        assert_eq!(b64.len(), 64);
        assert!(!b64.contains('='));

        let decoded = decode_base64(&b64).expect("valid decode");
        assert_eq!(decoded, sample);

        let hex = encode_hex(&sample);
        assert_eq!(hex.len(), 96);
    }
}
