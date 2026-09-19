/**
 * @file tui_render.h
 * @brief Form state and rendering interface for blkcp-tui
 */

#ifndef TUI_RENDER_H
#define TUI_RENDER_H

#include <stdbool.h>
#include <stdint.h>
#include <ncurses.h>

/**
 * @brief Engine execution mode in TUI
 */
typedef enum tui_engine_mode {
    TUI_ENGINE_AUTO = 0,
    TUI_ENGINE_URING,
    TUI_ENGINE_ASYNC,
    TUI_ENGINE_REFLINK,
    TUI_ENGINE_SYNC,
    TUI_ENGINE_COUNT
} tui_engine_mode_t;

/**
 * @brief Interactive field IDs for focus and keyboard navigation
 */
typedef enum tui_field_id {
    FIELD_IF = 0,
    FIELD_IF_SEARCH_FILE,
    FIELD_IF_SEARCH_DEV,
    FIELD_IF_SEARCH_PIPE,
    FIELD_OF,
    FIELD_OF_SEARCH_FILE,
    FIELD_OF_SEARCH_DEV,
    FIELD_OF_SEARCH_PIPE,
    FIELD_ENGINE,
    FIELD_BS,
    FIELD_LIMIT,
    FIELD_COUNT,
    FIELD_SKIP,
    FIELD_SEEK,
    FIELD_OPT_AUTOTUNE,
    FIELD_OPT_SHA256,
    FIELD_OPT_DIRECT,
    FIELD_OPT_FORCE,
    FIELD_OPT_SPARSE,
    FIELD_OPT_SYNC,
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
    bool if_is_pipe;
    char of_path[512];
    bool of_is_pipe;

    tui_engine_mode_t engine;
    char bs[64];
    char limit[64];
    char count[64];
    char skip[64];
    char seek[64];

    /* Modern features & flags */
    bool opt_autotune;
    bool opt_sha256;
    bool opt_direct;
    bool opt_force;
    bool opt_sparse;
    bool opt_sync;

    /* Telemetry verbosity */
    int status_mode; /* 0=quiet (-q), 1=default, 2=progress (-p) */

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
 * @param form Form model
 * @param blkcp_bin Binary path to invoke (defaults to "./blkcp" if NULL)
 * @param cmd Output buffer
 * @param cmd_len Buffer size
 */
void tui_build_command(tui_form_t const *form, char const *blkcp_bin, char *cmd, size_t cmd_len);

/**
 * @brief Renders the entire TUI form window
 */
void tui_render(WINDOW *win, tui_form_t const *form);

#endif /* TUI_RENDER_H */
