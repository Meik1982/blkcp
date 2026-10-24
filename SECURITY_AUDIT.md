# Security & OSS Audit Protocol (`blkcp` v2.0.0)

[![German Audit Docs](https://img.shields.io/badge/Audit_Protokoll-Deutsch-red.svg)](SECURITY_AUDIT.de.md)

This document defines the formal Open-Source Software (OSS) Security & Soundness Audit Protocol enforced across the `blkcp` repository (covering both the primary **Rust 2021 Core (`blkcp`, `blkcp-tui`)** and the **C11 Reference Core (`blkcp-c`, `blkcp-tui-c`)**).

---

## 1. Standards & Frameworks Applied

1. **Rust Standard Library & Rust-for-Linux Safety Contract:**
   - Enforced at compile-time via crate-level attributes in `rust/src/lib.rs` and `rust/crates/blks-core/src/lib.rs`:
     - `#![deny(unsafe_op_in_unsafe_fn)]` (Rust 2024 strictness)
     - `#![deny(clippy::undocumented_unsafe_blocks)]`
     - `#![deny(clippy::missing_safety_doc)]`
     - `#![deny(clippy::ptr_as_ptr)]`
     - `#![deny(clippy::cast_ptr_alignment)]`
2. **ANSSI Secure Rust Guidelines (*Recommandations pour le développement sécurisé en Rust*):**
   - **C-ABI Boundary Isolation:** Every `extern "C"` entrypoint in `blks-core` (`rust/crates/blks-core/src/ffi.rs`) is wrapped in `std::panic::catch_unwind` to guarantee that no Rust panic ever unwinds across the FFI boundary into C callers (`blkcp-c`).
   - **Minimal `unsafe` Footprint:** All kernel syscalls (`posix_fadvise`, `copy_file_range`, `splice`) use safe file-descriptor abstractions (`std::os::fd::AsFd`, `nix::fcntl`). Endian byte-swapping (`swab.rs`) uses 100% Safe Rust `as_chunks_mut::<2>()` auto-vectorized by LLVM.
3. **RustSec & OpenSSF Supply-Chain Policy (`rust/deny.toml`):**
   - Declarative verification of crate advisories (CVEs/soundness), SPDX license compatibility (`GPL-3.0-or-later`, `LGPL-3.0-or-later`, `MIT`, `Apache-2.0`, `BSD`), ban on wildcard dependencies, and restriction to verified registry sources.
4. **HOL Guard Native Agentic Protection (`command.blkcp`):**
   - Built-in command protection in [HOL Guard](https://github.com/hashgraph-online/hol-guard) intercepts autonomous AI coding agents attempting raw `/dev/*` block writes or `--force` / `-f` overrides.

---

## 2. Complete Inventory of `unsafe` Blocks

### A. Primary Application Crate: `blkcp` (`rust/src/`) — **Exactly 7 `unsafe` Blocks**

| # | File & Line | Construct | Safety Invariant (`// SAFETY:`) |
| :- | :--- | :--- | :--- |
| 1 | `rust/src/aligned.rs:25` | `alloc_zeroed(layout)` | `layout` is verified non-zero (`cap > 0`) and `align` is a power-of-two (`>= 4096`). Null returns trigger `handle_alloc_error`. |
| 2 | `rust/src/aligned.rs:56` | `slice::from_raw_parts` | `self.ptr` is non-null, aligned to `self.layout.align()`, and initialized for `self.capacity` bytes. |
| 3 | `rust/src/aligned.rs:64` | `slice::from_raw_parts_mut` | Exclusive `&mut self` borrow guarantees alias-free mutable access to `self.capacity` initialized bytes. |
| 4 | `rust/src/aligned.rs:71` | `dealloc(self.ptr, self.layout)` | `self.ptr` was allocated with the exact `self.layout` in `AlignedBuffer::new` and is deallocated once in `Drop`. |
| 5 | `rust/src/aligned.rs:78` | `unsafe impl Send for AlignedBuffer` | `AlignedBuffer` exclusively owns its heap allocation (`ptr`); no thread-local state or aliased pointers exist. |
| 6 | `rust/src/aligned.rs:80` | `unsafe impl Sync for AlignedBuffer` | Shared `&AlignedBuffer` references only expose immutable slices (`&[u8]`). |
| 7 | `rust/src/uring.rs:20` | `ring.submission().push(entry)` | Encapsulated in `push_sqe()`: referenced `AlignedBuffer` is pinned to the current stack frame and outlives the synchronous `ring.submit_and_wait(1)` completion cycle. |

*(Note: `rust/src/engine.rs`, `rust/src/swab.rs`, `rust/src/safety.rs`, `rust/src/hash.rs`, `rust/src/stats.rs`, and all TUI modules contain **0 `unsafe` blocks**).*

### B. Embedded Cryptographic Crate: `blks-core` (`rust/crates/blks-core/src/`)

| Module | `unsafe` Count | Purpose & Verification |
| :--- | :---: | :--- |
| `compress.rs` | **0** | Pure Safe Rust ARX compression core. |
| `encoding.rs` | **2** | `String::from_utf8_unchecked` over fixed 7-bit ASCII lookup tables (`BASE64_*`, `HEX_CHARS`). |
| `tree.rs` | **1** | Runtime-gated `is_x86_feature_detected!("avx2")` dispatch to `hash_4_chunks_avx2`. |
| `simd_avx2.rs` | **2** | Pure register-level AVX2 4-way ARX rounds (`#[target_feature(enable = "avx2")]`); zero raw pointer casts. |
| `lib.rs` | **2** | Read-only `Mmap::map(&file)` on verified regular files and AVX2 unit-test parity check. |
| `ffi.rs` | **5** | C-ABI entrypoints (`blks_hash_*`), null-checked, length-checked, and wrapped in `catch_unwind`. |

---

## 3. Reproducible Audit Execution (`make audit`)

Run the complete audit suite locally at any time via:

```bash
make audit
```

This executes:
1. **Rust Compiler & Clippy Safety Gate:** Enforces all `unsafe` documentation, pointer-cast, and pedantic lints across the workspace.
2. **`unsafe` Inventory Verification:** Scans `rust/src/` to verify the exact count of 7 documented `unsafe` blocks.
3. **Supply-Chain & Vulnerability Audit:** Runs `cargo audit` and `cargo deny check` if installed on the host.
4. **Unit & Cryptographic Test Suite:** Runs all 18 Rust workspace unit tests (including Strict Avalanche Criterion, AVX2 parity, and Merkle Combiner proofs).
5. **Dual End-to-End Regression Suite:** Runs all 42 end-to-end tests against both `blkcp` (Rust) and `blkcp-c` (C11).
