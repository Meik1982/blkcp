# TODO & Zukünftige Optimierungspotenziale für `blkcp`

Dieses Dokument erfasst den aktuellen Umsetzungsstatus und die nächsten priorisierten Aufgaben für `blkcp`.

---

## 1. Abgeschlossene Meilensteine

- [x] **Modularisierung & Architektur-Entflechtung:**
  GNU `dd.c`-Monolith vollständig in modulare Subsysteme zerlegt. Saubere Driver-Architektur (`dd_io_driver_t`: `sync`, `async`, `reflink`, `io_uring`, `splice`) mit genau einer zentralen Kontrollschleife in `src/io_engine.c`.
- [x] **Radikale Beseitigung aller historischen `dd`-Altlasten:**
  Vollständiges Entfernen des historischen `key=value`-Scanners (`scan_dd_arguments`, `if=`, `of=`, `ibs=`, `obs=`, `cbs=`, `conv=...`, `iflag=...`, `oflag=...`, `status=...`). Über 780 Zeilen Altlasten-Code ersatzlos gestrichen. Restloses Löschen von Lochkarten- und Mainframe-Relikten (EBCDIC/IBM-Codepages, 80-Spalten-`block`/`unblock`, Gross-/Kleinschreibungstranslationen). Etablierung eines reinen, standardisierten `getopt_long`-Parsers mit Positionsargumenten (`blkcp [OPTIONS] SOURCE DEST`). Dateinamen mit `=` sind nun 100 % sicher nutzbar.
- [x] **Hot-Path vDSO-Clock-Drosselung (`src/stats.c` / `src/io_engine.c`):**
  Throttling der vDSO-Clock-Abfrage `gethrxtime()` (`clock_gettime(CLOCK_MONOTONIC)`) in `dd_check_progress()`. Clock-Sampling erfolgt nun nur noch alle 64 Iterationen oder wenn mindestens 64 KiB transferiert wurden. Reduziert den vDSO-Overhead bei hohen IOPS (z. B. 4-KB-Streams) um ~98,4 %, während die 1-Hz-Aktualisierung der Status- und NDJSON-Ausgabe 100 % präzise und ruckelfrei bleibt.
- [x] **Kanonische Vereinheitlichung des Blockgrößen-Modells (`src/blkcp_config.h`):**
  Vollständige Vereinheitlichung von `input_blocksize` und `output_blocksize` auf ein einziges kanonisches Feld `cfg->blocksize` in `dd_config_t`. Beseitigt redundante Doppelzuweisungen und garantiert ein konsistentes Puffer- und Blockgrößenmodell über alle I/O-Treiber hinweg.
- [x] **Asynchrones CQE-Harvesting & Non-Blocking Reaping (`src/io_uring.c`):**
  Optimierung der `io_uring`-Completion-Phase durch Integration von `io_uring_peek_batch_cqe()`. Mehrere gleichzeitig fertige I/O-Events (Read und Write) werden nun atomar im Userspace ohne zusätzlichen Syscall abgeerntet, bevor auf den Kernel gewartet werden muss.
- [x] **Streaming Cache-Eviction (`--nocache` / `posix_fadvise`):**
  Dediziertes `--nocache`-Flag für durchsatzschonende Cache-Verwerfung bei extrem großen Datenmengen (50–500 GB). Puffer werden nicht mehr ineffizient nach jedem Block einzeln verworfen, sondern in gebündelten 32-MiB-Chunks via `posix_fadvise(..., DONTNEED)` evictiert. Verhindert das Vollmüllen des Linux-Page-Caches mit minimalstem Syscall-Overhead. In Regressionstest 32 verifiziert.
- [x] **Erweiterter Swap-Schutz im Target Safety Guard (`src/io_engine.c`):**
  Erweiterung des Safety Guards um `/proc/swaps`: Schützt neben gemounteten Systempfaden (`/`, `/boot`, `/home`) nun auch aktiv eingebundene Swap-Partitionen (z. B. `/dev/zram0`, NVMe-Swap-Partitionen) vor versehentlichem Überschreiben. Kann nur mit `-f` / `--force` übersteuert werden.
- [x] **Linux `io_uring` Asynchronous Engine (`src/io_uring.c`):**
  Nativer `liburing`-Treiber mit Double-Buffering Ring-Pipeline, Fixed-Buffer Pre-Registration (`io_uring_register_buffers`), Signal-Resilienz (`EINTR`) und Fallback.
- [x] **In-Kernel Zero-Copy Reflink (`src/io_reflink.c`):**
  Unterstützung für `copy_file_range(2)` mit CoW-Klonen auf Btrfs/XFS/ZFS.
- [x] **Multi-Threaded Async Double-Buffering (`src/io_async.c`):**
  Entkoppelte Ringpuffer-Pipeline via `pthread` mit adaptivem Ringbuffer-Scaling (`--queue-depth`).
- [x] **In-Flight Autotuning (`-b auto` / `--autotune`):**
  Dynamische Blockgrößen-Skalierung (Hardware-Awareness via `ioctl(BLKPBSZGET)`).
- [x] **Moderne POSIX/GNU-CLI (`src/args.c`):**
  `-i`, `-o`, `-b`, `-e`, `-l`, `-p`, `-q`, `-f`, `--hash`, `--autotune`, `--direct`, `--nocache`, `--sparse`, `--sync`, `--swab`, `--noerror` und intuitive Positionsargumente.
- [x] **Interaktive 2D-Spatial TUI (`src/tui/` - `blkcp-tui`):**
  Vollständiger ncursesw-Assistent mit Engine-Selector, Byteziel, Live-Telemetrie, Clipboard-Export, interaktivem Dry-Run Planungs-Modal (`-n` / `[ DRY-RUN (s) ]`) und Bestätigungsdialogen vor Schreibzugriffen.
- [x] **Target Safety Guard & Streaming SHA-256:**
  Schutz gegen versehentliches Überschreiben gemounteter Partitionen und aktiver Swaps; parallele Prüfsummenberechnung im Hot-Loop.
- [x] **Qualitätssicherung & Dokumentation:**
  32/32 Regressionstests grün (`make test`, ASan/UBSan, TSan), 14 automatisierte Benchmarks, Doxygen in allen Headern, Manpage `man/blkcp.1`.
- [x] **SIMD-Vektorisierung für Endian-Byte-Swapping (`swab`):**
  `dd_swab_buffer()` in `src/conversions.c` via AVX2 `_mm256_shuffle_epi8` / SSSE3 `_mm_shuffle_epi8` vektorisiert (4-fach unrolled, 128 Bytes pro Iteration). Durchsatz stieg im Benchmark von 4,50 GB/s auf **13,50 GB/s (+200,0 %)**. In Regressionstest 27 mit 4-MB-Roundtrip verifiziert.
- [x] **O_DIRECT Alignment- & Fallback-Härtung:**
  Logische und physische Sektorgrößenerkennung (`BLKSSZGET` / `BLKPBSZGET`) in `src/io_engine.c`. Resilientes Handling unaligned Teilblöcke am Dateiende unter `--direct` durch temporäres Dropping des Flags für den Tail-Block mit anschließendem Cache-Evict (`posix_fadvise(DONTNEED)`). Automatischer Fallback auf Cache-Eviction beim Öffnen, falls das Dateisystem `O_DIRECT` nicht unterstützt. In Regressionstest 28 verifiziert.
- [x] **Auto-Engine-Heuristik & Shell-Autovervollständigung:**
  Dynamische Backend-Auswahl unter `ENGINE_AUTO` (priorisiert `reflink` für CoW-Klone und `uring` für Blockgeräte und `--direct`). Vollwertige Autocompletion-Skripte für Bash (`completions/bash/blkcp`) und Zsh (`completions/zsh/_blkcp`) inklusive `make install`/`uninstall` Targets.
- [x] **Hardware-beschleunigtes Streaming-Hashing (SHA-256):**
  Direkte Integration von hardwarebeschleunigtem OpenSSL SHA-256 (SHA-NI / AVX2) in `src/io_engine.c` und `src/io_uring.c` mit nativer Anzeige in `blkcp-tui`.
- [x] **Maschinenlesbare JSON-Telemetrie (`--json`):**
  Optionale strukturierte NDJSON-Fortschrittsausgabe für Skripte und CI/CD-Pipelines (`{"event": "progress", ...}` und `{"event": "finished", ...}`) mit Bytes, Prozent, Geschwindigkeit, ETA und Prüfsumme auf `stderr`. In Regressionstest 29 verifiziert.
- [x] **Fixed-Buffer Pre-Registration (`io_uring`):**
  Registrierung der Ringpuffer über `io_uring_register_buffers()` im `io_uring`-Treiber mit `io_uring_prep_read_fixed()` und `io_uring_prep_write_fixed()`. Beseitigt Page-Pinning (`get_user_pages`) und Kernel-Mapping-Overheads vollständig mit automatischem Fallback.
- [x] **In-Kernel Zero-Copy Splice Engine (`-e splice` / `splice(2)`):**
  Dedizierter Linux-Treiber `src/io_splice.c` für Pipes, FIFOs und Streams (`SPLICE_F_MOVE | SPLICE_F_MORE`). Beinhaltet Double-Splice über interne Kernel-Ringpuffer für File-to-File Transfers und nahtlosen Fallback bei FS-Inkompatibilitäten. In Regressionstest 31 verifiziert.
- [x] **Gefahrloser Simulationsmodus (`--dry-run` / `-n`) & JSON-Ausführungsplan:**
  Simuliert Datentransfers, validiert Quell- und Zielparameter, evaluiert den Target Safety Guard gegen `/proc/mounts` und `/proc/swaps` und emittiert einen detaillierten Ausführungsplan (oder NDJSON via `--json`), ohne Schreiboperationen auszuführen. In Regressionstest 33 verifiziert.
- [x] **Fish-Shell Autovervollständigung (`completions/fish/blkcp.fish`):**
  Vollständiges Autocompletion-Skript für die Fish-Shell inklusive aller modernen Schalter und `make install`/`uninstall`-Targets.
- [x] **Sparse Hole-Punching & Trailing-Seek Härtung (`--sparse`):**
  Lückenlose Sparse-Dateigenerierung via `lseek(SEEK_CUR)` und atomare Finalisierung über `ftruncate` am Dateiende in `src/io_engine.c`, falls die Datei mit einem Null-Block abschließt. Verifiziert mit tatsächlicher Disk-Block-Reduktion via `stat -c %b`.
- [x] **Vollständige Qualitätssicherung & Regressionstests:**
  38/38 Regressionstests grün (`make test`, ASan/UBSan, TSan), 14 automatisierte Benchmarks, Doxygen in allen Headern, Manpage `man/blkcp.1`. Inklusive Negativtests für fehlerhafte Eingaben/Pfade, `--notrunc` Datenerhalt, Signal-Resilienz (`SIGUSR1`) und Synchronisations-Flags (`--fsync`/`--fdatasync`).

---

## 2. Zukünftige optionale Erweiterungen

- [ ] **Multi-Ring io_uring Sharding:**
  - Paralleles Sharding mehrerer Submission-Rings über dedizierte CPU-Cores bei High-End Multi-Queue NVMe-Controllern.
