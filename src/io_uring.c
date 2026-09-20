/**
 * @file io_uring.c
 * @brief High-performance asynchronous I/O backend driver powered by Linux io_uring.
 *
 * Implements the dd_io_driver_t interface utilizing liburing for asynchronous,
 * pipelined block streaming between input and output descriptors.
 * Features:
 * - Overlapped double-buffering pipeline: submits Read-Ahead (slot N+1) concurrently
 *   with Write (slot N) in a single batched io_uring_submit() syscall.
 * - Resilient EINTR signal handling ensuring uninterrupted telemetry and SIGUSR1 stats.
 * - Dynamic fallback negotiation on systems without io_uring kernel support or restricted memory locks.
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

enum uring_tag
{
  TAG_NONE = 0,
  TAG_READ = 1,
  TAG_WRITE = 2
};

/**
 * @brief Buffer slot state within the io_uring pipeline.
 */
typedef struct uring_buffer_slot
{
  char *buf;                    /**< Aligned data buffer */
  size_t capacity;              /**< Allocated buffer size */
  size_t length;                /**< Actual data bytes read */
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

  /* Pipelined execution state */
  int read_slot;                /**< Slot currently reading or next to read */
  int write_slot;               /**< Slot currently writing or next to write */
  bool read_in_flight;          /**< True if an asynchronous read SQE is active */
  bool write_in_flight;         /**< True if an asynchronous write SQE is active */
  bool read_eof;                /**< True if input reached end-of-file */
  bool pipeline_primed;         /**< True once initial read-ahead is submitted */
  bool buffers_registered;      /**< True if kernel fixed buffers registered via io_uring_register_buffers */
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
      st->in_file_pos = 0;
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
      st->out_file_pos = 0;
    }

  /* Allocate page-aligned memory buffers for each slot in the ring pool */
  for (int i = 0; i < URING_NUM_BUFFERS; i++)
    {
      st->slots[i].capacity = st->slot_size;
      st->slots[i].length = 0;
      void *ptr = NULL;
      if (posix_memalign (&ptr, ctx->page_size, st->slots[i].capacity) != 0 || !ptr)
        {
          dd_diagnose (errno, _("io_uring: failed to allocate aligned buffer for slot %d"), i);
          for (int j = 0; j < i; j++)
            free (st->slots[j].buf);
          io_uring_queue_exit (&st->ring);
          free (st);
          return EXIT_FAILURE;
        }
      st->slots[i].buf = (char *) ptr;
    }

  /* Pre-register fixed buffers in kernel to eliminate per-I/O get_user_pages() overhead */
  struct iovec iov[URING_NUM_BUFFERS];
  for (int i = 0; i < URING_NUM_BUFFERS; i++)
    {
      iov[i].iov_base = st->slots[i].buf;
      iov[i].iov_len = st->slots[i].capacity;
    }
  if (io_uring_register_buffers (&st->ring, iov, URING_NUM_BUFFERS) == 0)
    st->buffers_registered = true;
  else
    st->buffers_registered = false;

  st->read_slot = 0;
  st->write_slot = 0;
  st->read_in_flight = false;
  st->write_in_flight = false;
  st->read_eof = false;
  st->pipeline_primed = false;

  *state = st;
  return EXIT_SUCCESS;
}

/**
 * @brief Helper to compute the next read request size observing exact limits.
 */
static size_t
get_next_read_size (dd_context_t const *ctx, size_t default_bs, off_t total_planned_bytes)
{
  size_t read_size = default_bs;
  if (ctx->cfg.bytes_to_copy >= 0)
    {
      intmax_t remaining = ctx->cfg.bytes_to_copy - total_planned_bytes;
      if (remaining <= 0)
        return 0;
      if ((uintmax_t) remaining < read_size)
        read_size = (size_t) remaining;
    }
  return read_size;
}

/**
 * @brief Transfer block chunks using a fully pipelined Read-Ahead / Write overlap.
 */
static int
uring_driver_step (dd_context_t *ctx, void *state, bool *eof, bool *fallback)
{
  uring_driver_state_t *st = (uring_driver_state_t *) state;
  *fallback = false;

  /* ------------------------------------------------------------- */
  /* Phase 1: Pipeline Bootstrap (First Step Only)                 */
  /* ------------------------------------------------------------- */
  if (!st->pipeline_primed)
    {
      size_t first_read = get_next_read_size (ctx, ctx->cfg.input_blocksize, 0);
      if (first_read == 0)
        {
          *eof = true;
          return EXIT_SUCCESS;
        }

      struct io_uring_sqe *sqe = io_uring_get_sqe (&st->ring);
      if (!sqe)
        {
          dd_diagnose (0, _("io_uring: SQE queue full on startup"));
          return EXIT_FAILURE;
        }

      if (st->buffers_registered)
        io_uring_prep_read_fixed (sqe, STDIN_FILENO, st->slots[0].buf, first_read,
                                  st->in_seekable ? (uint64_t) st->in_file_pos : (uint64_t) -1, 0);
      else
        io_uring_prep_read (sqe, STDIN_FILENO, st->slots[0].buf, first_read,
                            st->in_seekable ? st->in_file_pos : -1);

      sqe->user_data = TAG_READ;
      st->read_in_flight = true;
      st->read_slot = 0;
      st->pipeline_primed = true;

      int ret = io_uring_submit (&st->ring);
      if (ret < 0 && ret != -EINTR)
        {
          dd_diagnose (-ret, _("io_uring: initial submission failed"));
          return EXIT_FAILURE;
        }
    }

  /* ------------------------------------------------------------- */
  /* Phase 2: Asynchronous CQE-Harvesting & Completion Handling    */
  /* ------------------------------------------------------------- */
  while (st->read_in_flight || st->write_in_flight)
    {
      struct io_uring_cqe *cqes[URING_NUM_BUFFERS];
      unsigned int n_cqes = io_uring_peek_batch_cqe (&st->ring, cqes, URING_NUM_BUFFERS);

      if (n_cqes == 0)
        {
          /* No completions pending in user ring; block on kernel once */
          struct io_uring_cqe *cqe = NULL;
          int ret = io_uring_wait_cqe (&st->ring, &cqe);
          if (ret == -EINTR)
            return EXIT_SUCCESS; /* Signal interrupt: yield to signal dispatcher */
          if (ret < 0)
            {
              dd_diagnose (-ret, _("io_uring: wait_cqe failed"));
              return EXIT_FAILURE;
            }
          cqes[0] = cqe;
          n_cqes = 1;
        }

      for (unsigned int i = 0; i < n_cqes; i++)
        {
          struct io_uring_cqe *cqe = cqes[i];
          uint64_t tag = cqe->user_data;
          int res = cqe->res;
          io_uring_cqe_seen (&st->ring, cqe);

          if (tag == TAG_WRITE)
            {
              st->write_in_flight = false;
              if (res < 0)
                {
                  if (res == -EINTR || res == -EAGAIN)
                    continue;
                  dd_diagnose (-res, _("io_uring: write error on %s"), quoteaf (ctx->cfg.output_file));
                  return EXIT_FAILURE;
                }

              if (st->out_seekable)
                st->out_file_pos += res;

              ctx->stats.w_bytes += res;
              if (ctx->cfg.blocksize > 0 && (size_t) res == (size_t) ctx->cfg.blocksize)
                ctx->stats.w_full++;
              else
                ctx->stats.w_partial++;
            }
          else if (tag == TAG_READ)
            {
              st->read_in_flight = false;
              if (res < 0)
                {
                  if (res == -EINTR || res == -EAGAIN)
                    continue;
                  dd_diagnose (-res, _("io_uring: read error on %s"), quoteaf (ctx->cfg.input_file));
                  return EXIT_FAILURE;
                }

              if (res == 0)
                {
                  st->read_eof = true;
                  st->slots[st->read_slot].length = 0;
                }
              else
                {
                  st->slots[st->read_slot].length = (size_t) res;
                  if (st->in_seekable)
                    st->in_file_pos += res;

                  if (ctx->cfg.blocksize > 0 && (size_t) res == (size_t) ctx->cfg.blocksize)
                    ctx->stats.r_full++;
                  else
                    ctx->stats.r_partial++;
                }
            }
        }

      /* When both read and write are settled (or read finished and no write pending), proceed */
      if (!st->write_in_flight && !st->read_in_flight)
        break;
    }

  /* ------------------------------------------------------------- */
  /* Phase 3: EOF Termination Evaluation                           */
  /* ------------------------------------------------------------- */
  if (st->read_eof && st->slots[st->read_slot].length == 0)
    {
      *eof = true;
      return EXIT_SUCCESS;
    }

  /* ------------------------------------------------------------- */
  /* Phase 4: Batched Overlapped Read-Ahead & Write Submission      */
  /* ------------------------------------------------------------- */
  int cur_write_slot = st->read_slot;
  size_t bytes_to_write = st->slots[cur_write_slot].length;

  if (ctx->cfg.bytes_to_copy >= 0)
    {
      intmax_t max_allowed = ctx->cfg.bytes_to_copy - ctx->stats.w_bytes;
      if (max_allowed <= 0)
        {
          *eof = true;
          return EXIT_SUCCESS;
        }
      if ((uintmax_t) max_allowed < bytes_to_write)
        bytes_to_write = (size_t) max_allowed;
    }

  /* Compute streaming SHA-256 for the exact written slice */
  if ((ctx->cfg.conversions_mask & C_SHA256) && bytes_to_write > 0 && ctx->sha_evp_ctx)
    EVP_DigestUpdate (ctx->sha_evp_ctx, st->slots[cur_write_slot].buf, bytes_to_write);

  /* 1. Prepare Write SQE for completed read slot */
  struct io_uring_sqe *sqe_w = io_uring_get_sqe (&st->ring);
  if (!sqe_w)
    {
      io_uring_submit (&st->ring);
      sqe_w = io_uring_get_sqe (&st->ring);
      if (!sqe_w)
        {
          dd_diagnose (0, _("io_uring: SQE full for pipelined write"));
          return EXIT_FAILURE;
        }
    }

  if (st->buffers_registered)
    io_uring_prep_write_fixed (sqe_w, STDOUT_FILENO, st->slots[cur_write_slot].buf,
                               bytes_to_write,
                               st->out_seekable ? (uint64_t) st->out_file_pos : (uint64_t) -1,
                               cur_write_slot);
  else
    {
      if (st->out_seekable)
        io_uring_prep_write (sqe_w, STDOUT_FILENO, st->slots[cur_write_slot].buf, bytes_to_write, st->out_file_pos);
      else
        io_uring_prep_write (sqe_w, STDOUT_FILENO, st->slots[cur_write_slot].buf, bytes_to_write, -1);
    }

  sqe_w->user_data = TAG_WRITE;
  st->write_in_flight = true;
  st->write_slot = cur_write_slot;

  /* 2. Prepare Read-Ahead SQE for next buffer slot (if not EOF / limit reached) */
  if (!st->read_eof)
    {
      off_t planned_bytes = ctx->stats.w_bytes + bytes_to_write;
      size_t next_read_size = get_next_read_size (ctx, ctx->cfg.input_blocksize, planned_bytes);

      if (next_read_size > 0)
        {
          int next_read_slot = (cur_write_slot + 1) % URING_NUM_BUFFERS;
          struct io_uring_sqe *sqe_r = io_uring_get_sqe (&st->ring);
          if (sqe_r)
            {
              if (st->buffers_registered)
                io_uring_prep_read_fixed (sqe_r, STDIN_FILENO, st->slots[next_read_slot].buf,
                                          next_read_size,
                                          st->in_seekable ? (uint64_t) st->in_file_pos : (uint64_t) -1,
                                          next_read_slot);
              else
                {
                  if (st->in_seekable)
                    io_uring_prep_read (sqe_r, STDIN_FILENO, st->slots[next_read_slot].buf, next_read_size, st->in_file_pos);
                  else
                    io_uring_prep_read (sqe_r, STDIN_FILENO, st->slots[next_read_slot].buf, next_read_size, -1);
                }

              sqe_r->user_data = TAG_READ;
              st->read_in_flight = true;
              st->read_slot = next_read_slot;
            }
        }
      else
        {
          st->read_eof = true;
        }
    }

  /* 3. Submit both SQEs together in ONE single kernel syscall */
  int ret = io_uring_submit (&st->ring);
  if (ret < 0 && ret != -EINTR)
    {
      dd_diagnose (-ret, _("io_uring: pipelined submission failed"));
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}

/**
 * @brief Flush any remaining in-flight write operations.
 */
static int
uring_driver_flush (dd_context_t *ctx, void *state)
{
  uring_driver_state_t *st = (uring_driver_state_t *) state;
  if (!st || !st->ring_initialized)
    return EXIT_SUCCESS;

  /* Wait for final in-flight write to land on disk */
  while (st->write_in_flight)
    {
      struct io_uring_cqe *cqe = NULL;
      int ret = io_uring_wait_cqe (&st->ring, &cqe);
      if (ret == -EINTR)
        continue;
      if (ret < 0)
        break;

      uint64_t tag = cqe->user_data;
      int res = cqe->res;
      io_uring_cqe_seen (&st->ring, cqe);

      if (tag == TAG_WRITE)
        {
          st->write_in_flight = false;
          if (res > 0)
            {
              if (st->out_seekable)
                st->out_file_pos += res;
              ctx->stats.w_bytes += res;
              if (ctx->cfg.output_blocksize > 0 && (size_t) res == (size_t) ctx->cfg.output_blocksize)
                ctx->stats.w_full++;
              else
                ctx->stats.w_partial++;
            }
        }
    }

  return EXIT_SUCCESS;
}

/**
 * @brief Cleanup io_uring ring instance and free allocated slot buffers.
 */
static void
uring_driver_cleanup (dd_context_t *ctx, void *state)
{
  (void) ctx;
  uring_driver_state_t *st = (uring_driver_state_t *) state;
  if (!st)
    return;

  if (st->ring_initialized)
    {
      if (st->buffers_registered)
        {
          io_uring_unregister_buffers (&st->ring);
          st->buffers_registered = false;
        }
      io_uring_queue_exit (&st->ring);
      st->ring_initialized = false;
    }

  for (int i = 0; i < URING_NUM_BUFFERS; i++)
    {
      if (st->slots[i].buf)
        {
          free (st->slots[i].buf);
          st->slots[i].buf = NULL;
        }
    }

  free (st);
}

const dd_io_driver_t uring_io_driver = {
  .name = "uring",
  .init = uring_driver_init,
  .step = uring_driver_step,
  .flush = uring_driver_flush,
  .cleanup = uring_driver_cleanup
};

#endif /* __linux__ */
