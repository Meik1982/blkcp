# Contributing to blkcp

Thank you for your interest in contributing to `blkcp`! We welcome contributions, bug reports, performance benchmarks, and feature enhancements.

## 1. Core Principles

- **Zero Data-Loss Guarantee:** `blkcp` is a low-level block copy tool. Correctness and data integrity take absolute priority over micro-optimizations.
- **Strict Modularity:** All I/O logic is organized under the `dd_io_driver_t` interface in `src/io_*.c`. The main engine (`src/io_engine.c`) remains backend-agnostic.
- **Modern Standards:** Code is written in standard **ISO C11**. Warnings are treated as errors (`-Wall -Wextra -Wpedantic`). Avoid non-standard compiler extensions where portable C standard idioms suffice.
- **Sanitizer Cleanliness:** Any patch must compile and pass all tests under:
  - AddressSanitizer & UndefinedBehaviorSanitizer (`-fsanitize=address,undefined`)
  - ThreadSanitizer (`-fsanitize=thread`)

## 2. Building & Testing

### Dependencies
On Debian/Ubuntu:
```bash
sudo apt-get install build-essential liburing-dev libssl-dev libncurses-dev
```
On Arch Linux / CachyOS / Manjaro:
```bash
sudo pacman -S base-devel liburing openssl ncurses
```

### Build Commands
```bash
# Debug build:
make clean && make all

# Release build (-O3, -flto, stripped):
make clean && make release

# Run all 32 regression tests:
make test
```

### Running Sanitizer Suites
```bash
# ASan / UBSan:
make clean && make CFLAGS="-fsanitize=address,undefined -O1 -g -pthread -Iinclude -Isrc -MMD -MP" LDFLAGS="-fsanitize=address,undefined"
make test

# ThreadSanitizer:
make clean && make CFLAGS="-fsanitize=thread -O1 -g -pthread -Iinclude -Isrc -MMD -MP" LDFLAGS="-fsanitize=thread"
make test
```

## 3. Submitting Pull Requests

1. Fork the repository and create a feature branch (`git checkout -b feat/your-feature`).
2. Write clean, atomic commits following the Conventional Commits specification (`feat: ...`, `fix: ...`, `perf: ...`, `docs: ...`).
3. Add or update tests in `tests/run_tests.sh` covering your new functionality.
4. Update `man/blkcp.1`, `README.md`, and shell completions if CLI options are modified.
5. Ensure `make release && make test` passes without failures or memory leaks.
6. Open a Pull Request against the `main` branch.
