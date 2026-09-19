/**
 * @file io_reflink.c
 * @brief Linux Kernel Zero-Copy and Reflink I/O driver via copy_file_range(2).
 *
 * This driver offloads block copying directly into the Linux VFS kernel space.
 * On Copy-on-Write filesystems (Btrfs, XFS, ZFS), it provides instantaneous,
 * zero-disk-space reflink clones. On other filesystems, it executes in-kernel
 * server-side copies without roundtripping through userspace buffers.
 */

#include <config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "system.h"
#include "quote.h"
#include "quotearg.h"
#include "error.h"
#include "verror.h"
#include "xalloc.h"

#include "blkcp_config.h"
#include "stats.h"
#include "signals.h"
#include "conversions.h"
#include "io_driver.h"
#include "io_engine_internal.h"

#if defined __linux__

/**
 * @brief Private runtime state for reflink / zero-copy transfers.
 */
typedef struct
{
  size_t chunk_size;          /**< Chunk size per copy_file_range call (16 MiB - 64 MiB) */
  intmax_t total_byte_limit;  /**< Hard limit on transferred bytes (-1 if unbounded) */
} reflink_driver_state_t;

/**
 * @brief Initializes the reflink zero-copy driver and validates prerequisites.
 */
static int
reflink_driver_init (dd_context_t *ctx, void **state)
{
  /* Disallow conversions that mutate or pad stream content in userspace */
  const int incompatible_conv = C_ASCII | C_EBCDIC | C_IBM | C_BLOCK | C_UNBLOCK
                              | C_LCASE | C_UCASE | C_SWAB | C_SYNC | C_SHA256
                              | C_SPARSE;

  if (ctx->cfg.conversions_mask & incompatible_conv)
    return EXIT_FAILURE;

  /* Custom readers, fadvise nocache and direct I/O are incompatible with CFR */
  if (ctx->iread_fnc != NULL || ctx->cfg.i_nocache || ctx->cfg.o_nocache)
    return EXIT_FAILURE;

  if ((ctx->cfg.input_flags & (O_DIRECT | O_NOCACHE))
      || (ctx->cfg.output_flags & (O_DIRECT | O_NOCACHE)))
    return EXIT_FAILURE;

  /* Both descriptors must point to regular files */
  struct stat st_in, st_out;
  if (fstat (STDIN_FILENO, &st_in) != 0 || !S_ISREG (st_in.st_mode))
    return EXIT_FAILURE;
  if (fstat (STDOUT_FILENO, &st_out) != 0 || !S_ISREG (st_out.st_mode))
    return EXIT_FAILURE;

  /* Don't hijack dynamic autotuning unless reflink was explicitly requested */
  if (!(ctx->cfg.conversions_mask & C_REFLINK) && (ctx->cfg.conversions_mask & C_AUTOTUNE))
    return EXIT_FAILURE;

  reflink_driver_state_t *st = xmalloc (sizeof *st);
  st->total_byte_limit = -1;

  if (ctx->cfg.bytes_to_copy >= 0)
    st->total_byte_limit = ctx->cfg.bytes_to_copy;
  else if ((ctx->cfg.input_flags & O_COUNT_BYTES) && ctx->cfg.max_records != INTMAX_MAX)
    st->total_byte_limit = ctx->cfg.max_records * ctx->cfg.input_blocksize + ctx->cfg.max_bytes;
  else if (ctx->cfg.max_records != INTMAX_MAX || ctx->cfg.max_bytes != 0)
    st->total_byte_limit = ctx->cfg.max_records * ctx->cfg.input_blocksize + ctx->cfg.max_bytes;

  /* Interactive chunking: clamp between 16 MiB and 64 MiB so signals/progress remain responsive */
  st->chunk_size = MAX (ctx->cfg.output_blocksize, 16 * 1024 * 1024);
  if (st->chunk_size > 64 * 1024 * 1024)
    st->chunk_size = 64 * 1024 * 1024;

  *state = st;
  return EXIT_SUCCESS;
}

/**
 * @brief Transfers one chunk of data using copy_file_range(2).
 */
static int
reflink_driver_step (dd_context_t *ctx, void *state, bool *eof, bool *fallback)
{
  reflink_driver_state_t *st = (reflink_driver_state_t *) state;
  size_t to_copy = st->chunk_size;

  if (st->total_byte_limit >= 0)
    {
      intmax_t remaining = st->total_byte_limit - ctx->stats.w_bytes;
      if (remaining <= 0)
        {
          *eof = true;
          return EXIT_SUCCESS;
        }
      if ((uintmax_t)remaining < to_copy)
        to_copy = (size_t)remaining;
    }

  ssize_t ret = copy_file_range (STDIN_FILENO, NULL, STDOUT_FILENO, NULL, to_copy, 0);

  if (ret < 0)
    {
      if (errno == EINTR)
        return EXIT_SUCCESS;

      /* First chunk error fallback: if filesystem does not support CFR (EXDEV, ENOSYS, etc.) */
      if (ctx->stats.w_bytes == 0 && (errno == EXDEV || errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL))
        {
          if (ctx->cfg.conversions_mask & C_REFLINK)
            {
              diagnose (errno, _("copy_file_range not supported for %s to %s"),
                        quoteaf (ctx->cfg.input_file), quoteaf (ctx->cfg.output_file));
              return EXIT_FAILURE;
            }
          *fallback = true;
          return EXIT_SUCCESS;
        }

      diagnose (errno, _("error copying %s to %s via copy_file_range"),
                quoteaf (ctx->cfg.input_file), quoteaf (ctx->cfg.output_file));
      return EXIT_FAILURE;
    }

  if (ret == 0)
    {
      *eof = true;
      return EXIT_SUCCESS;
    }

  /* Update central telemetry accounting */
  ctx->stats.w_bytes += ret;
  ctx->stats.r_full += ret / ctx->cfg.input_blocksize;
  if (ret % ctx->cfg.input_blocksize)
    ctx->stats.r_partial++;

  ctx->stats.w_full += ret / ctx->cfg.output_blocksize;
  if (ret % ctx->cfg.output_blocksize)
    ctx->stats.w_partial++;

  return EXIT_SUCCESS;
}

/**
 * @brief Flush driver state (no-op for kernel zero-copy).
 */
static int
reflink_driver_flush (dd_context_t *ctx, void *state)
{
  (void)ctx;
  (void)state;
  return EXIT_SUCCESS;
}

/**
 * @brief Free driver state.
 */
static void
reflink_driver_cleanup (dd_context_t *ctx, void *state)
{
  (void)ctx;
  free (state);
}

const dd_io_driver_t reflink_io_driver = {
  .name = "kernel_reflink",
  .init = reflink_driver_init,
  .step = reflink_driver_step,
  .flush = reflink_driver_flush,
  .cleanup = reflink_driver_cleanup
};

#endif /* __linux__ */
