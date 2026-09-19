/**
 * @file args.h
 * @brief Modern CLI parser and runtime configuration initializer for blkcp.
 *
 * Implements robust command-line parsing supporting modern POSIX/GNU long options
 * (-i, -o, -b, -e, -l, -p, -q, -f, --hash, --autotune, --direct, etc.) as well
 * as intuitive positional operands.
 */

#ifndef DD_ARGS_H
#define DD_ARGS_H

#include <stdbool.h>
#include "blkcp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize a dd_config_t structure with safe, deterministic runtime defaults.
 *
 * Sets standard block sizes to 0 (triggering automatic platform/driver defaults),
 * initializes limits to unbounded, sets status level to STATUS_DEFAULT, and
 * prepares engine configuration.
 *
 * @param cfg Pointer to target configuration structure to initialize.
 */
void dd_init_default_config (dd_config_t *cfg);

/**
 * @brief Parse command-line switches, flags and positional operands from argv.
 *
 * Processes modern getopt_long options (-i, -o, -b, -e, -l, -p, --hash, etc.)
 * with full argument permutation, evaluates multiplicative suffixes (SI/IEC),
 * and validates semantic options combinations.
 *
 * @param argc Argument count from main().
 * @param argv Argument vector from main().
 * @param cfg Target configuration structure to populate.
 * @param warn_partial_read Output flag set to true if configuration warrants warnings on short reads.
 * @param use_fullblock Output flag set to true if fullblock accumulation reader should be engaged.
 */
void dd_scanargs (int argc, char *const *argv, dd_config_t *cfg, bool *warn_partial_read, bool *use_fullblock);

#ifdef __cplusplus
}
#endif

#endif /* DD_ARGS_H */
