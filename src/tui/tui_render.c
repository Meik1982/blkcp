/**
 * @file tui_render.c
 * @brief Form rendering and visual command generator for blkcp-tui
 */

#include <config.h>
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "tui_render.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void
tui_init_form(tui_form_t *form)
{
    memset(form, 0, sizeof *form);
    form->engine = TUI_ENGINE_AUTO;
    snprintf(form->queue_depth, sizeof form->queue_depth, "auto");
    snprintf(form->bs, sizeof form->bs, "auto");
    form->status_mode = 2; /* -p (progress) by default */
    form->opt_autotune = true;
    form->active_field = FIELD_IF;
    snprintf(form->status_msg, sizeof form->status_msg, "Bereit. Waehle Input/Output und druecke START.");
}

void
tui_build_command(tui_form_t const *form, char const *blkcp_bin, char *cmd, size_t cmd_len)
{
    char const *bin = (blkcp_bin && blkcp_bin[0]) ? blkcp_bin : "./blkcp";
    char prefix[600] = "";
    char buf[1024];
    char suffix[600] = "";

    snprintf(buf, sizeof buf, "%s", bin);

    /* Input stream / pipe */
    if (form->if_is_pipe) {
        if (form->if_path[0] && strcmp(form->if_path, "stdin") != 0 && strcmp(form->if_path, "-") != 0) {
            snprintf(prefix, sizeof prefix, "%.500s | ", form->if_path);
        }
    } else if (form->if_path[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " -i \"%.500s\"", form->if_path);
    }

    /* Output stream / pipe */
    if (form->of_is_pipe) {
        if (form->of_path[0] && strcmp(form->of_path, "stdout") != 0 && strcmp(form->of_path, "-") != 0) {
            snprintf(suffix, sizeof suffix, " | %.500s", form->of_path);
        }
    } else if (form->of_path[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " -o \"%.500s\"", form->of_path);
    }

    /* Engine */
    switch (form->engine) {
    case TUI_ENGINE_URING:
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " -e uring");
        break;
    case TUI_ENGINE_ASYNC:
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " -e async");
        break;
    case TUI_ENGINE_REFLINK:
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " -e reflink");
        break;
    case TUI_ENGINE_SPLICE:
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " -e splice");
        break;
    case TUI_ENGINE_SYNC:
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " -e sync");
        break;
    default:
        break; /* auto engine is default */
    }

    /* Queue depth for async engine */
    if (form->queue_depth[0] && strcmp(form->queue_depth, "auto") != 0) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " --queue-depth=%s", form->queue_depth);
    }

    /* Block size */
    if (form->bs[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " -b %s", form->bs);
    }

    /* Exact byte limit */
    if (form->limit[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " -l %s", form->limit);
    }

    /* Block count */
    if (form->count[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " -c %s", form->count);
    }

    /* Skip & Seek */
    if (form->skip[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " --skip=%s", form->skip);
    }
    if (form->seek[0]) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " --seek=%s", form->seek);
    }

    /* Flags */
    if (form->opt_autotune && strcmp(form->bs, "auto") != 0) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " --autotune");
    }
    if (form->opt_sha256) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " --hash");
    }
    if (form->opt_direct) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " --direct");
    }
    if (form->opt_sparse) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " --sparse");
    }
    if (form->opt_sync) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " --sync");
    }
    if (form->opt_force) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " -f");
    }

    /* Status level */
    if (form->status_mode == 0) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " -q");
    } else if (form->status_mode == 2) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " -p");
    } else if (form->status_mode == 3) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " --json");
    }

    snprintf(cmd, cmd_len, "%s%s%s", prefix, buf, suffix);
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
    mvwprintw(win, y, x, "[%c] %s", checked ? 'x' : ' ', label);
    if (focused) wattroff(win, A_REVERSE | A_BOLD | COLOR_PAIR(3));
}

static char const *
engine_name_str(tui_engine_mode_t engine)
{
    switch (engine) {
    case TUI_ENGINE_URING:   return "io_uring (Async Kernel Queue)";
    case TUI_ENGINE_ASYNC:   return "async (Pthread Ringbuffer)";
    case TUI_ENGINE_REFLINK: return "reflink (Zero-Copy copy_file_range)";
    case TUI_ENGINE_SPLICE:  return "splice (Kernel Zero-Copy Pipe)";
    case TUI_ENGINE_SYNC:    return "sync (Standard Block I/O)";
    default:                 return "auto (Intelligent Auto-Detection)";
    }
}

void
tui_render(WINDOW *win, tui_form_t const *form)
{
    werase(win);
    box(win, 0, 0);

    /* Header */
    wattron(win, A_BOLD | COLOR_PAIR(4));
    mvwprintw(win, 1, 2, "=== blkcp-tui :: Next-Gen Block Copy & Imaging Assistant ===");
    wattroff(win, A_BOLD | COLOR_PAIR(4));

    /* Section 1: Input & Output */
    wattron(win, A_BOLD);
    mvwprintw(win, 3, 2, "[ 1. Input & Output Streams ]");
    wattroff(win, A_BOLD);

    draw_field_str(win, 4, 2, "Input (-i):  ", form->if_path, 28, form->active_field == FIELD_IF);
    draw_button(win, 4, 45, "File", form->active_field == FIELD_IF_SEARCH_FILE, 4);
    draw_button(win, 4, 54, "Disk", form->active_field == FIELD_IF_SEARCH_DEV, 3);
    draw_button(win, 4, 63, "Pipe", form->active_field == FIELD_IF_SEARCH_PIPE, 4);

    draw_field_str(win, 5, 2, "Output (-o): ", form->of_path, 28, form->active_field == FIELD_OF);
    draw_button(win, 5, 45, "File", form->active_field == FIELD_OF_SEARCH_FILE, 4);
    draw_button(win, 5, 54, "Disk", form->active_field == FIELD_OF_SEARCH_DEV, 3);
    draw_button(win, 5, 63, "Pipe", form->active_field == FIELD_OF_SEARCH_PIPE, 4);

    /* Section 2: Engine & Performance */
    wattron(win, A_BOLD);
    mvwprintw(win, 7, 2, "[ 2. Engine & Performance Configuration ]");
    wattroff(win, A_BOLD);

    /* Engine Selector */
    mvwprintw(win, 8, 2, "Engine (-e): ");
    if (form->active_field == FIELD_ENGINE) {
        wattron(win, A_REVERSE | A_BOLD | COLOR_PAIR(3));
    }
    mvwprintw(win, 8, 15, "< %-33s > (Leertaste)", engine_name_str(form->engine));
    if (form->active_field == FIELD_ENGINE) {
        wattroff(win, A_REVERSE | A_BOLD | COLOR_PAIR(3));
    }

    draw_field_str(win, 8, 56, "Queue: ", form->queue_depth, 8, form->active_field == FIELD_QUEUE_DEPTH);

    draw_field_str(win, 9, 2, "Blocksize (-b): ", form->bs, 10, form->active_field == FIELD_BS);
    mvwprintw(win, 9, 32, "(z.B. auto, 64K, 1M, 4M, 16M)");

    draw_field_str(win, 10, 2, "Limit (-l):     ", form->limit, 10, form->active_field == FIELD_LIMIT);
    mvwprintw(win, 10, 32, "(Exakte Bytes, z.B. 10G, 500M, 4529848)");

    draw_field_str(win, 11, 2, "Count (-c):     ", form->count, 10, form->active_field == FIELD_COUNT);
    draw_field_str(win, 11, 32, "Skip (--skip): ", form->skip, 8, form->active_field == FIELD_SKIP);
    draw_field_str(win, 11, 55, "Seek (--seek): ", form->seek, 8, form->active_field == FIELD_SEEK);

    /* Section 3: Modifikatoren & Flags */
    wattron(win, A_BOLD);
    mvwprintw(win, 13, 2, "[ 3. Optimizers & Safety Guardrails ]");
    wattroff(win, A_BOLD);

    draw_checkbox(win, 14, 2, "Autotune (--autotune)", form->opt_autotune, form->active_field == FIELD_OPT_AUTOTUNE);
    draw_checkbox(win, 14, 38, "SHA-256 Checksum (--hash)", form->opt_sha256, form->active_field == FIELD_OPT_SHA256);

    draw_checkbox(win, 15, 2, "Direct I/O (--direct)", form->opt_direct, form->active_field == FIELD_OPT_DIRECT);
    draw_checkbox(win, 15, 38, "Force Target Guard Override (-f)", form->opt_force, form->active_field == FIELD_OPT_FORCE);

    draw_checkbox(win, 16, 2, "Sparse Punch-Hole (--sparse)", form->opt_sparse, form->active_field == FIELD_OPT_SPARSE);
    draw_checkbox(win, 16, 38, "Zero Padding Short Reads (--sync)", form->opt_sync, form->active_field == FIELD_OPT_SYNC);

    /* Section 4: Telemetrie */
    mvwprintw(win, 18, 2, "Status: (%c) Quiet (-q)  (%c) Standard  (%c) Progress (-p)  (%c) NDJSON (--json)",
              form->status_mode == 0 ? '*' : ' ',
              form->status_mode == 1 ? '*' : ' ',
              form->status_mode == 2 ? '*' : ' ',
              form->status_mode == 3 ? '*' : ' ');
    if (form->active_field == FIELD_STATUS) {
        wattron(win, A_REVERSE | A_BOLD | COLOR_PAIR(3));
        mvwprintw(win, 18, 2, "Status:");
        wattroff(win, A_REVERSE | A_BOLD | COLOR_PAIR(3));
    }

    /* Section 5: Live Status Box */
    int st_y = 19;
    int box_x = 2;
    int box_w = 75;
    int box_h = 6;

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
    int bar_w = 42;
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
    mvwprintw(win, st_y + 2, box_x + 2, "%-70.70s", form->status_msg);
    if (strncmp(form->status_msg, "FEHLER:", 7) == 0)
        wattroff(win, COLOR_PAIR(1) | A_BOLD);

    if (form->sha256_result[0]) {
        wattron(win, A_BOLD | COLOR_PAIR(2));
        mvwprintw(win, st_y + 3, box_x + 2, "SHA-256: %.64s", form->sha256_result);
        wattroff(win, A_BOLD | COLOR_PAIR(2));
    } else if (!form->opt_sha256) {
        wattron(win, A_DIM);
        mvwprintw(win, st_y + 3, box_x + 2, "SHA-256: (deaktiviert)                                          ");
        wattroff(win, A_DIM);
    } else if (form->running) {
        wattron(win, A_BOLD | COLOR_PAIR(4));
        mvwprintw(win, st_y + 3, box_x + 2, "SHA-256: [Streaming in-flight...]                               ");
        wattroff(win, A_BOLD | COLOR_PAIR(4));
    } else {
        mvwprintw(win, st_y + 3, box_x + 2, "SHA-256: (Aktiviert: On-the-Fly Streaming-Berechnung)           ");
    }

    /* Live Command Preview */
    char cmd[1024];
    tui_build_command(form, "./blkcp", cmd, sizeof cmd);
    wattron(win, A_DIM);
    mvwprintw(win, 25, 2, "Befehl: %-68.68s", cmd);
    wattroff(win, A_DIM);

    /* Action Buttons */
    int btn_y = 27;
    draw_button(win, btn_y, 6, "START (Enter)", form->active_field == FIELD_BTN_START, 2);      /* Green */
    draw_button(win, btn_y, 29, "BEFEHL KOPIEREN (c)", form->active_field == FIELD_BTN_COPY, 4);/* Blue */
    draw_button(win, btn_y, 56, "BEENDEN (Esc/q)", form->active_field == FIELD_BTN_QUIT, 1);   /* Red */

    wrefresh(win);
}
