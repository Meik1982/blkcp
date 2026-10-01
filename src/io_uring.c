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
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/ioctl.h>
#if defined __linux__
# include <linux/fs.h>
#endif
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
#define URING_SHARD_QUEUE_DEPTH 32
#define URING_DEFAULT_CHUNK_SIZE (64ULL * 1024 * 1024) /* 64 MiB */

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

struct uring_sharded_state;

/**
 * @brief Per-worker execution context for multi-ring io_uring sharding.
 */
typedef struct uring_shard_worker
{
  int worker_id;
  pthread_t tid;
  struct io_uring ring;
  bool ring_initialized;
  char *buf;
  size_t buf_capacity;
  struct uring_sharded_state *parent;
} uring_shard_worker_t;

/**
 * @brief Global coordinator state across all parallel io_uring shards.
 */
typedef struct uring_sharded_state
{
  int num_workers;
  uring_shard_worker_t *workers;
  dd_context_t *ctx;

  uint64_t chunk_size;
  uint64_t total_bytes;
  off_t in_start_pos;
  off_t out_start_pos;

  atomic_uint_fast64_t next_chunk_offset;
  atomic_uint_fast64_t bytes_written;
  atomic_uint_fast64_t records_full;
  atomic_uint_fast64_t records_partial;
  atomic_int active_workers;
  atomic_bool abort_requested;
  atomic_int error_code;
} uring_sharded_state_t;

/**
 * @brief Private runtime state for the io_uring execution backend.
 */
typedef struct uring_driver_state
{
  bool is_sharded;
  uring_sharded_state_t *sharded;

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
 * @brief Worker thread routine executing an isolated io_uring instance.
 */
static void *
uring_shard_worker_thread (void *arg)
{
  uring_shard_worker_t *w = (uring_shard_worker_t *) arg;
  uring_sharded_state_t *st = w->parent;
  size_t bs = w->buf_capacity;

  while (!atomic_load_explicit (&st->abort_requested, memory_order_relaxed))
    {
      uint64_t chunk_offset = atomic_fetch_add_explicit (&st->next_chunk_offset,
                                                         st->chunk_size,
                                                         memory_order_relaxed);
      if (chunk_offset >= st->total_bytes)
        break;

      uint64_t chunk_len = st->chunk_size;
      if (chunk_offset + chunk_len > st->total_bytes)
        chunk_len = st->total_bytes - chunk_offset;

      uint64_t chunk_done = 0;
      while (chunk_done < chunk_len &&
             !atomic_load_explicit (&st->abort_requested, memory_order_relaxed))
        {
          size_t cur_len = bs;
          if (chunk_done + cur_len > chunk_len)
            cur_len = (size_t) (chunk_len - chunk_done);

          off_t in_pos = st->in_start_pos + (off_t) (chunk_offset + chunk_done);
          off_t out_pos = st->out_start_pos + (off_t) (chunk_offset + chunk_done);

          /* Prepare & submit read SQE */
          struct io_uring_sqe *sqe = io_uring_get_sqe (&w->ring);
          if (!sqe)
            {
              io_uring_submit (&w->ring);
              sqe = io_uring_get_sqe (&w->ring);
            }
          if (!sqe)
            {
              atomic_store_explicit (&st->abort_requested, true, memory_order_relaxed);
              atomic_store_explicit (&st->error_code, EIO, memory_order_relaxed);
              break;
            }

          io_uring_prep_read (sqe, STDIN_FILENO, w->buf, cur_len, in_pos);
          io_uring_sqe_set_data64 (sqe, TAG_READ);

          int ret = io_uring_submit (&w->ring);
          if (ret < 0 && ret != -EINTR && ret != -EAGAIN)
            {
              atomic_store_explicit (&st->abort_requested, true, memory_order_relaxed);
              atomic_store_explicit (&st->error_code, -ret, memory_order_relaxed);
              break;
            }

          /* Wait for read CQE */
          struct io_uring_cqe *cqe = NULL;
          while (!atomic_load_explicit (&st->abort_requested, memory_order_relaxed))
            {
              ret = io_uring_wait_cqe (&w->ring, &cqe);
              if (ret == -EINTR || ret == -EAGAIN)
                continue;
              break;
            }
          if (ret < 0 || !cqe)
            {
              if (ret < 0 && ret != -EINTR && ret != -EAGAIN)
                {
                  atomic_store_explicit (&st->abort_requested, true, memory_order_relaxed);
                  atomic_store_explicit (&st->error_code, -ret, memory_order_relaxed);
                }
              break;
            }

          int r_res = cqe->res;
          io_uring_cqe_seen (&w->ring, cqe);

          if (r_res <= 0)
            {
              if (r_res < 0)
                {
                  atomic_store_explicit (&st->abort_requested, true, memory_order_relaxed);
                  atomic_store_explicit (&st->error_code, -r_res, memory_order_relaxed);
                }
              break;
            }

          size_t bytes_to_write = (size_t) r_res;

          /* Positioned write loop until all read bytes are safely written */
          size_t written_so_far = 0;
          while (written_so_far < bytes_to_write &&
                 !atomic_load_explicit (&st->abort_requested, memory_order_relaxed))
            {
              sqe = io_uring_get_sqe (&w->ring);
              if (!sqe)
                {
                  io_uring_submit (&w->ring);
                  sqe = io_uring_get_sqe (&w->ring);
                }
              if (!sqe)
                {
                  atomic_store_explicit (&st->abort_requested, true, memory_order_relaxed);
                  atomic_store_explicit (&st->error_code, EIO, memory_order_relaxed);
                  break;
                }

              io_uring_prep_write (sqe, STDOUT_FILENO, w->buf + written_so_far,
                                   bytes_to_write - written_so_far,
                                   out_pos + (off_t) written_so_far);
              io_uring_sqe_set_data64 (sqe, TAG_WRITE);

              ret = io_uring_submit (&w->ring);
              if (ret < 0 && ret != -EINTR && ret != -EAGAIN)
                {
                  atomic_store_explicit (&st->abort_requested, true, memory_order_relaxed);
                  atomic_store_explicit (&st->error_code, -ret, memory_order_relaxed);
                  break;
                }

              cqe = NULL;
              while (!atomic_load_explicit (&st->abort_requested, memory_order_relaxed))
                {
                  ret = io_uring_wait_cqe (&w->ring, &cqe);
                  if (ret == -EINTR || ret == -EAGAIN)
                    continue;
                  break;
                }
              if (ret < 0 || !cqe)
                {
                  if (ret < 0 && ret != -EINTR && ret != -EAGAIN)
                    {
                      atomic_store_explicit (&st->abort_requested, true, memory_order_relaxed);
                      atomic_store_explicit (&st->error_code, -ret, memory_order_relaxed);
                    }
                  break;
                }

              int w_res = cqe->res;
              io_uring_cqe_seen (&w->ring, cqe);

              if (w_res <= 0)
                {
                  if (w_res < 0)
                    {
                      atomic_store_explicit (&st->abort_requested, true, memory_order_relaxed);
                      atomic_store_explicit (&st->error_code, -w_res, memory_order_relaxed);
                    }
                  break;
                }

              atomic_fetch_add_explicit (&st->bytes_written, (uint64_t) w_res, memory_order_relaxed);
              if ((size_t) w_res == bs)
                atomic_fetch_add_explicit (&st->records_full, 1, memory_order_relaxed);
              else
                atomic_fetch_add_explicit (&st->records_partial, 1, memory_order_relaxed);

              written_so_far += (size_t) w_res;
            }

          if (written_so_far < bytes_to_write)
            break;

          chunk_done += (uint64_t) bytes_to_write;

          /* Short read signifies EOF */
          if (bytes_to_write < cur_len)
            break;
        }
    }

  atomic_fetch_sub_explicit (&st->active_workers, 1, memory_order_release);
  return NULL;
}

/**
 * @brief Initialize multi-ring sharded worker pool.
 */
static int
init_sharded_uring (dd_context_t *ctx, uring_driver_state_t *st,
                    off_t in_pos, off_t out_pos, uint64_t total_bytes)
{
  int num_workers = ctx->cfg.threads;
  if (num_workers <= 0)
    num_workers = 1;
  if (num_workers > 256)
    num_workers = 256;

  uring_sharded_state_t *sh = calloc (1, sizeof (*sh));
  if (!sh)
    return EXIT_FAILURE;

  sh->ctx = ctx;
  sh->num_workers = num_workers;
  sh->total_bytes = total_bytes;
  sh->in_start_pos = in_pos;
  sh->out_start_pos = out_pos;

  size_t bs = ctx->cfg.blocksize > 0 ? (size_t) ctx->cfg.blocksize : 1048576;
  if (bs == 0)
    bs = 1048576;

  /* Dynamically adjust chunk size for smaller inputs */
  uint64_t chunk_size = URING_DEFAULT_CHUNK_SIZE;
  if (total_bytes < chunk_size * (uint64_t) num_workers * 2)
    {
      chunk_size = total_bytes / (uint64_t) (num_workers * 2);
      if (chunk_size < bs)
        chunk_size = bs;
    }
  chunk_size = (chunk_size / bs) * bs;
  if (chunk_size == 0)
    chunk_size = bs;
  sh->chunk_size = chunk_size;

  atomic_init (&sh->next_chunk_offset, 0);
  atomic_init (&sh->bytes_written, 0);
  atomic_init (&sh->records_full, 0);
  atomic_init (&sh->records_partial, 0);
  atomic_init (&sh->active_workers, num_workers);
  atomic_init (&sh->abort_requested, false);
  atomic_init (&sh->error_code, 0);

  sh->workers = calloc ((size_t) num_workers, sizeof (*sh->workers));
  if (!sh->workers)
    {
      free (sh);
      return EXIT_FAILURE;
    }

  for (int i = 0; i < num_workers; i++)
    {
      uring_shard_worker_t *w = &sh->workers[i];
      w->worker_id = i;
      w->parent = sh;
      w->buf_capacity = bs;

      void *ptr = NULL;
      if (posix_memalign (&ptr, ctx->page_size > 0 ? ctx->page_size : 4096, bs) != 0 || !ptr)
        {
          dd_diagnose (errno, _("io_uring sharding: failed to allocate aligned buffer for worker %d"), i);
          for (int j = 0; j < i; j++)
            {
              io_uring_queue_exit (&sh->workers[j].ring);
              free (sh->workers[j].buf);
            }
          free (sh->workers);
          free (sh);
          return EXIT_FAILURE;
        }
      w->buf = (char *) ptr;

      int ret = io_uring_queue_init (URING_SHARD_QUEUE_DEPTH, &w->ring, 0);
      if (ret < 0)
        {
          dd_diagnose (-ret, _("io_uring sharding: ring init failed for worker %d"), i);
          free (w->buf);
          for (int j = 0; j < i; j++)
            {
              io_uring_queue_exit (&sh->workers[j].ring);
              free (sh->workers[j].buf);
            }
          free (sh->workers);
          free (sh);
          return EXIT_FAILURE;
        }
      w->ring_initialized = true;
    }

  /* Spawn parallel worker threads */
  for (int i = 0; i < num_workers; i++)
    {
      if (pthread_create (&sh->workers[i].tid, NULL, uring_shard_worker_thread, &sh->workers[i]) != 0)
        {
          dd_diagnose (errno, _("io_uring sharding: failed to create worker thread %d"), i);
          atomic_store_explicit (&sh->abort_requested, true, memory_order_relaxed);
          for (int j = 0; j < i; j++)
            pthread_join (sh->workers[j].tid, NULL);
          for (int j = 0; j < num_workers; j++)
            {
              if (sh->workers[j].ring_initialized)
                io_uring_queue_exit (&sh->workers[j].ring);
              if (sh->workers[j].buf)
                free (sh->workers[j].buf);
            }
          free (sh->workers);
          free (sh);
          return EXIT_FAILURE;
        }
    }

  st->is_sharded = true;
  st->sharded = sh;
  return EXIT_SUCCESS;
}

/**
 * @brief Step function for multi-ring sharded execution.
 */
static int
uring_sharded_step (dd_context_t *ctx, uring_sharded_state_t *sh, bool *eof)
{
  struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000 }; /* 5 ms sleep */
  nanosleep (&ts, NULL);

  uint64_t w_bytes = atomic_load_explicit (&sh->bytes_written, memory_order_relaxed);
  ctx->stats.w_bytes = (intmax_t) w_bytes;
  ctx->stats.r_full = (intmax_t) atomic_load_explicit (&sh->records_full, memory_order_relaxed);
  ctx->stats.w_full = ctx->stats.r_full;
  ctx->stats.r_partial = (intmax_t) atomic_load_explicit (&sh->records_partial, memory_order_relaxed);
  ctx->stats.w_partial = ctx->stats.r_partial;

  if (atomic_load_explicit (&sh->abort_requested, memory_order_relaxed))
    {
      int err = atomic_load_explicit (&sh->error_code, memory_order_relaxed);
      if (err != 0)
        dd_diagnose (err, _("io_uring sharding: transfer error"));
      return EXIT_FAILURE;
    }

  int active = atomic_load_explicit (&sh->active_workers, memory_order_acquire);
  if (active == 0)
    {
      *eof = true;
      if (sh->out_start_pos >= 0)
        lseek (STDOUT_FILENO, sh->out_start_pos + (off_t) w_bytes, SEEK_SET);
      if (sh->in_start_pos >= 0)
        lseek (STDIN_FILENO, sh->in_start_pos + (off_t) w_bytes, SEEK_SET);
      return EXIT_SUCCESS;
    }

  return EXIT_SUCCESS;
}

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

  /* Determine seekability of input and output */
  off_t cur_in = lseek (STDIN_FILENO, 0, SEEK_CUR);
  bool in_seekable = (cur_in >= 0);
  off_t cur_out = lseek (STDOUT_FILENO, 0, SEEK_CUR);
  bool out_seekable = (cur_out >= 0 && !(ctx->cfg.output_flags & O_APPEND));

  /* Check if Multi-Ring Sharding is requested and eligible */
  if (ctx->cfg.threads > 1)
    {
      if (ctx->cfg.conversions_mask & C_SHA256)
        {
          if (ctx->cfg.status_level != STATUS_NONE)
            dd_diagnose (0, _("io_uring: --hash requires sequential digest; falling back to single-ring mode"));
        }
      else if (ctx->cfg.conversions_mask & (C_SPARSE | C_SWAB | C_SYNC | C_NOERROR))
        {
          if (ctx->cfg.status_level != STATUS_NONE)
            dd_diagnose (0, _("io_uring: conversion requires sequential streaming; falling back to single-ring mode"));
        }
      else if (!in_seekable || !out_seekable)
        {
          if (ctx->cfg.status_level != STATUS_NONE)
            dd_diagnose (0, _("io_uring: non-seekable streams require sequential I/O; falling back to single-ring mode"));
        }
      else
        {
          uint64_t total_bytes = 0;
          if (ctx->cfg.bytes_to_copy >= 0)
            total_bytes = (uint64_t) ctx->cfg.bytes_to_copy;
          else if (ctx->total_input_size > 0 && ctx->total_input_size > cur_in)
            total_bytes = (uint64_t) (ctx->total_input_size - cur_in);

          if (total_bytes > 0)
            {
              int rc = init_sharded_uring (ctx, st, cur_in, cur_out, total_bytes);
              if (rc == EXIT_SUCCESS)
                {
                  *state = st;
                  return EXIT_SUCCESS;
                }
              /* If sharded initialization fails, fall back to single-ring */
            }
        }
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

  st->in_seekable = in_seekable;
  st->in_file_pos = in_seekable ? cur_in : 0;
  st->out_seekable = out_seekable;
  st->out_file_pos = out_seekable ? cur_out : 0;

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

  if (st && st->is_sharded)
    return uring_sharded_step (ctx, st->sharded, eof);

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
  if (!st)
    return EXIT_SUCCESS;

  if (st->is_sharded)
    return EXIT_SUCCESS;

  if (!st->ring_initialized)
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

  if (st->is_sharded)
    {
      uring_sharded_state_t *sh = st->sharded;
      if (sh)
        {
          atomic_store_explicit (&sh->abort_requested, true, memory_order_relaxed);
          for (int i = 0; i < sh->num_workers; i++)
            {
              if (sh->workers[i].tid)
                pthread_join (sh->workers[i].tid, NULL);
              if (sh->workers[i].ring_initialized)
                io_uring_queue_exit (&sh->workers[i].ring);
              if (sh->workers[i].buf)
                free (sh->workers[i].buf);
            }
          free (sh->workers);
          free (sh);
          st->sharded = NULL;
        }
      free (st);
      return;
    }

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
