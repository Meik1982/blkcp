/**
 * @file tui_main.c
 * @brief Interactive TUI executable for blkcp using ncursesw
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
    int win_w = 64;
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

    /* Fallback: Write to helper script */
    char const *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof path, "%s/blkcp_command.sh", home ? home : "/tmp");
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
execute_blkcp_job(WINDOW *main_win, tui_form_t *form)
{
    if (form->if_path[0] == '\0' || form->of_path[0] == '\0') {
        snprintf(form->status_msg, sizeof form->status_msg, "FEHLER: Bitte Input (-i) und Output (-o) angeben!");
        return;
    }

    struct stat st;
    if (stat(form->of_path, &st) == 0 && S_ISBLK(st.st_mode)) {
        if (!confirm_safety_modal(form->of_path)) {
            snprintf(form->status_msg, sizeof form->status_msg, "Abgebrochen durch Benutzer.");
            return;
        }
    }

    char full_bin[600] = "./blkcp";
    char cwd[512];
    if (getcwd(cwd, sizeof cwd)) {
        snprintf(full_bin, sizeof full_bin, "%.500s/blkcp", cwd);
    }

    char cmd_body[1600];
    tui_build_command(form, full_bin, cmd_body, sizeof cmd_body);

    char full_cmd[2048];
    snprintf(full_cmd, sizeof full_cmd, "%s 2>&1", cmd_body);

    snprintf(form->status_msg, sizeof form->status_msg, "Kopieren laeuft... bitte warten.");
    form->progress_pct = 10.0;
    form->running = true;
    form->sha256_result[0] = '\0';
    tui_render(main_win, form);

    FILE *fp = popen(full_cmd, "r");
    if (!fp) {
        form->running = false;
        snprintf(form->status_msg, sizeof form->status_msg, "Fehler beim Starten von blkcp!");
        return;
    }

    char line[512];
    char last_err[256] = "";

    while (fgets(line, sizeof line, fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (strncmp(line, "sha256: ", 8) == 0) {
            snprintf(form->sha256_result, sizeof form->sha256_result, "%.64s", line + 8);
        } else if (strstr(line, "copied") || strstr(line, "kopiert")) {
            snprintf(form->status_msg, sizeof form->status_msg, "%.68s", line);
            form->progress_pct = 75.0;
            tui_render(main_win, form);
        } else if (strstr(line, "SAFETY GUARD")) {
            snprintf(last_err, sizeof last_err, "Safety Guard: Ziel enthaelt gemountetes Root!");
        } else {
            char const *p = line;
            if (strncmp(p, "./blkcp: ", 9) == 0) p += 9;
            else if (strncmp(p, "blkcp: ", 7) == 0) p += 7;
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

    int min_h = 30;
    int min_w = 80;
    if (LINES < min_h || COLS < min_w) {
        endwin();
        fprintf(stderr, "Terminal zu klein! Minimum: %dx%d (Aktuell: %dx%d)\n", min_w, min_h, COLS, LINES);
        return EXIT_FAILURE;
    }

    WINDOW *win = newwin(min_h, min_w, (LINES - min_h) / 2, (COLS - min_w) / 2);
    keypad(win, TRUE);

    tui_form_t form;
    tui_init_form(&form);

    bool running = true;
    while (running) {
        tui_render(win, &form);

        int ch = wgetch(win);
        switch (ch) {
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
        case '\t':
            form.active_field = (form.active_field + 1) % FIELD_COUNT_TOTAL;
            break;
        case KEY_BTAB:
            form.active_field = (form.active_field + FIELD_COUNT_TOTAL - 1) % FIELD_COUNT_TOTAL;
            break;

        case ' ':
            if (form.active_field == FIELD_ENGINE)
                form.engine = (form.engine + 1) % TUI_ENGINE_COUNT;
            else if (form.active_field == FIELD_OPT_AUTOTUNE)
                form.opt_autotune = !form.opt_autotune;
            else if (form.active_field == FIELD_OPT_SHA256)
                form.opt_sha256 = !form.opt_sha256;
            else if (form.active_field == FIELD_OPT_DIRECT)
                form.opt_direct = !form.opt_direct;
            else if (form.active_field == FIELD_OPT_FORCE)
                form.opt_force = !form.opt_force;
            else if (form.active_field == FIELD_OPT_SPARSE)
                form.opt_sparse = !form.opt_sparse;
            else if (form.active_field == FIELD_OPT_SYNC)
                form.opt_sync = !form.opt_sync;
            else if (form.active_field == FIELD_STATUS)
                form.status_mode = (form.status_mode + 1) % 4;
            break;

        case 10:
        case KEY_ENTER:
            if (form.active_field == FIELD_IF) {
                edit_text_modal("Input-Pfad (-i) oder Pipe eingeben:", form.if_path, sizeof form.if_path);
                if (form.if_path[0] == '|') form.if_is_pipe = true;
            } else if (form.active_field == FIELD_IF_SEARCH_FILE) {
                if (tui_pick_file(form.if_path[0] ? form.if_path : NULL, form.if_path, sizeof form.if_path) == 0)
                    form.if_is_pipe = false;
            } else if (form.active_field == FIELD_IF_SEARCH_DEV) {
                if (tui_pick_device(form.if_path, sizeof form.if_path) == 0)
                    form.if_is_pipe = false;
            } else if (form.active_field == FIELD_IF_SEARCH_PIPE) {
                if (tui_pick_pipe(true, form.if_path, sizeof form.if_path) == 0)
                    form.if_is_pipe = true;
            } else if (form.active_field == FIELD_OF) {
                edit_text_modal("Output-Pfad (-o) oder Pipe eingeben:", form.of_path, sizeof form.of_path);
                if (form.of_path[0] == '|') form.of_is_pipe = true;
            } else if (form.active_field == FIELD_OF_SEARCH_FILE) {
                if (tui_pick_file(form.of_path[0] ? form.of_path : NULL, form.of_path, sizeof form.of_path) == 0)
                    form.of_is_pipe = false;
            } else if (form.active_field == FIELD_OF_SEARCH_DEV) {
                if (tui_pick_device(form.of_path, sizeof form.of_path) == 0)
                    form.of_is_pipe = false;
            } else if (form.active_field == FIELD_OF_SEARCH_PIPE) {
                if (tui_pick_pipe(false, form.of_path, sizeof form.of_path) == 0)
                    form.of_is_pipe = true;
            } else if (form.active_field == FIELD_ENGINE) {
                form.engine = (form.engine + 1) % TUI_ENGINE_COUNT;
            } else if (form.active_field == FIELD_QUEUE_DEPTH) {
                edit_text_modal("Ringbuffer Queue Depth (z.B. auto, 8, 16, 64, 128):", form.queue_depth, sizeof form.queue_depth);
            } else if (form.active_field == FIELD_BS) {
                edit_text_modal("Block Size (-b, z.B. auto, 1M, 64k, 4M):", form.bs, sizeof form.bs);
            } else if (form.active_field == FIELD_LIMIT) {
                edit_text_modal("Limit (-l, Exakte Bytes z.B. 10G, 500M, 4194304):", form.limit, sizeof form.limit);
            } else if (form.active_field == FIELD_COUNT) {
                edit_text_modal("Count (-c, Anzahl Bloecke):", form.count, sizeof form.count);
            } else if (form.active_field == FIELD_SKIP) {
                edit_text_modal("Skip Offset (--skip, z.B. 10M, 65536):", form.skip, sizeof form.skip);
            } else if (form.active_field == FIELD_SEEK) {
                edit_text_modal("Seek Offset (--seek, z.B. 10M, 65536):", form.seek, sizeof form.seek);
            } else if (form.active_field == FIELD_OPT_AUTOTUNE) {
                form.opt_autotune = !form.opt_autotune;
            } else if (form.active_field == FIELD_OPT_SHA256) {
                form.opt_sha256 = !form.opt_sha256;
            } else if (form.active_field == FIELD_OPT_DIRECT) {
                form.opt_direct = !form.opt_direct;
            } else if (form.active_field == FIELD_OPT_FORCE) {
                form.opt_force = !form.opt_force;
            } else if (form.active_field == FIELD_OPT_SPARSE) {
                form.opt_sparse = !form.opt_sparse;
            } else if (form.active_field == FIELD_OPT_SYNC) {
                form.opt_sync = !form.opt_sync;
            } else if (form.active_field == FIELD_STATUS) {
                form.status_mode = (form.status_mode + 1) % 4;
            } else if (form.active_field == FIELD_BTN_START) {
                execute_blkcp_job(win, &form);
            } else if (form.active_field == FIELD_BTN_COPY) {
                char cmd[1024];
                tui_build_command(&form, "./blkcp", cmd, sizeof cmd);
                copy_to_clipboard(&form, cmd);
            } else if (form.active_field == FIELD_BTN_QUIT) {
                running = false;
            }
            break;

        case 'c':
        case 'C': {
            char cmd[1024];
            tui_build_command(&form, "./blkcp", cmd, sizeof cmd);
            copy_to_clipboard(&form, cmd);
            break;
        }

        case 'q':
        case 'Q':
        case 27: /* ESC */
            running = false;
            break;

        default:
            break;
        }
    }

    delwin(win);
    endwin();
    return EXIT_SUCCESS;
}
