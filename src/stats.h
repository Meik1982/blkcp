/**
 * @file stats.h
 * @brief High-resolution throughput telemetry, progress reporting and hash formatting.
 *
 * Implements real-time transfer telemetry (\r), human-readable SI/IEC scaling,
 * machine-readable NDJSON telemetry (--json), final record accounting, and
 * cryptographic SHA-256 digest output.
 */

#ifndef DD_STATS_H
#define DD_STATS_H

#include <stdbool.h>
#include <stdio.h>
#include "blkcp_config.h"
#include "xtime.h"

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
 * @brief Periodically check elapsed timestamp and render in-flight live progress line or JSON.
 *
 * @param ctx Reentrant execution context.
 */
void dd_check_progress (dd_context_t *ctx);

/**
 * @brief Print comprehensive completion summary (records in/out, bytes, throughput, or JSON).
 *
 * @param ctx Reentrant execution context.
 */
void dd_print_stats (const dd_context_t *ctx);

/**
 * @brief Print formatted 64-character hexadecimal SHA-256 digest to standard output.
 *
 * @param digest Raw 32-byte binary SHA-256 digest buffer.
 */
void dd_print_hash (const unsigned char *digest);

/**
 * @brief Emit machine-readable NDJSON live progress event to standard error.
 *
 * @param stats Transfer statistics tracking structure.
 * @param total_bytes Total expected bytes (-1 if unbounded/unknown).
 * @param progress_time Current timestamp.
 */
void dd_print_json_progress (const dd_stats_t *stats, intmax_t total_bytes, xtime_t progress_time);

/**
 * @brief Emit machine-readable NDJSON completion summary event to standard error.
 *
 * @param stats Transfer statistics tracking structure.
 * @param digest Raw 32-byte binary SHA-256 digest buffer (or NULL if not computed).
 * @param has_digest True if digest buffer is valid.
 */
void dd_print_json_summary (const dd_stats_t *stats, const unsigned char *digest, bool has_digest);

#ifdef __cplusplus
}
#endif

#endif /* DD_STATS_H */
