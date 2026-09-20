# TODO & Zukünftige Optimierungspotenziale für `blkcp`

Dieses Dokument erfasst den aktuellen Umsetzungsstatus und die nächsten priorisierten Aufgaben für `blkcp`.

---

## 1. Abgeschlossene Meilensteine

- [x] **Modularisierung & Architektur-Entflechtung:**
  GNU `dd.c`-Monolith vollständig in modulare Subsysteme zerlegt. Saubere Driver-Architektur (`dd_io_driver_t`: `sync`, `async`, `reflink`, `io_uring`, `splice`) mit genau einer zentralen Kontrollschleife in `src/io_engine.c`.
- [x] **Radikale Beseitigung aller historischen `dd`-Altlasten:**
  Vollständiges Entfernen des historischen `key=value`-Scanners (`scan_dd_arguments`, `if=`, `of=`, `ibs=`, `obs=`, `cbs=`, `conv=...`, `iflag=...`, `oflag=...`, `status=...`). Über 780 Zeilen Altlasten-Code ersatzlos gestrichen. Restloses Löschen von Lochkarten- und Mainframe-Relikten (EBCDIC/IBM-Codepages, 80-Spalten-`block`/`unblock`, Gross-/Kleinschreibungstranslationen). Etablierung eines reinen, standardisierten `getopt_long`-Parsers mit Positionsargumenten (`blkcp [OPTIONS] SOURCE DEST`). Dateinamen mit `=` sind nun 100 % sicher nutzbar.
- [x] **Linux `io_uring` Asynchronous Engine (`src/io_uring.c`):**
  Nativer `liburing`-Treiber mit Double-Buffering Ring-Pipeline, Signal-Resilienz (`EINTR`) und Fallback.
- [x] **In-Kernel Zero-Copy Reflink (`src/io_reflink.c`):**
  Unterstützung für `copy_file_range(2)` mit CoW-Klonen auf Btrfs/XFS/ZFS.
- [x] **Multi-Threaded Async Double-Buffering (`src/io_async.c`):**
  Entkoppelte Ringpuffer-Pipeline via `pthread` (+50 % Durchsatz bei File-to-File).
- [x] **In-Flight Autotuning (`-b auto` / `--autotune`):**
  Dynamische Blockgrößen-Skalierung (Hardware-Awareness via `ioctl(BLKPBSZGET)`).
- [x] **Moderne POSIX/GNU-CLI (`src/args.c`):**
  `-i`, `-o`, `-b`, `-e`, `-l`, `-p`, `-q`, `-f`, `--hash`, `--autotune`, `--direct`, `--sparse`, `--sync`, `--swab`, `--noerror` und intuitive Positionsargumente.
- [x] **Interaktive 2D-Spatial TUI (`src/tui/` - `blkcp-tui`):**
  Vollständiger ncursesw-Assistent mit Engine-Selector, Byteziel, Live-Telemetrie und Clipboard-Export.
- [x] **Target Safety Guard & Streaming SHA-256:**
  Schutz gegen versehentliches Überschreiben gemounteter Partitionen; parallele Prüfsummenberechnung im Hot-Loop.
- [x] **Qualitätssicherung & Dokumentation:**
  31/31 Regressionstests grün (`make test`, ASan/UBSan, TSan), 14 automatisierte Benchmarks, Doxygen in allen Headern, Manpage `man/blkcp.1`.
- [x] **SIMD-Vektorisierung für Endian-Byte-Swapping (`swab`):**
  `dd_swab_buffer()` in `src/conversions.c` via AVX2 `_mm256_shuffle_epi8` / SSSE3 `_mm_shuffle_epi8` vektorisiert (4-fach unrolled, 128 Bytes pro Iteration). Durchsatz stieg im Benchmark von 4,50 GB/s auf **13,50 GB/s (+200,0 %)**. In Regressionstest 27 mit 4-MB-Roundtrip verifiziert.
- [x] **O_DIRECT Alignment- & Fallback-Härtung:**
  Logische und physische Sektorgrößenerkennung (`BLKSSZGET` / `BLKPBSZGET`) in `src/io_engine.c`. Resilientes Handling unaligned Teilblöcke am Dateiende unter `--direct` durch temporäres Dropping des Flags für den Tail-Block mit anschließendem Cache-Evict (`posix_fadvise(DONTNEED)`). Automatischer Fallback auf Cache-Eviction beim Öffnen, falls das Dateisystem `O_DIRECT` nicht unterstützt. In Regressionstest 28 verifiziert.
- [x] **Überlappende Asynchron-Pipeline für `io_uring` (Pipelined Double-Queue):**
  Vollständig asynchrones Pipelining in `src/io_uring.c`: Überlappen von Read-Ahead SQEs (Slot N+1) mit Write SQEs (Slot N) in einem einzigen gebatchten `io_uring_submit()`-Syscall. Zero-Syscall-Loop mit atomarem CQE-Reaping, strikter Byteziel-Begrenzung (`-l`), On-the-Fly SHA-256 Digest und Resilienz gegen `-EINTR`.
- [x] **Auto-Engine-Heuristik & Shell-Autovervollständigung:**
  Dynamische Backend-Auswahl unter `ENGINE_AUTO` (priorisiert `reflink` für CoW-Klone und `uring` für Blockgeräte und `--direct`). Vollwertige Autocompletion-Skripte für Bash (`completions/bash/blkcp`) und Zsh (`completions/zsh/_blkcp`) inklusive `make install`/`uninstall` Targets.
- [x] **Hardware-beschleunigtes Streaming-Hashing (SHA-256):**
  Direkte Integration von hardwarebeschleunigtem OpenSSL SHA-256 (SHA-NI / AVX2) in `src/io_engine.c` und `src/io_uring.c` mit nativer Anzeige in `blkcp-tui`.
- [x] **Maschinenlesbare JSON-Telemetrie (`--json`):**
  Optionale strukturierte NDJSON-Fortschrittsausgabe für Skripte und CI/CD-Pipelines (`{"event": "progress", ...}` und `{"event": "finished", ...}`) mit Bytes, Prozent, Geschwindigkeit, ETA und Prüfsumme auf `stderr`. In Regressionstest 29 verifiziert.
- [x] **Dynamic Ringbuffer Scaling (`--queue-depth`):**
  Adaptive Pufferanpassung im `io_async`-Treiber basierend auf Blockgröße und Ziel-Puffervolumen (32 MiB Working-Set, 4 bis 128 Slots). Manuelle Konfigurierbarkeit via `--queue-depth=N` / `--async-queue=N` (2 bis 1024 Slots), Graceful-Degradation-Fallback bei Speicherdruck und Erfassung von Reader-/Writer-Stalls in der NDJSON-Telemetrie. In Regressionstest 30 verifiziert.
- [x] **Fixed-Buffer Pre-Registration (`io_uring`):**
  Registrierung der Ringpuffer über `io_uring_register_buffers()` im `io_uring`-Treiber mit `io_uring_prep_read_fixed()` und `io_uring_prep_write_fixed()`. Beseitigt Page-Pinning (`get_user_pages`) und Kernel-Mapping-Overheads vollständig mit automatischem Fallback.
- [x] **In-Kernel Zero-Copy Splice Engine (`-e splice` / `splice(2)`):**
  Dedizierter Linux-Treiber `src/io_splice.c` für Pipes, FIFOs und Streams (`SPLICE_F_MOVE | SPLICE_F_MORE`). Beinhaltet Double-Splice über interne Kernel-Ringpuffer für File-to-File Transfers und nahtlosen Fallback bei FS-Inkompatibilitäten. In Regressionstest 31 verifiziert.
- [x] **TUI-Assistent Erweiterungen (`blkcp-tui`):**
  Unterstützung der `splice`-Engine, interaktives Einstellen der `--queue-depth` und Umschaltung auf NDJSON-Telemetrie (`--json`) inklusive 2D-Spatial-Tastaturnavigation.

---

## 2. Nächste anstehende Optimierungen & Erweiterungen

### Priorität 1: Durchsatz- & Hot-Path-Optimierungen

- [ ] **Hot-Path vDSO-Clock-Drosselung (`src/stats.c` / `src/io_engine.c`):**
  - **Problem:** Bei kleinen Blöcken (z. B. 4 KB oder 512 B) ruft `dd_check_progress(ctx)` in jedem Schleifendurchlauf `gethrxtime()` (`clock_gettime(CLOCK_MONOTONIC)`) auf. Bei hohen IOPS-Raten (z. B. 500.000 Blöcke/s) führt das zu 500.000 vDSO-Aufrufen pro Sekunde, was messbare CPU-Zyklen kostet.
  - **Lösung:** Throttling der Clock-Abfrage über einen Iterationszähler (z. B. `if (++counter >= 64)`) oder eine Byte-Schwelle (mindestens 64 KiB transferiert seit dem letzten Check). Reduziert vDSO-Overhead um ~98,4 %, während die 1-Hz-Aktualisierung der Statusausgabe völlig unberührt bleibt.

- [ ] **Kanonische Vereinheitlichung des Blockgrößen-Modells (`src/blkcp_config.h`):**
  - **Problem:** Trotz des Wegfalls historischer `ibs=`/`obs=`-Tape-Blockgrößen existieren in `dd_config_t` weiterhin zwei separate Felder (`input_blocksize` und `output_blocksize`), was zu redundanten Doppelzuweisungen im gesamten Code führt.
  - **Lösung:** Vereinheitlichung auf ein kanonisches `size_t blocksize` in `dd_config_t` mit Treiber-Kompatibilitäts-Aliassen oder Getter-Makros. Bereinigt die Datenstrukturen und verhindert inkonsistente Blockgrößenzustände.

### Priorität 2: I/O-Engine Pipelining & Cache-Verwaltung

- [ ] **Asynchrones CQE-Harvesting & Read-Ahead Pipelining (`src/io_uring.c`):**
  - **Problem:** Aktuell arbeitet `io_uring` im Lock-Step-Verfahren (Write N und Read N+1 werden submitted, danach blockiert der Treiber mit `io_uring_wait_cqe`, bis *beide* Operationen abgeschlossen sind). Das begrenzt den Durchsatz auf die langsamere der beiden Operationen plus Synchronisations-Overhead.
  - **Lösung:** Entkopplung über `io_uring_peek_batch_cqe()`: Bei seekbaren Zielen bis zu 4 Reads im Voraus queuen (`read_in_flight <= 4`). Sobald ein Read fertig ist, sofort den Write submittieren und den frei gewordenen Slot mit dem nächsten Read-Ahead belegen, ohne auf den vorherigen Write zu warten.

- [ ] **Streaming Cache-Eviction (`--nocache` / `posix_fadvise`):**
  - **Problem:** Bei extrem großen Transfers (50–500 GB) ohne `--direct` füllt der Kernel den gesamten Systemspeicher mit Dirty Page Cache Pages, was das Gesamtsystem träge machen kann.
  - **Lösung:** Periodisches Aufrufen von `posix_fadvise(fd, offset, len, POSIX_FADV_DONTNEED)` in Streaming-Intervallen (z. B. alle 32–64 MiB), um durchströmte Puffer gezielt aus dem Page Cache zu verwerfen.

### Priorität 3: Erweiterte Systemsicherheit & Multi-Queue Skalierung

- [ ] **Swap- & Device-Mapper-Schutz im Target Safety Guard (`src/io_engine.c`):**
  - **Problem:** Der Safety Guard prüft derzeit aktive Dateisystem-Mounts via `/proc/mounts`. Aktive Swap-Partitionen oder LUKS/LVM-Header ohne direktes Mounten werden dabei nicht automatisch abgefangen.
  - **Lösung:** Auswertung von `/proc/swaps` und `dm-crypt`/`dm-mod`-Status vor Schreibzugriffen, um ein versehentliches Überschreiben aktiver Swap-Bereiche zu verhindern.

- [ ] **Multi-Ring io_uring Sharding:**
  - Paralleles Sharding mehrerer Submission-Rings über dedizierte CPU-Cores bei High-End Multi-Queue NVMe-Controllern.
