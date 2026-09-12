/**
 * @file tui_nav.c
 * @brief 2D spatial grid navigation implementation for dd-tui
 */

#include <config.h>
#include "tui_nav.h"

tui_field_id_t
tui_nav_right(tui_field_id_t cur)
{
    switch (cur) {
    case FIELD_IF:              return FIELD_IF_SEARCH_FILE;
    case FIELD_IF_SEARCH_FILE:  return FIELD_IF_SEARCH_DEV;
    case FIELD_IF_SEARCH_DEV:   return FIELD_IF_SEARCH_PIPE;
    case FIELD_IF_SEARCH_PIPE:  return FIELD_IF;

    case FIELD_OF:              return FIELD_OF_SEARCH_FILE;
    case FIELD_OF_SEARCH_FILE:  return FIELD_OF_SEARCH_DEV;
    case FIELD_OF_SEARCH_DEV:   return FIELD_OF_SEARCH_PIPE;
    case FIELD_OF_SEARCH_PIPE:  return FIELD_OF;

    case FIELD_BS:              return FIELD_BS;

    case FIELD_COUNT:           return FIELD_COUNT_BYTES;
    case FIELD_COUNT_BYTES:     return FIELD_COUNT;

    case FIELD_SKIP:            return FIELD_SKIP_BYTES;
    case FIELD_SKIP_BYTES:      return FIELD_SKIP;

    case FIELD_SEEK:            return FIELD_SEEK_BYTES;
    case FIELD_SEEK_BYTES:      return FIELD_SEEK;

    case FIELD_OPT_AUTOTUNE:    return FIELD_OPT_ASYNC;
    case FIELD_OPT_ASYNC:       return FIELD_OPT_AUTOTUNE;

    case FIELD_OPT_SHA256:      return FIELD_OPT_FORCE;
    case FIELD_OPT_FORCE:       return FIELD_OPT_SHA256;

    case FIELD_CONV:            return FIELD_IFLAG;
    case FIELD_IFLAG:           return FIELD_OFLAG;
    case FIELD_OFLAG:           return FIELD_CONV;

    case FIELD_STATUS:          return FIELD_STATUS;

    case FIELD_BTN_START:       return FIELD_BTN_COPY;
    case FIELD_BTN_COPY:        return FIELD_BTN_QUIT;
    case FIELD_BTN_QUIT:        return FIELD_BTN_START;

    default:                    return cur;
    }
}

tui_field_id_t
tui_nav_left(tui_field_id_t cur)
{
    switch (cur) {
    case FIELD_IF_SEARCH_PIPE:  return FIELD_IF_SEARCH_DEV;
    case FIELD_IF_SEARCH_DEV:   return FIELD_IF_SEARCH_FILE;
    case FIELD_IF_SEARCH_FILE:  return FIELD_IF;
    case FIELD_IF:              return FIELD_IF_SEARCH_PIPE;

    case FIELD_OF_SEARCH_PIPE:  return FIELD_OF_SEARCH_DEV;
    case FIELD_OF_SEARCH_DEV:   return FIELD_OF_SEARCH_FILE;
    case FIELD_OF_SEARCH_FILE:  return FIELD_OF;
    case FIELD_OF:              return FIELD_OF_SEARCH_PIPE;

    case FIELD_BS:              return FIELD_BS;

    case FIELD_COUNT_BYTES:     return FIELD_COUNT;
    case FIELD_COUNT:           return FIELD_COUNT_BYTES;

    case FIELD_SKIP_BYTES:      return FIELD_SKIP;
    case FIELD_SKIP:            return FIELD_SKIP_BYTES;

    case FIELD_SEEK_BYTES:      return FIELD_SEEK;
    case FIELD_SEEK:            return FIELD_SEEK_BYTES;

    case FIELD_OPT_ASYNC:       return FIELD_OPT_AUTOTUNE;
    case FIELD_OPT_AUTOTUNE:    return FIELD_OPT_ASYNC;

    case FIELD_OPT_FORCE:       return FIELD_OPT_SHA256;
    case FIELD_OPT_SHA256:      return FIELD_OPT_FORCE;

    case FIELD_OFLAG:           return FIELD_IFLAG;
    case FIELD_IFLAG:           return FIELD_CONV;
    case FIELD_CONV:            return FIELD_OFLAG;

    case FIELD_STATUS:          return FIELD_STATUS;

    case FIELD_BTN_QUIT:        return FIELD_BTN_COPY;
    case FIELD_BTN_COPY:        return FIELD_BTN_START;
    case FIELD_BTN_START:       return FIELD_BTN_QUIT;

    default:                    return cur;
    }
}

tui_field_id_t
tui_nav_down(tui_field_id_t cur)
{
    switch (cur) {
    case FIELD_IF:              return FIELD_OF;
    case FIELD_IF_SEARCH_FILE:  return FIELD_OF_SEARCH_FILE;
    case FIELD_IF_SEARCH_DEV:   return FIELD_OF_SEARCH_DEV;
    case FIELD_IF_SEARCH_PIPE:  return FIELD_OF_SEARCH_PIPE;

    case FIELD_OF:
    case FIELD_OF_SEARCH_FILE:
    case FIELD_OF_SEARCH_DEV:
    case FIELD_OF_SEARCH_PIPE:  return FIELD_BS;

    case FIELD_BS:              return FIELD_COUNT;

    case FIELD_COUNT:           return FIELD_SKIP;
    case FIELD_COUNT_BYTES:     return FIELD_SKIP_BYTES;

    case FIELD_SKIP:            return FIELD_SEEK;
    case FIELD_SKIP_BYTES:      return FIELD_SEEK_BYTES;

    case FIELD_SEEK:            return FIELD_OPT_AUTOTUNE;
    case FIELD_SEEK_BYTES:      return FIELD_OPT_ASYNC;

    case FIELD_OPT_AUTOTUNE:    return FIELD_OPT_SHA256;
    case FIELD_OPT_ASYNC:       return FIELD_OPT_FORCE;

    case FIELD_OPT_SHA256:      return FIELD_CONV;
    case FIELD_OPT_FORCE:       return FIELD_OFLAG;

    case FIELD_CONV:
    case FIELD_IFLAG:
    case FIELD_OFLAG:           return FIELD_STATUS;

    case FIELD_STATUS:          return FIELD_BTN_START;

    case FIELD_BTN_START:       return FIELD_IF;
    case FIELD_BTN_COPY:        return FIELD_IF_SEARCH_FILE;
    case FIELD_BTN_QUIT:        return FIELD_IF_SEARCH_DEV;

    default:                    return cur;
    }
}

tui_field_id_t
tui_nav_up(tui_field_id_t cur)
{
    switch (cur) {
    case FIELD_IF:              return FIELD_BTN_START;
    case FIELD_IF_SEARCH_FILE:  return FIELD_BTN_COPY;
    case FIELD_IF_SEARCH_DEV:
    case FIELD_IF_SEARCH_PIPE:  return FIELD_BTN_QUIT;

    case FIELD_OF:              return FIELD_IF;
    case FIELD_OF_SEARCH_FILE:  return FIELD_IF_SEARCH_FILE;
    case FIELD_OF_SEARCH_DEV:   return FIELD_IF_SEARCH_DEV;
    case FIELD_OF_SEARCH_PIPE:  return FIELD_IF_SEARCH_PIPE;

    case FIELD_BS:              return FIELD_OF;

    case FIELD_COUNT:           return FIELD_BS;
    case FIELD_COUNT_BYTES:     return FIELD_BS;

    case FIELD_SKIP:            return FIELD_COUNT;
    case FIELD_SKIP_BYTES:      return FIELD_COUNT_BYTES;

    case FIELD_SEEK:            return FIELD_SKIP;
    case FIELD_SEEK_BYTES:      return FIELD_SKIP_BYTES;

    case FIELD_OPT_AUTOTUNE:    return FIELD_SEEK;
    case FIELD_OPT_ASYNC:       return FIELD_SEEK_BYTES;

    case FIELD_OPT_SHA256:      return FIELD_OPT_AUTOTUNE;
    case FIELD_OPT_FORCE:       return FIELD_OPT_ASYNC;

    case FIELD_CONV:
    case FIELD_IFLAG:           return FIELD_OPT_SHA256;
    case FIELD_OFLAG:           return FIELD_OPT_FORCE;

    case FIELD_STATUS:          return FIELD_CONV;

    case FIELD_BTN_START:
    case FIELD_BTN_COPY:
    case FIELD_BTN_QUIT:        return FIELD_STATUS;

    default:                    return cur;
    }
}
