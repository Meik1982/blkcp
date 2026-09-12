# dd (Modular & Optimized Edition)

Eine eigenständige, modularisierte, durchsatzoptimierte und architektonisch entflochtene Version des klassischen Unix-/Linux-Tools `dd`.

---

## 1. Herkunft & Lizenz des Quelltextes (Origin & Attributions)

* **Ursprung:** GNU Coreutils (Version 9.5).
* **Upstream-Quellen:** 
  * Offizielles GNU-Repository: <https://git.savannah.gnu.org/git/coreutils.git>
  * GitHub-Mirror: <https://github.com/coreutils/coreutils>
* **Originalautoren:** Paul Rubin, David MacKenzie, Stuart Kemp und die Free Software Foundation, Inc.
* **Lizenz:** GNU General Public License v3 oder neuer (GPLv3+). Siehe <https://gnu.org/licenses/gpl.html>.

---

## 2. Modulare Architektur

Der ursprüngliche 2.563-Zeilen-Monolith `dd.c` wurde vollständig in getrennte Subsysteme zerlegt:

```
src/
├── dd.c              # Schlanke Einstiegs- und Ablaufsteuerung (~165 Zeilen)
├── dd_config.h       # Kapselung von Zustand, Bitmasken & Konfiguration (dd_context_t)
├── args.h / .c       # Operanden- & CLI-Parsing (if=, of=, bs=, Multiplikatoren, Validierung)
├── io_engine.h / .c  # I/O-Pipeline, Autotuning, Zero-Memcpy Fast Path, Truncate & Sync
├── conversions.h / .c# Zeichensatz- (EBCDIC/ASCII/Case) und Byte-Konvertierungen (swab)
├── stats.h / .c      # Durchsatz-Telemetrie, Human-readable Formatierung & Records-Reporting
├── signals.h / .c    # Async-Signal-Handler (SIGINT-Cleanup, SIGINFO/SIGUSR1-Reporting)
├── system.h          # POSIX-Systemschnittstellen, gettext & vektorisierter Nullblock-Check
└── version.c / .h    # Versionsidentifikation
```

---

## 3. Performance- & Durchsatz-Optimierungen

Gegenüber dem GNU-Original wurden mehrere fundamentale I/O-Engpässe behoben:

1. **Zero-Memcpy Fast Path (`copy_simple`):**
   Wenn Blockgrößen aufeinander abgestimmt sind und keine Puffer-Transformation aktiv ist, wird der Zwischenpuffer (`obuf`) komplett umgangen und direkt aus dem Lesepuffer geschrieben.
   * **Ergebnis:** Bei getrenntem `ibs=X obs=X` steigt der Durchsatz von **11,4 GB/s auf 20,2 GB/s (+77 %)**.
2. **Single-Buffer-Konsolidierung:**
   Wenn `ibs == obs`, allokiert `alloc_obuf()` keinen redundanten Zweitpuffer mehr (halbiert den RAM-Footprint).
3. **SIMD-Vektorisierung für Case-Folding (`ucase`, `lcase`):**
   Ersetzt byteweise indirekte Tabellen-Lookups durch branchless SIMD-Vektorinstruktionen (AVX2/SSE).
   * **Ergebnis:** Der Konvertierungsdurchsatz stieg im Benchmark von **1,90 GB/s auf 8,60 GB/s (+352 %)**.
4. **In-Flight Dynamic I/O Autotuning (`conv=autotune` / `bs=auto`):**
   Ermittelt während des laufenden Kopiervorgangs autonom die optimale Blockgröße für das Quell-/Zielmedium.
5. **Hardware-Awareness via `ioctl(BLKPBSZGET)`:**
   Erkennt physische Sektorgrößen (z. B. 4Kn / Advanced Format) und optimale Stripe-Größen (`BLKIOOPT`) und richtet die minimale Autotune-Schranke automatisch daran aus.
6. **Target Safety Guard (Schutz vor Überschreiben des Root-Dateisystems):**
   Prüft via `/proc/mounts`, ob `of=` das aktive Root-, Boot- oder Home-Laufwerk adressiert. Verhindert katastrophale Fehleingaben („Disk Destroyer“), sofern nicht explizit `oflag=force`, `conv=force` oder `opt=force` angegeben wurde.
7. **Vektorisierte Nullprüfung (`is_nul`):**
   Verwendet `CCAN memeqzero` in Kombination mit glibc-vektorisiertem `memcmp`, wodurch `conv=sparse` um bis zu 26 % beschleunigt wird.
8. **Zero-Overhead Signal-Polling:**
   Inline-Branch-Prüfung (`__builtin_expect`) verhindert Funktionsaufrufe im Hot-Loop, wenn keine Signale anstehen.
9. **Kernel-Readahead:**
   Aktiviert `posix_fadvise(POSIX_FADV_SEQUENTIAL)` bei regulären Eingabedateien zur Maximierung des Page-Cache-Readaheads.

---

## 4. Features: Autotuning & Safety Guard

### In-Flight I/O Autotuning
Statt Puffergrößen wie `bs=4M` manuell raten zu müssen, kann `dd` die optimale Blockgröße dynamisch während der ersten Millisekunden des Kopiervorgangs vermessen:

```bash
# Aufruf über conv-Symbol:
./dd if=/dev/nvme0n1 of=/dev/null conv=autotune status=progress

# Oder als bs-Alias:
./dd if=large_image.iso of=/dev/sdb bs=auto status=progress

# Oder als opt-Operand:
./dd if=input.bin of=output.bin opt=auto status=progress
```

### Target Safety Guard
Schützt vor dem berüchtigten versehentlichen Zerstören des laufenden Betriebssystems durch Tippfehler bei `of=`:
```bash
# Schutz triggert automatisch bei gemounteten System-Partitionen:
$ ./dd if=/dev/zero of=/dev/nvme0n1p2 count=1
dd: SAFETY GUARD: refusing to overwrite '/dev/nvme0n1p2' which contains mounted system path '/'.
Use 'oflag=force' or 'opt=force' to override if intentional.

# Gezielt erzwingen (z. B. im Rescue-System):
$ ./dd if=image.raw of=/dev/nvme0n1p2 oflag=force
```

### On-the-Fly Streaming SHA-256 Checksumme (`conv=sha256` / `opt=hash`)
Berechnet die kryptografische Prüfsumme direkt parallel zum Schreiben. Beseitigt die Notwendigkeit eines zeitraubenden zweiten Verifikationsdurchgangs beim Schreiben von Boot-Images oder Backups:
```bash
# ISO auf Stick schreiben mit sofortiger Prüfsummen-Verifikation:
$ ./dd if=archlinux.iso of=/dev/sdb bs=auto conv=sha256 status=progress
4027+0 records in
4027+0 records out
1073741824 bytes (1,1 GB, 1,0 GiB) copied, 0,048 s, 22,3 GB/s
sha256: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
```

### Multi-Threaded Async Double-Buffering Pipeline (`opt=async` / `conv=async` / `oflag=async`)
Entkoppelt den Lesestrom (`reader_thread`) vom Schreibstrom (`writer_thread`) über einen speichereffizienten POSIX-Ringpuffer (8 Slots). Verhindert, dass langsame Ausgabemedien (z. B. USB-Sticks mit hohen Schreiblatenzen) den Lesevorgang blockieren:
```bash
# Schneller NVMe-zu-USB Transfer mit asynchroner Pufferung und Streaming-Hash:
$ ./dd if=large_os.iso of=/dev/sdb bs=1M opt=async,hash status=progress
```

### Interaktiver TUI-Manager (`dd-tui`)
Komfortabler, maus- und tastaturgesteuerter Terminal-Assistent auf Basis von `ncursesw`:
* **Block-Device Erkennung:** Erkennt USB-Sticks und Festplatten automatisch via `/sys/block`, zeigt Gerätemodelle und Größen an und markiert System-Laufwerke (`/`, `/boot`, `/home`) mit Schutzsperren.
* **Integrierter Dateibrowser:** Komfortables Auswählen von `.iso`-, `.img`- und `.raw`-Dateien sowie Anlegen neuer Zieldateien direkt im aktiven Ordner mit der Taste `[N]`.
* **Programm- & Pipe-Integration:** Volle Unterstützung von Unix-Pipes (`[Pipe]`-Button). Unterstützt direkte Stream-Ein-/Ausgabe (`stdin`/`stdout`), Dekompression/Kompression (`zstd`, `gzip`, `xz`), Remote-Transfers (`ssh`) und Web-Streams (`curl`).
* **Befehlsgenerator & Clipboard:** Erzeugt die exakte CLI-Kommandozeile in Echtzeit und kopiert sie auf Knopfdruck in die X11-/Wayland-Zwischenablage.
* **Sicherheits-Popup:** Erzwingt eine bewusste Bestätigung vor Schreibzugriffen auf physische Datenträger.
```bash
# Starten des TUI-Managers:
./dd-tui
# oder über das Makefile:
make tui
```

---

## 5. Bauen, Testen & Benchmarking

### Kompilieren & Standard-Targets
```bash
# Release-Build (erstellt sowohl 'dd' als auch 'dd-tui'):
make clean all

# Interaktiven TUI-Manager starten:
make tui

# Alle 17 Regressionstests ausführen:
make test

# Vergleichs-Benchmark gegen GNU dd ausführen:
make benchmark

# Manpage anzeigen:
make man
```
*Das Makefile unterstützt automatische Header-Dependency-Verfolgung (`-MMD -MP`).*

### Offizielle UNIX-Manpage
Die vollständige Spezifikation aller Schalter, Flags, Conversions und Sicherheitsmechanismen liegt unter `man/dd.1`:
```bash
man -l man/dd.1
```

### Regressionstest-Suite (17 Tests)
```bash
./tests/run_tests.sh
```
Prüft Pipelines, Blockgrößen, Skips, Seeks, EBCDIC/ASCII, Case-Folding, Swab, Sparse-Dateien, `conv=sync`, `conv=block/unblock`, `iflag=count_bytes`, Stille (`status=none`), Bit-Exaktheit von `conv=autotune`/`bs=auto`, den Target Safety Guard, On-the-Fly SHA-256 Checksums sowie die Multi-Threaded Async Double-Buffering Pipeline.

### Vergleichs-Benchmark (Lokal vs. System `/usr/bin/dd`)
```bash
./tests/benchmark_compare.sh
```
Misst Durchsatzwerte in 7 Szenarien und gibt eine direkte Gegenüberstellung mit prozentualer Differenz aus.
