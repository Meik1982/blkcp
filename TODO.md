# TODO & Zukünftige Optimierungspotenziale für `blkcp`

Dieses Dokument erfasst den aktuellen Umsetzungsstatus und die nächsten priorisierten Aufgaben für `blkcp`.

---

## 1. Abgeschlossene Meilensteine

- [x] **Modularisierung & Architektur-Entflechtung:**
  GNU `dd.c`-Monolith vollständig in modulare Subsysteme zerlegt. Saubere Driver-Architektur (`dd_io_driver_t`: `sync`, `async`, `reflink`, `io_uring`) mit genau einer zentralen Kontrollschleife in `src/io_engine.c`.
- [x] **Linux `io_uring` Asynchronous Engine (`src/io_uring.c`):**
  Nativer `liburing`-Treiber mit Double-Buffering Ring-Pipeline, Signal-Resilienz (`EINTR`) und Fallback.
- [x] **In-Kernel Zero-Copy Reflink (`src/io_reflink.c`):**
  Unterstützung für `copy_file_range(2)` mit CoW-Klonen auf Btrfs/XFS/ZFS.
- [x] **Multi-Threaded Async Double-Buffering (`src/io_async.c`):**
  Entkoppelte Ringpuffer-Pipeline via `pthread` (+50 % Durchsatz bei File-to-File).
- [x] **In-Flight Autotuning (`-b auto` / `--autotune`):**
  Dynamische Blockgrößen-Skalierung (Hardware-Awareness via `ioctl(BLKPBSZGET)`).
- [x] **Moderne POSIX/GNU-CLI (`src/args.c`):**
  `-i`, `-o`, `-b`, `-e`, `-l`, `-p`, `-q`, `-f`, `--hash`, `--autotune`, `--direct`, `--sparse`, `--sync` und intuitive Positionsargumente.
- [x] **Interaktive 2D-Spatial TUI (`src/tui/` - `blkcp-tui`):**
  Vollständiger ncursesw-Assistent mit Engine-Selector, Byteziel, Live-Telemetrie und Clipboard-Export.
- [x] **Target Safety Guard & Streaming SHA-256:**
  Schutz gegen versehentliches Überschreiben gemounteter Partitionen; parallele Prüfsummenberechnung im Hot-Loop.
- [x] **Qualitätssicherung & Dokumentation:**
  26/26 Regressionstests grün (`make test`), 13 automatisierte Benchmarks, Doxygen in allen Headern, Manpage `man/blkcp.1`.

---

## 2. Nächste geplante Ausbaustufen

### A. SIMD-Vektorisierung für Endian-Byte-Swapping (`swab`)
- **Ziel:** Vektorisierung von `dd_swab_buffer()` in `src/conversions.c` via SSSE3/AVX2 `_mm_shuffle_epi8` / `_mm256_shuffle_epi8`.
- **Nutzen:** Beseitigt den skalaren Flaschenhals bei Big-Endian / Little-Endian Konvertierungen; skaliert Durchsatz auf Multi-Gigabyte/s-Niveau.

### B. O_DIRECT Memory & Offset Auto-Alignment Guard
- **Ziel:** Automatische Erkennung fehlausgerichteter Puffer oder Offsets bei `--direct` und transparenter Fallback / Sektor-Padding, um `EINVAL` auf NVMe 4Kn-Laufwerken proaktiv abzufangen.

### C. Überlappende Asynchron-Pipeline für `io_uring` (Pipelined Double-Queue)
- **Ziel:** Überlappen von Read-SQE (Slot N+1) mit Write-SQE (Slot N) im Linux-Kernel, um Kontextwechsel noch weiter gegen 0 zu drücken.
