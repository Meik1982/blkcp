/**
 * @file tui_main.c
 * @brief Interactive TUI executable for modular dd using ncursesw
 */

#include <config.h>
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "tui_render.h"
#include "tui_device.h"
#include "tui_file_picker.h"
#include "tui_nav.h"

#include <locale.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>

static void
edit_text_modal(char const *title, char *target, size_t target_len)
{
    int win_h = 7;
    int win_w = 60;
    int start_y = (LINES - win_h) / 2;
    int start_x = (COLS - win_w) / 2;

    WINDOW *w = newwin(win_h, win_w, start_y, start_x);
    keypad(w, TRUE);
    echo();
    curs_set(1);

    box(w, 0, 0);
    mvwprintw(w, 1, 2, "%s", title);
    mvwprintw(w, 4, 2, "[Enter]: Bestaetigen | [Esc]: Abbrechen");

    char input[512] = "";
    snprintf(input, sizeof input, "%s", target);

    mvwprintw(w, 2, 2, "> ");
    wrefresh(w);

    wgetnstr(w, input, (int)target_len - 1);
    if (input[0] != '\0') {
        snprintf(target, target_len, "%s", input);
    }

    noecho();
    curs_set(0);
    delwin(w);
}

static bool
confirm_safety_modal(char const *target)
{
    int win_h = 9;
    int win_w = 68;
    int start_y = (LINES - win_h) / 2;
    int start_x = (COLS - win_w) / 2;

    WINDOW *w = newwin(win_h, win_w, start_y, start_x);
    keypad(w, TRUE);

    werase(w);
    wattron(w, COLOR_PAIR(1) | A_BOLD);
    box(w, 0, 0);
    mvwprintw(w, 0, 2, " [ WARNUNG: SCHREIBZUGRIFF AUF BLOCKGERAET ] ");
    wattroff(w, COLOR_PAIR(1) | A_BOLD);

    mvwprintw(w, 2, 2, "Ziel: %s", target);
    mvwprintw(w, 3, 2, "Alle Daten auf diesem Datentraeger werden UNWIDERRUFLICH geloescht!");
    mvwprintw(w, 5, 2, "Moechtest du den Schreibvorgang wirklich starten?");
    mvwprintw(w, 7, 2, "Druecke [ J ] fuer JA oder [ N / Esc ] zum Abbrechen.");
    wrefresh(w);

    bool confirmed = false;
    while (true) {
        int ch = wgetch(w);
        if (ch == 'j' || ch == 'J' || ch == 'y' || ch == 'Y') {
            confirmed = true;
            break;
        } else if (ch == 'n' || ch == 'N' || ch == 27 || ch == 'q') {
            confirmed = false;
            break;
        }
    }

    delwin(w);
    return confirmed;
}

static void
copy_to_clipboard(tui_form_t *form, char const *cmd)
{
    /* Try wl-copy (Wayland) */
    FILE *p = popen("wl-copy 2>/dev/null", "w");
    if (p) {
        fputs(cmd, p);
        if (pclose(p) == 0) {
            snprintf(form->status_msg, sizeof form->status_msg, "Befehl in Wayland-Zwischenablage kopiert!");
            return;
        }
    }

    /* Try xclip (X11) */
    p = popen("xclip -selection clipboard 2>/dev/null", "w");
    if (p) {
        fputs(cmd, p);
        if (pclose(p) == 0) {
            snprintf(form->status_msg, sizeof form->status_msg, "Befehl in X11-Zwischenablage kopiert!");
            return;
        }
    }

    /* Fallback: Write to helper file */
    char const *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof path, "%s/dd_command.sh", home ? home : "/tmp");
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "#!/bin/sh\n%s\n", cmd);
        fclose(f);
        chmod(path, 0755);
        snprintf(form->status_msg, sizeof form->status_msg, "Gespeichert in: %.80s", path);
    } else {
        snprintf(form->status_msg, sizeof form->status_msg, "Befehl: %.90s", cmd);
    }
}

static void
execute_dd_job(WINDOW *main_win, tui_form_t *form)
{
    if (form->if_path[0] == '\0' || form->of_path[0] == '\0') {
        snprintf(form->status_msg, sizeof form->status_msg, "FEHLER: Bitte Input (if) und Output (of) angeben!");
        return;
    }

    struct stat st;
    if (stat(form->of_path, &st) == 0 && S_ISBLK(st.st_mode)) {
        if (!confirm_safety_modal(form->of_path)) {
            snprintf(form->status_msg, sizeof form->status_msg, "Abgebrochen durch Benutzer.");
            return;
        }
    }

    char cmd[1024];
    tui_build_command(form, cmd, sizeof cmd);

    /* Direct absolute binary path */
    char full_cmd[1600];
    char cwd[512];
    if (getcwd(cwd, sizeof cwd)) {
        snprintf(full_cmd, sizeof full_cmd, "%.400s/dd %.1024s 2>&1", cwd, cmd + 4);
    } else {
        snprintf(full_cmd, sizeof full_cmd, "%.1024s 2>&1", cmd);
    }

    snprintf(form->status_msg, sizeof form->status_msg, "Kopieren laeuft... bitte warten.");
    form->progress_pct = 10.0;
    form->running = true;
    form->sha256_result[0] = '\0';
    tui_render(main_win, form);

    FILE *fp = popen(full_cmd, "r");
    if (!fp) {
        form->running = false;
        snprintf(form->status_msg, sizeof form->status_msg, "Fehler beim Starten von dd!");
        return;
    }

    char line[512];
    char last_err[256] = "";

    while (fgets(line, sizeof line, fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (strncmp(line, "sha256: ", 8) == 0) {
            snprintf(form->sha256_result, sizeof form->sha256_result, "%.64s", line + 8);
        } else if (strstr(line, "copied")) {
            snprintf(form->status_msg, sizeof form->status_msg, "%.60s", line);
            form->progress_pct = 75.0;
            tui_render(main_win, form);
        } else if (strstr(line, "SAFETY GUARD")) {
            snprintf(last_err, sizeof last_err, "Safety Guard: Ziel enthaelt gemountetes Root!");
        } else {
            char const *p = line;
            if (strncmp(p, "./dd: ", 6) == 0) p += 6;
            else if (strncmp(p, "dd: ", 4) == 0) p += 4;
            while (*p == ' ') p++;
            if (*p != '\0') {
                snprintf(last_err, sizeof last_err, "%.200s", p);
            }
        }
    }

    int rc = pclose(fp);
    form->running = false;
    if (rc == 0) {
        form->progress_pct = 100.0;
        if (form->status_msg[0] == '\0' || strstr(form->status_msg, "Kopieren laeuft"))
            snprintf(form->status_msg, sizeof form->status_msg, "Erfolgreich abgeschlossen (100%%)!");
    } else {
        form->progress_pct = 0.0;
        if (last_err[0] != '\0') {
            snprintf(form->status_msg, sizeof form->status_msg, "FEHLER: %.54s", last_err);
        } else {
            snprintf(form->status_msg, sizeof form->status_msg, "FEHLER: Kopieren fehlgeschlagen (Exit %d)", WEXITSTATUS(rc));
        }
    }
}

int
main(void)
{
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_RED, -1);
        init_pair(2, COLOR_GREEN, -1);
        init_pair(3, COLOR_YELLOW, -1);
        init_pair(4, COLOR_CYAN, -1);
    }

    int win_h = 31;
    int win_w = 78;
    int start_y = (LINES - win_h) / 2;
    int start_x = (COLS - win_w) / 2;
    if (start_y < 0) start_y = 0;
    if (start_x < 0) start_x = 0;

    WINDOW *win = newwin(win_h, win_w, start_y, start_x);
    keypad(win, TRUE);

    tui_form_t form;
    tui_init_form(&form);

    bool running = true;
    while (running) {
        tui_render(win, &form);
        int ch = wgetch(win);

        switch (ch) {
        case KEY_RESIZE:
            start_y = (LINES - win_h) / 2;
            start_x = (COLS - win_w) / 2;
            if (start_y < 0) start_y = 0;
            if (start_x < 0) start_x = 0;
            erase();
            refresh();
            mvwin(win, start_y, start_x);
            break;

        case '\t':
            form.active_field = (form.active_field + 1) % FIELD_COUNT_TOTAL;
            break;

        case KEY_BTAB:
            if (form.active_field == 0)
                form.active_field = FIELD_COUNT_TOTAL - 1;
            else
                form.active_field--;
            break;

        case KEY_UP:
            form.active_field = tui_nav_up(form.active_field);
            break;

        case KEY_DOWN:
            form.active_field = tui_nav_down(form.active_field);
            break;

        case KEY_LEFT:
            form.active_field = tui_nav_left(form.active_field);
            break;

        case KEY_RIGHT:
            form.active_field = tui_nav_right(form.active_field);
            break;

        case ' ':
            /* Checkbox toggle or Radio increment */
            if (form.active_field == FIELD_COUNT_BYTES) form.count_bytes = !form.count_bytes;
            else if (form.active_field == FIELD_SKIP_BYTES) form.skip_bytes = !form.skip_bytes;
            else if (form.active_field == FIELD_SEEK_BYTES) form.seek_bytes = !form.seek_bytes;
            else if (form.active_field == FIELD_OPT_AUTOTUNE) form.opt_autotune = !form.opt_autotune;
            else if (form.active_field == FIELD_OPT_ASYNC) form.opt_async = !form.opt_async;
            else if (form.active_field == FIELD_OPT_SHA256) form.opt_sha256 = !form.opt_sha256;
            else if (form.active_field == FIELD_OPT_FORCE) form.opt_force = !form.opt_force;
            else if (form.active_field == FIELD_STATUS) form.status_mode = (form.status_mode + 1) % 3;
            break;

        case 10:
        case KEY_ENTER:
            if (form.active_field == FIELD_IF)
                edit_text_modal("Input-Pfad (if=) eingeben:", form.if_path, sizeof form.if_path);
            else if (form.active_field == FIELD_IF_SEARCH_FILE) {
                tui_pick_file(form.if_path[0] ? form.if_path : NULL, form.if_path, sizeof form.if_path);
            } else if (form.active_field == FIELD_IF_SEARCH_DEV) {
                tui_pick_device(form.if_path, sizeof form.if_path);
            } else if (form.active_field == FIELD_OF)
                edit_text_modal("Output-Pfad (of=) eingeben:", form.of_path, sizeof form.of_path);
            else if (form.active_field == FIELD_OF_SEARCH_FILE) {
                tui_pick_file(form.of_path[0] ? form.of_path : NULL, form.of_path, sizeof form.of_path);
            } else if (form.active_field == FIELD_OF_SEARCH_DEV) {
                tui_pick_device(form.of_path, sizeof form.of_path);
            } else if (form.active_field == FIELD_BS)
                edit_text_modal("Block Size (bs=, z.B. auto, 1M, 64k):", form.bs, sizeof form.bs);
            else if (form.active_field == FIELD_COUNT)
                edit_text_modal("Count (Anzahl Bloecke oder Bytes):", form.count, sizeof form.count);
            else if (form.active_field == FIELD_SKIP)
                edit_text_modal("Skip Bloecke:", form.skip, sizeof form.skip);
            else if (form.active_field == FIELD_SEEK)
                edit_text_modal("Seek Bloecke:", form.seek, sizeof form.seek);
            else if (form.active_field == FIELD_CONV)
                edit_text_modal("conv= (z.B. noerror,sync,ucase):", form.conv, sizeof form.conv);
            else if (form.active_field == FIELD_IFLAG)
                edit_text_modal("iflag= (z.B. direct):", form.iflag, sizeof form.iflag);
            else if (form.active_field == FIELD_OFLAG)
                edit_text_modal("oflag= (z.B. direct):", form.oflag, sizeof form.oflag);
            else if (form.active_field == FIELD_COUNT_BYTES) form.count_bytes = !form.count_bytes;
            else if (form.active_field == FIELD_SKIP_BYTES) form.skip_bytes = !form.skip_bytes;
            else if (form.active_field == FIELD_SEEK_BYTES) form.seek_bytes = !form.seek_bytes;
            else if (form.active_field == FIELD_OPT_AUTOTUNE) form.opt_autotune = !form.opt_autotune;
            else if (form.active_field == FIELD_OPT_ASYNC) form.opt_async = !form.opt_async;
            else if (form.active_field == FIELD_OPT_SHA256) form.opt_sha256 = !form.opt_sha256;
            else if (form.active_field == FIELD_OPT_FORCE) form.opt_force = !form.opt_force;
            else if (form.active_field == FIELD_STATUS) form.status_mode = (form.status_mode + 1) % 3;
            else if (form.active_field == FIELD_BTN_START) {
                execute_dd_job(win, &form);
            } else if (form.active_field == FIELD_BTN_COPY) {
                char cmd[1024];
                tui_build_command(&form, cmd, sizeof cmd);
                copy_to_clipboard(&form, cmd);
            } else if (form.active_field == FIELD_BTN_QUIT) {
                running = false;
            }
            break;

        case 27: /* Esc */
        case 'q':
        case 'Q':
            running = false;
            break;
        }
    }

    delwin(win);
    endwin();
    return 0;
}
