/**
 * @file io_engine_internal.h
 * @brief Internal declarations and shared primitives for dd I/O engine drivers.
 */

#ifndef DD_IO_ENGINE_INTERNAL_H
#define DD_IO_ENGINE_INTERNAL_H

#include <config.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include "system.h"
#include "fd-reopen.h"
#include "quote.h"
#include "quotearg.h"
#include "error.h"
#include "verror.h"
#include "io_driver.h"
#include "signals.h"
#include "conversions.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Global pointer for signal delivery & diagnosis telemetry clearing */
extern dd_context_t *active_ctx;

/* Signal-safe diagnostic reporting */
void dd_diagnose (int errnum, char const *fmt, ...)
  ATTRIBUTE_FORMAT ((__printf__, 2, 3));
#define diagnose dd_diagnose

/* Retry syscalls interrupted by signals (EINTR) while checking pending signals */
#define RETRY_ON_EINTR(call) \
  ({ \
    int __ret; \
    do \
      { \
        if (active_ctx) dd_check_signals (active_ctx); \
        __ret = (call); \
      } \
    while (__ret < 0 && errno == EINTR); \
    __ret; \
  })

#define ifdatasync(fd)                      RETRY_ON_EINTR (fdatasync (fd))
#define ifd_reopen(desired, f, flag, mode)  RETRY_ON_EINTR (fd_reopen (desired, f, flag, mode))
#define ofd_reopen(desired, f, flag, mode)  RETRY_ON_EINTR (fd_reopen (desired, f, flag, mode))
#define ifstat(fd, st)                      RETRY_ON_EINTR (fstat (fd, st))
#define ifsync(fd)                          RETRY_ON_EINTR (fsync (fd))
#define iftruncate(fd, len)                 RETRY_ON_EINTR (ftruncate (fd, len))

/* Common buffer management */
void dd_alloc_ibuf (dd_context_t *ctx);
void dd_alloc_obuf (dd_context_t *ctx);

/* I/O Syscall wrappers */
ssize_t dd_iread (int fd, char *buf, idx_t size);
idx_t dd_iwrite (dd_context_t *ctx, int fd, char const *buf, idx_t size);
void dd_advance_input_offset (dd_context_t *ctx, off_t nread);
void dd_invalidate_cache (int fd, off_t len);

/* Hardware blocksize detection */
idx_t dd_detect_optimal_blocksize (int fd);

/* Low-level writing copiers */
void dd_write_output (dd_context_t *ctx);
void dd_copy_simple (dd_context_t *ctx, char const *buf, idx_t nbytes);

#ifdef __cplusplus
}
#endif

#endif /* DD_IO_ENGINE_INTERNAL_H */
