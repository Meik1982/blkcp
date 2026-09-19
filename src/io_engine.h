/**
 * @file io_engine.h
 * @brief High-level orchestration engine and execution entry point for blkcp.
 *
 * Coordinates stream setup, Target Safety Guard validation, skip/seek alignment,
 * backend driver selection, signal-aware hot loop execution, and post-transfer
 * cache and file synchronization.
 */

#ifndef DD_IO_ENGINE_H
#define DD_IO_ENGINE_H

#include "blkcp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Synchronize written output data and optional metadata to physical storage.
 *
 * Executes fdatasync(2) or fsync(2) on the output file descriptor based on
 * active conversion flags (C_FDATASYNC, C_FSYNC).
 *
 * @param ctx Active runtime context.
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on sync error.
 */
int dd_synchronize_output (dd_context_t *ctx);

/**
 * @brief Clean up descriptors and pending buffers.
 *
 * Closes allocated input and output file descriptors and flushes pending writes.
 *
 * @param ctx Active runtime context.
 */
void dd_engine_cleanup (dd_context_t *ctx);

/**
 * @brief Fullblock reader function guaranteeing exact block retrieval before EOF.
 *
 * Accumulates short reads into buffer until requested size is satisfied or EOF occurs.
 *
 * @param fd File descriptor to read from.
 * @param buf Target buffer.
 * @param size Target size in bytes.
 * @return Total bytes accumulated, 0 on EOF, or -1 on fatal error.
 */
ssize_t dd_iread_fullblock (int fd, char *buf, idx_t size);

/**
 * @brief Execute the complete blkcp I/O pipeline.
 *
 * Sets up streams, applies Target Safety Guard checks, invokes skip/seek offsets,
 * initializes the optimal backend driver (io_uring, async, reflink, or sync),
 * and enters the central orchestration loop.
 *
 * @param ctx Fully configured runtime context.
 * @return EXIT_SUCCESS on successful completion, EXIT_FAILURE on error.
 */
int dd_execute (dd_context_t *ctx);

/**
 * @brief Explicitly release all allocated runtime buffers and heap state in context.
 *
 * Deallocates input/output buffers (ibuf, obuf) via alignfree.
 *
 * @param ctx Runtime context to clean up.
 */
void dd_context_free (dd_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DD_IO_ENGINE_H */
