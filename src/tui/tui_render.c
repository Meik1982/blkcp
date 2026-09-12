/**
 * @file tui_render.c
 * @brief Rendering and command generation implementation for dd-tui
 */

#include <config.h>
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "tui_render.h"
#include <stdio.h>
#include <string.h>

void
tui_init_form(tui_form_t *form)
{
    memset(form, 0, sizeof *form);
    snprintf(form->bs, sizeof form->bs, "auto");
    form->opt_autotune = true;
    form->opt_async = true;
    form->opt_sha256 = true;
    form->opt_force = false;
    form->status_mode = 2; /* progress */
    snprintf(form->eta_str, sizeof form->eta_str, "--:--:--");
    snprintf(form->status_msg, sizeof form->status_msg, "Bereit. Waehle Quelle & Ziel.");
    form->active_field = FIELD_IF_SEARCH_FILE;
}

void
tui_build_command(tui_form_t const *form, char *cmd, size_t cmd_len)
{
    char buf[1024] = "./dd";

    if (form->if_path[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " if=\"%s\"", form->if_path);
    }
    if (form->of_path[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " of=\"%s\"", form->of_path);
    }
    if (form->bs[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " bs=%s", form->bs);
    }
    if (form->count[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " count=%s", form->count);
    }
    if (form->skip[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " skip=%s", form->skip);
    }
    if (form->seek[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " seek=%s", form->seek);
    }

    /* Build opt= list */
    char opts[128] = "";
    if (form->opt_autotune && strcmp(form->bs, "auto") != 0) {
        snprintf(opts + strlen(opts), sizeof(opts) - strlen(opts), "%sauto", opts[0] ? "," : "");
    }
    if (form->opt_async) {
        snprintf(opts + strlen(opts), sizeof(opts) - strlen(opts), "%sasync", opts[0] ? "," : "");
    }
    if (form->opt_sha256) {
        snprintf(opts + strlen(opts), sizeof(opts) - strlen(opts), "%shash", opts[0] ? "," : "");
    }
    if (form->opt_force) {
        snprintf(opts + strlen(opts), sizeof(opts) - strlen(opts), "%sforce", opts[0] ? "," : "");
    }
    if (opts[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " opt=%s", opts);
    }

    /* conv */
    if (form->conv[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " conv=%s", form->conv);
    }

    /* iflag */
    char iflags[128] = "";
    if (form->iflag[0]) snprintf(iflags, sizeof iflags, "%s", form->iflag);
    if (form->count_bytes) {
        snprintf(iflags + strlen(iflags), sizeof(iflags) - strlen(iflags), "%scount_bytes", iflags[0] ? "," : "");
    }
    if (form->skip_bytes) {
        snprintf(iflags + strlen(iflags), sizeof(iflags) - strlen(iflags), "%sskip_bytes", iflags[0] ? "," : "");
    }
    if (iflags[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " iflag=%s", iflags);
    }

    /* oflag */
    char oflags[128] = "";
    if (form->oflag[0]) snprintf(oflags, sizeof oflags, "%s", form->oflag);
    if (form->seek_bytes) {
        snprintf(oflags + strlen(oflags), sizeof(oflags) - strlen(oflags), "%sseek_bytes", oflags[0] ? "," : "");
    }
    if (oflags[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " oflag=%s", oflags);
    }

    /* status */
    if (form->status_mode == 0) snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " status=none");
    else if (form->status_mode == 1) snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " status=noxfer");
    else if (form->status_mode == 2) snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " status=progress");

    snprintf(cmd, cmd_len, "%s", buf);
}

static void
draw_field_str(WINDOW *win, int y, int x, char const *label, char const *val, int w, bool focused)
{
    mvwprintw(win, y, x, "%s", label);
    int lx = x + (int)strlen(label);
    if (focused) wattron(win, A_REVERSE | A_BOLD | COLOR_PAIR(3));
    mvwprintw(win, y, lx, "[%-*.*s]", w, w, val ? val : "");
    if (focused) wattroff(win, A_REVERSE | A_BOLD | COLOR_PAIR(3));
}

static void
draw_button(WINDOW *win, int y, int x, char const *label, bool focused, int color_pair)
{
    if (focused) wattron(win, A_REVERSE | A_BOLD | COLOR_PAIR(color_pair ? color_pair : 3));
    mvwprintw(win, y, x, "[ %s ]", label);
    if (focused) wattroff(win, A_REVERSE | A_BOLD | COLOR_PAIR(color_pair ? color_pair : 3));
}

static void
draw_checkbox(WINDOW *win, int y, int x, char const *label, bool checked, bool focused)
{
    if (focused) wattron(win, A_REVERSE | A_BOLD | COLOR_PAIR(3));
    mvwprintw(win, y, x, "[%c] %s", checked ? '*' : ' ', label);
    if (focused) wattroff(win, A_REVERSE | A_BOLD | COLOR_PAIR(3));
}

void
tui_render(WINDOW *win, tui_form_t const *form)
{
    werase(win);
    box(win, 0, 0);

    /* Title */
    wattron(win, A_BOLD | COLOR_PAIR(4));
    mvwprintw(win, 0, 13, " [ dd TUI: High-Performance Disk & Image Manager ] ");
    wattroff(win, A_BOLD | COLOR_PAIR(4));

    /* Section 1: Ein- und Ausgabe */
    wattron(win, A_BOLD);
    mvwprintw(win, 2, 2, "[ Ein- und Ausgabe ]");
    wattroff(win, A_BOLD);

    draw_field_str(win, 3, 2, "Input (if):  ", form->if_path, 34, form->active_field == FIELD_IF);
    draw_button(win, 3, 53, "Datei", form->active_field == FIELD_IF_SEARCH_FILE, 3);
    draw_button(win, 3, 64, "Disk", form->active_field == FIELD_IF_SEARCH_DEV, 3);

    draw_field_str(win, 4, 2, "Output (of): ", form->of_path, 34, form->active_field == FIELD_OF);
    draw_button(win, 4, 53, "Datei", form->active_field == FIELD_OF_SEARCH_FILE, 3);
    draw_button(win, 4, 64, "Disk", form->active_field == FIELD_OF_SEARCH_DEV, 3);

    /* Section 2: Block-Konfiguration */
    wattron(win, A_BOLD);
    mvwprintw(win, 6, 2, "[ Block-Konfiguration (bs, count, skip, seek) ]");
    wattroff(win, A_BOLD);

    draw_field_str(win, 7, 2, "Block Size (bs): ", form->bs, 10, form->active_field == FIELD_BS);
    mvwprintw(win, 7, 33, "(z.B. auto, 1M, 64k, 4M)");

    draw_field_str(win, 8, 2, "Count:           ", form->count, 10, form->active_field == FIELD_COUNT);
    draw_checkbox(win, 8, 33, "count_bytes (iflag)", form->count_bytes, form->active_field == FIELD_COUNT_BYTES);

    draw_field_str(win, 9, 2, "Skip (Input):    ", form->skip, 10, form->active_field == FIELD_SKIP);
    draw_checkbox(win, 9, 33, "skip_bytes  (iflag)", form->skip_bytes, form->active_field == FIELD_SKIP_BYTES);

    draw_field_str(win, 10, 2, "Seek (Output):   ", form->seek, 10, form->active_field == FIELD_SEEK);
    draw_checkbox(win, 10, 33, "seek_bytes  (oflag)", form->seek_bytes, form->active_field == FIELD_SEEK_BYTES);

    /* Section 3: Moderne Erweiterungen */
    wattron(win, A_BOLD);
    mvwprintw(win, 12, 2, "[ Moderne Erweiterungen (opt / flags) ]");
    wattroff(win, A_BOLD);

    draw_checkbox(win, 13, 2, "autotune (Dynamischer Benchmark)", form->opt_autotune, form->active_field == FIELD_OPT_AUTOTUNE);
    draw_checkbox(win, 13, 40, "async  (Ringpuffer Pipeline)", form->opt_async, form->active_field == FIELD_OPT_ASYNC);

    draw_checkbox(win, 14, 2, "sha256   (On-the-fly Checksumme)", form->opt_sha256, form->active_field == FIELD_OPT_SHA256);
    draw_checkbox(win, 14, 40, "force  (System-Root Override)", form->opt_force, form->active_field == FIELD_OPT_FORCE);

    /* Section 4: Standard-Optionen */
    wattron(win, A_BOLD);
    mvwprintw(win, 16, 2, "[ Standard-Optionen ]");
    wattroff(win, A_BOLD);

    draw_field_str(win, 17, 2, "conv:  ", form->conv, 12, form->active_field == FIELD_CONV);
    draw_field_str(win, 17, 25, "iflag: ", form->iflag, 12, form->active_field == FIELD_IFLAG);
    draw_field_str(win, 17, 48, "oflag: ", form->oflag, 12, form->active_field == FIELD_OFLAG);

    mvwprintw(win, 18, 2, "Status: (%c) none   (%c) noxfer   (%c) progress",
              form->status_mode == 0 ? '*' : ' ',
              form->status_mode == 1 ? '*' : ' ',
              form->status_mode == 2 ? '*' : ' ');
    if (form->active_field == FIELD_STATUS) {
        wattron(win, A_REVERSE);
        mvwprintw(win, 18, 2, "Status:");
        wattroff(win, A_REVERSE);
    }

    /* Section 5: Live Status Box */
    int st_y = 20;
    int box_x = 2;
    int box_w = 74;
    int box_h = 5;

    /* Render Box Border using standard curses ACS characters */
    mvwaddch(win, st_y, box_x, ACS_ULCORNER);
    for (int i = 1; i < box_w - 1; i++)
        waddch(win, ACS_HLINE);
    mvwaddch(win, st_y, box_x + box_w - 1, ACS_URCORNER);
    mvwprintw(win, st_y, box_x + 28, " [ Live Status ] ");

    for (int y = 1; y < box_h - 1; y++) {
        mvwaddch(win, st_y + y, box_x, ACS_VLINE);
        mvwaddch(win, st_y + y, box_x + box_w - 1, ACS_VLINE);
    }

    mvwaddch(win, st_y + box_h - 1, box_x, ACS_LLCORNER);
    for (int i = 1; i < box_w - 1; i++)
        waddch(win, ACS_HLINE);
    mvwaddch(win, st_y + box_h - 1, box_x + box_w - 1, ACS_LRCORNER);

    /* Progress bar */
    int bar_w = 40;
    int filled = (int)(form->progress_pct * bar_w / 100.0);
    if (filled < 0) filled = 0;
    if (filled > bar_w) filled = bar_w;

    char bar[64];
    for (int i = 0; i < bar_w; i++) {
        if (i < filled - 1) bar[i] = '=';
        else if (i == filled - 1) bar[i] = '>';
        else bar[i] = ' ';
    }
    bar[bar_w] = '\0';

    mvwprintw(win, st_y + 1, box_x + 2, "[%s] %3.0f%%", bar, form->progress_pct);

    if (strncmp(form->status_msg, "FEHLER:", 7) == 0)
        wattron(win, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(win, st_y + 2, box_x + 2, "%-68.68s", form->status_msg);
    if (strncmp(form->status_msg, "FEHLER:", 7) == 0)
        wattroff(win, COLOR_PAIR(1) | A_BOLD);

    if (form->sha256_result[0])
        mvwprintw(win, st_y + 3, box_x + 2, "SHA256: %.60s", form->sha256_result);
    else
        mvwprintw(win, st_y + 3, box_x + 2, "SHA256: (wird bei Abschluss berechnet)");

    /* Live Command Preview */
    char cmd[1024];
    tui_build_command(form, cmd, sizeof cmd);
    wattron(win, A_DIM);
    mvwprintw(win, 26, 2, "Befehl: %-66.66s", cmd);
    wattroff(win, A_DIM);

    /* Action Buttons */
    int btn_y = 28;
    draw_button(win, btn_y, 7, "START (Enter)", form->active_field == FIELD_BTN_START, 2);      /* Green */
    draw_button(win, btn_y, 30, "BEFEHL KOPIEREN", form->active_field == FIELD_BTN_COPY, 4);   /* Blue */
    draw_button(win, btn_y, 55, "BEENDEN (Esc/q)", form->active_field == FIELD_BTN_QUIT, 1);   /* Red */

    wrefresh(win);
}
