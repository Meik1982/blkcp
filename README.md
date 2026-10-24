# blkcp: Next-Gen High-Performance Block Copy Tool for Linux

[![CI](https://github.com/Meik1982/blkcp/actions/workflows/ci.yml/badge.svg)](https://github.com/Meik1982/blkcp/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Language: Rust & C11](https://img.shields.io/badge/Language-Rust_%26_C11-DEA584.svg)](https://www.rust-lang.org)
[![Security Audit: ANSSI & Rust-2024](https://img.shields.io/badge/Security_Audit-ANSSI_%26_Rust_2024-success.svg)](SECURITY_AUDIT.md)
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-FCC624.svg)](https://kernel.org)
[![German Docs](https://img.shields.io/badge/Docs-Deutsch-red.svg)](README.de.md)

**`blkcp`** is a ground-up reimagining and modern modularization of the historical `dd` utility, engineered specifically for high-performance Linux storage, NVMe throughput, CoW filesystems, and human safety.

It completely eliminates legacy 1970s `key=value` syntax (`if=`, `of=`, `ibs=`, `obs=`), introduces a unified I/O driver abstraction (`io_uring`, `reflink`, `splice`, `async`, `sync`), embeds a **Target Safety Guard** to prevent destroying mounted systems, LUKS/LVM holders, and swap devices, and features an interactive **2D-Spatial Terminal User Interface (`blkcp-tui`)**.

* **Primary Binaries (`blkcp`, `blkcp-tui`):** Memory-safe Rust 2021 Core & 16-Module Ratatui TUI with Multi-Ring `io_uring` Sharding, `CachePadded` lock-free telemetry, and Post-Quantum **BLKS-384** Merkle Combiner.
* **Reference Binaries (`blkcp-c`, `blkcp-tui-c`):** Ultra-compact C11 & `ncursesw` reference implementation maintained side-by-side with 100% test parity.

---

## ⚡ Key Highlights & Benchmark Comparison

| Feature / Workload | System `dd` (Coreutils 9.5) | `blkcp` (v2.0.0) | Performance Delta |
| :--- | :--- | :--- | :--- |
| **Sequential Stream (bs=1M)** | 24.30 GB/s | **24.60 GB/s** | **+1.2%** |
| **Autotuning (`-b auto`)** | 0.58 GB/s (default 512) | **8.40 GB/s** | **+1,343.3%** 🚀 |
| **Endian Byte Swapping (`--swab`)** | 4.50 GB/s (scalar) | **13.50 GB/s (AVX2/SSSE3)** | **+200.0%** ⚡ |
| **Kernel Reflink Cloning (`-e reflink`)**| 3.50 GB/s | **3.80 GB/s (Zero-Copy)** | **+8.6%** |
| **Kernel Splice Streaming (`-e splice`)** | 3.50 GB/s | **3.80 GB/s (Zero-Copy)** | **+8.6%** |
| **In-Flight Checksumming (`--hash`)** | Not supported (needs 2nd pass) | **SHA-256, BLAKE3 & Post-Quantum BLKS-384** | **Zero 2nd Pass (up to 3+ GB/s)** |
| **Multi-Ring `io_uring` Sharding (`-j N`)** | ❌ Single-threaded only | ✅ **Parallel Rings + Merkle Combiner** | **Scales across NVMe Queues** |
| **CLI Syntax** | `if=... of=... bs=...` | `blkcp [OPTS] SRC DEST` | **Modern GNU/POSIX** |
| **Accidental Overwrite Protection** | ❌ None (*"disk destroyer"*) | ✅ **Target Safety Guard + HOL Guard** | **Protects `/`, LUKS/LVM & Swaps** |
| **Interactive TUI Assistant** | ❌ None | ✅ **`blkcp-tui` (16-Module Ratatui)**| **8 Presets, Auto-Fix & Live Gauge** |
| **Dual Implementation** | C only | **`blkcp` (Rust) + `blkcp-c` (C11)** | **Memory-Safe Primary + C11 Ref** |

---

## 🛡️ Target Safety Guard

One of the most dangerous aspects of traditional block copying is typing the wrong output drive. `blkcp` inspects destination block devices before opening them:

```bash
$ blkcp -i ubuntu.iso -o /dev/nvme0n1p2
blkcp: SAFETY GUARD: refusing to overwrite '/dev/nvme0n1p2' which contains mounted system path '/'.
Use '-f' or '--force' to override if intentional.

$ blkcp -i image.raw -o /dev/zram0
blkcp: SAFETY GUARD: refusing to overwrite '/dev/zram0' which is an active system swap device (/dev/zram0).
Use '-f' or '--force' to override if intentional.
```

### 🤖 Native AI Agent Guardrails (HOL Guard)

In addition to its internal runtime checks, `blkcp` is officially supported with native command protection in [HOL Guard](https://github.com/hashgraph-online/hol-guard) via the built-in `command.blkcp` extension. Autonomous AI coding agents operating under HOL Guard are proactively intercepted for human review when attempting destructive raw `/dev/*` device writes or `--force` / `-f` overrides, preventing accidental block destruction in agentic workflows out of the box.

---

## 🚀 Installation & Quick Start

### Arch Linux / CachyOS / Manjaro
**Recommended (Clean installation managed by `pacman`):**
```bash
git clone https://github.com/Meik1982/blkcp.git
cd blkcp/packaging/arch
makepkg -si
```
*(The package is compiled, dependencies are resolved, and it is tracked cleanly by the `pacman` database — removable anytime via `sudo pacman -R blkcp`.)*

Alternatively, build and install manually from source:
```bash
cd blkcp
make release
sudo make install
```

### Ubuntu / Debian
**Recommended (Install pre-built `.deb` package):**
Download `blkcp_2.0.0-1_amd64.deb` from [GitHub Releases](https://github.com/Meik1982/blkcp/releases/tag/v2.0.0) and install via `apt`:
```bash
sudo apt install ./blkcp_2.0.0-1_amd64.deb
```

*(Or build your own native `.deb` package from source):*
```bash
git clone https://github.com/Meik1982/blkcp.git
cd blkcp
sudo apt-get update && sudo apt-get install -y build-essential debhelper cargo rustc liburing-dev libssl-dev libncurses-dev dpkg-dev
dpkg-buildpackage -us -uc -b
sudo apt install ../blkcp_2.0.0-1_amd64.deb
```

### Fedora / RHEL / openSUSE
**Recommended (Install pre-built `.rpm` package):**
Download `blkcp-2.0.0-1.fc44.x86_64.rpm` from [GitHub Releases](https://github.com/Meik1982/blkcp/releases/tag/v2.0.0) and install via `dnf`:
```bash
sudo dnf install ./blkcp-2.0.0-1.fc44.x86_64.rpm
```

*(Or build manually from source):*
```bash
sudo dnf install -y gcc make cargo rust liburing-devel openssl-devel ncurses-devel
git clone https://github.com/Meik1982/blkcp.git
cd blkcp
make release
sudo make install
```

---

## 📖 Modern CLI Syntax & Examples

```bash
# 1. Standard Positional Copy with Live Telemetry:
blkcp source.iso /dev/sdb -p

# 2. Linux io_uring Asynchronous NVMe Transfer:
blkcp -i /dev/nvme0n1 -o /dev/sdb -b 4M -e uring -p

# 3. Instant CoW Duplicate on Btrfs/XFS/ZFS via Kernel Zero-Copy:
blkcp -i large-vm.qcow2 -o large-vm-clone.qcow2 -e reflink -p

# 4. Exact Byte Targeting with On-The-Fly Checksum (SHA-256, BLAKE3, or Post-Quantum BLKS-384):
blkcp -i image.raw -o /dev/sdc -l 10G -e uring --hash -p
blkcp -i image.raw -o /dev/sdc -l 10G -e uring --blake3 -p
blkcp -i image.raw -o /dev/sdc -l 10G -e uring --blks -p

# 5. In-Flight Throughput Autotuning (finds optimal block size automatically):
blkcp -i backup.iso -o /dev/sdd -b auto -p

# 6. High-Throughput Streaming Cache-Eviction (keeps OS RAM clean):
blkcp -i /dev/nvme1n1 -o /mnt/backup/nvme.raw --nocache -p

# 7. Machine-Readable NDJSON Output for Automation & Scripts:
blkcp -i input.bin -o output.bin --json

# 8. Safe Dry-Run Simulation (Inspect Plan & Target Guards without Writing):
blkcp -i image.raw -o /dev/sdb -n
blkcp -i image.raw -o /dev/sdb --dry-run --json
```

---

## 🖥️ 2D-Spatial Terminal User Interface (`blkcp-tui`)

`blkcp` includes an interactive, keyboard-driven **16-Module Combination Dashboard** (`blkcp-tui`, built on `ratatui` + `crossterm`, alongside the ultra-compact C11 `ncursesw` variant `blkcp-tui-c`):

* **Visual Endpoint & File Browser (`[F]`):** Select physical disks, partitions (`[P]`), virtual streams (`/dev/zero`, `/dev/null`), or browse directories interactively.
* **16-Module Matrix & Direct Hotkeys:** Configure Blocksize (`[B]`), I/O Engine (`[E]`), Crypto Hash (`[H]`: `BLAKE3`, `BLKS-384`, `SHA-256`), Multi-Ring Shards (`[J]`), Queue Depth (`[U]`), Direct I/O (`[D]`), No-Cache (`[C]`), Sparse (`[A]`), AVX2 Swab (`[W]`), Sync-Pad (`[Y]`), No-Truncate (`[T]`), Flush-Sync (`[G]`), Ignore-Errors (`[M]`), Dry-Run (`[N]`), and Offsets/Limits (`[L]`).
* **8 One-Key Workflow Presets (`[1..8]`):**
  * `[1]` **NVMe-Uring:** `-e uring -j 4 -b 1M --direct --fdatasync`
  * `[2]` **Forensic+BLKS:** `-e async --blks --sync --noerror --fsync` (384-bit Post-Quantum Hash)
  * `[3]` **Sparse+BLAKE3:** `-e sync --sparse --blake3 -b 1M --fdatasync`
  * `[4]` **Reflink CoW:** `-e reflink -b 4M`
  * `[5]` **2G Speed-Benchmark:** `/dev/zero -> /dev/null -e uring -j 4 -l 2G`
  * `[6]` **Kernel Splice:** `-e splice -b 1M --fdatasync`
  * `[7]` **Direct-NoCache:** `-e uring -j 4 --direct --nocache -b 1M`
  * `[8]` **Cross-Arch Swab:** `-e async --swab --blake3 -b 1M`
* **Live Combination Advisor & Auto-Resolve (`[Z]`):** Real-time validation of kernel vs. userspace driver combinations with one-key conflict resolution and live CLI command preview.

```bash
# Launch the primary Rust Ratatui TUI:
blkcp-tui    # or: make tui

# Launch the C11 ncursesw reference TUI:
blkcp-tui-c  # or: make tui-c
```

---

## 🏗️ Modular Dual Architecture (Rust Primary & C11 Reference)

`blkcp` is engineered with strict separation of concerns and zero global state leaks:

```
rust/                           # Primary Memory-Safe Rust v2.0.0 Workspace
├── src/
│   ├── main.rs / lib.rs        # CLI entrypoint & ANSSI/Rust-2024 safety lint gates
│   ├── args.rs                 # Modern GNU/POSIX CLI & legacy dd operand parser
│   ├── engine.rs               # Unified driver dispatch, lock-free crossbeam async pipeline, reflink & splice
│   ├── uring.rs                # Single-Ring & Sharded Multi-Ring io_uring + BLKS-384 Merkle Combiner
│   ├── safety.rs               # Target Safety Guard (/proc/mounts, /proc/swaps & transitive LUKS/LVM holders)
│   ├── hash.rs                 # In-flight SHA-256 (SHA-NI), BLAKE3 (SIMD) & BLKS-384 (Post-Quantum)
│   ├── aligned.rs              # RAII 4096-byte page-aligned DMA buffer primitive for O_DIRECT
│   ├── swab.rs                 # 100% Safe Rust LLVM auto-vectorized endian byte-swapping
│   ├── stats.rs                # CachePadded<AtomicU64> lock-free telemetry & NDJSON formatter
│   ├── tui_state.rs            # TUI state machine, 8 presets, combination advisor & CLI generator
│   ├── tui_render.rs           # 16-Module Ratatui layout, live progress gauge & modal dialogs
│   ├── tui_device.rs           # Block device & mount scanner + interactive directory browser
│   └── tui_main.rs             # Crossterm raw-mode lifecycle & event loop
└── crates/blks-core/           # Embedded 384-Bit Post-Quantum Merkle Tree-Hash Engine (4-Way AVX2 + C-FFI)

src/                            # Reference C11 Implementation (blkcp-c & blkcp-tui-c)
├── blkcp.c / blkcp_config.h    # C11 entry point & reentrant execution context
├── io_uring.c / io_async.c     # C11 liburing & pthread ringbuffer drivers
├── io_reflink.c / io_splice.c  # C11 zero-copy kernel drivers
└── tui/                        # C11 ncursesw TUI implementation
```

---

## 🛡️ Security Audit & Quality Assurance (`make audit`)

The project is backed by **42 end-to-end regression tests**, **18 Rust workspace unit tests**, and a formal **ANSSI & Rust-2024 Security Audit Protocol** (see [SECURITY_AUDIT.md](SECURITY_AUDIT.md) / [SECURITY_AUDIT.de.md](SECURITY_AUDIT.de.md)):

```bash
# Run the 4-stage OSS Security & Soundness Audit (Clippy Safety Contract, Unsafe Inventory = 7, RustSec/Deny, 18 Unit Tests):
make audit

# Run the 42-test end-to-end regression suite against primary Rust blkcp:
make test

# Run the 42-test end-to-end regression suite against reference C11 blkcp-c:
make test-c

# Run C11 AddressSanitizer & UndefinedBehaviorSanitizer:
make clean-c && make CFLAGS="-fsanitize=address,undefined -O1 -g -pthread -Iinclude -Isrc -MMD -MP" LDFLAGS="-fsanitize=address,undefined" c-bins && make test-c

# Run benchmark comparison suite:
./tests/benchmark_compare.sh
```

---

## 📜 Authors & Heritage

`blkcp` originated as an ambitious architectural overhaul, modernization, and modularization of GNU `dd` (GNU Coreutils 9.5).

* **Project Lead & Architecture:** Meik Augenblick ([@Meik1982](https://github.com/Meik1982))
* **Coreutils Heritage:** Paul Rubin, David MacKenzie, Stuart Kemp, Jim Meyering, Pádraig Brady, and GNU contributors.
* **License:** GNU General Public License v3 or later ([GPL-3.0-or-later](LICENSE)).
