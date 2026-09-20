# blkcp (Block Copy: Next-Generation High-Performance Block Copy & Imaging Tool)

Eine eigenständige, modularisierte, durchsatzoptimierte und architektonisch entflochtene Neuentwicklung zur modernen Stream- und Block-Replikation. Ausgestattet mit moderner POSIX/GNU-CLI, Linux `io_uring` Asynchronous Streaming, In-Flight Autotuning, Zero-Copy Reflink (`copy_file_range`), Multi-Threaded Double-Buffering und Hardware-Sicherheitsfunktionen.

---

## 1. Herkunft & Lizenz des Quelltextes (Origin & Attributions)

* **Ursprung:** Basiert auf Kernkonzepten der GNU Coreutils (Version 9.5).
* **Upstream-Quellen:** 
  * Offizielles GNU-Repository: <https://git.savannah.gnu.org/git/coreutils.git>
  * GitHub-Mirror: <https://github.com/coreutils/coreutils>
* **Originalautoren:** Paul Rubin, David MacKenzie, Stuart Kemp und die Free Software Foundation, Inc.
* **Weiterentwicklung & Architektur:** Meik (2026).
* **Lizenz:** GNU General Public License v3 oder neuer (GPLv3+). Siehe <https://gnu.org/licenses/gpl.html>.

---

## 2. Modulare Architektur

Der historische Monolith wurde vollständig in getrennte, wartbare Subsysteme zerlegt:

```
src/
├── blkcp.c                 # Schlanke Einstiegs- und Ablaufsteuerung
├── blkcp_config.h          # Kapselung von Zustand, Bitmasken & Konfiguration (dd_context_t)
├── args.h / .c            # Moderne CLI-Syntax (-i, -o, -b, -e, -l, -p, --hash) via getopt_long
├── io_driver.h            # Einheitliches Backend-Treiber-Interface (Strategy Pattern / Inversion of Control)
├── io_engine_internal.h   # Geteilte I/O-Primitive und Diagnose-Deklarationen
├── io_engine.h / .c       # Zentrale Stream-Orchestrierung, Skip/Seek, Safety Guard & Transfer-Loop
├── io_uring.c             # Linux io_uring Asynchronous I/O Treiber (Zero-Syscall Queue Pipeline)
├── io_sync.c              # Synchroner Block-I/O Treiber mit dynamischem Autotuning (-b auto)
├── io_async.c             # Multi-Threaded Double-Buffering Ringpuffer Pipeline (-e async)
├── io_reflink.c           # Linux Kernel-Space Zero-Copy Reflink Treiber (copy_file_range(2))
├── conversions.h / .c     # Zeichensatz- und Byte-Konvertierungen (SIMD-beschleunigt)
├── stats.h / .c           # Durchsatz-Telemetrie, Human-readable Formatierung & Live-Fortschritt
├── signals.h / .c         # Async-Signal-Handler (SIGINT-Cleanup, SIGUSR1-Reporting)
├── system.h               # POSIX-Systemschnittstellen, gettext & vektorisierter Nullblock-Check
└── version.c / .h         # Versionsidentifikation
```

---

## 3. Moderne CLI-Syntax & Beispiele

`blkcp` bricht mit den veralteten `key=value`-Operanden und bietet eine erstklassige, moderne Kommandozeilenschnittstelle:

### Schnelle Übersicht der Kernoptionen:
* `-i, --input <FILE>`: Eingabedatei oder Blockgerät (Default: `stdin`)
* `-o, --output <FILE>`: Ausgabedatei oder Blockgerät (Default: `stdout`)
* `-b, --block-size <SIZE>`: Blockgröße (z. B. `64K`, `4M`, `1G`); `-b auto` aktiviert dynamisches Autotuning
* `-e, --engine <NAME>`: Transfer-Engine: `uring` (Linux io_uring), `async` (Pthread-Ringpuffer), `reflink` (Kernel Zero-Copy), `splice` (Kernel Pipe-Splice), `sync` (synchron), `auto` (intelligente Auto-Erkennung)
* `-l, --limit <SIZE>` (auch `-s, --size`): Exakte Byte-Begrenzung entkoppelt von Blockgrößen
* `-c, --count <N>`: Anzahl der zu kopierenden Blöcke
* `-p, --progress`: Echtzeit-Durchsatzanzeige und Fortschrittsbalken
* `--json`: Maschinenlesbare NDJSON-Telemetrie auf `stderr` für CI/CD und Automatisierung
* `-q, --quiet`: Stiller Modus (nur fatale Fehlermeldungen)
* `-f, --force`: Schutzsperre gegen Überschreiben gemounteter Partitionen übersteuern
* `--hash`, `--sha256`: Berechnet on-the-fly die Streaming-SHA-256-Prüfsumme
* `--autotune`: Dynamisches Durchsatz-Autotuning
* `--queue-depth <N>`: Ringpuffer-Slotanzahl für die `async`-Engine (Standard: adaptive dynamische Skalierung)
* `--direct`: Direct I/O (`O_DIRECT`) unter Umgehung des OS Page-Caches
* `--nocache`: Durchsatzschonende Streaming-Cache-Eviction (`posix_fadvise(DONTNEED)` in 32-MiB-Chunks)
* `--skip <SIZE>`: Offset am Eingang überspringen
* `--seek <SIZE>`: Offset am Ausgang vor dem Schreiben anspringen
* `--sparse`: Nullblöcke als Sparse-Holes erzeugen

### Anwendungsbeispiele:

```bash
# 1. Asynchrones NVMe/Festplatten-Cloning mit io_uring und Live-Fortschritt:
blkcp -i /dev/nvme0n1 -o /dev/sdb -b 4M -e uring -p

# 2. Exakte Image-Größe schreiben mit automatischer Streaming-SHA-256-Verifikation:
blkcp -i image.raw -o /dev/sdc -l 10G -e uring --hash -p

# 3. Großes VM-Image instantan duplizieren via Kernel Zero-Copy Reflink:
blkcp -i ubuntu-vm.qcow2 -o ubuntu-vm-clone.qcow2 -e reflink -p

# 4. Dynamisches Durchsatz-Autotuning (findet die ideale Puffergröße selbst):
blkcp -i backup.iso -o /dev/sdd -b auto -p

# 5. Intuitive Positional Syntax:
blkcp source.bin destination.bin -e uring -p
```

---

## 4. Kern-Features im Detail

### 1. Linux `io_uring` Asynchronous Engine (`-e uring`)
Nutzt liburing für asynchrones, unterbrechungsfreies Double-Buffering direkt auf Kernel-Queue-Ebene. Minimiert Syscall-Overhead und Kontextwechsel für maximale Bus-Auslastung auf modernen NVMe-SSDs.

### 2. Multi-Threaded Double-Buffering Ringpuffer (`-e async`)
Entkoppelt Lesestrom und Schreibstrom über einen speichereffizienten POSIX-Ringpuffer. Verhindert, dass langsame Ausgabemedien (z. B. USB-Sticks) den Lesevorgang blockieren.

### 3. In-Kernel Zero-Copy & CoW Reflink-Cloning (`-e reflink`)
Nutzt unter Linux `copy_file_range(2)`:
* **Instantanes Klonen:** Auf CoW-Dateisystemen (Btrfs, XFS, ZFS) werden Abbilder in Millisekunden ohne zusätzlichen Speicherplatzbedarf erzeugt.
* **In-Kernel Zero-Copy:** Bei herkömmlichen Dateisystemen entfällt der Userspace-Pufferaufwand vollständig.

### 4. Target Safety Guard
Verhindert das versehentliche Zerstören laufender Betriebssystem-Installationen durch Tippfehler:
```bash
$ blkcp -i image.iso -o /dev/nvme0n1p2
blkcp: SAFETY GUARD: refusing to overwrite '/dev/nvme0n1p2' which contains mounted system path '/'.
Use '-f' or '--force' to override if intentional.
```

### 5. On-the-Fly Streaming SHA-256 Checksumme (`--hash` / `--sha256`)
Berechnet die kryptografische Prüfsumme direkt parallel zum Datentransfer im selben Durchlauf. Beseitigt die Notwendigkeit eines zeitraubenden zweiten Verifikationsdurchgangs.

---

## 5. Interaktive 2D-Spatial TUI (`blkcp-tui`)

Das Projekt beinhaltet einen vollwertigen Terminal-Assistenten auf Basis von `ncursesw` (`bin/blkcp-tui`):
* **Visuelle Stream-Auswahl:** Schnellauswahl für reguläre Dateien, physische Blockgeräte (`/sys/class/block`) und Pipes (`|`).
* **Engine-Umschaltung im Flug:** Auswahl zwischen `io_uring`, `async`, `reflink`, `sync` und `auto` via Leertaste.
* **Direktes Byte-Limit & Parameter:** Intuitive Konfiguration von Limits (`-l`), Blockgrößen (`-b`), Skip/Seek und Direkt-I/O (`--direct`, `--sparse`, `--sync`).
* **Live-Telemetrie & Streaming-Hash:** Echtzeit-Fortschrittsbalken, Transferrate und Anzeige des Streaming-SHA-256-Hashes direkt im TUI-Fenster.
* **Clipboard-Integration:** Exportiert den exakt generierten CLI-Befehl auf Tastendruck (`c`) direkt in die Wayland-/X11-Zwischenablage.
* **Sicherheits-Dialog:** Bestätigungsabfrage vor Schreibzugriffen auf physische Datenträger.

---

## 6. Bauen, Testen & Benchmarking

```bash
# Debug-/Entwicklungsbuild:
make all

# Optimierter Release-Build (-O3, -flto, vollständig gestrippt):
make release

# Erweiterte Regressionstest-Suite (31 Tests):
make test

# Vergleichende Performance-Benchmarks (15 Szenarien):
./tests/benchmark_compare.sh

# Manpage einsehen:
make man

# Interaktiven TUI-Manager starten:
make tui
```
