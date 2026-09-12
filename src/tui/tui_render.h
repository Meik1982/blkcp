/**
 * @file tui_render.h
 * @brief Form state and rendering interface for dd-tui
 */

#ifndef TUI_RENDER_H
#define TUI_RENDER_H

#include <stdbool.h>
#include <stdint.h>
#include <ncurses.h>

/**
 * @brief Interactive field IDs for focus and keyboard navigation
 */
typedef enum tui_field_id {
    FIELD_IF = 0,
    FIELD_IF_SEARCH_FILE,
    FIELD_IF_SEARCH_DEV,
    FIELD_OF,
    FIELD_OF_SEARCH_FILE,
    FIELD_OF_SEARCH_DEV,
    FIELD_BS,
    FIELD_COUNT,
    FIELD_COUNT_BYTES,
    FIELD_SKIP,
    FIELD_SKIP_BYTES,
    FIELD_SEEK,
    FIELD_SEEK_BYTES,
    FIELD_OPT_AUTOTUNE,
    FIELD_OPT_ASYNC,
    FIELD_OPT_SHA256,
    FIELD_OPT_FORCE,
    FIELD_CONV,
    FIELD_IFLAG,
    FIELD_OFLAG,
    FIELD_STATUS,
    FIELD_BTN_START,
    FIELD_BTN_COPY,
    FIELD_BTN_QUIT,
    FIELD_COUNT_TOTAL
} tui_field_id_t;

/**
 * @brief Form model holding all user-configured parameters
 */
typedef struct tui_form {
    char if_path[512];
    char of_path[512];
    char bs[64];
    char count[64];
    bool count_bytes;
    char skip[64];
    bool skip_bytes;
    char seek[64];
    bool seek_bytes;

    /* Modern features */
    bool opt_autotune;
    bool opt_async;
    bool opt_sha256;
    bool opt_force;

    /* Standard options */
    char conv[128];
    char iflag[128];
    char oflag[128];
    int status_mode; /* 0=none, 1=noxfer, 2=progress */

    /* Live state */
    bool running;
    double progress_pct;
    uint64_t bytes_copied;
    double speed_mb_s;
    char eta_str[32];
    char sha256_result[65];
    char status_msg[128];

    tui_field_id_t active_field;
} tui_form_t;

/**
 * @brief Initializes form with sensible modern defaults
 */
void tui_init_form(tui_form_t *form);

/**
 * @brief Generates the command-line string matching current form settings
 */
void tui_build_command(tui_form_t const *form, char *cmd, size_t cmd_len);

/**
 * @brief Renders the entire TUI form window
 */
void tui_render(WINDOW *win, tui_form_t const *form);

#endif /* TUI_RENDER_H */
