/**
 * @file tui_file_picker.h
 * @brief Interactive file & directory selection modal for dd-tui
 */

#ifndef TUI_FILE_PICKER_H
#define TUI_FILE_PICKER_H

#include <stddef.h>

/**
 * @brief Opens an interactive ncurses modal to choose a file or directory
 * @param start_path Initial directory path (e.g. current working directory or /home/meik)
 * @param out_selected Output buffer for selected absolute path
 * @param out_len Maximum buffer length
 * @return 0 on success (file selected), -1 on cancel (Esc/Q)
 */
int tui_pick_file(char const *start_path, char *out_selected, size_t out_len);

/**
 * @brief Opens an interactive ncurses modal to choose a block storage device
 * @param out_selected Output buffer for selected device path (e.g. /dev/sdb)
 * @param out_len Maximum buffer length
 * @return 0 on success, -1 on cancel
 */
int tui_pick_device(char *out_selected, size_t out_len);

/**
 * @brief Opens an interactive modal to configure an input or output pipeline command / stream
 * @param is_input true for input pipeline, false for output pipeline
 * @param out_cmd Output buffer for configured pipe command
 * @param out_len Maximum buffer length
 * @return 0 on success, -1 on cancel
 */
int tui_pick_pipe(bool is_input, char *out_cmd, size_t out_len);

#endif /* TUI_FILE_PICKER_H */
