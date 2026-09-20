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
- [x] **SIMD-Vektorisierung für Endian-Byte-Swapping (`swab`):**
  `dd_swab_buffer()` in `src/conversions.c` via AVX2 `_mm256_shuffle_epi8` / SSSE3 `_mm_shuffle_epi8` vektorisiert (4-fach unrolled, 128 Bytes pro Iteration). Durchsatz stieg im Benchmark von 4,50 GB/s auf **13,50 GB/s (+200,0 %)**. In Regressionstest 27 mit 4-MB-Roundtrip verifiziert.
- [x] **O_DIRECT Alignment- & Fallback-Härtung:**
  Logische und physische Sektorgrößenerkennung (`BLKSSZGET` / `BLKPBSZGET`) in `src/io_engine.c`. Resilientes Handling unaligned Teilblöcke am Dateiende unter `--direct` durch temporäres Dropping des Flags für den Tail-Block mit anschließendem Cache-Evict (`posix_fadvise(DONTNEED)`). Automatischer Fallback auf Cache-Eviction beim Öffnen, falls das Dateisystem `O_DIRECT` nicht unterstützt (z. B. OverlayFS / ältere tmpfs). In Regressionstest 28 verifiziert.
- [x] **Überlappende Asynchron-Pipeline für `io_uring` (Pipelined Double-Queue):**
  Vollständig asynchrones Pipelining in `src/io_uring.c`: Überlappen von Read-Ahead SQEs (Slot N+1) mit Write SQEs (Slot N) in einem einzigen gebatchten `io_uring_submit()`-Syscall. Zero-Syscall-Loop mit atomarem CQE-Reaping, strikter Byteziel-Begrenzung (`-l`), On-the-Fly SHA-256 Digest und Resilienz gegen `-EINTR`.
- [x] **Auto-Engine-Heuristik & Shell-Autovervollständigung:**
  Dynamische Backend-Auswahl unter `ENGINE_AUTO` (priorisiert `reflink` für CoW-Klone und `uring` für Blockgeräte und `--direct`). Vollwertige Autocompletion-Skripte für Bash (`completions/bash/blkcp`) und Zsh (`completions/zsh/_blkcp`) inklusive `make install`/`uninstall` Targets.
- [x] **Hardware-beschleunigtes Streaming-Hashing (SHA-256):**
  Direkte Integration von hardwarebeschleunigtem OpenSSL SHA-256 (SHA-NI / AVX2) in `src/io_engine.c` und `src/io_uring.c` mit nativer Anzeige in `blkcp-tui`.

- [x] **Maschinenlesbare JSON-Telemetrie (`--json`):**
  Optionale strukturierte NDJSON-Fortschrittsausgabe für Skripte und CI/CD-Pipelines (`{"event": "progress", ...}` und `{"event": "finished", ...}`) mit Bytes, Prozent, Geschwindigkeit, ETA und Prüfsumme auf `stderr`. In Regressionstest 29 verifiziert.

---

## 2. Optionale zukünftige Erweiterungen

- **Dynamic Ringbuffer Scaling:**
  Adaptive Pufferanpassung im `io_async`-Treiber basierend auf NVMe/SSD Buslatenzen.
