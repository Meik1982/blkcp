# dd (Modular Edition)

Eine eigenständige, modularisierte und architektonisch entflochtene Version des klassischen Unix-/Linux-Tools `dd`.

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

Der ursprüngliche 2.563-Zeilen-Monolith `dd.c` wurde vollständig in getrennte, reentrante Subsysteme zerlegt:

```
src/
├── dd.c              # Schlanke Einstiegs- und Ablaufsteuerung (~165 Zeilen)
├── dd_config.h       # Vollständige Kapselung von Zustand & Konfiguration (dd_context_t)
├── args.h / .c       # Operanden- & CLI-Parsing (if=, of=, bs=, Multiplikatoren, Validierung)
├── io_engine.h / .c  # I/O-Pipeline, Blockpufferung, Direct-I/O & Fsync-Synchronisation
├── conversions.h / .c# Zeichensatz- (EBCDIC/ASCII/Case) und Byte-Konvertierungen (swab)
├── stats.h / .c      # Durchsatz-Telemetrie, Human-readable Formatierung & Records-Reporting
├── signals.h / .c    # Signal-Handler (SIGINT-Cleanup, SIGINFO/SIGUSR1-Reporting)
└── system.h          # POSIX-Systemschnittstellen mit Include-Guards
```

### Kern-Verbesserungen:
1. **Kein verstreuter globaler Zustand:** Alle Konfigurations- und Laufzeitvariablen liegen zentral in `dd_context_t` / `dd_config_t`.
2. **100 % Schnittstellen-Kompatibilität:** Sämtliche CLI-Flags, Operanden, Signal-Trigger (`SIGUSR1`) und `stderr`-Ausgaben verhalten sich bit- und formatidentisch zum GNU-Standard.
3. **Erweiterbarkeit:** Die I/O-Engine ist isoliert und vorbereitet für moderne Backends (wie `io_uring` oder Multi-Threaded Double-Buffering).

---

## 3. Bauen & Testen

```bash
# Kompilieren
make

# Erweiterte Kompatibilitäts-Testsuite ausführen (12 Kernszenarien)
./tests/run_tests.sh

# Aufräumen
make clean
```
