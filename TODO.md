# TODO & Zukünftige Optimierungspotenziale für `dd`

Dieses Dokument erfasst alle im abschließenden Code-Check identifizierten Optimierungspotenziale für künftige Ausbaustufen.

---

## 1. High-Performance I/O & Kernel-Backends
- [ ] **Linux `io_uring` Backend:**
  - *Beschreibung:* Ergänzung eines alternativen I/O-Treibers in `src/io_engine.c` basierend auf `io_uring` Submission-/Completion-Queues.
  - *Nutzen:* Vollständig asynchroner I/O, Reduktion von Kontextwechseln (Syscalls) auf fast 0 bei hohen Queue-Depths; ideal für NVMe-Arrays.
- [ ] **`copy_file_range(2)` & Reflink-Support:**
  - *Beschreibung:* Automatische Nutzung von In-Kernel Zero-Copy und Copy-on-Write-Klonen (Btrfs, XFS), wenn Source und Target reguläre Dateien sind und keine blockverändernden Konvertierungen anliegen.
  - *Nutzen:* Nahezu instantanes Duplizieren großer Images ohne physische Disk-Schreiblast.
- [ ] **Multi-Threaded Double-Buffering (Async Pipeline):**
  - *Beschreibung:* Aufteilung von `read()` und `write()` in getrennte Threads mit Ringpuffer (Producer-Consumer-Pattern).
  - *Nutzen:* Volle I/O-Überlappung; Lesevorgänge warten nicht mehr auf synchrone Schreibbestätigungen langsamer Zielmedien.

---

## 2. Vollständige Reentrancy & Library-Tauglichkeit
- [x] **Kapselung verbleibender statischer Zustände:**
  - *Status:* Erledigt. `trans_table[256]` und `pending_spaces` wurden vollständig in `dd_context_t` überführt. Translationen und Unblock-Operationen sind thread-sicher und reentrant.
- [x] **Deterministisches Buffer-Lifecycle-Management (`dd_context_free`):**
  - *Status:* Erledigt. `dd_context_free()` gibt allokierte I/O-Puffer (`ibuf`, `obuf`) deterministisch via `alignfree()` frei.

---

## 3. SIMD- & Vektor-Optimierungen für Transformationen
- [x] **SIMD-Vektorisierung für Case-Folding (`ucase`, `lcase`):**
  - *Status:* Erledigt. `dd_vector_ucase()` und `dd_vector_lcase()` implementiert. Durch branchless Transformationen vektorisiert GCC/Clang den Code automatisch in AVX2/SSE-Instruktionen. Der Durchsatz stieg im Benchmark von 1,9 GB/s auf **8,6 GB/s (+352,6 %)**.
- [ ] **SIMD-optimiertes Byte-Swapping (`conv=swab`):**
  - *Beschreibung:* Vektorisierung von `dd_swab_buffer()` via `_mm256_shuffle_epi8` / `vrev16q_u8`.
  - *Nutzen:* Durchsatzsteigerung bei Endianness-Transfers auf Multi-Gigabyte-Niveau.

---

## 4. Hardware-Awareness & Autotuning-Erweiterungen
- [x] **Device Block Limits via `ioctl`:**
  - *Status:* Erledigt. `detect_optimal_blocksize()` ermittelt via `BLKPBSZGET`, `BLKIOOPT` (Linux) und `st_blksize` (POSIX) die physische Sektor- und optimale I/O-Größe. Das Autotuning richtet die minimale Blockgröße automatisch an den Hardware-Grenzen aus, um RMW-Penalties auf 4Kn-Laufwerken und RAID-Stripes zu eliminieren.
- [ ] **Persistentes Profiling-Caching:**
  - *Beschreibung:* Speichern optimaler Blockgrößen basierend auf Quell-/Ziel-Device-UUIDs in einer lokalen Cache-Tabelle.
  - *Nutzen:* Überspringen der anfänglichen 50-ms-Profiling-Phase bei wiederholten Übertragungen auf dieselben Laufwerke.

---

## 5. Ergonomie, Datenintegrität & Safety-Guards
- [x] **Target Safety-Guard (Root / Mounted Partition Check):**
  - *Status:* Erledigt. `check_target_safety()` prüft via `/proc/mounts` und Major-/Device-IDs, ob `of=` das aktive `/`, `/boot`, `/boot/efi` oder `/home`-Dateisystem oder dessen übergeordnete Disk adressiert. Bricht mit klarer Schutzmeldung ab, es sei denn `oflag=force`, `conv=force` oder `opt=force` wurde explizit angegeben. In Regressionstest 15 erfolgreich verifiziert.
- [ ] **Integrierte On-the-Fly Streaming-Prüfsumme:**
  - *Beschreibung:* Optionale Berechnung eines SHA-256- oder BLAKE3-Hashes parallel zum Schreiben (`status=hash` oder `conv=sha256`).
  - *Nutzen:* Sofortige Integritätsverifikation von geschriebenen OS-Images/USB-Sticks ohne zweiten zeitraubenden Lesedurchlauf.
