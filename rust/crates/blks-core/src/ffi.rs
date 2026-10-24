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
        // SAFETY: Caller guarantees `data` is valid for `len` bytes and `out_digest` is valid for 48 bytes.
        unsafe {
            let slice = slice::from_raw_parts(data, len);
            let digest = hash_slice_parallel(slice);
            std::ptr::copy_nonoverlapping(digest.as_ptr(), out_digest, 48);
        }
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
        // SAFETY: Caller guarantees `hasher` is a valid non-null pointer from `blks_hasher_new`
        // and `data` is valid for `len` bytes (or `len == 0`).
        unsafe {
            let h = &mut *hasher;
            let slice = if len == 0 {
                &[]
            } else {
                slice::from_raw_parts(data, len)
            };
            h.update(slice);
        }
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
        // SAFETY: Caller hands back ownership of `hasher` allocated by `blks_hasher_new`
        // and guarantees `out_digest` points to at least 48 writable bytes.
        unsafe {
            let h = Box::from_raw(hasher);
            let digest = h.finalize();
            std::ptr::copy_nonoverlapping(digest.as_ptr(), out_digest, 48);
        }
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
            // SAFETY: Caller guarantees `hasher` is a valid non-null pointer allocated by `blks_hasher_new`.
            unsafe {
                drop(Box::from_raw(hasher));
            }
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
        // SAFETY: `in_digest` is valid for 48 bytes (align 1) and `out_str` has >= 65 writable bytes.
        unsafe {
            let digest: &[u8; 48] = &*in_digest.cast::<[u8; 48]>();
            let b64 = encode_base64(digest);
            std::ptr::copy_nonoverlapping(b64.as_ptr(), out_str.cast::<u8>(), 64);
            *out_str.add(64) = 0; // null-terminator
        }
    });
    if res.is_ok() {
        0
    } else {
        -1
    }
}

// Compile-time verification that Rust `blake3::Hasher` fits inside C `blake3_hasher` (1920 bytes, align 8)
const _: () = assert!(std::mem::size_of::<blake3::Hasher>() <= 1920);
const _: () = assert!(std::mem::align_of::<blake3::Hasher>() <= 8);

/// Initialize a C `blake3_hasher` struct in-place using the SIMD-accelerated Rust `blake3::Hasher`.
///
/// # Safety
/// - `self_ptr` must be a non-null, 8-byte aligned pointer to a writable `blake3_hasher` (1912 bytes).
#[no_mangle]
pub unsafe extern "C" fn blake3_hasher_init(self_ptr: *mut u64) {
    if self_ptr.is_null() {
        return;
    }
    let _ = catch_unwind(|| {
        // SAFETY: `self_ptr` points to a valid, 8-byte aligned `blake3_hasher` struct of 1920 bytes.
        unsafe {
            std::ptr::write(self_ptr.cast::<blake3::Hasher>(), blake3::Hasher::new());
        }
    });
}

/// Feed data into an initialized C `blake3_hasher` struct.
///
/// # Safety
/// - `self_ptr` must point to a `blake3_hasher` previously initialized via `blake3_hasher_init`.
/// - `input` must point to at least `input_len` readable bytes (if `input_len > 0`).
#[no_mangle]
pub unsafe extern "C" fn blake3_hasher_update(
    self_ptr: *mut u64,
    input: *const u8,
    input_len: usize,
) {
    if self_ptr.is_null() || (input.is_null() && input_len > 0) {
        return;
    }
    let _ = catch_unwind(|| {
        // SAFETY: `self_ptr` was initialized by `blake3_hasher_init` and `input` is valid for `input_len`.
        unsafe {
            let hasher = &mut *self_ptr.cast::<blake3::Hasher>();
            let slice = if input_len == 0 {
                &[]
            } else {
                slice::from_raw_parts(input, input_len)
            };
            hasher.update(slice);
        }
    });
}

/// Finalize a C `blake3_hasher` struct and write `out_len` digest bytes into `out`.
///
/// # Safety
/// - `self_ptr` must point to a `blake3_hasher` previously initialized via `blake3_hasher_init`.
/// - `out` must point to at least `out_len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn blake3_hasher_finalize(
    self_ptr: *const u64,
    out: *mut u8,
    out_len: usize,
) {
    if self_ptr.is_null() || out.is_null() || out_len == 0 {
        return;
    }
    let _ = catch_unwind(|| {
        // SAFETY: `self_ptr` was initialized by `blake3_hasher_init` and `out` is valid for `out_len`.
        unsafe {
            let hasher = &*self_ptr.cast::<blake3::Hasher>();
            let out_slice = slice::from_raw_parts_mut(out, out_len);
            hasher.finalize_xof().fill(out_slice);
        }
    });
}
