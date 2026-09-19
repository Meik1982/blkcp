/**
 * @file io_driver.h
 * @brief Unified backend driver interface for pluggable dd I/O execution strategies.
 *
 * This header defines the contract for all data transfer backends:
 * - Synchronous Block I/O (io_sync.c)
 * - Multi-Threaded Ringbuffer Async Pipeline (io_async.c)
 * - Linux Kernel Zero-Copy Reflink via copy_file_range(2) (io_reflink.c)
 *
 * Inversion of Control:
 * Drivers ONLY handle raw chunk movement and driver-internal state.
 * The central orchestration loop in io_engine.c strictly handles:
 * - Signal dispatching (SIGINT, SIGINFO, SIGUSR1) via dd_check_signals()
 * - Real-time progress telemetry (\r) via dd_check_progress()
 * - Bounds checking and limits (count=, iflag=count_bytes)
 * - Streaming cryptographic hash finalization (SHA-256)
 */

#ifndef DD_IO_DRIVER_H
#define DD_IO_DRIVER_H

#include <stdbool.h>
#include "blkcp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Driver operations table implemented by each transfer backend.
 */
typedef struct dd_io_driver
{
  /** Printable backend name (e.g. "sync_block", "async_pipeline", "kernel_reflink") */
  const char *name;

  /**
   * @brief Initialize driver state and allocate necessary buffers or worker threads.
   * @param ctx Active dd runtime context.
   * @param state Output pointer to driver-allocated private state.
   * @return EXIT_SUCCESS on success, EXIT_FAILURE on error.
   */
  int (*init) (dd_context_t *ctx, void **state);

  /**
   * @brief Transfer a single chunk or block of data.
   * @param ctx Active dd runtime context.
   * @param state Driver-private state pointer.
   * @param eof Set to true when the input reaches EOF.
   * @param fallback Set to true if the driver requests seamless fallback to synchronous driver.
   * @return EXIT_SUCCESS on success, EXIT_FAILURE on fatal I/O error.
   */
  int (*step) (dd_context_t *ctx, void *state, bool *eof, bool *fallback);

  /**
   * @brief Flush any pending buffers (e.g. unwritten partial output blocks).
   * @param ctx Active dd runtime context.
   * @param state Driver-private state pointer.
   * @return EXIT_SUCCESS on success, EXIT_FAILURE on error.
   */
  int (*flush) (dd_context_t *ctx, void *state);

  /**
   * @brief Release driver resources, join worker threads and deallocate memory.
   * @param ctx Active dd runtime context.
   * @param state Driver-private state pointer.
   */
  void (*cleanup) (dd_context_t *ctx, void *state);
} dd_io_driver_t;

/* Exported driver instances */
extern const dd_io_driver_t sync_io_driver;
extern const dd_io_driver_t async_io_driver;
#if defined __linux__
extern const dd_io_driver_t reflink_io_driver;
extern const dd_io_driver_t uring_io_driver;
#endif

/**
 * @brief Selects the optimal I/O driver based on configuration flags and file types.
 * @param ctx Active dd runtime context.
 * @return Pointer to selected constant dd_io_driver_t.
 */
const dd_io_driver_t *dd_select_io_driver (dd_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DD_IO_DRIVER_H */
