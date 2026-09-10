#ifndef DD_STATS_H
#define DD_STATS_H

#include <stdbool.h>
#include <stdio.h>
#include "dd_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Print transfer statistics (throughput, human-readable volume, elapsed time) */
void dd_print_xfer_stats (const dd_stats_t *stats, int *progress_len, xtime_t progress_time);

/* Print overall summary statistics (records in/out, truncated, transfer stats) */
void dd_print_stats (const dd_stats_t *stats, int status_level, int *progress_len);

/* Print SHA-256 streaming hash digest */
void dd_print_hash (const unsigned char *digest);

#ifdef __cplusplus
}
#endif

#endif /* DD_STATS_H */
