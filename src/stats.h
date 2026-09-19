/**
 * @file stats.h
 * @brief High-resolution throughput telemetry, progress reporting and hash formatting.
 *
 * Implements real-time transfer telemetry (\r), human-readable SI/IEC scaling,
 * final record accounting, and cryptographic SHA-256 digest output.
 */

#ifndef DD_STATS_H
#define DD_STATS_H

#include <stdbool.h>
#include <stdio.h>
#include "blkcp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Print transfer rate and volume statistics (bytes, human scale, elapsed, speed).
 *
 * @param stats Transfer statistics tracking structure.
 * @param progress_len Pointer tracking length of line to cleanly overwrite.
 * @param progress_time Nanosecond timestamp of the progress measurement.
 */
void dd_print_xfer_stats (const dd_stats_t *stats, int *progress_len, xtime_t progress_time);

/**
 * @brief Periodically check elapsed timestamp and render in-flight live progress line.
 *
 * @param stats Transfer statistics tracking structure.
 * @param status_level Telemetry level (STATUS_NONE, STATUS_PROGRESS, STATUS_DEFAULT).
 */
void dd_check_progress (dd_stats_t *stats, int status_level);

/**
 * @brief Print comprehensive completion summary (records in/out, bytes, throughput).
 *
 * @param stats Transfer statistics tracking structure.
 * @param status_level Configured verbosity level.
 * @param progress_len Pointer tracking length of current progress line.
 */
void dd_print_stats (const dd_stats_t *stats, int status_level, int *progress_len);

/**
 * @brief Print formatted 64-character hexadecimal SHA-256 digest to standard output.
 *
 * @param digest Raw 32-byte binary SHA-256 digest buffer.
 */
void dd_print_hash (const unsigned char *digest);

#ifdef __cplusplus
}
#endif

#endif /* DD_STATS_H */
