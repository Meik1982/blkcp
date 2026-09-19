/**
 * @file io_sync.c
 * @brief Synchronous block-by-block data transfer driver with dynamic throughput autotuning.
 *
 * Implements the standard POSIX dd block copying paradigm, augmented with
 * zero-overhead fast paths for matching ibs/obs, translation table lookups,
 * record padding (conv=sync), and in-flight throughput autotuning (bs=auto).
 */

#include <config.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "system.h"
#include "quote.h"
#include "quotearg.h"
#include "error.h"
#include "verror.h"
#include "xalloc.h"
#include "gethrxtime.h"
#include "xtime.h"
#include "human.h"

#include "dd_config.h"
#include "stats.h"
#include "signals.h"
#include "conversions.h"
#include "io_driver.h"
#include "io_engine_internal.h"

static int const human_opts =
  (human_autoscale | human_round_to_nearest
   | human_space_before_unit | human_SI | human_B);

/* Number of distinct stages evaluated during in-flight autotuning */
static const idx_t autotune_stages[] = {
  64 * 1024,       /* 64 KB */
  256 * 1024,      /* 256 KB */
  1024 * 1024,     /* 1 MB */
  4 * 1024 * 1024  /* 4 MB */
};
enum { NUM_AUTOTUNE_STAGES = sizeof (autotune_stages) / sizeof (autotune_stages[0]) };

/**
 * @brief State tracker for in-flight dynamic blocksize autotuning.
 */
typedef struct
{
  bool active;                  /**< True while tuning stages are actively being sampled */
  size_t current_stage;         /**< Current autotune benchmark stage index */
  int stage_blocks_done;        /**< Blocks processed in current stage */
  xtime_t stage_start_time;     /**< Timestamp when current stage was started */
  intmax_t stage_bytes_start;   /**< Transferred byte count when stage started */
  double stage_rates[NUM_AUTOTUNE_STAGES]; /**< Measured transfer rates per stage */
  intmax_t total_byte_limit;    /**< Total transfer limit or -1 */
} autotune_state_t;

/**
 * @brief Private runtime state for synchronous block driver.
 */
typedef struct
{
  autotune_state_t at;
  idx_t partread;
  int saved_byte;
} sync_driver_state_t;

/**
 * @brief Samples current transfer speed and shifts to next autotuning stage.
 */
static void
autotune_sample_tick (dd_context_t *ctx, autotune_state_t *at)
{
  if (!at->active)
    return;

  at->stage_blocks_done++;
  xtime_t now = gethrxtime ();
  xtime_t stage_elapsed = now - at->stage_start_time;

  if (at->stage_blocks_done < 4 && (stage_elapsed < (XTIME_PRECISION / 50) || at->stage_blocks_done < 2))
    return;

  intmax_t bytes_transferred = ctx->stats.w_bytes - at->stage_bytes_start;
  if (stage_elapsed > 0)
    at->stage_rates[at->current_stage] = (double) bytes_transferred / ((double) stage_elapsed / XTIME_PRECISION);

  at->current_stage++;

  bool abort_autotune = false;
  if (at->total_byte_limit >= 0)
    {
      intmax_t remaining = at->total_byte_limit - ctx->stats.w_bytes;
      if (remaining <= 0 || (at->current_stage < NUM_AUTOTUNE_STAGES && remaining < (intmax_t) autotune_stages[at->current_stage]))
        abort_autotune = true;
    }

  if (at->current_stage < NUM_AUTOTUNE_STAGES && !abort_autotune)
    {
      ctx->cfg.input_blocksize = autotune_stages[at->current_stage];
      ctx->cfg.output_blocksize = autotune_stages[at->current_stage];
      if (at->total_byte_limit >= 0)
        {
          intmax_t remaining = at->total_byte_limit - ctx->stats.w_bytes;
          if (remaining > 0)
            {
              ctx->cfg.max_records = (ctx->stats.r_full + ctx->stats.r_partial) + remaining / ctx->cfg.input_blocksize;
              ctx->cfg.max_bytes = remaining % ctx->cfg.input_blocksize;
            }
        }
      at->stage_blocks_done = 0;
      at->stage_start_time = gethrxtime ();
      at->stage_bytes_start = ctx->stats.w_bytes;
    }
  else
    {
      size_t best_stage = 0;
      double best_rate = at->stage_rates[0];
      size_t max_evaluated = at->current_stage < NUM_AUTOTUNE_STAGES ? at->current_stage : NUM_AUTOTUNE_STAGES;
      for (size_t s = 1; s < max_evaluated; s++)
        {
          if (at->stage_rates[s] > best_rate)
            {
              best_rate = at->stage_rates[s];
              best_stage = s;
            }
        }

      ctx->cfg.input_blocksize = autotune_stages[best_stage];
      ctx->cfg.output_blocksize = autotune_stages[best_stage];
      if (at->total_byte_limit >= 0)
        {
          intmax_t remaining = at->total_byte_limit - ctx->stats.w_bytes;
          if (remaining > 0)
            {
              ctx->cfg.max_records = (ctx->stats.r_full + ctx->stats.r_partial) + remaining / ctx->cfg.input_blocksize;
              ctx->cfg.max_bytes = remaining % ctx->cfg.input_blocksize;
            }
        }
      at->active = false;

      if (ctx->cfg.status_level != STATUS_NONE)
        {
          char hbuf[LONGEST_HUMAN_READABLE + 1];
          diagnose (0, _("autotune: selected optimal blocksize %s (measured %.1f GB/s)"),
                    human_readable (ctx->cfg.input_blocksize, hbuf, human_opts | human_base_1024, 1, 1),
                    best_rate / (1000.0 * 1000.0 * 1000.0));
        }
    }
}

/**
 * @brief Initializes buffers and autotune state.
 */
static int
sync_driver_init (dd_context_t *ctx, void **state)
{
  dd_alloc_ibuf (ctx);
  dd_alloc_obuf (ctx);

  sync_driver_state_t *st = xmalloc (sizeof *st);
  memset (st, 0, sizeof *st);
  st->saved_byte = -1;
  st->at.active = !!(ctx->cfg.conversions_mask & C_AUTOTUNE);
  st->at.total_byte_limit = -1;

  if ((ctx->cfg.input_flags & O_COUNT_BYTES) && ctx->cfg.max_records != INTMAX_MAX)
    st->at.total_byte_limit = ctx->cfg.max_records * ctx->cfg.input_blocksize + ctx->cfg.max_bytes;

  if (st->at.active)
    {
      idx_t hw_in = dd_detect_optimal_blocksize (STDIN_FILENO);
      idx_t hw_out = dd_detect_optimal_blocksize (STDOUT_FILENO);
      idx_t hw_min = MAX (hw_in, hw_out);

      idx_t start_bs = autotune_stages[0];
      if (hw_min > start_bs)
        {
          while (st->at.current_stage + 1 < NUM_AUTOTUNE_STAGES
                 && autotune_stages[st->at.current_stage] < hw_min)
            st->at.current_stage++;
          start_bs = autotune_stages[st->at.current_stage];
        }

      ctx->cfg.input_blocksize = start_bs;
      ctx->cfg.output_blocksize = start_bs;
      if (st->at.total_byte_limit >= 0)
        {
          ctx->cfg.max_records = st->at.total_byte_limit / ctx->cfg.input_blocksize;
          ctx->cfg.max_bytes = st->at.total_byte_limit % ctx->cfg.input_blocksize;
        }
      st->at.stage_start_time = gethrxtime ();
      st->at.stage_bytes_start = ctx->stats.w_bytes;
    }

  *state = st;
  return EXIT_SUCCESS;
}

/**
 * @brief Executes one synchronous block read, conversion and write step.
 */
static int
sync_driver_step (dd_context_t *ctx, void *state, bool *eof, bool *fallback)
{
  (void)fallback;
  sync_driver_state_t *st = (sync_driver_state_t *) state;

  if (st->at.total_byte_limit >= 0 && ctx->stats.w_bytes >= st->at.total_byte_limit)
    {
      *eof = true;
      return EXIT_SUCCESS;
    }

  idx_t to_read = ctx->cfg.input_blocksize;
  if (ctx->stats.r_partial + ctx->stats.r_full >= ctx->cfg.max_records)
    to_read = ctx->cfg.max_bytes;

  ssize_t nread = (ctx->iread_fnc ? ctx->iread_fnc : dd_iread)
                  (STDIN_FILENO, ctx->ibuf, to_read);

  if (nread > 0)
    {
      dd_advance_input_offset (ctx, nread);
      if (ctx->cfg.i_nocache)
        dd_invalidate_cache (STDIN_FILENO, nread);
    }
  else if (nread == 0)
    {
      ctx->cfg.i_nocache_eof |= ctx->cfg.i_nocache;
      ctx->cfg.o_nocache_eof |= ctx->cfg.o_nocache && ! (ctx->cfg.conversions_mask & C_NOTRUNC);
      *eof = true;
      return EXIT_SUCCESS;
    }
  else
    {
      if (!(ctx->cfg.conversions_mask & C_NOERROR) || ctx->cfg.status_level != STATUS_NONE)
        diagnose (errno, _("error reading %s"), quoteaf (ctx->cfg.input_file));

      if (ctx->cfg.conversions_mask & C_NOERROR)
        {
          dd_print_stats (&ctx->stats, ctx->cfg.status_level, &ctx->stats.progress_len);
          idx_t bad_portion = ctx->cfg.input_blocksize - st->partread;
          dd_invalidate_cache (STDIN_FILENO, bad_portion);
          if ((ctx->cfg.conversions_mask & C_SYNC) && !st->partread)
            nread = 0;
          else
            return EXIT_SUCCESS;
        }
      else
        return EXIT_FAILURE;
    }

  idx_t n_bytes_read = nread;
  if (n_bytes_read < ctx->cfg.input_blocksize)
    {
      ctx->stats.r_partial++;
      st->partread = n_bytes_read;
      if (ctx->cfg.conversions_mask & C_SYNC)
        {
          if (!(ctx->cfg.conversions_mask & C_NOERROR))
            memset (ctx->ibuf + n_bytes_read,
                    (ctx->cfg.conversions_mask & (C_BLOCK | C_UNBLOCK)) ? ' ' : '\0',
                    ctx->cfg.input_blocksize - n_bytes_read);
          n_bytes_read = ctx->cfg.input_blocksize;
        }
    }
  else
    {
      ctx->stats.r_full++;
      st->partread = 0;
    }

  if (ctx->translation_needed)
    {
      if (ctx->trans_mode == TRANS_MODE_FAST_UCASE)
        dd_vector_ucase (ctx->ibuf, n_bytes_read);
      else if (ctx->trans_mode == TRANS_MODE_FAST_LCASE)
        dd_vector_lcase (ctx->ibuf, n_bytes_read);
      else
        dd_translate_buffer (ctx->trans_table, ctx->ibuf, n_bytes_read);
    }

  /* Matching block size fast path */
  if (ctx->ibuf == ctx->obuf)
    {
      idx_t nwritten = dd_iwrite (ctx, STDOUT_FILENO, ctx->obuf, n_bytes_read);
      ctx->stats.w_bytes += nwritten;
      if (nwritten != n_bytes_read)
        {
          diagnose (errno, _("error writing %s"), quoteaf (ctx->cfg.output_file));
          return EXIT_FAILURE;
        }
      else if (n_bytes_read == ctx->cfg.input_blocksize)
        ctx->stats.w_full++;
      else
        ctx->stats.w_partial++;

      if (st->at.active)
        autotune_sample_tick (ctx, &st->at);
      return EXIT_SUCCESS;
    }

  char const *bufstart;
  if (ctx->cfg.conversions_mask & C_SWAB)
    bufstart = dd_swab_buffer (ctx->ibuf, &n_bytes_read, &st->saved_byte);
  else
    bufstart = ctx->ibuf;

  if (ctx->cfg.conversions_mask & C_BLOCK)
    dd_copy_with_block (ctx, bufstart, n_bytes_read);
  else if (ctx->cfg.conversions_mask & C_UNBLOCK)
    dd_copy_with_unblock (ctx, bufstart, n_bytes_read);
  else
    dd_copy_simple (ctx, bufstart, n_bytes_read);

  if (st->at.active)
    autotune_sample_tick (ctx, &st->at);

  return EXIT_SUCCESS;
}

/**
 * @brief Flushes pending partial block data to stdout.
 */
static int
sync_driver_flush (dd_context_t *ctx, void *state)
{
  sync_driver_state_t *st = (sync_driver_state_t *) state;
  if (0 <= st->saved_byte)
    {
      char saved_char = st->saved_byte;
      if (ctx->cfg.conversions_mask & C_BLOCK)
        dd_copy_with_block (ctx, &saved_char, 1);
      else if (ctx->cfg.conversions_mask & C_UNBLOCK)
        dd_copy_with_unblock (ctx, &saved_char, 1);
      else
        dd_copy_simple (ctx, &saved_char, 1);
    }

  if (ctx->col && (ctx->cfg.conversions_mask & C_BLOCK))
    {
      for (idx_t j = ctx->col; j < ctx->cfg.conversion_blocksize; j++)
        dd_output_char (ctx, ctx->space_character);
    }

  if (ctx->col && (ctx->cfg.conversions_mask & C_UNBLOCK))
    {
      dd_output_char (ctx, ctx->newline_character);
    }

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
 * @brief Releases driver state.
 */
static void
sync_driver_cleanup (dd_context_t *ctx, void *state)
{
  (void)ctx;
  free (state);
}

const dd_io_driver_t sync_io_driver = {
  .name = "sync_block",
  .init = sync_driver_init,
  .step = sync_driver_step,
  .flush = sync_driver_flush,
  .cleanup = sync_driver_cleanup
};
