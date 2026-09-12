/**
 * @file tui_file_picker.c
 * @brief Interactive file and block-device selection dialogs using ncursesw
 */

#include <config.h>
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "tui_file_picker.h"
#include "tui_device.h"
#include <ncurses.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_ENTRIES 512

typedef struct file_entry {
    char name[256];
    bool is_dir;
    uint64_t size;
} file_entry_t;

static int
compare_entries(const void *a, const void *b)
{
    const file_entry_t *fa = (const file_entry_t *)a;
    const file_entry_t *fb = (const file_entry_t *)b;
    if (strcmp(fa->name, "..") == 0) return -1;
    if (strcmp(fb->name, "..") == 0) return 1;
    if (fa->is_dir && !fb->is_dir) return -1;
    if (!fa->is_dir && fb->is_dir) return 1;
    return strcmp(fa->name, fb->name);
}

static bool
prompt_new_file(char const *dir, char *out_selected, size_t out_len)
{
    int win_h = 8;
    int win_w = 64;
    int start_y = (LINES - win_h) / 2;
    int start_x = (COLS - win_w) / 2;

    WINDOW *w = newwin(win_h, win_w, start_y, start_x);
    keypad(w, TRUE);
    echo();
    curs_set(1);

    box(w, 0, 0);
    wattron(w, A_BOLD | COLOR_PAIR(2));
    mvwprintw(w, 1, 2, "[ Neue Datei in aktuellem Ordner anlegen ]");
    wattroff(w, A_BOLD | COLOR_PAIR(2));
    mvwprintw(w, 2, 2, "Pfad: %.50s", dir);
    mvwprintw(w, 4, 2, "Dateiname (z.B. backup.img):");
    mvwprintw(w, 5, 2, "> ");
    mvwprintw(w, 6, 2, "[Enter]: Bestaetigen | [Leer lassen]: Abbrechen");
    wrefresh(w);

    char name[256] = "";
    wmove(w, 5, 4);
    wgetnstr(w, name, sizeof(name) - 1);

    noecho();
    curs_set(0);
    delwin(w);

    if (name[0] == '\0')
        return false;

    if (strcmp(dir, "/") == 0)
        snprintf(out_selected, out_len, "/%.255s", name);
    else
        snprintf(out_selected, out_len, "%.500s/%.255s", dir, name);

    return true;
}

int
tui_pick_device(char *out_selected, size_t out_len)
{
    tui_device_t devs[MAX_DEVICES];
    size_t count = tui_scan_devices(devs, MAX_DEVICES);
    if (count == 0) return -1;

    int win_h = 16;
    int win_w = 74;
    int start_y = (LINES - win_h) / 2;
    int start_x = (COLS - win_w) / 2;
    if (start_y < 0) start_y = 0;
    if (start_x < 0) start_x = 0;

    WINDOW *win = newwin(win_h, win_w, start_y, start_x);
    keypad(win, TRUE);

    size_t selected = 0;
    int res = -1;

    while (true) {
        werase(win);
        box(win, 0, 0);
        mvwprintw(win, 0, 2, " [ Laufwerk / Block-Device auswaehlen ] ");

        for (size_t i = 0; i < count && i < (size_t)(win_h - 4); i++) {
            char size_str[32];
            tui_format_size(devs[i].size_bytes, size_str, sizeof size_str);

            char line[128];
            char tag[32] = "";
            if (devs[i].is_system_root) {
                snprintf(tag, sizeof tag, "[ROOT GESPERRT]");
            } else if (devs[i].is_removable) {
                snprintf(tag, sizeof tag, "[WECHSEL]");
            } else if (devs[i].mountpoint[0]) {
                snprintf(tag, sizeof tag, "[GEMOUNTET]");
            }

            snprintf(line, sizeof line, "%-10s %-8s %-32.32s %s",
                     devs[i].path, size_str, devs[i].model, tag);

            if (i == selected) {
                wattron(win, A_REVERSE | A_BOLD);
                if (devs[i].is_system_root)
                    wattron(win, COLOR_PAIR(1)); /* Red / alert */
                mvwprintw(win, 2 + i, 2, " > %-68.68s", line);
                wattroff(win, A_REVERSE | A_BOLD | COLOR_PAIR(1));
            } else {
                if (devs[i].is_system_root)
                    wattron(win, A_DIM);
                mvwprintw(win, 2 + i, 2, "   %-68.68s", line);
                wattroff(win, A_DIM);
            }
        }

        mvwprintw(win, win_h - 2, 2, "[Pfeile]: Navigieren | [Enter]: Auswaehlen | [Esc/q]: Abbrechen");
        wrefresh(win);

        int ch = wgetch(win);
        if (ch == 27 || ch == 'q' || ch == 'Q') {
            res = -1;
            break;
        } else if (ch == KEY_UP && selected > 0) {
            selected--;
        } else if (ch == KEY_DOWN && selected + 1 < count) {
            selected++;
        } else if (ch == 10 || ch == KEY_ENTER) {
            snprintf(out_selected, out_len, "%s", devs[selected].path);
            res = 0;
            break;
        }
    }

    delwin(win);
    return res;
}

int
tui_pick_file(char const *start_path, char *out_selected, size_t out_len)
{
    char cur_dir[1024];
    if (start_path && start_path[0] == '/')
        snprintf(cur_dir, sizeof cur_dir, "%s", start_path);
    else if (getcwd(cur_dir, sizeof cur_dir) == NULL)
        snprintf(cur_dir, sizeof cur_dir, "/");

    /* If start_path was a file, jump to its directory */
    struct stat st;
    if (stat(cur_dir, &st) == 0 && !S_ISDIR(st.st_mode)) {
        char *slash = strrchr(cur_dir, '/');
        if (slash) {
            if (slash == cur_dir) slash[1] = '\0';
            else *slash = '\0';
        }
    }

    int win_h = 20;
    int win_w = 76;
    int start_y = (LINES - win_h) / 2;
    int start_x = (COLS - win_w) / 2;
    if (start_y < 0) start_y = 0;
    if (start_x < 0) start_x = 0;

    WINDOW *win = newwin(win_h, win_w, start_y, start_x);
    keypad(win, TRUE);

    size_t selected = 0;
    size_t scroll_offset = 0;
    file_entry_t entries[MAX_ENTRIES];
    int res = -1;

    while (true) {
        /* Read directory */
        size_t n_entries = 0;
        DIR *d = opendir(cur_dir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL && n_entries < MAX_ENTRIES) {
                if (strcmp(de->d_name, ".") == 0) continue;
                if (strcmp(cur_dir, "/") == 0 && strcmp(de->d_name, "..") == 0) continue;

                file_entry_t *e = &entries[n_entries];
                snprintf(e->name, sizeof e->name, "%s", de->d_name);

                char full[1400];
                snprintf(full, sizeof full, "%.1000s/%.255s", strcmp(cur_dir, "/") == 0 ? "" : cur_dir, de->d_name);
                struct stat fst;
                if (stat(full, &fst) == 0) {
                    e->is_dir = S_ISDIR(fst.st_mode);
                    e->size = (uint64_t)fst.st_size;
                } else {
                    e->is_dir = false;
                    e->size = 0;
                }
                n_entries++;
            }
            closedir(d);
        }
        qsort(entries, n_entries, sizeof(file_entry_t), compare_entries);

        if (selected >= n_entries && n_entries > 0)
            selected = n_entries - 1;

        int list_h = win_h - 5;
        if (selected < scroll_offset)
            scroll_offset = selected;
        else if (selected >= scroll_offset + list_h)
            scroll_offset = selected - list_h + 1;

        werase(win);
        box(win, 0, 0);
        mvwprintw(win, 0, 2, " [ Datei / ISO auswaehlen: %.48s ] ", cur_dir);

        for (int i = 0; i < list_h && (scroll_offset + i) < n_entries; i++) {
            size_t idx = scroll_offset + i;
            file_entry_t *e = &entries[idx];

            char display[128];
            if (e->is_dir) {
                snprintf(display, sizeof display, "[DIR]  %s/", e->name);
            } else {
                char sz[32];
                tui_format_size(e->size, sz, sizeof sz);
                snprintf(display, sizeof display, "%-8s %s", sz, e->name);
            }

            if (idx == selected) {
                wattron(win, A_REVERSE | A_BOLD);
                mvwprintw(win, 2 + i, 2, " > %-70.70s", display);
                wattroff(win, A_REVERSE | A_BOLD);
            } else {
                if (e->is_dir) wattron(win, A_BOLD);
                mvwprintw(win, 2 + i, 2, "   %-70.70s", display);
                if (e->is_dir) wattroff(win, A_BOLD);
            }
        }

        mvwprintw(win, win_h - 2, 2, "[Enter]: Waehlen | [N]: Neue Datei | [Pfeile]: Nav | [Esc]: Zurueck");
        wrefresh(win);

        int ch = wgetch(win);
        if (ch == 27 || ch == 'q' || ch == 'Q') {
            res = -1;
            break;
        } else if (ch == 'n' || ch == 'N') {
            if (prompt_new_file(cur_dir, out_selected, out_len)) {
                res = 0;
                break;
            }
        } else if (ch == KEY_UP && selected > 0) {
            selected--;
        } else if (ch == KEY_DOWN && selected + 1 < n_entries) {
            selected++;
        } else if (ch == 10 || ch == KEY_ENTER) {
            if (n_entries == 0) continue;
            file_entry_t *sel = &entries[selected];
            if (sel->is_dir) {
                if (strcmp(sel->name, "..") == 0) {
                    char *slash = strrchr(cur_dir, '/');
                    if (slash && slash != cur_dir) *slash = '\0';
                    else if (slash == cur_dir) slash[1] = '\0';
                } else {
                    if (strcmp(cur_dir, "/") == 0)
                        snprintf(cur_dir, sizeof cur_dir, "/%s", sel->name);
                    else {
                        size_t l = strlen(cur_dir);
                        snprintf(cur_dir + l, sizeof(cur_dir) - l, "/%s", sel->name);
                    }
                }
                selected = 0;
                scroll_offset = 0;
            } else {
                /* Selected file */
                if (strcmp(cur_dir, "/") == 0)
                    snprintf(out_selected, out_len, "/%s", sel->name);
                else
                    snprintf(out_selected, out_len, "%s/%s", cur_dir, sel->name);
                res = 0;
                break;
            }
        }
    }

    delwin(win);
    return res;
}
