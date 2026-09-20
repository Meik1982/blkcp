/**
 * @file io_splice.c
 * @brief Linux Kernel Zero-Copy Splice I/O driver via splice(2).
 *
 * Offloads data movement directly inside the Linux kernel page cache and pipe buffers
 * via the splice(2) system call. Enables true zero-copy throughput between pipes,
 * block devices, character streams, and regular files without roundtripping through
 * userspace memory buffers.
 */

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include <config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdckdint.h>

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

#ifndef SPLICE_F_MOVE
# define SPLICE_F_MOVE 1
#endif
#ifndef SPLICE_F_NONBLOCK
# define SPLICE_F_NONBLOCK 2
#endif
#ifndef SPLICE_F_MORE
# define SPLICE_F_MORE 4
#endif
#ifndef SPLICE_F_GIFT
# define SPLICE_F_GIFT 8
#endif

/**
 * @brief Private runtime state for the splice(2) zero-copy driver.
 */
typedef struct
{
  size_t chunk_size;          /**< Byte size chunk requested per splice call */
  intmax_t total_byte_limit;  /**< Hard limit on total bytes to transfer (-1 if unbounded) */

  bool in_is_pipe;            /**< Input file descriptor is a FIFO or pipe */
  bool out_is_pipe;           /**< Output file descriptor is a FIFO or pipe */
  bool double_splice;         /**< Both sides are non-pipes; routed via kernel pipe buffer */

  int pipefd[2];              /**< Internal kernel pipe buffer pair for double-splice */

  off_t in_file_pos;          /**< Explicit input seek position if seekable */
  off_t out_file_pos;         /**< Explicit output seek position if seekable */
  bool in_seekable;           /**< True if input supports positioned I/O */
  bool out_seekable;          /**< True if output supports positioned I/O */
} splice_driver_state_t;

/**
 * @brief Initializes the splice zero-copy driver, verifies constraints, and configures kernel pipes.
 */
static int
splice_driver_init (dd_context_t *ctx, void **state)
{
  /* Disallow conversions that mutate data in userspace */
  const int incompatible_conv = C_ASCII | C_EBCDIC | C_IBM | C_BLOCK | C_UNBLOCK
                              | C_LCASE | C_UCASE | C_SWAB | C_SYNC | C_SHA256
                              | C_SPARSE;

  if (ctx->cfg.conversions_mask & incompatible_conv)
    return EXIT_FAILURE;

  /* Custom reader functions, direct I/O, and fadvise nocache bypass are incompatible with splice */
  if (ctx->iread_fnc != NULL || ctx->cfg.i_nocache || ctx->cfg.o_nocache)
    return EXIT_FAILURE;

  if ((ctx->cfg.input_flags & (O_DIRECT | O_NOCACHE))
      || (ctx->cfg.output_flags & (O_DIRECT | O_NOCACHE)))
    return EXIT_FAILURE;

  struct stat st_in, st_out;
  if (fstat (STDIN_FILENO, &st_in) != 0 || fstat (STDOUT_FILENO, &st_out) != 0)
    return EXIT_FAILURE;

  splice_driver_state_t *st = xcalloc (1, sizeof (*st));
  st->pipefd[0] = -1;
  st->pipefd[1] = -1;

  st->in_is_pipe = S_ISFIFO (st_in.st_mode);
  st->out_is_pipe = S_ISFIFO (st_out.st_mode);

  /* If neither side is a pipe, we execute double-splice via an internal kernel ring pipe */
  if (!st->in_is_pipe && !st->out_is_pipe)
    {
      if (pipe2 (st->pipefd, O_CLOEXEC) != 0)
        {
          free (st);
          return EXIT_FAILURE;
        }
#if defined F_SETPIPE_SZ
      /* Attempt to expand kernel pipe buffer to 1 MiB for maximum burst throughput */
      (void) fcntl (st->pipefd[0], F_SETPIPE_SZ, 1048576);
#endif
      st->double_splice = true;
    }

  /* Check seekability and initialize file offsets */
  off_t cur_in = lseek (STDIN_FILENO, 0, SEEK_CUR);
  if (cur_in >= 0 && !st->in_is_pipe)
    {
      st->in_seekable = true;
      st->in_file_pos = cur_in;
    }

  off_t cur_out = lseek (STDOUT_FILENO, 0, SEEK_CUR);
  if (cur_out >= 0 && !st->out_is_pipe && !(ctx->cfg.output_flags & O_APPEND))
    {
      st->out_seekable = true;
      st->out_file_pos = cur_out;
    }

  /* Compute total transfer limit if configured */
  st->total_byte_limit = -1;
  if (ctx->cfg.bytes_to_copy >= 0)
    st->total_byte_limit = ctx->cfg.bytes_to_copy;
  else if (ctx->cfg.max_records != INTMAX_MAX || ctx->cfg.max_bytes != 0)
    {
      intmax_t prod;
      if (ckd_mul (&prod, ctx->cfg.max_records, ctx->cfg.input_blocksize)
          || ckd_add (&st->total_byte_limit, prod, ctx->cfg.max_bytes))
        st->total_byte_limit = INTMAX_MAX;
    }

  /* Determine chunk size (default 1 MiB, bounded between 64 KiB and 16 MiB) */
  size_t cs = ctx->cfg.input_blocksize > 0 ? (size_t) ctx->cfg.input_blocksize : 1048576;
  if (cs < 65536)
    cs = 65536;
  if (cs > 16777216)
    cs = 16777216;
  st->chunk_size = cs;

  *state = st;
  return EXIT_SUCCESS;
}

/**
 * @brief Transfers one chunk of data using kernel zero-copy splice(2).
 */
static int
splice_driver_step (dd_context_t *ctx, void *state, bool *eof, bool *fallback)
{
  splice_driver_state_t *st = (splice_driver_state_t *) state;
  size_t to_transfer = st->chunk_size;

  if (st->total_byte_limit >= 0)
    {
      intmax_t remaining = st->total_byte_limit - ctx->stats.w_bytes;
      if (remaining <= 0)
        {
          *eof = true;
          return EXIT_SUCCESS;
        }
      if ((uintmax_t) remaining < to_transfer)
        to_transfer = (size_t) remaining;
    }

  if (st->double_splice)
    {
      /* Step 1: Splice from input into internal kernel pipe buffer */
      loff_t *poff_in = st->in_seekable ? (loff_t *) &st->in_file_pos : NULL;
      ssize_t n_in = splice (STDIN_FILENO, poff_in, st->pipefd[1], NULL, to_transfer,
                             SPLICE_F_MOVE | SPLICE_F_MORE);
      if (n_in < 0)
        {
          if (errno == EINTR)
            return EXIT_SUCCESS;
          if (ctx->stats.w_bytes == 0 && (errno == EINVAL || errno == ENOSYS || errno == ESPIPE))
            {
              *fallback = true;
              return EXIT_SUCCESS;
            }
          diagnose (errno, _("splice error from input %s into pipe"), quoteaf (ctx->cfg.input_file));
          return EXIT_FAILURE;
        }
      if (n_in == 0)
        {
          *eof = true;
          return EXIT_SUCCESS;
        }

      /* Step 2: Splice from internal kernel pipe buffer into output */
      size_t written = 0;
      while (written < (size_t) n_in)
        {
          loff_t *poff_out = st->out_seekable ? (loff_t *) &st->out_file_pos : NULL;
          ssize_t n_out = splice (st->pipefd[0], NULL, STDOUT_FILENO, poff_out, n_in - written,
                                  SPLICE_F_MOVE | SPLICE_F_MORE);
          if (n_out < 0)
            {
              if (errno == EINTR)
                continue;
              diagnose (errno, _("splice error from pipe to output %s"), quoteaf (ctx->cfg.output_file));
              return EXIT_FAILURE;
            }
          if (n_out == 0)
            {
              diagnose (0, _("unexpected EOF splicing pipe to output %s"), quoteaf (ctx->cfg.output_file));
              return EXIT_FAILURE;
            }
          written += n_out;
        }

      ctx->stats.r_full++;
      ctx->stats.w_full++;
      ctx->stats.w_bytes += n_in;
      return EXIT_SUCCESS;
    }

  /* Single splice directly between input and output */
  loff_t *poff_in = (!st->in_is_pipe && st->in_seekable) ? (loff_t *) &st->in_file_pos : NULL;
  loff_t *poff_out = (!st->out_is_pipe && st->out_seekable) ? (loff_t *) &st->out_file_pos : NULL;

  ssize_t ret = splice (STDIN_FILENO, poff_in, STDOUT_FILENO, poff_out, to_transfer,
                        SPLICE_F_MOVE | SPLICE_F_MORE);
  if (ret < 0)
    {
      if (errno == EINTR)
        return EXIT_SUCCESS;

      if (ctx->stats.w_bytes == 0 && (errno == EINVAL || errno == ENOSYS || errno == ESPIPE))
        {
          *fallback = true;
          return EXIT_SUCCESS;
        }

      diagnose (errno, _("error splicing %s to %s"),
                quoteaf (ctx->cfg.input_file), quoteaf (ctx->cfg.output_file));
      return EXIT_FAILURE;
    }

  if (ret == 0)
    {
      *eof = true;
      return EXIT_SUCCESS;
    }

  ctx->stats.w_bytes += ret;
  if (ctx->cfg.output_blocksize > 0 && (size_t) ret == (size_t) ctx->cfg.output_blocksize)
    ctx->stats.w_full++;
  else
    ctx->stats.w_partial++;

  ctx->stats.r_full++;
  return EXIT_SUCCESS;
}

/**
 * @brief Flushes pending state (no-op for synchronous kernel splice).
 */
static int
splice_driver_flush (dd_context_t *ctx, void *state)
{
  (void) ctx;
  (void) state;
  return EXIT_SUCCESS;
}

/**
 * @brief Closes internal pipe file descriptors and deallocates driver state.
 */
static void
splice_driver_cleanup (dd_context_t *ctx, void *state)
{
  (void) ctx;
  splice_driver_state_t *st = (splice_driver_state_t *) state;
  if (!st)
    return;

  if (st->pipefd[0] >= 0)
    close (st->pipefd[0]);
  if (st->pipefd[1] >= 0)
    close (st->pipefd[1]);

  free (st);
}

const dd_io_driver_t splice_io_driver = {
  .name = "kernel_splice",
  .init = splice_driver_init,
  .step = splice_driver_step,
  .flush = splice_driver_flush,
  .cleanup = splice_driver_cleanup
};

#endif /* __linux__ */
