/**
 * @file tui_nav.c
 * @brief 2D spatial grid navigation implementation for blkcp-tui
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

    case FIELD_ENGINE:          return FIELD_QUEUE_DEPTH;
    case FIELD_QUEUE_DEPTH:     return FIELD_ENGINE;
    case FIELD_BS:              return FIELD_BS;
    case FIELD_LIMIT:           return FIELD_LIMIT;

    case FIELD_COUNT:           return FIELD_SKIP;
    case FIELD_SKIP:            return FIELD_SEEK;
    case FIELD_SEEK:            return FIELD_COUNT;

    case FIELD_OPT_AUTOTUNE:    return FIELD_OPT_SHA256;
    case FIELD_OPT_SHA256:      return FIELD_OPT_AUTOTUNE;

    case FIELD_OPT_DIRECT:      return FIELD_OPT_FORCE;
    case FIELD_OPT_FORCE:       return FIELD_OPT_DIRECT;

    case FIELD_OPT_SPARSE:      return FIELD_OPT_SYNC;
    case FIELD_OPT_SYNC:        return FIELD_OPT_SPARSE;
    case FIELD_OPT_DRY_RUN:     return FIELD_OPT_DRY_RUN;

    case FIELD_STATUS:          return FIELD_STATUS;

    case FIELD_BTN_START:       return FIELD_BTN_SIMULATE;
    case FIELD_BTN_SIMULATE:    return FIELD_BTN_COPY;
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

    case FIELD_ENGINE:          return FIELD_QUEUE_DEPTH;
    case FIELD_QUEUE_DEPTH:     return FIELD_ENGINE;
    case FIELD_BS:              return FIELD_BS;
    case FIELD_LIMIT:           return FIELD_LIMIT;

    case FIELD_SEEK:            return FIELD_SKIP;
    case FIELD_SKIP:            return FIELD_COUNT;
    case FIELD_COUNT:           return FIELD_SEEK;

    case FIELD_OPT_SHA256:      return FIELD_OPT_AUTOTUNE;
    case FIELD_OPT_AUTOTUNE:    return FIELD_OPT_SHA256;

    case FIELD_OPT_FORCE:       return FIELD_OPT_DIRECT;
    case FIELD_OPT_DIRECT:      return FIELD_OPT_FORCE;

    case FIELD_OPT_SYNC:        return FIELD_OPT_SPARSE;
    case FIELD_OPT_SPARSE:      return FIELD_OPT_SYNC;
    case FIELD_OPT_DRY_RUN:     return FIELD_OPT_DRY_RUN;

    case FIELD_STATUS:          return FIELD_STATUS;

    case FIELD_BTN_QUIT:        return FIELD_BTN_COPY;
    case FIELD_BTN_COPY:        return FIELD_BTN_SIMULATE;
    case FIELD_BTN_SIMULATE:    return FIELD_BTN_START;
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
    case FIELD_OF_SEARCH_PIPE:  return FIELD_ENGINE;

    case FIELD_ENGINE:          return FIELD_BS;
    case FIELD_QUEUE_DEPTH:     return FIELD_BS;
    case FIELD_BS:              return FIELD_LIMIT;
    case FIELD_LIMIT:           return FIELD_COUNT;

    case FIELD_COUNT:           return FIELD_OPT_AUTOTUNE;
    case FIELD_SKIP:            return FIELD_OPT_SHA256;
    case FIELD_SEEK:            return FIELD_OPT_SHA256;

    case FIELD_OPT_AUTOTUNE:    return FIELD_OPT_DIRECT;
    case FIELD_OPT_SHA256:      return FIELD_OPT_FORCE;

    case FIELD_OPT_DIRECT:      return FIELD_OPT_SPARSE;
    case FIELD_OPT_FORCE:       return FIELD_OPT_SYNC;

    case FIELD_OPT_SPARSE:      return FIELD_OPT_DRY_RUN;
    case FIELD_OPT_SYNC:        return FIELD_OPT_DRY_RUN;
    case FIELD_OPT_DRY_RUN:     return FIELD_STATUS;

    case FIELD_STATUS:          return FIELD_BTN_START;

    case FIELD_BTN_START:       return FIELD_IF;
    case FIELD_BTN_SIMULATE:    return FIELD_IF_SEARCH_FILE;
    case FIELD_BTN_COPY:        return FIELD_IF_SEARCH_DEV;
    case FIELD_BTN_QUIT:        return FIELD_IF_SEARCH_PIPE;

    default:                    return cur;
    }
}

tui_field_id_t
tui_nav_up(tui_field_id_t cur)
{
    switch (cur) {
    case FIELD_IF:              return FIELD_BTN_START;
    case FIELD_IF_SEARCH_FILE:  return FIELD_BTN_SIMULATE;
    case FIELD_IF_SEARCH_DEV:   return FIELD_BTN_COPY;
    case FIELD_IF_SEARCH_PIPE:  return FIELD_BTN_QUIT;

    case FIELD_OF:              return FIELD_IF;
    case FIELD_OF_SEARCH_FILE:  return FIELD_IF_SEARCH_FILE;
    case FIELD_OF_SEARCH_DEV:   return FIELD_IF_SEARCH_DEV;
    case FIELD_OF_SEARCH_PIPE:  return FIELD_IF_SEARCH_PIPE;

    case FIELD_ENGINE:          return FIELD_OF;
    case FIELD_QUEUE_DEPTH:     return FIELD_OF;
    case FIELD_BS:              return FIELD_ENGINE;
    case FIELD_LIMIT:           return FIELD_BS;
    case FIELD_COUNT:           return FIELD_LIMIT;
    case FIELD_SKIP:            return FIELD_LIMIT;
    case FIELD_SEEK:            return FIELD_LIMIT;

    case FIELD_OPT_AUTOTUNE:    return FIELD_COUNT;
    case FIELD_OPT_SHA256:      return FIELD_SKIP;

    case FIELD_OPT_DIRECT:      return FIELD_OPT_AUTOTUNE;
    case FIELD_OPT_FORCE:       return FIELD_OPT_SHA256;

    case FIELD_OPT_SPARSE:      return FIELD_OPT_DIRECT;
    case FIELD_OPT_SYNC:        return FIELD_OPT_FORCE;

    case FIELD_OPT_DRY_RUN:     return FIELD_OPT_SPARSE;

    case FIELD_STATUS:          return FIELD_OPT_DRY_RUN;

    case FIELD_BTN_START:
    case FIELD_BTN_SIMULATE:
    case FIELD_BTN_COPY:
    case FIELD_BTN_QUIT:        return FIELD_STATUS;

    default:                    return cur;
    }
}
