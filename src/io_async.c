/**
 * @file io_async.c
 * @brief Multi-threaded asynchronous double-buffering ringbuffer pipeline driver.
 *
 * Decouples the block reader from the writer thread using a circular queue
 * of page-aligned memory slots. Allows concurrent reading from slow block devices
 * while writing / translating data without stalling.
 */

#include <config.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "system.h"
#include "alignalloc.h"
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

#define ASYNC_MIN_CAPACITY 4
#define ASYNC_MAX_CAPACITY 128
#define ASYNC_TARGET_BUFFER_BYTES ((size_t) 32 * 1024 * 1024)

/**
 * @brief Represents an individual slot in the circular ringbuffer.
 */
typedef struct async_slot
{
  char *buf;          /**< Page-aligned block memory */
  idx_t nread;        /**< Number of bytes read into the buffer */
  bool is_eof;        /**< Set to true when EOF or read error occurred */
  int err;            /**< errno from read syscall (0 on success) */
} async_slot_t;

/**
 * @brief Shared thread synchronization state between reader and writer threads.
 */
typedef struct async_pipeline
{
  dd_context_t *ctx;
  pthread_t reader_tid;

  pthread_mutex_t mutex;
  pthread_cond_t cond_not_full;
  pthread_cond_t cond_not_empty;

  async_slot_t *slots;    /**< Dynamically allocated circular slot array */
  size_t capacity;        /**< Active queue depth capacity */
  size_t head;            /**< Consumer/writer read index */
  size_t tail;            /**< Producer/reader write index */
  size_t count;           /**< Number of filled slots ready to be written */

  uint64_t reader_stalls; /**< Count of times reader paused because queue was full */
  uint64_t writer_stalls; /**< Count of times writer starved because queue was empty */

  bool stop_requested;    /**< Flag set by writer to terminate reader */
  bool reader_done;       /**< Flag set by reader upon thread completion */
  int reader_exit;        /**< Reader exit code (EXIT_SUCCESS / EXIT_FAILURE) */

  intmax_t r_records_limit;
  idx_t r_bytes_limit;
  intmax_t r_records_done;
  intmax_t total_bytes_read;
} async_pipeline_t;

/**
 * @brief Private driver state handle.
 */
typedef struct
{
  async_pipeline_t pipe;
} async_driver_state_t;

/**
 * @brief Dedicated worker thread that continuously fills ringbuffer slots from stdin.
 */
static void *
async_reader_worker (void *arg)
{
  async_pipeline_t *pipe = (async_pipeline_t *) arg;
  dd_context_t *ctx = pipe->ctx;

  while (true)
    {
      pthread_mutex_lock (&pipe->mutex);
      while (pipe->count == pipe->capacity && !pipe->stop_requested)
        {
          pipe->reader_stalls++;
          pthread_cond_wait (&pipe->cond_not_full, &pipe->mutex);
        }

      if (pipe->stop_requested)
        {
          pthread_mutex_unlock (&pipe->mutex);
          break;
        }

      size_t slot_idx = pipe->tail;
      async_slot_t *slot = &pipe->slots[slot_idx];
      pthread_mutex_unlock (&pipe->mutex);

      /* Respect max_records / max_bytes limits */
      if (pipe->r_records_done >= pipe->r_records_limit + !!pipe->r_bytes_limit)
        {
          slot->is_eof = true;
          slot->err = 0;
          slot->nread = 0;

          pthread_mutex_lock (&pipe->mutex);
          pipe->tail = (pipe->tail + 1) % pipe->capacity;
          pipe->count++;
          pthread_cond_signal (&pipe->cond_not_empty);
          pthread_mutex_unlock (&pipe->mutex);
          break;
        }

      idx_t to_read = ctx->cfg.input_blocksize;
      if (pipe->r_records_done >= pipe->r_records_limit)
        to_read = pipe->r_bytes_limit;

      if (ctx->cfg.bytes_to_copy >= 0)
        {
          intmax_t remaining = ctx->cfg.bytes_to_copy - pipe->total_bytes_read;
          if (remaining <= 0)
            {
              slot->is_eof = true;
              slot->err = 0;
              slot->nread = 0;

              pthread_mutex_lock (&pipe->mutex);
              pipe->tail = (pipe->tail + 1) % pipe->capacity;
              pipe->count++;
              pthread_cond_signal (&pipe->cond_not_empty);
              pthread_mutex_unlock (&pipe->mutex);
              break;
            }
          if (remaining < to_read)
            to_read = (idx_t) remaining;
        }

      ssize_t nread = (ctx->iread_fnc ? ctx->iread_fnc : dd_iread) (STDIN_FILENO, slot->buf, to_read);

      if (nread > 0)
        {
          slot->nread = nread;
          slot->is_eof = false;
          slot->err = 0;
          pipe->r_records_done++;
          pipe->total_bytes_read += nread;
          dd_advance_input_offset (ctx, nread);
        }
      else if (nread == 0)
        {
          slot->nread = 0;
          slot->is_eof = true;
          slot->err = 0;
        }
      else
        {
          slot->nread = 0;
          slot->is_eof = true;
          slot->err = errno;
          pipe->reader_exit = EXIT_FAILURE;
        }

      pthread_mutex_lock (&pipe->mutex);
      pipe->tail = (pipe->tail + 1) % pipe->capacity;
      pipe->count++;
      pthread_cond_signal (&pipe->cond_not_empty);

      if (slot->is_eof || pipe->stop_requested)
        {
          pthread_mutex_unlock (&pipe->mutex);
          break;
        }
      pthread_mutex_unlock (&pipe->mutex);
    }

  pthread_mutex_lock (&pipe->mutex);
  pipe->reader_done = true;
  pthread_cond_signal (&pipe->cond_not_empty);
  pthread_mutex_unlock (&pipe->mutex);

  return NULL;
}

/**
 * @brief Initialize asynchronous pipeline driver, allocate slots and spawn reader thread.
 */
static int
async_driver_init (dd_context_t *ctx, void **state)
{
  async_driver_state_t *st = xmalloc (sizeof *st);
  memset (&st->pipe, 0, sizeof st->pipe);
  st->pipe.ctx = ctx;
  st->pipe.r_records_limit = ctx->cfg.max_records;
  st->pipe.r_bytes_limit = ctx->cfg.max_bytes;

  pthread_mutex_init (&st->pipe.mutex, NULL);
  pthread_cond_init (&st->pipe.cond_not_full, NULL);
  pthread_cond_init (&st->pipe.cond_not_empty, NULL);

  /* Dynamic Ringbuffer Scaling: adapt capacity based on blocksize and target memory */
  size_t capacity = ctx->cfg.async_queue_depth;
  if (capacity == 0)
    {
      if (ctx->cfg.input_blocksize > 0)
        capacity = (size_t) (ASYNC_TARGET_BUFFER_BYTES / ctx->cfg.input_blocksize);
      else
        capacity = 8;

      if (capacity < ASYNC_MIN_CAPACITY)
        capacity = ASYNC_MIN_CAPACITY;
      if (capacity > ASYNC_MAX_CAPACITY)
        capacity = ASYNC_MAX_CAPACITY;
    }
  else
    {
      if (capacity < 2)
        capacity = 2;
      if (capacity > 1024)
        capacity = 1024;
    }

  st->pipe.capacity = capacity;
  st->pipe.slots = xcalloc (st->pipe.capacity, sizeof (async_slot_t));

  /* Allocate aligned memory buffers with graceful fallback on memory pressure */
  size_t allocated = 0;
  while (true)
    {
      bool alloc_ok = true;
      for (size_t i = 0; i < st->pipe.capacity; i++)
        {
          st->pipe.slots[i].buf = alignalloc (ctx->page_size, ctx->cfg.input_blocksize);
          if (!st->pipe.slots[i].buf)
            {
              alloc_ok = false;
              allocated = i;
              break;
            }
        }

      if (alloc_ok)
        break;

      /* Free any partially allocated slots */
      for (size_t i = 0; i < allocated; i++)
        {
          alignfree (st->pipe.slots[i].buf);
          st->pipe.slots[i].buf = NULL;
        }

      /* Graceful degradation: halve queue capacity down to 2 slots before failing */
      if (st->pipe.capacity > 2)
        {
          st->pipe.capacity /= 2;
          if (st->pipe.capacity < 2)
            st->pipe.capacity = 2;
        }
      else
        {
          xalloc_die ();
        }
    }

  ctx->stats.async_capacity = st->pipe.capacity;
  dd_alloc_obuf (ctx);

  if (pthread_create (&st->pipe.reader_tid, NULL, async_reader_worker, &st->pipe) != 0)
    {
      error (0, errno, _("failed to create async reader thread"));
      for (size_t i = 0; i < st->pipe.capacity; i++)
        alignfree (st->pipe.slots[i].buf);
      free (st->pipe.slots);
      free (st);
      return EXIT_FAILURE;
    }

  *state = st;
  return EXIT_SUCCESS;
}

/**
 * @brief Consumes one slot from the ringbuffer, transforms data and writes to stdout.
 */
static int
async_driver_step (dd_context_t *ctx, void *state, bool *eof, bool *fallback)
{
  (void)fallback;
  async_driver_state_t *st = (async_driver_state_t *) state;
  async_pipeline_t *pipe = &st->pipe;

  pthread_mutex_lock (&pipe->mutex);
  while (pipe->count == 0 && !pipe->reader_done)
    {
      pipe->writer_stalls++;
      pthread_cond_wait (&pipe->cond_not_empty, &pipe->mutex);
    }

  if (pipe->count == 0 && pipe->reader_done)
    {
      ctx->stats.async_reader_stalls = pipe->reader_stalls;
      ctx->stats.async_writer_stalls = pipe->writer_stalls;
      pthread_mutex_unlock (&pipe->mutex);
      *eof = true;
      return EXIT_SUCCESS;
    }

  size_t slot_idx = pipe->head;
  async_slot_t *slot = &pipe->slots[slot_idx];
  pthread_mutex_unlock (&pipe->mutex);

  if (slot->is_eof)
    {
      if (slot->err != 0)
        {
          diagnose (slot->err, _("error reading %s"), quoteaf (ctx->cfg.input_file));
          return EXIT_FAILURE;
        }
      *eof = true;
      return EXIT_SUCCESS;
    }

  if (slot->nread == ctx->cfg.input_blocksize)
    ctx->stats.r_full++;
  else
    ctx->stats.r_partial++;

  idx_t nwritten = dd_iwrite (ctx, STDOUT_FILENO, slot->buf, slot->nread);
  ctx->stats.w_bytes += nwritten;
  if (nwritten != slot->nread)
    {
      diagnose (errno, _("error writing %s"), quoteaf (ctx->cfg.output_file));
      if (nwritten != 0)
        ctx->stats.w_partial++;
      return EXIT_FAILURE;
    }
  ctx->stats.w_full++;

  /* Release slot back to producer */
  pthread_mutex_lock (&pipe->mutex);
  pipe->head = (pipe->head + 1) % pipe->capacity;
  pipe->count--;
  pthread_cond_signal (&pipe->cond_not_full);
  pthread_mutex_unlock (&pipe->mutex);

  return EXIT_SUCCESS;
}

/**
 * @brief Flushes any remaining partial blocks accumulated in ctx->obuf.
 */
static int
async_driver_flush (dd_context_t *ctx, void *state)
{
  (void)state;
  if (ctx->oc > 0)
    {
      idx_t nwritten = dd_iwrite (ctx, STDOUT_FILENO, ctx->obuf, ctx->oc);
      ctx->stats.w_bytes += nwritten;
      if (nwritten != ctx->oc)
        {
          diagnose (errno, _("error writing %s"), quoteaf (ctx->cfg.output_file));
          return EXIT_FAILURE;
        }
      if (nwritten != 0)
        ctx->stats.w_partial++;
      ctx->oc = 0;
    }
  return EXIT_SUCCESS;
}

/**
 * @brief Stops reader thread, deallocates slots and destroys mutexes.
 */
static void
async_driver_cleanup (dd_context_t *ctx, void *state)
{
  (void)ctx;
  async_driver_state_t *st = (async_driver_state_t *) state;
  async_pipeline_t *pipe = &st->pipe;

  pthread_mutex_lock (&pipe->mutex);
  pipe->stop_requested = true;
  pthread_cond_signal (&pipe->cond_not_full);
  ctx->stats.async_reader_stalls = pipe->reader_stalls;
  ctx->stats.async_writer_stalls = pipe->writer_stalls;
  pthread_mutex_unlock (&pipe->mutex);

  pthread_join (pipe->reader_tid, NULL);

  for (size_t i = 0; i < pipe->capacity; i++)
    {
      if (pipe->slots[i].buf)
        alignfree (pipe->slots[i].buf);
    }
  free (pipe->slots);
  pipe->slots = NULL;

  pthread_mutex_destroy (&pipe->mutex);
  pthread_cond_destroy (&pipe->cond_not_full);
  pthread_cond_destroy (&pipe->cond_not_empty);
  free (st);
}

const dd_io_driver_t async_io_driver = {
  .name = "async_pipeline",
  .init = async_driver_init,
  .step = async_driver_step,
  .flush = async_driver_flush,
  .cleanup = async_driver_cleanup
};
