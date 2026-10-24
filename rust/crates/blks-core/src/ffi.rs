//! C-FFI Bindings for blks-384
//!
//! Provides clean, zero-overhead extern "C" functions for linking with blkcp and other C utilities.
//! All entrypoints are hardened with `std::panic::catch_unwind` to ensure no unwinding crosses the C-ABI boundary.

use crate::encoding::encode_base64;
use crate::tree::{hash_slice_parallel, BlksHasher};
use std::os::raw::c_char;
use std::panic::catch_unwind;
use std::slice;

/// Compute blks-384 hash over a memory buffer into a 48-byte buffer.
/// Returns 0 on success, -1 if null pointers are passed or an exception occurs.
///
/// # Safety
/// - `data` must point to at least `len` valid, readable bytes.
/// - `out_digest` must point to at least 48 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn blks_hash_buffer(data: *const u8, len: usize, out_digest: *mut u8) -> i32 {
    if data.is_null() || out_digest.is_null() {
        return -1;
    }
    let res = catch_unwind(|| {
        let slice = slice::from_raw_parts(data, len);
        let digest = hash_slice_parallel(slice);
        std::ptr::copy_nonoverlapping(digest.as_ptr(), out_digest, 48);
    });
    if res.is_ok() {
        0
    } else {
        -1
    }
}

/// Allocate a new streaming blks hasher
#[no_mangle]
pub extern "C" fn blks_hasher_new() -> *mut BlksHasher {
    let res = catch_unwind(|| Box::into_raw(Box::new(BlksHasher::new())));
    res.unwrap_or(std::ptr::null_mut())
}

/// Update streaming hasher with incoming data
///
/// # Safety
/// - `hasher` must be a valid, non-null pointer created by `blks_hasher_new`.
/// - `data` must point to at least `len` valid, readable bytes if `len > 0`.
#[no_mangle]
pub unsafe extern "C" fn blks_hasher_update(
    hasher: *mut BlksHasher,
    data: *const u8,
    len: usize,
) -> i32 {
    if hasher.is_null() || (data.is_null() && len > 0) {
        return -1;
    }
    let res = catch_unwind(|| {
        let h = &mut *hasher;
        let slice = slice::from_raw_parts(data, len);
        h.update(slice);
    });
    if res.is_ok() {
        0
    } else {
        -1
    }
}

/// Finalize streaming hasher, write 48 bytes to out_digest, and deallocate hasher
///
/// # Safety
/// - `hasher` must be a valid, non-null pointer created by `blks_hasher_new`. It will be deallocated.
/// - `out_digest` must point to at least 48 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn blks_hasher_finalize(hasher: *mut BlksHasher, out_digest: *mut u8) -> i32 {
    if hasher.is_null() || out_digest.is_null() {
        return -1;
    }
    let res = catch_unwind(|| {
        let h = Box::from_raw(hasher);
        let digest = h.finalize();
        std::ptr::copy_nonoverlapping(digest.as_ptr(), out_digest, 48);
    });
    if res.is_ok() {
        0
    } else {
        -1
    }
}

/// Free a streaming hasher without finalizing
///
/// # Safety
/// - `hasher` must be a valid pointer created by `blks_hasher_new`, or null.
#[no_mangle]
pub unsafe extern "C" fn blks_hasher_free(hasher: *mut BlksHasher) {
    if !hasher.is_null() {
        let _ = catch_unwind(|| {
            drop(Box::from_raw(hasher));
        });
    }
}

/// Encode 48-byte binary digest into a null-terminated 64-char Base64 string.
/// out_str must have at least 65 bytes of capacity (64 chars + '\0').
///
/// # Safety
/// - `in_digest` must point to at least 48 readable bytes.
/// - `out_str` must point to a writable buffer of at least `out_str_size` bytes.
#[no_mangle]
pub unsafe extern "C" fn blks_digest_to_base64(
    in_digest: *const u8,
    out_str: *mut c_char,
    out_str_size: usize,
) -> i32 {
    if in_digest.is_null() || out_str.is_null() || out_str_size < 65 {
        return -1;
    }
    let res = catch_unwind(|| {
        let digest: &[u8; 48] = &*(in_digest as *const [u8; 48]);
        let b64 = encode_base64(digest);
        std::ptr::copy_nonoverlapping(b64.as_ptr(), out_str as *mut u8, 64);
        *out_str.add(64) = 0; // null-terminator
    });
    if res.is_ok() {
        0
    } else {
        -1
    }
}
