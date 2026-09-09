#ifndef DD_ARGS_H
#define DD_ARGS_H

#include <stdbool.h>
#include "dd_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize default configuration values */
void dd_init_default_config (dd_config_t *cfg);

/* Parse operands (if=, of=, bs=, count=, conv=, etc.) from argv */
void dd_scanargs (int argc, char *const *argv, dd_config_t *cfg, bool *warn_partial_read, bool *use_fullblock);

#ifdef __cplusplus
}
#endif

#endif /* DD_ARGS_H */
