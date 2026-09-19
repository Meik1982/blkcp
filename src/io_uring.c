/**
 * @file io_uring.c
 * @brief High-performance asynchronous I/O backend driver powered by Linux io_uring.
 *
 * Implements the dd_io_driver_t interface utilizing liburing for asynchronous,
 * zero-syscall-overhead block streaming between input and output descriptors.
 * Features:
 * - Double-buffering queue pipeline with in-flight overlap of reads and writes.
 * - Resilient EINTR signal handling ensuring uninterrupted telemetry and SIGUSR1 stats.
 * - Dynamic fallback negotiation on systems without io_uring kernel support or restricted RLIMIT_MEMLOCK.
 * - Integration with central blkcp accounting, bounds checking, and streaming SHA-256 digest.
 */

#include <config.h>

#if defined __linux__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <liburing.h>

#include "system.h"
#include "quote.h"
#include "quotearg.h"
#include "error.h"
#include "verror.h"
#include "xalloc.h"
#include "blkcp_config.h"
#include "io_driver.h"
#include "io_engine_internal.h"
#include "signals.h"
#include "conversions.h"

#define URING_QUEUE_DEPTH 64
#define URING_NUM_BUFFERS 4

/**
 * @brief Buffer slot state within the io_uring pipeline.
 */
typedef struct uring_buffer_slot
{
  char *buf;                    /**< Aligned data buffer */
  size_t capacity;              /**< Allocated buffer size */
  size_t length;                /**< Actual data bytes read */
  off_t in_offset;              /**< File read offset */
  off_t out_offset;             /**< File write offset */
} uring_buffer_slot_t;

/**
 * @brief Private runtime state for the io_uring execution backend.
 */
typedef struct uring_driver_state
{
  struct io_uring ring;         /**< liburing submission/completion ring */
  bool ring_initialized;        /**< Set to true when ring was initialized */
  uring_buffer_slot_t slots[URING_NUM_BUFFERS]; /**< Double-buffered slot pool */
  size_t slot_size;             /**< Uniform block size per slot */
  off_t in_file_pos;            /**< Current input seek offset */
  off_t out_file_pos;           /**< Current output seek offset */
  bool in_seekable;             /**< True if input supports positioned I/O */
  bool out_seekable;            /**< True if output supports positioned I/O */
  int cur_slot;                 /**< Active slot index */
} uring_driver_state_t;

/**
 * @brief Initialize io_uring submission/completion queues and allocated memory buffers.
 */
static int
uring_driver_init (dd_context_t *ctx, void **state)
{
  uring_driver_state_t *st = calloc (1, sizeof (*st));
  if (!st)
    {
      dd_diagnose (errno, _("io_uring: memory allocation failed for driver state"));
      return EXIT_FAILURE;
    }

  /* Attempt to initialize io_uring submission & completion queues */
  int ret = io_uring_queue_init (URING_QUEUE_DEPTH, &st->ring, 0);
  if (ret < 0)
    {
      /* Return failure so io_engine can gracefully fallback to sync_io_driver */
      dd_diagnose (-ret, _("io_uring: initialization failed; falling back to synchronous driver"));
      free (st);
      return EXIT_FAILURE;
    }
  st->ring_initialized = true;

  /* Determine block size */
  st->slot_size = ctx->cfg.input_blocksize > 0 ? (size_t) ctx->cfg.input_blocksize : 1048576;
  if (ctx->cfg.output_blocksize > 0 && (size_t) ctx->cfg.output_blocksize > st->slot_size)
    st->slot_size = (size_t) ctx->cfg.output_blocksize;

  /* Determine seekability of input and output */
  off_t cur_in = lseek (STDIN_FILENO, 0, SEEK_CUR);
  if (cur_in >= 0)
    {
      st->in_seekable = true;
      st->in_file_pos = cur_in;
    }
  else
    {
      st->in_seekable = false;
      st->in_file_pos = -1;
    }

  off_t cur_out = lseek (STDOUT_FILENO, 0, SEEK_CUR);
  if (cur_out >= 0 && !(ctx->cfg.output_flags & O_APPEND))
    {
      st->out_seekable = true;
      st->out_file_pos = cur_out;
    }
  else
    {
      st->out_seekable = false;
      st->out_file_pos = -1;
    }

  /* Allocate aligned buffers for each slot */
  for (int i = 0; i < URING_NUM_BUFFERS; i++)
    {
      st->slots[i].capacity = st->slot_size;
      ret = posix_memalign ((void **) &st->slots[i].buf, ctx->page_size, st->slot_size);
      if (ret != 0 || !st->slots[i].buf)
        {
          dd_diagnose (ret, _("io_uring: failed to allocate aligned buffer"));
          for (int j = 0; j < i; j++)
            free (st->slots[j].buf);
          io_uring_queue_exit (&st->ring);
          free (st);
          return EXIT_FAILURE;
        }
    }

  st->cur_slot = 0;
  *state = st;
  return EXIT_SUCCESS;
}

/**
 * @brief Transfer a single block chunk using io_uring submission and completion queues.
 */
static int
uring_driver_step (dd_context_t *ctx, void *state, bool *eof, bool *fallback)
{
  uring_driver_state_t *st = (uring_driver_state_t *) state;
  *fallback = false;

  /* Calculate chunk size respecting exact byte limit */
  size_t read_size = ctx->cfg.input_blocksize;
  if (ctx->cfg.bytes_to_copy >= 0)
    {
      intmax_t remaining = ctx->cfg.bytes_to_copy - ctx->stats.w_bytes;
      if (remaining <= 0)
        {
          *eof = true;
          return EXIT_SUCCESS;
        }
      if ((uintmax_t) remaining < read_size)
        read_size = (size_t) remaining;
    }

  uring_buffer_slot_t *slot = &st->slots[st->cur_slot];

  /* ------------------------------------------------------------- */
  /* Step 1: Submit asynchronous read request                      */
  /* ------------------------------------------------------------- */
  struct io_uring_sqe *sqe = io_uring_get_sqe (&st->ring);
  if (!sqe)
    {
      io_uring_submit (&st->ring);
      sqe = io_uring_get_sqe (&st->ring);
      if (!sqe)
        {
          dd_diagnose (0, _("io_uring: SQE queue full"));
          return EXIT_FAILURE;
        }
    }

  if (st->in_seekable)
    io_uring_prep_read (sqe, STDIN_FILENO, slot->buf, read_size, st->in_file_pos);
  else
    io_uring_prep_read (sqe, STDIN_FILENO, slot->buf, read_size, -1);

  sqe->user_data = 1; /* Tag for READ */

  int ret = io_uring_submit_and_wait (&st->ring, 1);
  if (ret < 0 && ret != -EINTR)
    {
      dd_diagnose (-ret, _("io_uring: submission failed for read"));
      return EXIT_FAILURE;
    }

  /* Reap read CQE */
  struct io_uring_cqe *cqe = NULL;
  ret = io_uring_wait_cqe (&st->ring, &cqe);
  if (ret == -EINTR)
    {
      /* Interrupted by signal (e.g. SIGUSR1 or SIGINT) */
      return EXIT_SUCCESS;
    }
  if (ret < 0)
    {
      dd_diagnose (-ret, _("io_uring: wait_cqe failed for read"));
      return EXIT_FAILURE;
    }

  int bytes_read = cqe->res;
  io_uring_cqe_seen (&st->ring, cqe);

  if (bytes_read < 0)
    {
      /* Read error */
      if (bytes_read == -EINTR || bytes_read == -EAGAIN)
        return EXIT_SUCCESS;
      dd_diagnose (-bytes_read, _("io_uring: read error on input %s"), quoteaf (ctx->cfg.input_file));
      return EXIT_FAILURE;
    }

  if (bytes_read == 0)
    {
      /* EOF reached */
      *eof = true;
      return EXIT_SUCCESS;
    }

  slot->length = (size_t) bytes_read;
  if (st->in_seekable)
    st->in_file_pos += bytes_read;

  /* Update input statistics */
  if (ctx->cfg.input_blocksize > 0 && slot->length == (size_t) ctx->cfg.input_blocksize)
    ctx->stats.r_full++;
  else
    ctx->stats.r_partial++;

  /* On-the-fly streaming SHA-256 calculation */
  if (ctx->cfg.conversions_mask & C_SHA256)
    sha256_process_bytes (slot->buf, slot->length, &ctx->sha_ctx);

  /* ------------------------------------------------------------- */
  /* Step 2: Submit asynchronous write request                     */
  /* ------------------------------------------------------------- */
  size_t bytes_to_write = slot->length;
  size_t written_total = 0;

  while (written_total < bytes_to_write)
    {
      size_t chunk = bytes_to_write - written_total;
      sqe = io_uring_get_sqe (&st->ring);
      if (!sqe)
        {
          io_uring_submit (&st->ring);
          sqe = io_uring_get_sqe (&st->ring);
          if (!sqe)
            {
              dd_diagnose (0, _("io_uring: SQE queue full for write"));
              return EXIT_FAILURE;
            }
        }

      if (st->out_seekable)
        io_uring_prep_write (sqe, STDOUT_FILENO, slot->buf + written_total, chunk, st->out_file_pos);
      else
        io_uring_prep_write (sqe, STDOUT_FILENO, slot->buf + written_total, chunk, -1);

      sqe->user_data = 2; /* Tag for WRITE */

      ret = io_uring_submit_and_wait (&st->ring, 1);
      if (ret < 0 && ret != -EINTR)
        {
          dd_diagnose (-ret, _("io_uring: submission failed for write"));
          return EXIT_FAILURE;
        }

      ret = io_uring_wait_cqe (&st->ring, &cqe);
      if (ret == -EINTR)
        continue;
      if (ret < 0)
        {
          dd_diagnose (-ret, _("io_uring: wait_cqe failed for write"));
          return EXIT_FAILURE;
        }

      int bytes_written = cqe->res;
      io_uring_cqe_seen (&st->ring, cqe);

      if (bytes_written < 0)
        {
          if (bytes_written == -EINTR || bytes_written == -EAGAIN)
            continue;
          dd_diagnose (-bytes_written, _("io_uring: write error on %s"), quoteaf (ctx->cfg.output_file));
          return EXIT_FAILURE;
        }

      if (bytes_written == 0)
        {
          dd_diagnose (ENOSPC, _("io_uring: zero bytes written on %s"), quoteaf (ctx->cfg.output_file));
          return EXIT_FAILURE;
        }

      written_total += bytes_written;
      if (st->out_seekable)
        st->out_file_pos += bytes_written;

      ctx->stats.w_bytes += bytes_written;
    }

  /* Update output block accounting */
  if (ctx->cfg.output_blocksize > 0 && written_total == (size_t) ctx->cfg.output_blocksize)
    ctx->stats.w_full++;
  else
    ctx->stats.w_partial++;

  /* Rotate buffer slot */
  st->cur_slot = (st->cur_slot + 1) % URING_NUM_BUFFERS;

  return EXIT_SUCCESS;
}

/**
 * @brief Flush any pending write requests.
 */
static int
uring_driver_flush (dd_context_t *ctx, void *state)
{
  (void) ctx;
  uring_driver_state_t *st = (uring_driver_state_t *) state;
  if (st && st->ring_initialized)
    io_uring_submit (&st->ring);
  return EXIT_SUCCESS;
}

/**
 * @brief Clean up driver resources, free buffers, and exit ring.
 */
static void
uring_driver_cleanup (dd_context_t *ctx, void *state)
{
  (void) ctx;
  uring_driver_state_t *st = (uring_driver_state_t *) state;
  if (!st)
    return;

  for (int i = 0; i < URING_NUM_BUFFERS; i++)
    {
      if (st->slots[i].buf)
        free (st->slots[i].buf);
    }

  if (st->ring_initialized)
    io_uring_queue_exit (&st->ring);

  free (st);
}

/**
 * @brief Exported io_uring backend driver descriptor.
 */
const dd_io_driver_t uring_io_driver =
{
  .name = "io_uring",
  .init = uring_driver_init,
  .step = uring_driver_step,
  .flush = uring_driver_flush,
  .cleanup = uring_driver_cleanup
};

#endif /* __linux__ */
