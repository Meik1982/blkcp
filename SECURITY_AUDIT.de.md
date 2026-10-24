# Sicherheits- & OSS-Audit-Protokoll (`blkcp` v2.0.0)

[![English Audit Docs](https://img.shields.io/badge/Audit_Protocol-English-blue.svg)](SECURITY_AUDIT.md)

Dieses Dokument definiert das formale Open-Source-Software (OSS) Sicherheits- und Soundness-Audit-Protokoll für das `blkcp`-Repository (sowohl für die primäre **Rust-2021-Implementierung (`blkcp`, `blkcp-tui`)** als auch für die **C11-Referenzimplementierung (`blkcp-c`, `blkcp-tui-c`)**).

---

## 1. Angewandte Standards & Richtlinien

1. **Rust Standard Library & Rust-for-Linux Safety Contract:**
   - Technisch zur Compile-Zeit erzwungen über Crate-Attribute in `rust/src/lib.rs` und `rust/crates/blks-core/src/lib.rs`:
     - `#![deny(unsafe_op_in_unsafe_fn)]` (Rust-2024-Striktheit)
     - `#![deny(clippy::undocumented_unsafe_blocks)]`
     - `#![deny(clippy::missing_safety_doc)]`
     - `#![deny(clippy::ptr_as_ptr)]`
     - `#![deny(clippy::cast_ptr_alignment)]`
2. **ANSSI Secure Rust Guidelines (*Recommandations pour le développement sécurisé en Rust*):**
   - **C-ABI-Grenzabsicherung:** Jeder `extern "C"`-Einstiegspunkt in `blks-core` (`rust/crates/blks-core/src/ffi.rs`) ist durch `std::panic::catch_unwind` abgesichert, sodass niemals ein Rust-Panic über die FFI-Grenze in C-Aufrufer (`blkcp-c`) entweichen kann.
   - **Minimaler `unsafe`-Footprint:** Sämtliche Kernel-Syscalls (`posix_fadvise`, `copy_file_range`, `splice`) nutzen typsichere Deskriptor-Abstraktionen (`std::os::fd::AsFd`, `nix::fcntl`). Das Endian-Byte-Swapping (`swab.rs`) nutzt 100 % Safe Rust (`as_chunks_mut::<2>()`), das durch LLVM automatisch in AVX2/NEON-SIMD-Instruktionen übersetzt wird.
3. **RustSec & OpenSSF Supply-Chain-Policy (`rust/deny.toml`):**
   - Deklarative Prüfung auf bekannte Schwachstellen (RustSec Advisory DB), SPDX-Lizenzkompatibilität (`GPL-3.0-or-later`, `LGPL-3.0-or-later`, `MIT`, `Apache-2.0`, `BSD`), Verbot von Wildcard-Abhängigkeiten und Beschränkung auf verifizierte Paketquellen.
4. **HOL Guard Native Agentic Protection (`command.blkcp`):**
   - Nativer Befehlsschutz in [HOL Guard](https://github.com/hashgraph-online/hol-guard) fängt autonome KI-Agenten bei direkten Schreibzugriffen auf `/dev/*` oder bei Verwendung von `--force` / `-f` vorab ab.

---

## 2. Vollständiges Inventar aller `unsafe`-Blöcke

### A. Hauptanwendung: `blkcp` (`rust/src/`) — **Exakt 7 `unsafe`-Blöcke**

| # | Datei & Zeile | Konstrukt | Sicherheits-Invariante (`// SAFETY:`) |
| :- | :--- | :--- | :--- |
| 1 | `rust/src/aligned.rs:25` | `alloc_zeroed(layout)` | `layout` ist garantiert größer 0 (`cap > 0`) und `align` eine Zweierpotenz (`>= 4096`). Null-Pointer lösen `handle_alloc_error` aus. |
| 2 | `rust/src/aligned.rs:56` | `slice::from_raw_parts` | `self.ptr` ist nicht-null, auf `self.layout.align()` ausgerichtet und für `self.capacity` Bytes null-initialisiert. |
| 3 | `rust/src/aligned.rs:64` | `slice::from_raw_parts_mut` | Exklusiver `&mut self`-Borrow garantiert alias-freien Schreibzugriff auf `self.capacity` initialisierte Bytes. |
| 4 | `rust/src/aligned.rs:71` | `dealloc(self.ptr, self.layout)` | `self.ptr` wurde mit exakt demselben `self.layout` in `AlignedBuffer::new` allokiert und wird genau einmal in `Drop` freigegeben. |
| 5 | `rust/src/aligned.rs:78` | `unsafe impl Send for AlignedBuffer` | `AlignedBuffer` besitzt seinen Heap-Speicher (`ptr`) exklusiv; keine Thread-Local-Daten oder geteilten Rohzeiger. |
| 6 | `rust/src/aligned.rs:80` | `unsafe impl Sync for AlignedBuffer` | Geteilte `&AlignedBuffer`-Referenzen gewähren ausschließlich unveränderlichen Lesezugriff (`&[u8]`). |
| 7 | `rust/src/uring.rs:20` | `ring.submission().push(entry)` | Gekapselt in `push_sqe()`: Der referenzierte `AlignedBuffer` lebt auf dem aktuellen Stack-Frame und überlebt den synchronen `ring.submit_and_wait(1)`-Zyklus garantiert. |

*(Hinweis: `rust/src/engine.rs`, `rust/src/swab.rs`, `rust/src/safety.rs`, `rust/src/hash.rs`, `rust/src/stats.rs` sowie alle TUI-Module enthalten **0 `unsafe`-Blöcke**).*

### B. Eingebettete Krypto-Engine: `blks-core` (`rust/crates/blks-core/src/`)

| Modul | `unsafe`-Anzahl | Zweck & Absicherung |
| :--- | :---: | :--- |
| `compress.rs` | **0** | Reiner Safe-Rust ARX-Kompressionskern. |
| `encoding.rs` | **2** | `String::from_utf8_unchecked` über feste 7-Bit-ASCII-Tabellen (`BASE64_*`, `HEX_CHARS`). |
| `tree.rs` | **1** | Laufzeitgeprüfter `is_x86_feature_detected!("avx2")`-Aufruf von `hash_4_chunks_avx2`. |
| `simd_avx2.rs` | **2** | Reine Register-basierte AVX2 4-Wege-ARX-Runden (`#[target_feature(enable = "avx2")]`); null Rohzeiger-Casts. |
| `lib.rs` | **2** | Read-Only `Mmap::map(&file)` auf geprüften regulären Dateien sowie AVX2-Unit-Test. |
| `ffi.rs` | **8** | C-ABI-Funktionen (`blks_hash_*` & `blake3_hasher_*`), Null-Pointer-, Alignment- und Längen-geprüft, gekapselt in `catch_unwind`. |

---

## 3. Reproduzierbare Audit-Ausführung (`make audit`)

Das vollständige Audit-Protokoll lässt sich jederzeit lokal mit einem einzigen Befehl ausführen:

```bash
make audit
```

Dies führt nacheinander aus:
1. **Rust Compiler & Clippy Safety Gate:** Erzwingt alle `unsafe`-Dokumentations-, Pointer-Cast- und Linter-Regeln über den gesamten Workspace.
2. **`unsafe`-Inventar-Prüfung:** Verifiziert, dass in `rust/src/` exakt die 7 dokumentierten `unsafe`-Blöcke vorliegen.
3. **Supply-Chain- & Schwachstellen-Prüfung:** Führt `cargo audit` und `cargo deny check` aus (sobald auf dem System installiert).
4. **Unit- & Krypto-Testsuite:** Führt alle 18 Rust-Workspace-Unit-Tests aus (inkl. Strict Avalanche Criterion, AVX2-Parität und Merkle-Combiner-Beweis).
5. **Duale End-to-End Regressionstests:** Prüft alle 42 Regressionstests sowohl gegen `blkcp` (Rust) als auch gegen `blkcp-c` (C11).
