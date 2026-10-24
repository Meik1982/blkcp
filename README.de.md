# blkcp (Block Copy: Next-Generation High-Performance Block Copy & Imaging Tool)

Eine eigenständige, modularisierte, durchsatzoptimierte und architektonisch entflochtene Neuentwicklung zur modernen Stream- und Block-Replikation. Ausgestattet mit moderner POSIX/GNU-CLI, Linux `io_uring` Asynchronous Streaming, In-Flight Autotuning, Zero-Copy Reflink (`copy_file_range`), Multi-Threaded Double-Buffering und Hardware-Sicherheitsfunktionen.

---

## 1. Herkunft & Lizenz des Quelltextes (Origin & Attributions)

* **Ursprung:** Basiert auf Kernkonzepten der GNU Coreutils (Version 9.5).
* **Upstream-Quellen:** 
  * Offizielles GNU-Repository: <https://git.savannah.gnu.org/git/coreutils.git>
  * GitHub-Mirror: <https://github.com/coreutils/coreutils>
* **Originalautoren:** Paul Rubin, David MacKenzie, Stuart Kemp und die Free Software Foundation, Inc.
* **Weiterentwicklung & Architektur:** Meik Augenblick (2026).
* **Lizenz:** GNU General Public License v3 oder neuer (GPLv3+). Siehe <https://gnu.org/licenses/gpl.html>.

---

## 2. Modulare Dual-Architektur (Rust Primary & C11 Reference)

* **Haupt-Binaries (`blkcp`, `blkcp-tui` in `rust/`):** Speichersicherer Rust-2021-Core & modulare 16-Modul-Ratatui-TUI (`tui_state.rs`, `tui_render.rs`, `tui_main.rs`) mit nativem Multi-Ring `io_uring`, `crossbeam-channel` Ringpuffer, `CachePadded` Telemetrie und parallelem BLKS-384 Merkle Combiner.
* **Referenz-Binaries (`blkcp-c`, `blkcp-tui-c` in `src/`):** Schlanke C11- und `ncursesw`-Referenzimplementierung bei 100 % CLI- und Test-Parität (42/42 Regressionstests).

Der historische Monolith wurde vollständig in getrennte, wartbare Subsysteme zerlegt:

```
rust/                           # Primärer Speichersicherer Rust v2.0.0 Workspace (blkcp & blkcp-tui)
├── src/
│   ├── main.rs / lib.rs        # CLI-Einstieg & ANSSI/Rust-2024 Safety-Gates
│   ├── args.rs                 # Moderner GNU/POSIX CLI- & Legacy-dd-Parser
│   ├── engine.rs               # Einheitliches Treiber-Dispatching, lock-free crossbeam Async-Pipeline, Reflink & Splice
│   ├── uring.rs                # Single-Ring & Sharded Multi-Ring io_uring + BLKS-384 Merkle Combiner
│   ├── safety.rs               # Target Safety Guard (/proc/mounts, /proc/swaps & rekursive LUKS/LVM-Holder)
│   ├── hash.rs                 # In-Flight SHA-256 (SHA-NI), BLAKE3 (SIMD) & BLKS-384 (Post-Quantum)
│   ├── aligned.rs              # RAII 4096-Byte Page-Aligned DMA-Puffer-Primitive für O_DIRECT
│   ├── swab.rs                 # 100 % Safe Rust LLVM-autovektorisiertes Endian-Byte-Swapping
│   ├── stats.rs                # CachePadded<AtomicU64> Telemetrie & NDJSON-Reporting
│   ├── tui_state.rs            # TUI-Zustandsmaschine, 8 Presets, Kombinations-Advisor & CLI-Generator
│   ├── tui_render.rs           # 16-Modul Ratatui-Layout, Live-Fortschrittsbalken & Modale Dialoge
│   ├── tui_device.rs           # Blockgeräte-/Mount-Scanner & interaktiver Datei-Browser
│   └── tui_main.rs             # Crossterm Raw-Mode & Event-Loop
└── crates/blks-core/           # Eingebettete 384-Bit Post-Quantum Merkle-Tree-Hash Engine (4-Way AVX2 + C-FFI)

src/                            # C11-Referenzimplementierung (blkcp-c & blkcp-tui-c)
├── blkcp.c / blkcp_config.h    # Schlanke Einstiegssteuerung & reentranter Kontext (dd_context_t)
├── io_uring.c / io_async.c     # C11 liburing- & Pthread-Ringpuffer-Treiber
├── io_reflink.c / io_splice.c  # C11 Zero-Copy Kernel-Treiber
└── tui/                        # C11 ncursesw 2D-Spatial TUI
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
* `--hash[=ALGO]`, `--sha256`, `--blake3`, `--blks`: Berechnet on-the-fly Streaming-Prüfsummen (`sha256`, `blake3` oder 384-Bit Post-Quantum `blks` in 64-Zeichen Base64)
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
* **Multi-Ring Sharding:** Mit `-j <N>` bzw. `--threads=<N>` skaliert `blkcp` über $N$ parallele Worker-Threads mit jeweils eigener `io_uring`-Instanz und dynamischem 64-MiB Work-Stealing für Multi-Queue-SSDs. Inklusive automatischer Fallbacks bei linearen Hashes (`--hash`) oder Pipes.

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

#### Nativer Schutz für autonome KI-Agenten (HOL Guard)
Ergänzend zu den internen Laufzeitprüfungen wird `blkcp` ab Werk offiziell durch [HOL Guard](https://github.com/hashgraph-online/hol-guard) (`command.blkcp`) geschützt. Autonome KI-Entwicklungsagenten (z. B. Claude Code, Codex, Hermes), die in durch HOL Guard abgesicherten Umgebungen agieren, werden bei potenziell destruktiven `blkcp`-Befehlen (wie direkten Schreibzugriffen auf `/dev/*`-Blockgeräte oder erzwungener `--force` / `-f`-Ausführung) vorab zur menschlichen Bestätigung angehalten.

### 5. On-the-Fly Streaming Checksummen (`--hash`, `--sha256`, `--blake3`, `--blks`)
Berechnet die kryptografische Prüfsumme direkt parallel zum Datentransfer im selben Durchlauf:
* **`--hash=sha256` (Standard):** FIPS-konform, bitgenau kompatibel mit `sha256sum`.
* **`--hash=blake3` / `--blake3`:** SIMD-beschleunigter Merkle-Tree-Hash via `libblake3` mit über 3 GB/s Durchsatz für High-End NVMe-Transfers zur Erkennung von Silent Data Corruption, 100 % kompatibel mit `b3sum`.
* **`--hash=blks` / `--blks`:** 384-Bit Post-Quantum Merkle-Tree-Hash via `blks` mit **192 Bit Kollisionssicherheit** in sauberen 64 Zeichen Base64 ohne Padding. Identische Zeilenbreite wie SHA-256, aber astronomisch höhere Krypto-Sicherheit bei Durchsätzen von über 5 GB/s. Beseitigt die Notwendigkeit eines zeitraubenden zweiten Verifikationsdurchgangs.

### 6. Gefahrloser Simulationsmodus (`--dry-run` / `-n`)
Prüft Quell- und Zielparameter, evaluiert den Target Safety Guard und gibt den exakten Ausführungsplan aus (optional als maschinenlesbares NDJSON via `--json`), ohne Daten zu schreiben oder Zielmedien zu verändern. Perfekt für System-Skripte und KI-Agenten:
```bash
blkcp -i backup.iso -o /dev/sdb -n
blkcp -i backup.iso -o /dev/sdb --dry-run --json
```

---

## 5. Interaktive 16-Modul-TUI (`blkcp-tui` & `blkcp-tui-c`)

Das Projekt beinhaltet das interaktive 16-Modul-Kombinations-Dashboard **`blkcp-tui`** (auf Basis von `ratatui` + `crossterm`, ergänzt durch die kompakte C11-`ncursesw`-Referenz **`blkcp-tui-c`**):
* **Visuelle Endpunkt- & Dateiauswahl (`[F]`):** Schnellauswahl für physische Laufwerke, Partitionen (`[P]`), virtuelle Geräte (`/dev/zero`, `/dev/null`) und interaktiver Verzeichnis-Browser.
* **16-Modul-Matrix & Direkt-Hotkeys:** Blockgröße (`[B]`), I/O-Engine (`[E]`), Krypto-Hash (`[H]`: `BLAKE3`, `BLKS-384`, `SHA-256`), Multi-Ring Shards (`[J]`), Queue-Depth (`[U]`), Direct I/O (`[D]`), No-Cache (`[C]`), Sparse (`[A]`), AVX2 Swab (`[W]`), Sync-Pad (`[Y]`), No-Truncate (`[T]`), Flush-Sync (`[G]`), Ignore-Err (`[M]`), Dry-Run (`[N]`) und Limits/Offsets (`[L]`).
* **8 Workflow-Presets per Tastendruck (`[1..8]`):**
  * `[1]` **NVMe-Uring:** `-e uring -j 4 -b 1M --direct --fdatasync`
  * `[2]` **Forensik+BLKS:** `-e async --blks --sync --noerror --fsync`
  * `[3]` **Sparse+BLAKE3:** `-e sync --sparse --blake3 -b 1M --fdatasync`
  * `[4]` **Reflink CoW:** `-e reflink -b 4M`
  * `[5]` **2G Speed-Benchmark:** `/dev/zero -> /dev/null -e uring -j 4 -l 2G`
  * `[6]` **Kernel Splice:** `-e splice -b 1M --fdatasync`
  * `[7]` **Direct-NoCache:** `-e uring -j 4 --direct --nocache -b 1M`
  * `[8]` **Cross-Arch Swab:** `-e async --swab --blake3 -b 1M`
* **Kombinations-Advisor & Auto-Resolve (`[Z]`):** Echtzeit-Prüfung auf Treiber-/Transformations-Konflikte mit automatischer Ein-Tasten-Bereinigung und Live-CLI-Vorschau.

---

## 6. Installation & Paketverwaltung

### Arch Linux / CachyOS / Manjaro
**Empfohlen (Sauber verwaltet über `pacman` mit PKGBUILD):**
```bash
git clone https://github.com/Meik1982/blkcp.git
cd blkcp/packaging/arch
makepkg -si
```
*Vorteil:* Das Paket wird vom Paketmanager `pacman` erfasst, sauber verwaltet und lässt sich jederzeit rückstandslos mit `sudo pacman -R blkcp` deinstallieren.

### Ubuntu / Debian
**Empfohlen (Fertiges `.deb`-Paket via `apt`):**
Lade `blkcp_2.0.0-1_amd64.deb` von den [GitHub Releases](https://github.com/Meik1982/blkcp/releases/tag/v2.0.0) herunter:
```bash
sudo apt install ./blkcp_2.0.0-1_amd64.deb
```

*(Oder selbst ein natives `.deb`-Paket aus dem Quellcode bauen):*
```bash
git clone https://github.com/Meik1982/blkcp.git
cd blkcp
sudo apt-get update && sudo apt-get install -y build-essential debhelper cargo rustc liburing-dev libssl-dev libncurses-dev dpkg-dev
dpkg-buildpackage -us -uc -b
sudo apt install ../blkcp_2.0.0-1_amd64.deb
```

### Fedora / RHEL / openSUSE
**Empfohlen (Fertiges `.rpm`-Paket via `dnf`):**
Lade `blkcp-2.0.0-1.fc44.x86_64.rpm` von den [GitHub Releases](https://github.com/Meik1982/blkcp/releases/tag/v2.0.0) herunter:
```bash
sudo dnf install ./blkcp-2.0.0-1.fc44.x86_64.rpm
```

### Manuelle Installation aus dem Quellcode (alle Distributionen):
```bash
cd blkcp
make release
sudo make install
```

---

## 7. Bauen, Auditieren, Testen & Benchmarking

Details zum Sicherheits- und Soundness-Audit (ANSSI & Rust-2024 Richtlinien, Inventar aller 7 `unsafe`-Blöcke) finden sich in **[SECURITY_AUDIT.de.md](SECURITY_AUDIT.de.md)** (englisch: [SECURITY_AUDIT.md](SECURITY_AUDIT.md)).

```bash
# Optimierter Release-Build (Rust blkcp/blkcp-tui + C11 blkcp-c/blkcp-tui-c):
make release

# 4-stufiges OSS Security & Soundness Audit (Clippy Safety Gate, Unsafe-Zählung = 7, RustSec/Deny, 18 Unit-Tests):
make audit

# Erweiterte End-to-End Regressionstest-Suite (42 Tests gegen Rust blkcp):
make test

# End-to-End Regressionstest-Suite gegen die C11-Referenz (42 Tests gegen blkcp-c):
make test-c

# Vergleichende Performance-Benchmarks (15 Szenarien):
./tests/benchmark_compare.sh

# Manpage einsehen:
make man

# Interaktiven TUI-Manager starten:
make tui
```
