# dd (Standalone Edition)

Eine eigenständige, isolierte Kopie des klassischen Unix-/Linux-Tools `dd`.

---

## 1. Herkunft & Lizenz des Quelltextes (Origin & Attributions)

* **Ursprung:** GNU Coreutils (Version 9.5).
* **Upstream-Quellen:** 
  * Offizielles GNU-Repository: <https://git.savannah.gnu.org/git/coreutils.git>
  * GitHub-Mirror: <https://github.com/coreutils/coreutils>
* **Originalautoren:** Paul Rubin, David MacKenzie, Stuart Kemp und die Free Software Foundation, Inc.
* **Lizenz:** GNU General Public License v3 oder neuer (GPLv3+). Siehe <https://gnu.org/licenses/gpl.html>.

Dieses Projekt extrahiert den Code von `dd` aus dem monolithischen GNU Coreutils-Buildsystem, um eine autark baubare, leicht verständliche und modular veränderbare Basis für zukünftige Optimierungen und Modernisierungen bereitzustellen. Der Quelltext in `src/dd.c` ist im Originalzustand belassen worden.

---

## 2. Projektstruktur

```
.
├── Makefile          # Schlankes, autarkes Makefile
├── README.md         # Diese Dokumentation & Herkunftsnachweis
├── include/          # Notwendige Header (POSIX/Gnulib-Kompatibilität & Interfaces)
├── lib/
│   └── libcoreutils.a# Extrahierte Hilfsroutinen (Gnulib / Coreutils-Subsysteme)
└── src/
    ├── dd.c          # Der originale Quellcode von dd (GNU Coreutils 9.5)
    ├── version.c     # Versionsdeklarationen
    ├── system.h      # System- und Plattform-Header
    └── ...
```

---

## 3. Bauen & Ausführen

Voraussetzung ist ein C-Compiler (`gcc` oder `clang`) sowie GNU Make.

```bash
# Bauen
make

# Ausführen
./dd --version
./dd --help

# Aufräumen
make clean
```
