# blkcp: Next-Gen High-Performance Block Copy Tool for Linux

[![CI](https://github.com/Meik1982/blkcp/actions/workflows/ci.yml/badge.svg)](https://github.com/Meik1982/blkcp/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Language: C11](https://img.shields.io/badge/Language-C11-00599C.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-FCC624.svg)](https://kernel.org)
[![German Docs](https://img.shields.io/badge/Docs-Deutsch-red.svg)](README.de.md)

**`blkcp`** is a ground-up reimagining and modern modularization of the historical `dd` utility, engineered specifically for high-performance Linux storage, NVMe throughput, CoW filesystems, and human safety.

It completely eliminates legacy 1970s `key=value` syntax (`if=`, `of=`, `ibs=`, `obs=`), introduces a unified I/O driver abstraction (`io_uring`, `reflink`, `splice`, `async`, `sync`), embeds a **Target Safety Guard** to prevent destroying mounted systems and swap devices, and features an interactive **2D-Spatial Terminal User Interface (`blkcp-tui`)**.

---

## ⚡ Key Highlights & Benchmark Comparison

| Feature / Workload | System `dd` (Coreutils 9.5) | `blkcp` (v1.0.0) | Performance Delta |
| :--- | :--- | :--- | :--- |
| **Sequential Stream (bs=1M)** | 24.30 GB/s | **24.60 GB/s** | **+1.2%** |
| **Autotuning (`-b auto`)** | 0.58 GB/s (default 512) | **8.40 GB/s** | **+1,343.3%** 🚀 |
| **Endian Byte Swapping (`--swab`)** | 4.50 GB/s (scalar) | **13.50 GB/s (AVX2/SSSE3)** | **+200.0%** ⚡ |
| **Kernel Reflink Cloning (`-e reflink`)**| 3.50 GB/s | **3.80 GB/s (Zero-Copy)** | **+8.6%** |
| **Kernel Splice Streaming (`-e splice`)** | 3.50 GB/s | **3.80 GB/s (Zero-Copy)** | **+8.6%** |
| **In-Flight SHA-256 (`--hash`)** | Not supported (needs 2nd pass) | **Integrated (OpenSSL EVP)** | **Zero 2nd Pass** |
| **CLI Syntax** | `if=... of=... bs=...` | `blkcp [OPTS] SRC DEST` | **Modern GNU/POSIX** |
| **Accidental Overwrite Protection** | ❌ None (*"disk destroyer"*) | ✅ **Target Safety Guard** | **Protects `/` & Swaps** |
| **Interactive TUI Assistant** | ❌ None | ✅ **`blkcp-tui` (ncursesw)**| **Visual Device Selector** |
| **Binary Size (-O3, -flto, stripped)** | 95 KB | **99 KB** | **Ultra-Compact C11** |

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
Download `blkcp_1.0.0-1_amd64.deb` from [GitHub Releases](https://github.com/Meik1982/blkcp/releases) and install via `apt`:
```bash
sudo apt install ./blkcp_1.0.0-1_amd64.deb
```

*(Or build your own native `.deb` package from source):*
```bash
git clone https://github.com/Meik1982/blkcp.git
cd blkcp
sudo apt-get update && sudo apt-get install -y build-essential debhelper liburing-dev libssl-dev libncurses-dev dpkg-dev
dpkg-buildpackage -us -uc -b
sudo apt install ../blkcp_1.0.0-1_amd64.deb
```

### Fedora / RHEL / openSUSE
**Recommended (Install pre-built `.rpm` package):**
Download `blkcp-1.0.0-1.x86_64.rpm` from [GitHub Releases](https://github.com/Meik1982/blkcp/releases) and install via `dnf`:
```bash
sudo dnf install ./blkcp-1.0.0-1.x86_64.rpm
```

*(Or build manually from source):*
```bash
sudo dnf install -y gcc make liburing-devel openssl-devel ncurses-devel
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

# 4. Exact Byte Targeting with On-The-Fly SHA-256 Checksum:
blkcp -i image.raw -o /dev/sdc -l 10G -e uring --hash -p

# 5. In-Flight Throughput Autotuning (finds optimal block size automatically):
blkcp -i backup.iso -o /dev/sdd -b auto -p

# 6. High-Throughput Streaming Cache-Eviction (keeps OS RAM clean):
blkcp -i /dev/nvme1n1 -o /mnt/backup/nvme.raw --nocache -p

# 7. Machine-Readable NDJSON Output for Automation & Scripts:
blkcp -i input.bin -o output.bin --json
```

---

## 🖥️ 2D-Spatial Terminal User Interface (`blkcp-tui`)

`blkcp` includes an interactive, mouse- and keyboard-driven terminal dashboard (`blkcp-tui`):

* **Visual Stream Picker:** Easily select regular files, block devices (`/sys/class/block`), or pipes (`|`).
* **Engine Switching on the Fly:** Toggle between `io_uring`, `async`, `reflink`, `splice`, `sync`, and `auto` with Space.
* **Live Telemetry & In-Flight Hash:** Real-time speed charts, progress bars, and streaming SHA-256 hash displayed directly in the UI.
* **Clipboard Integration:** Press `c` to export the exact generated command to your system clipboard (Wayland / X11).
* **Safety Verification Dialog:** Dedicated confirmation modal before issuing writes to physical block devices.

```bash
# Launch the TUI:
blkcp-tui
# or
make tui
```

---

## 🏗️ Modular Driver Architecture

`blkcp` is structured with clean separation of concerns and zero global state leaks:

```
src/
├── blkcp.c                 # Minimal CLI entry point & driver dispatch
├── blkcp_config.h          # Execution state, canonical blocksize & context
├── args.h / .c             # Modern POSIX/GNU CLI parser (getopt_long)
├── io_driver.h             # Strategy-pattern driver abstraction (dd_io_driver_t)
├── io_engine.h / .c        # Master control loop, Safety Guard & Direct I/O fallback
├── io_uring.c              # Linux io_uring driver with fixed buffers & batch reaping
├── io_async.c              # Multi-threaded ringbuffer engine with adaptive scaling
├── io_reflink.c            # Linux copy_file_range(2) zero-copy engine
├── io_splice.c             # In-kernel splice(2) zero-copy pipe streaming engine
├── io_sync.c               # Synchronous POSIX block driver with autotuning
├── conversions.h / .c      # SIMD AVX2/SSSE3 vector routines & zero-detection
├── stats.h / .c            # Human-readable metrics, NDJSON & throttled vDSO clock
├── signals.h / .c          # Signal-safe dispatchers (SIGINT, SIGUSR1 progress)
└── tui/                    # Complete ncursesw 2D-Spatial TUI assistant
```

---

## 🧪 Quality Assurance & Sanitizers

The project is backed by **32 automated regression tests** covering bit-exactness, edge cases, Direct I/O tail handling, pipeline scaling, and safety intercepts.

```bash
# Run release test suite:
make test

# Run AddressSanitizer & UndefinedBehaviorSanitizer:
make clean && make CFLAGS="-fsanitize=address,undefined -O1 -g -pthread -Iinclude -Isrc -MMD -MP" LDFLAGS="-fsanitize=address,undefined"
make test

# Run ThreadSanitizer:
make clean && make CFLAGS="-fsanitize=thread -O1 -g -pthread -Iinclude -Isrc -MMD -MP" LDFLAGS="-fsanitize=thread"
make test

# Run benchmark suite:
./tests/benchmark_compare.sh
```

---

## 📜 Authors & Heritage

`blkcp` originated as an ambitious architectural overhaul, modernization, and modularization of GNU `dd` (GNU Coreutils 9.5).

* **Project Lead & Architecture:** Meik ([@Meik1982](https://github.com/Meik1982))
* **Coreutils Heritage:** Paul Rubin, David MacKenzie, Stuart Kemp, Jim Meyering, Pádraig Brady, and GNU contributors.
* **License:** GNU General Public License v3 or later ([GPL-3.0-or-later](LICENSE)).
