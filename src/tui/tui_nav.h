/**
 * @file tui_nav.h
 * @brief 2D spatial grid navigation for dd-tui
 */

#ifndef TUI_NAV_H
#define TUI_NAV_H

#include "tui_render.h"

/**
 * @brief Computes next field moving up in the 2D layout
 */
tui_field_id_t tui_nav_up(tui_field_id_t cur);

/**
 * @brief Computes next field moving down in the 2D layout
 */
tui_field_id_t tui_nav_down(tui_field_id_t cur);

/**
 * @brief Computes next field moving left in the 2D layout
 */
tui_field_id_t tui_nav_left(tui_field_id_t cur);

/**
 * @brief Computes next field moving right in the 2D layout
 */
tui_field_id_t tui_nav_right(tui_field_id_t cur);

#endif /* TUI_NAV_H */
