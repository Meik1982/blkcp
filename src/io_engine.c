#include <config.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdckdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>

#include "system.h"
#include "alignalloc.h"
#include "close-stream.h"
#include "fd-reopen.h"
#include "gethrxtime.h"
#include "human.h"
#include "ioblksize.h"
#include "quote.h"
#include "quotearg.h"
#include "xtime.h"
#include "verror.h"
#include "error.h"

#include "dd_config.h"
#include "stats.h"
#include "signals.h"
#include "conversions.h"
#include "io_engine.h"

#ifdef __linux__
# include <sys/ioctl.h>
# include <linux/fs.h>
# include <sys/sysmacros.h>
#endif

/* Detects physical sector size and optimal I/O size for block devices or files */
static idx_t
detect_optimal_blocksize (int fd)
{
  struct stat st;
  if (fstat (fd, &st) != 0)
    return 0;

#if defined __linux__ && defined BLKPBSZGET
  if (S_ISBLK (st.st_mode))
    {
      unsigned int phys_sector = 0;
      if (ioctl (fd, BLKPBSZGET, &phys_sector) == 0 && phys_sector > 0)
        {
# if defined BLKIOOPT
          unsigned int opt_io = 0;
          if (ioctl (fd, BLKIOOPT, &opt_io) == 0 && opt_io >= phys_sector)
            return opt_io;
# endif
          return phys_sector;
        }
    }
#endif

  if (S_ISREG (st.st_mode) && st.st_blksize > 0)
    return st.st_blksize;

  return 0;
}

/* Helper to check if dev_name is a partition of disk_name (e.g. sda1 of sda, nvme0n1p2 of nvme0n1)
   without matching non-partition prefixes (e.g. sdaa1 of sda) */
static inline bool
is_partition_of_device (char const *dev_name, char const *disk_name)
{
  size_t dlen = strlen (disk_name);
  if (dlen == 0 || strncmp (dev_name, disk_name, dlen) != 0)
    return false;

  char next = dev_name[dlen];
  if (next == '\0')
    return true;

  char last_disk_char = disk_name[dlen - 1];
  if (isdigit ((unsigned char) last_disk_char))
    {
      /* Disks ending in digit (nvme0n1, mmcblk0) separate partitions with 'p<digit>' */
      if (next == 'p' && isdigit ((unsigned char) dev_name[dlen + 1]))
        return true;
      return false;
    }
  else
    {
      /* Disks ending in letter (sda, vda) separate partitions directly with '<digit>' */
      if (isdigit ((unsigned char) next))
        return true;
      return false;
    }
}

/* Verifies that the target block device is not a mounted system partition unless forced */
static void
check_target_safety (dd_context_t *ctx)
{
  if (!ctx->cfg.output_file)
    return;

  if ((ctx->cfg.output_flags & O_FORCE) || (ctx->cfg.conversions_mask & C_FORCE))
    return;

  struct stat target_st;
  if (stat (ctx->cfg.output_file, &target_st) != 0)
    return;

  if (!S_ISBLK (target_st.st_mode))
    return;

#ifdef __linux__
  FILE *fp = fopen ("/proc/mounts", "r");
  if (!fp)
    return;

  char line[1024];
  char devpath[512];
  char mountpoint[512];

  while (fgets (line, sizeof line, fp))
    {
      if (sscanf (line, "%511s %511s", devpath, mountpoint) != 2)
        continue;

      if (strcmp (mountpoint, "/") == 0
          || strcmp (mountpoint, "/boot") == 0
          || strcmp (mountpoint, "/boot/efi") == 0
          || strcmp (mountpoint, "/home") == 0)
        {
          struct stat m_st;
          if (stat (devpath, &m_st) == 0 && S_ISBLK (m_st.st_mode))
            {
              bool match = false;
              if (m_st.st_rdev == target_st.st_rdev)
                match = true;
              else if (major (m_st.st_rdev) == major (target_st.st_rdev))
                {
                  char const *tgt_base = strrchr (ctx->cfg.output_file, '/');
                  char const *dev_base = strrchr (devpath, '/');
                  if (tgt_base && dev_base)
                    {
                      tgt_base++;
                      dev_base++;
                      if (is_partition_of_device (dev_base, tgt_base))
                        match = true;
                    }
                }

              if (match)
                {
                  fclose (fp);
                  error (EXIT_FAILURE, 0,
                         _("SAFETY GUARD: refusing to overwrite '%s' which contains mounted system path '%s'.\n"
                           "Use 'oflag=force' or 'opt=force' to override if intentional."),
                         quoteaf (ctx->cfg.output_file), quoteaf (mountpoint));
                }
            }
        }
    }

  fclose (fp);
#endif
}

static int const human_opts =
  (human_autoscale | human_round_to_nearest
   | human_space_before_unit | human_SI | human_B);

static dd_context_t *active_ctx = NULL;

ATTRIBUTE_FORMAT ((__printf__, 2, 3))
static void
diagnose (int errnum, char const *fmt, ...)
{
  if (active_ctx && 0 < active_ctx->stats.progress_len)
    {
      fputc ('\n', stderr);
      active_ctx->stats.progress_len = 0;
    }

  va_list ap;
  va_start (ap, fmt);
  verror (0, errnum, fmt, ap);
  va_end (ap);
}

#define AUTOTUNE_MAX_BLOCKSIZE (4 * 1024 * 1024)

static void
alloc_ibuf (dd_context_t *ctx)
{
  if (ctx->ibuf)
    return;

  char hbuf[LONGEST_HUMAN_READABLE + 1];
  idx_t alloc_size = ctx->cfg.input_blocksize;
  if ((ctx->cfg.conversions_mask & C_AUTOTUNE) && alloc_size < AUTOTUNE_MAX_BLOCKSIZE)
    alloc_size = AUTOTUNE_MAX_BLOCKSIZE;

  bool extra_byte_for_swab = !!(ctx->cfg.conversions_mask & C_SWAB);
  ctx->ibuf = alignalloc (ctx->page_size, alloc_size + extra_byte_for_swab);
  if (!ctx->ibuf)
    {
      error (EXIT_FAILURE, 0,
             _("memory exhausted by input buffer of size %td"
               " bytes (%s)"),
             alloc_size,
             human_readable (alloc_size, hbuf,
                             human_opts | human_base_1024, 1, 1));
    }
}

static void
alloc_obuf (dd_context_t *ctx)
{
  if (ctx->obuf)
    return;

  bool needs_separate_buf = (ctx->cfg.conversions_mask & C_TWOBUFS)
    && (ctx->cfg.input_blocksize != ctx->cfg.output_blocksize
        || (ctx->cfg.conversions_mask & (C_BLOCK | C_UNBLOCK | C_SWAB)));

  if (needs_separate_buf)
    {
      alloc_ibuf (ctx);
      char hbuf[LONGEST_HUMAN_READABLE + 1];
      idx_t alloc_size = ctx->cfg.output_blocksize;
      if ((ctx->cfg.conversions_mask & C_AUTOTUNE) && alloc_size < AUTOTUNE_MAX_BLOCKSIZE)
        alloc_size = AUTOTUNE_MAX_BLOCKSIZE;

      ctx->obuf = alignalloc (ctx->page_size, alloc_size);
      if (!ctx->obuf)
        {
          error (EXIT_FAILURE, 0,
                 _("memory exhausted by output buffer of size %td"
                   " bytes (%s)"),
                 alloc_size,
                 human_readable (alloc_size, hbuf,
                                 human_opts | human_base_1024, 1, 1));
        }
    }
  else
    {
      alloc_ibuf (ctx);
      ctx->obuf = ctx->ibuf;
    }
}

static int
iclose (int fd)
{
  if (close (fd) != 0)
    do
      if (errno != EINTR)
        return -1;
    while (close (fd) != 0 && errno != EBADF);

  return 0;
}

/* Retry syscalls interrupted by signals (EINTR) while processing signals */
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
#define ifstat(fd, st)                      RETRY_ON_EINTR (fstat (fd, st))
#define ifsync(fd)                          RETRY_ON_EINTR (fsync (fd))
#define iftruncate(fd, len)                 RETRY_ON_EINTR (ftruncate (fd, len))

int
dd_synchronize_output (dd_context_t *ctx)
{
  int exit_status = EXIT_SUCCESS;
  int mask = ctx->cfg.conversions_mask;

  if ((mask & C_FDATASYNC) && ifdatasync (STDOUT_FILENO) != 0)
    {
      if (errno != ENOSYS && errno != EINVAL)
        {
          diagnose (errno, _("fdatasync failed for %s"), quoteaf (ctx->cfg.output_file));
          exit_status = EXIT_FAILURE;
        }
      mask |= C_FSYNC;
    }

  if ((mask & C_FSYNC) && ifsync (STDOUT_FILENO) != 0)
    {
      diagnose (errno, _("fsync failed for %s"), quoteaf (ctx->cfg.output_file));
      return EXIT_FAILURE;
    }

  return exit_status;
}

void
dd_engine_cleanup (dd_context_t *ctx)
{
  if (!dd_interrupt_signal)
    {
      int sync_status = dd_synchronize_output (ctx);
      if (sync_status)
        exit (sync_status);
    }

  if (iclose (STDIN_FILENO) != 0)
    error (EXIT_FAILURE, errno, _("closing input file %s"),
           quoteaf (ctx->cfg.input_file));

  if (iclose (STDOUT_FILENO) != 0)
    error (EXIT_FAILURE, errno,
           _("closing output file %s"), quoteaf (ctx->cfg.output_file));
}

void
dd_cleanup (void)
{
  if (active_ctx)
    dd_engine_cleanup (active_ctx);
}

static off_t
cache_round (int fd, off_t len)
{
  static off_t i_pending, o_pending;
  off_t *pending = (fd == STDIN_FILENO ? &i_pending : &o_pending);

  if (len)
    {
      intmax_t c_pending;
      if (ckd_add (&c_pending, *pending, len))
        c_pending = INTMAX_MAX;
      *pending = c_pending % IO_BUFSIZE;
      if (c_pending > *pending)
        len = c_pending - *pending;
      else
        len = 0;
    }
  else
    len = *pending;

  return len;
}

static bool
invalidate_cache (int fd, off_t len)
{
  int adv_ret = -1;
  off_t clen = cache_round (fd, len);

  if (len && !clen)
    return true;

#if HAVE_POSIX_FADVISE
  off_t offset = lseek (fd, 0, SEEK_CUR);

  if (0 <= offset)
    {
      off_t alloc_len = clen ? clen : 1;
      off_t adv_start = (len ? offset - clen : 0);

      adv_ret = posix_fadvise (fd, adv_start, alloc_len, POSIX_FADV_DONTNEED);
    }
#endif

  return adv_ret != -1;
}

static void
set_fd_flags (int fd, int add_flags, char const *name)
{
  int fcntl_flags = add_flags & (O_APPEND | O_NONBLOCK);
  if (fcntl_flags)
    {
      int old_flags = fcntl (fd, F_GETFL);
      if (old_flags < 0
          || fcntl (fd, F_SETFL, old_flags | fcntl_flags) == -1)
        error (EXIT_FAILURE, errno, _("setting flags for %s"), quoteaf (name));
    }
}

static ssize_t
iread (int fd, char *buf, idx_t size)
{
  ssize_t nread;
  do
    {
      if (active_ctx) dd_process_signals (active_ctx);
      nread = read (fd, buf, size);
    }
  while (nread < 0 && errno == EINTR);

  return nread;
}

ssize_t
dd_iread_fullblock (int fd, char *buf, idx_t size)
{
  ssize_t nread = 0;
  while (0 < size)
    {
      ssize_t ncurr = iread (fd, buf, size);
      if (ncurr < 0)
        return ncurr;
      if (ncurr == 0)
        break;
      nread += ncurr;
      buf += ncurr;
      size -= ncurr;
    }
  return nread;
}

static idx_t
iwrite (dd_context_t *ctx, int fd, char const *buf, idx_t size)
{
  idx_t total_written = 0;

  if ((ctx->cfg.conversions_mask & C_SPARSE) && is_nul (buf, size))
    {
      off_t offset = lseek (fd, size, SEEK_CUR);
      if (0 <= offset)
        {
          ctx->final_op_was_seek = true;
          if (ctx->cfg.conversions_mask & C_SHA256)
            sha256_process_bytes (buf, size, &ctx->sha_ctx);
          return size;
        }
    }

  while (total_written < size)
    {
      if (active_ctx) dd_check_signals (active_ctx);
      ssize_t nwritten = write (fd, buf + total_written, size - total_written);
      if (nwritten < 0)
        {
          if (errno != EINTR)
            break;
        }
      else if (nwritten == 0)
        {
          errno = ENOSPC;
          break;
        }
      else
        total_written += nwritten;
    }

  if (total_written > 0 && (ctx->cfg.conversions_mask & C_SHA256))
    sha256_process_bytes (buf, total_written, &ctx->sha_ctx);

  if (ctx->cfg.o_nocache && total_written)
    invalidate_cache (fd, total_written);

  return total_written;
}

static void
write_output (dd_context_t *ctx)
{
  idx_t nwritten = iwrite (ctx, STDOUT_FILENO, ctx->obuf, ctx->cfg.output_blocksize);
  ctx->stats.w_bytes += nwritten;
  if (nwritten != ctx->cfg.output_blocksize)
    {
      diagnose (errno, _("error writing %s"), quoteaf (ctx->cfg.output_file));
      if (nwritten != 0)
        ctx->stats.w_partial++;
      exit (EXIT_FAILURE);
    }
  else
    ctx->stats.w_full++;
  ctx->oc = 0;
  dd_check_progress (&ctx->stats, ctx->cfg.status_level);
}

static inline void
output_char (dd_context_t *ctx, char c)
{
  ctx->obuf[ctx->oc++] = c;
  if (ctx->oc >= ctx->cfg.output_blocksize)
    write_output (ctx);
}

static void
copy_simple (dd_context_t *ctx, char const *buf, idx_t nread)
{
  /* Fast path: direct write without intermediate buffer copy if aligned to output blocksize */
  if (ctx->oc == 0 && nread == ctx->cfg.output_blocksize)
    {
      idx_t nwritten = iwrite (ctx, STDOUT_FILENO, buf, nread);
      ctx->stats.w_bytes += nwritten;
      if (nwritten != nread)
        {
          diagnose (errno, _("error writing %s"), quoteaf (ctx->cfg.output_file));
          if (nwritten != 0)
            ctx->stats.w_partial++;
          exit (EXIT_FAILURE);
        }
      ctx->stats.w_full++;
      dd_check_progress (&ctx->stats, ctx->cfg.status_level);
      return;
    }

  char const *start = buf;
  do
    {
      idx_t nfree = MIN (nread, ctx->cfg.output_blocksize - ctx->oc);
      memcpy (ctx->obuf + ctx->oc, start, nfree);

      nread -= nfree;
      start += nfree;
      ctx->oc += nfree;
      if (ctx->oc >= ctx->cfg.output_blocksize)
        write_output (ctx);
    }
  while (nread != 0);
}

static void
copy_with_block (dd_context_t *ctx, char const *buf, idx_t nread)
{
  for (idx_t i = nread; i; i--, buf++)
    {
      if (*buf == ctx->newline_character)
        {
          if (ctx->col < ctx->cfg.conversion_blocksize)
            {
              for (idx_t j = ctx->col; j < ctx->cfg.conversion_blocksize; j++)
                output_char (ctx, ctx->space_character);
            }
          ctx->col = 0;
        }
      else
        {
          if (ctx->col == ctx->cfg.conversion_blocksize)
            ctx->stats.r_truncate++;
          else if (ctx->col < ctx->cfg.conversion_blocksize)
            output_char (ctx, *buf);
          ctx->col++;
        }
    }
}

static void
copy_with_unblock (dd_context_t *ctx, char const *buf, idx_t nread)
{
  for (idx_t i = 0; i < nread; i++)
    {
      char c = buf[i];

      if (ctx->col++ >= ctx->cfg.conversion_blocksize)
        {
          ctx->col = ctx->pending_spaces = 0;
          i--;
          output_char (ctx, ctx->newline_character);
        }
      else if (c == ctx->space_character)
        ctx->pending_spaces++;
      else
        {
          while (ctx->pending_spaces)
            {
              output_char (ctx, ctx->space_character);
              --ctx->pending_spaces;
            }
          output_char (ctx, c);
        }
    }
}

static void
advance_input_offset (dd_context_t *ctx, intmax_t offset)
{
  if (0 <= ctx->input_offset && ckd_add (&ctx->input_offset, ctx->input_offset, offset))
    ctx->input_offset = -1;
}

static intmax_t
skip (dd_context_t *ctx, int fd, char const *file, intmax_t records, idx_t blocksize, idx_t *bytes)
{
  (void) file;
  off_t offset;
  if (! ckd_mul (&offset, records, blocksize)
      && ! ckd_add (&offset, offset, *bytes)
      && 0 <= offset)
    {
      if (lseek (fd, offset, SEEK_CUR) >= 0)
        {
          *bytes = 0;
          return 0;
        }
    }

  intmax_t skipped = 0;
  alloc_ibuf (ctx);
  while (skipped < records)
    {
      ssize_t nread = iread (fd, ctx->ibuf, blocksize);
      if (nread <= 0)
        return records - skipped;
      skipped++;
    }
  return 0;
}

/* Internal state tracking for dynamic in-flight I/O autotuning */
typedef struct
{
  bool active;
  size_t current_stage;
  int stage_blocks_done;
  xtime_t stage_start_time;
  intmax_t stage_bytes_start;
  double stage_rates[4];
  intmax_t total_byte_limit;
} autotune_state_t;

static const idx_t autotune_stages[] = {
  64 * 1024,       /* 64 KB */
  256 * 1024,      /* 256 KB */
  1024 * 1024,     /* 1 MB */
  4 * 1024 * 1024  /* 4 MB */
};
enum { NUM_AUTOTUNE_STAGES = sizeof (autotune_stages) / sizeof (autotune_stages[0]) };

/* Samples transfer rate and switches block size across autotune stages */
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

#define ASYNC_QUEUE_CAPACITY 8

typedef struct async_slot
{
  char *buf;
  idx_t nread;
  bool is_eof;
  int err;
} async_slot_t;

typedef struct async_pipeline
{
  dd_context_t *ctx;
  async_slot_t slots[ASYNC_QUEUE_CAPACITY];
  size_t head;
  size_t tail;
  size_t count;
  bool reader_done;
  bool stop_requested;
  int reader_exit;
  intmax_t r_records_limit;
  idx_t r_bytes_limit;

  pthread_mutex_t mutex;
  pthread_cond_t cond_not_full;
  pthread_cond_t cond_not_empty;
  pthread_t reader_tid;
} async_pipeline_t;

static void *
async_reader_worker (void *arg)
{
  async_pipeline_t *pipe = (async_pipeline_t *) arg;
  dd_context_t *ctx = pipe->ctx;

  sigset_t set;
  sigfillset (&set);
  pthread_sigmask (SIG_SETMASK, &set, NULL);

  intmax_t records_read = 0;

  while (true)
    {
      if (pipe->r_records_limit != INTMAX_MAX)
        {
          if (records_read >= pipe->r_records_limit + !!pipe->r_bytes_limit)
            break;
        }

      pthread_mutex_lock (&pipe->mutex);
      while (pipe->count == ASYNC_QUEUE_CAPACITY && !pipe->stop_requested)
        pthread_cond_wait (&pipe->cond_not_full, &pipe->mutex);

      if (pipe->stop_requested)
        {
          pthread_mutex_unlock (&pipe->mutex);
          break;
        }

      size_t slot_idx = pipe->tail;
      async_slot_t *slot = &pipe->slots[slot_idx];
      pthread_mutex_unlock (&pipe->mutex);

      idx_t to_read = ctx->cfg.input_blocksize;
      if (pipe->r_records_limit != INTMAX_MAX && records_read >= pipe->r_records_limit)
        to_read = pipe->r_bytes_limit;

      ssize_t nread = (ctx->iread_fnc ? ctx->iread_fnc : iread) (STDIN_FILENO, slot->buf, to_read);

      if (nread < 0)
        {
          slot->err = errno;
          slot->nread = 0;
          slot->is_eof = true;
          pipe->reader_exit = EXIT_FAILURE;

          pthread_mutex_lock (&pipe->mutex);
          pipe->tail = (pipe->tail + 1) % ASYNC_QUEUE_CAPACITY;
          pipe->count++;
          pthread_cond_signal (&pipe->cond_not_empty);
          pthread_mutex_unlock (&pipe->mutex);
          break;
        }

      if (nread == 0)
        {
          slot->err = 0;
          slot->nread = 0;
          slot->is_eof = true;

          pthread_mutex_lock (&pipe->mutex);
          pipe->tail = (pipe->tail + 1) % ASYNC_QUEUE_CAPACITY;
          pipe->count++;
          pthread_cond_signal (&pipe->cond_not_empty);
          pthread_mutex_unlock (&pipe->mutex);
          break;
        }

      slot->err = 0;
      slot->nread = nread;
      slot->is_eof = false;
      records_read++;

      advance_input_offset (ctx, nread);
      if (ctx->cfg.i_nocache)
        invalidate_cache (STDIN_FILENO, nread);

      pthread_mutex_lock (&pipe->mutex);
      pipe->tail = (pipe->tail + 1) % ASYNC_QUEUE_CAPACITY;
      pipe->count++;
      pthread_cond_signal (&pipe->cond_not_empty);
      pthread_mutex_unlock (&pipe->mutex);
    }

  pthread_mutex_lock (&pipe->mutex);
  pipe->reader_done = true;
  pthread_cond_signal (&pipe->cond_not_empty);
  pthread_mutex_unlock (&pipe->mutex);

  return NULL;
}

static int
dd_copy_async (dd_context_t *ctx)
{
  int exit_status = EXIT_SUCCESS;

  if (ctx->cfg.conversions_mask & C_SHA256)
    sha256_init_ctx (&ctx->sha_ctx);

  async_pipeline_t pipe;
  memset (&pipe, 0, sizeof pipe);
  pipe.ctx = ctx;
  pipe.r_records_limit = ctx->cfg.max_records;
  pipe.r_bytes_limit = ctx->cfg.max_bytes;

  pthread_mutex_init (&pipe.mutex, NULL);
  pthread_cond_init (&pipe.cond_not_full, NULL);
  pthread_cond_init (&pipe.cond_not_empty, NULL);

  for (size_t i = 0; i < ASYNC_QUEUE_CAPACITY; i++)
    {
      pipe.slots[i].buf = alignalloc (ctx->page_size, ctx->cfg.input_blocksize);
      if (!pipe.slots[i].buf)
        xalloc_die ();
    }

  alloc_obuf (ctx);

  if (pthread_create (&pipe.reader_tid, NULL, async_reader_worker, &pipe) != 0)
    {
      error (0, errno, _("failed to create async reader thread"));
      return EXIT_FAILURE;
    }

  while (true)
    {
      if (active_ctx)
        dd_check_signals (active_ctx);

      pthread_mutex_lock (&pipe.mutex);
      while (pipe.count == 0 && !pipe.reader_done)
        pthread_cond_wait (&pipe.cond_not_empty, &pipe.mutex);

      if (pipe.count == 0 && pipe.reader_done)
        {
          pthread_mutex_unlock (&pipe.mutex);
          break;
        }

      size_t slot_idx = pipe.head;
      async_slot_t *slot = &pipe.slots[slot_idx];
      pthread_mutex_unlock (&pipe.mutex);

      if (slot->is_eof)
        {
          if (slot->err != 0)
            {
              diagnose (slot->err, _("error reading %s"), quoteaf (ctx->cfg.input_file));
              exit_status = EXIT_FAILURE;
            }
          break;
        }

      if (slot->nread == ctx->cfg.input_blocksize)
        ctx->stats.r_full++;
      else
        ctx->stats.r_partial++;

      if (ctx->translation_needed)
        {
          if (ctx->trans_mode == TRANS_MODE_FAST_UCASE)
            dd_vector_ucase (slot->buf, slot->nread);
          else if (ctx->trans_mode == TRANS_MODE_FAST_LCASE)
            dd_vector_lcase (slot->buf, slot->nread);
          else
            dd_translate_buffer (ctx->trans_table, slot->buf, slot->nread);
        }

      if (ctx->translation_needed)
        copy_simple (ctx, slot->buf, slot->nread);
      else
        {
          idx_t nwritten = iwrite (ctx, STDOUT_FILENO, slot->buf, slot->nread);
          ctx->stats.w_bytes += nwritten;
          if (nwritten != slot->nread)
            {
              diagnose (errno, _("error writing %s"), quoteaf (ctx->cfg.output_file));
              if (nwritten != 0)
                ctx->stats.w_partial++;
              exit_status = EXIT_FAILURE;
              break;
            }
          ctx->stats.w_full++;
        }

      dd_check_progress (&ctx->stats, ctx->cfg.status_level);

      pthread_mutex_lock (&pipe.mutex);
      pipe.head = (pipe.head + 1) % ASYNC_QUEUE_CAPACITY;
      pipe.count--;
      pthread_cond_signal (&pipe.cond_not_full);
      pthread_mutex_unlock (&pipe.mutex);
    }

  pthread_mutex_lock (&pipe.mutex);
  pipe.stop_requested = true;
  pthread_cond_signal (&pipe.cond_not_full);
  pthread_mutex_unlock (&pipe.mutex);

  pthread_join (pipe.reader_tid, NULL);
  if (pipe.reader_exit != EXIT_SUCCESS)
    exit_status = pipe.reader_exit;

  if (ctx->oc > 0)
    write_output (ctx);

  if (ctx->cfg.conversions_mask & C_SHA256)
    {
      sha256_finish_ctx (&ctx->sha_ctx, ctx->sha_digest);
      ctx->sha_computed = true;
    }

  for (size_t i = 0; i < ASYNC_QUEUE_CAPACITY; i++)
    alignfree (pipe.slots[i].buf);

  pthread_mutex_destroy (&pipe.mutex);
  pthread_cond_destroy (&pipe.cond_not_full);
  pthread_cond_destroy (&pipe.cond_not_empty);

  return exit_status;
}

/**
 * @brief Attempts kernel-space zero-copy and reflink data transfer using copy_file_range(2).
 *
 * This function bypasses userspace buffer copies entirely by offloading chunk transfers
 * directly to the kernel VFS. On copy-on-write filesystems (Btrfs, XFS, ZFS), this yields
 * instantaneous, zero-space reflink clones.
 *
 * Compatibility conditions:
 * - Operating system is Linux (kernel >= 4.5).
 * - Both input and output descriptors refer to regular files (S_ISREG).
 * - No content-modifying conversions (case-folding, swab, sha256 hashing, unblock/block).
 * - No sparse emulation or direct I/O cache bypass modes requested.
 *
 * @param ctx Runtime execution context containing configuration, file descriptors, and stats.
 * @param handled Output flag set to true if copy_file_range processed the operation (or encountered
 *                a non-recoverable error), false if userspace fallback should be performed.
 * @return EXIT_SUCCESS on completion, or EXIT_FAILURE on fatal transfer error.
 */
static int
dd_copy_reflink (dd_context_t *ctx, bool *handled)
{
  *handled = false;

#if defined __linux__
  /* Incompatible conversions that inspect, pad or mutate data in userspace */
  int incompatible_conv = C_ASCII | C_EBCDIC | C_IBM | C_BLOCK | C_UNBLOCK
                        | C_LCASE | C_UCASE | C_SWAB | C_SYNC | C_SHA256
                        | C_SPARSE;

  if (ctx->cfg.conversions_mask & incompatible_conv)
    return EXIT_SUCCESS;

  if (ctx->iread_fnc != NULL)
    return EXIT_SUCCESS;

  if (ctx->cfg.i_nocache || ctx->cfg.o_nocache)
    return EXIT_SUCCESS;

  if ((ctx->cfg.input_flags & (O_DIRECT | O_NOCACHE))
      || (ctx->cfg.output_flags & (O_DIRECT | O_NOCACHE)))
    return EXIT_SUCCESS;

  struct stat st_in, st_out;
  if (fstat (STDIN_FILENO, &st_in) != 0 || !S_ISREG (st_in.st_mode))
    return EXIT_SUCCESS;
  if (fstat (STDOUT_FILENO, &st_out) != 0 || !S_ISREG (st_out.st_mode))
    return EXIT_SUCCESS;

  /* If user did not explicitly request reflink and autotune is active, let autotune measure throughput */
  if (!(ctx->cfg.conversions_mask & C_REFLINK))
    {
      if (ctx->cfg.conversions_mask & C_AUTOTUNE)
        return EXIT_SUCCESS;
    }

  /* Determine total byte transfer limit from count= and iflag=count_bytes if configured */
  intmax_t total_byte_limit = -1;
  if ((ctx->cfg.input_flags & O_COUNT_BYTES) && ctx->cfg.max_records != INTMAX_MAX)
    {
      total_byte_limit = ctx->cfg.max_records * ctx->cfg.input_blocksize + ctx->cfg.max_bytes;
    }
  else if (ctx->cfg.max_records != INTMAX_MAX || ctx->cfg.max_bytes != 0)
    {
      total_byte_limit = ctx->cfg.max_records * ctx->cfg.input_blocksize + ctx->cfg.max_bytes;
    }

  if (total_byte_limit == 0)
    {
      *handled = true;
      return EXIT_SUCCESS;
    }

  /* Choose an interactive chunk size between 16 MiB and 64 MiB to allow regular progress reports */
  size_t chunk_size = MAX (ctx->cfg.output_blocksize, 16 * 1024 * 1024);
  if (chunk_size > 64 * 1024 * 1024)
    chunk_size = 64 * 1024 * 1024;

  while (true)
    {
      if (active_ctx)
        dd_check_signals (active_ctx);

      size_t to_copy = chunk_size;
      if (total_byte_limit >= 0)
        {
          intmax_t remaining = total_byte_limit - ctx->stats.w_bytes;
          if (remaining <= 0)
            break;
          if ((uintmax_t)remaining < to_copy)
            to_copy = (size_t)remaining;
        }

      ssize_t ret = copy_file_range (STDIN_FILENO, NULL, STDOUT_FILENO, NULL, to_copy, 0);

      if (ret < 0)
        {
          if (errno == EINTR)
            continue;

          /* If zero bytes written so far and the filesystem/kernel rejects copy_file_range
             (e.g., cross-device EXDEV or unsupported filesystem),
             gracefully fall back to standard userspace copy loop unless explicitly forced. */
          if (ctx->stats.w_bytes == 0 && (errno == EXDEV || errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL))
            {
              if (ctx->cfg.conversions_mask & C_REFLINK)
                {
                  diagnose (errno, _("copy_file_range not supported for %s to %s"),
                            quoteaf (ctx->cfg.input_file), quoteaf (ctx->cfg.output_file));
                  *handled = true;
                  return EXIT_FAILURE;
                }
              /* Opportunistic fallback */
              *handled = false;
              return EXIT_SUCCESS;
            }

          diagnose (errno, _("error copying %s to %s via copy_file_range"),
                    quoteaf (ctx->cfg.input_file), quoteaf (ctx->cfg.output_file));
          *handled = true;
          return EXIT_FAILURE;
        }

      if (ret == 0)
        break; /* End of input file reached */

      ctx->stats.w_bytes += ret;

      /* Keep block telemetry consistent with configured input and output block sizes */
      ctx->stats.r_full += ret / ctx->cfg.input_blocksize;
      if (ret % ctx->cfg.input_blocksize)
        ctx->stats.r_partial++;

      ctx->stats.w_full += ret / ctx->cfg.output_blocksize;
      if (ret % ctx->cfg.output_blocksize)
        ctx->stats.w_partial++;

      dd_check_progress (&ctx->stats, ctx->cfg.status_level);
    }

  *handled = true;
  return EXIT_SUCCESS;
#else
  (void)ctx;
  *handled = false;
  return EXIT_SUCCESS;
#endif
}

static int
dd_copy (dd_context_t *ctx)
{
  char *bufstart;
  ssize_t nread;
  idx_t partread = 0;
  int exit_status = EXIT_SUCCESS;
  idx_t n_bytes_read;
  int saved_byte = -1;

  if (ctx->cfg.skip_records != 0 || ctx->cfg.skip_bytes != 0)
    {
      intmax_t us_bytes;
      bool us_bytes_overflow =
        (ckd_mul (&us_bytes, ctx->cfg.skip_records, ctx->cfg.input_blocksize)
         || ckd_add (&us_bytes, ctx->cfg.skip_bytes, us_bytes));
      off_t input_offset0 = ctx->input_offset;
      intmax_t us_blocks = skip (ctx, STDIN_FILENO, ctx->cfg.input_file,
                                 ctx->cfg.skip_records, ctx->cfg.input_blocksize, &ctx->cfg.skip_bytes);

      if ((us_blocks
           || (0 <= ctx->input_offset
               && (us_bytes_overflow
                   || us_bytes != ctx->input_offset - input_offset0)))
          && ctx->cfg.status_level != STATUS_NONE)
        {
          diagnose (0, _("%s: cannot skip to specified offset"),
                    quotef (ctx->cfg.input_file));
        }
    }

  if (ctx->cfg.seek_records != 0 || ctx->cfg.seek_bytes != 0)
    {
      intmax_t write_records = skip (ctx, STDOUT_FILENO, ctx->cfg.output_file,
                                     ctx->cfg.seek_records, ctx->cfg.output_blocksize, &ctx->cfg.seek_bytes);
      if (write_records != 0)
        {
          memset (ctx->obuf, 0, ctx->cfg.output_blocksize);
          do
            {
              idx_t size = ctx->cfg.output_blocksize;
              if (write_records == 1 && ctx->cfg.seek_bytes != 0)
                size = ctx->cfg.seek_bytes;
              if (iwrite (ctx, STDOUT_FILENO, ctx->obuf, size) != size)
                {
                  diagnose (errno, _("error writing %s"), quoteaf (ctx->cfg.output_file));
                  return EXIT_FAILURE;
                }
            }
          while (--write_records != 0);
        }
    }

  if (ctx->cfg.max_records == 0 && ctx->cfg.max_bytes == 0)
    return exit_status;

  if ((ctx->cfg.conversions_mask & C_ASYNC) || (ctx->cfg.output_flags & O_ASYNC_PIPELINE))
    return dd_copy_async (ctx);

  /* Attempt kernel-space zero-copy / reflink fast path via copy_file_range(2) */
  bool cfr_handled = false;
  int cfr_status = dd_copy_reflink (ctx, &cfr_handled);
  if (cfr_handled)
    return cfr_status;

  alloc_ibuf (ctx);
  alloc_obuf (ctx);

  if (ctx->cfg.conversions_mask & C_SHA256)
    sha256_init_ctx (&ctx->sha_ctx);

  autotune_state_t at;
  memset (&at, 0, sizeof at);
  at.active = !!(ctx->cfg.conversions_mask & C_AUTOTUNE);
  at.total_byte_limit = -1;

  if ((ctx->cfg.input_flags & O_COUNT_BYTES) && ctx->cfg.max_records != INTMAX_MAX)
    at.total_byte_limit = ctx->cfg.max_records * ctx->cfg.input_blocksize + ctx->cfg.max_bytes;

  if (at.active)
    {
      idx_t hw_in = detect_optimal_blocksize (STDIN_FILENO);
      idx_t hw_out = detect_optimal_blocksize (STDOUT_FILENO);
      idx_t hw_min = MAX (hw_in, hw_out);

      idx_t start_bs = autotune_stages[0];
      if (hw_min > start_bs)
        {
          /* Align starting autotune stage to hardware block/optimal size */
          while (at.current_stage + 1 < NUM_AUTOTUNE_STAGES
                 && autotune_stages[at.current_stage] < hw_min)
            at.current_stage++;
          start_bs = autotune_stages[at.current_stage];
        }

      ctx->cfg.input_blocksize = start_bs;
      ctx->cfg.output_blocksize = start_bs;
      if (at.total_byte_limit >= 0)
        {
          ctx->cfg.max_records = at.total_byte_limit / ctx->cfg.input_blocksize;
          ctx->cfg.max_bytes = at.total_byte_limit % ctx->cfg.input_blocksize;
        }
      at.stage_start_time = gethrxtime ();
      at.stage_bytes_start = ctx->stats.w_bytes;
    }

  while (true)
    {
      if (active_ctx) dd_check_signals (active_ctx);

      if (at.total_byte_limit >= 0 && ctx->stats.w_bytes >= at.total_byte_limit)
        break;

      if (ctx->stats.r_partial + ctx->stats.r_full >= ctx->cfg.max_records + !!ctx->cfg.max_bytes)
        break;

      if (ctx->stats.r_partial + ctx->stats.r_full >= ctx->cfg.max_records)
        nread = (ctx->iread_fnc ? ctx->iread_fnc : iread) (STDIN_FILENO, ctx->ibuf, ctx->cfg.max_bytes);
      else
        nread = (ctx->iread_fnc ? ctx->iread_fnc : iread) (STDIN_FILENO, ctx->ibuf, ctx->cfg.input_blocksize);

      if (nread > 0)
        {
          advance_input_offset (ctx, nread);
          if (ctx->cfg.i_nocache)
            invalidate_cache (STDIN_FILENO, nread);
        }
      else if (nread == 0)
        {
          ctx->cfg.i_nocache_eof |= ctx->cfg.i_nocache;
          ctx->cfg.o_nocache_eof |= ctx->cfg.o_nocache && ! (ctx->cfg.conversions_mask & C_NOTRUNC);
          break;
        }
      else
        {
          if (!(ctx->cfg.conversions_mask & C_NOERROR) || ctx->cfg.status_level != STATUS_NONE)
            diagnose (errno, _("error reading %s"), quoteaf (ctx->cfg.input_file));

          if (ctx->cfg.conversions_mask & C_NOERROR)
            {
              dd_print_stats (&ctx->stats, ctx->cfg.status_level, &ctx->stats.progress_len);
              idx_t bad_portion = ctx->cfg.input_blocksize - partread;
              invalidate_cache (STDIN_FILENO, bad_portion);
              if ((ctx->cfg.conversions_mask & C_SYNC) && !partread)
                nread = 0;
              else
                continue;
            }
          else
            {
              exit_status = EXIT_FAILURE;
              break;
            }
        }

      n_bytes_read = nread;

      if (n_bytes_read < ctx->cfg.input_blocksize)
        {
          ctx->stats.r_partial++;
          partread = n_bytes_read;
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
          partread = 0;
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

      if (ctx->ibuf == ctx->obuf)
        {
          idx_t nwritten = iwrite (ctx, STDOUT_FILENO, ctx->obuf, n_bytes_read);
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

          if (at.active)
            autotune_sample_tick (ctx, &at);

          dd_check_progress (&ctx->stats, ctx->cfg.status_level);
          continue;
        }

      if (ctx->cfg.conversions_mask & C_SWAB)
        bufstart = dd_swab_buffer (ctx->ibuf, &n_bytes_read, &saved_byte);
      else
        bufstart = ctx->ibuf;

      if (ctx->cfg.conversions_mask & C_BLOCK)
        copy_with_block (ctx, bufstart, n_bytes_read);
      else if (ctx->cfg.conversions_mask & C_UNBLOCK)
        copy_with_unblock (ctx, bufstart, n_bytes_read);
      else
        copy_simple (ctx, bufstart, n_bytes_read);

      if (at.active)
        autotune_sample_tick (ctx, &at);

      dd_check_progress (&ctx->stats, ctx->cfg.status_level);
    }

  if (0 <= saved_byte)
    {
      char saved_char = saved_byte;
      if (ctx->cfg.conversions_mask & C_BLOCK)
        copy_with_block (ctx, &saved_char, 1);
      else if (ctx->cfg.conversions_mask & C_UNBLOCK)
        copy_with_unblock (ctx, &saved_char, 1);
      else
        copy_simple (ctx, &saved_char, 1);
    }

  if (ctx->col && (ctx->cfg.conversions_mask & C_BLOCK))
    {
      for (idx_t j = ctx->col; j < ctx->cfg.conversion_blocksize; j++)
        output_char (ctx, ctx->space_character);
    }

  if (ctx->col && (ctx->cfg.conversions_mask & C_UNBLOCK))
    {
      output_char (ctx, ctx->newline_character);
    }

  if (ctx->oc > 0)
    {
      idx_t nwritten = iwrite (ctx, STDOUT_FILENO, ctx->obuf, ctx->oc);
      ctx->stats.w_bytes += nwritten;
      if (nwritten != ctx->oc)
        {
          diagnose (errno, _("error writing %s"), quoteaf (ctx->cfg.output_file));
          return EXIT_FAILURE;
        }
      if (nwritten != 0)
        ctx->stats.w_partial++;
    }

  if (ctx->cfg.conversions_mask & C_SHA256)
    {
      sha256_finish_ctx (&ctx->sha_ctx, ctx->sha_digest);
      ctx->sha_computed = true;
    }

  return exit_status;
}

/* Configures and opens input descriptor (stdin) with flags, readahead and seek check */
static void
setup_input_stream (dd_context_t *ctx)
{
  if (ctx->cfg.input_file == NULL)
    {
      ctx->cfg.input_file = _("standard input");
      set_fd_flags (STDIN_FILENO, ctx->cfg.input_flags, ctx->cfg.input_file);
    }
  else
    {
      if (ifd_reopen (STDIN_FILENO, ctx->cfg.input_file, O_RDONLY | ctx->cfg.input_flags, 0) < 0)
        error (EXIT_FAILURE, errno, _("failed to open %s"), quoteaf (ctx->cfg.input_file));
    }

  off_t offset = lseek (STDIN_FILENO, 0, SEEK_CUR);
  ctx->input_seekable = (0 <= offset);
  ctx->input_offset = (offset > 0 ? offset : 0);
  ctx->input_seek_errno = errno;

#if HAVE_POSIX_FADVISE
  if (ctx->input_seekable)
    posix_fadvise (STDIN_FILENO, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
}

/* Configures, creates, and optionally truncates output descriptor (stdout) */
static int
setup_output_stream (dd_context_t *ctx)
{
  check_target_safety (ctx);

  if (ctx->cfg.output_file == NULL)
    {
      ctx->cfg.output_file = _("standard output");
      set_fd_flags (STDOUT_FILENO, ctx->cfg.output_flags, ctx->cfg.output_file);
      return EXIT_SUCCESS;
    }

  mode_t perms = MODE_RW_UGO;
  int opts = (ctx->cfg.output_flags
              | (ctx->cfg.conversions_mask & C_NOCREAT ? 0 : O_CREAT)
              | (ctx->cfg.conversions_mask & C_EXCL ? O_EXCL : 0)
              | (ctx->cfg.seek_records || (ctx->cfg.conversions_mask & C_NOTRUNC) ? 0 : O_TRUNC));

  off_t size;
  if ((ckd_mul (&size, ctx->cfg.seek_records, ctx->cfg.output_blocksize)
       || ckd_add (&size, ctx->cfg.seek_bytes, size))
      && !(ctx->cfg.conversions_mask & C_NOTRUNC))
    error (EXIT_FAILURE, 0,
           _("offset too large: "
             "cannot truncate to a length of seek=%jd"
             " (%td-byte) blocks"),
           ctx->cfg.seek_records, ctx->cfg.output_blocksize);

  if ((! ctx->cfg.seek_records
       || ifd_reopen (STDOUT_FILENO, ctx->cfg.output_file, O_RDWR | opts, perms) < 0)
      && (ifd_reopen (STDOUT_FILENO, ctx->cfg.output_file, O_WRONLY | opts, perms)
          < 0))
    error (EXIT_FAILURE, errno, _("failed to open %s"),
           quoteaf (ctx->cfg.output_file));

  if (ctx->cfg.seek_records != 0 && !(ctx->cfg.conversions_mask & C_NOTRUNC))
    {
      if (iftruncate (STDOUT_FILENO, size) != 0)
        {
          int ftruncate_errno = errno;
          struct stat stdout_stat;
          if (ifstat (STDOUT_FILENO, &stdout_stat) != 0)
            {
              diagnose (errno, _("cannot fstat %s"), quoteaf (ctx->cfg.output_file));
              return EXIT_FAILURE;
            }
          else if (S_ISREG (stdout_stat.st_mode)
                   || S_ISDIR (stdout_stat.st_mode)
                   || S_TYPEISSHM (&stdout_stat))
            {
              diagnose (ftruncate_errno,
                        _("failed to truncate to %jd bytes"
                          " in output file %s"),
                        (intmax_t)size, quoteaf (ctx->cfg.output_file));
              return EXIT_FAILURE;
            }
        }
    }

  return EXIT_SUCCESS;
}

int
dd_execute (dd_context_t *ctx)
{
  active_ctx = ctx;

  setup_input_stream (ctx);

  int exit_status = setup_output_stream (ctx);
  if (exit_status != EXIT_SUCCESS)
    return exit_status;

  ctx->stats.start_time = gethrxtime ();
  ctx->stats.next_time = ctx->stats.start_time + XTIME_PRECISION;

  exit_status = dd_copy (ctx);

  int sync_status = dd_synchronize_output (ctx);
  if (sync_status)
    exit_status = sync_status;

  if (ctx->cfg.max_records == 0 && ctx->cfg.max_bytes == 0)
    {
      if (ctx->cfg.i_nocache && !invalidate_cache (STDIN_FILENO, 0))
        {
          diagnose (errno, _("failed to discard cache for: %s"),
                    quotef (ctx->cfg.input_file));
          exit_status = EXIT_FAILURE;
        }
      if (ctx->cfg.o_nocache && !invalidate_cache (STDOUT_FILENO, 0))
        {
          diagnose (errno, _("failed to discard cache for: %s"),
                    quotef (ctx->cfg.output_file));
          exit_status = EXIT_FAILURE;
        }
    }
  else
    {
      if (ctx->cfg.i_nocache || ctx->cfg.i_nocache_eof)
        invalidate_cache (STDIN_FILENO, 0);
      if (ctx->cfg.o_nocache || ctx->cfg.o_nocache_eof)
        invalidate_cache (STDOUT_FILENO, 0);
    }

  dd_engine_cleanup (ctx);
  dd_print_stats (&ctx->stats, ctx->cfg.status_level, &ctx->stats.progress_len);
  if (ctx->sha_computed)
    dd_print_hash (ctx->sha_digest);

  return exit_status;
}

/* Explicitly frees all allocated buffers for reusable context lifecycles */
void
dd_context_free (dd_context_t *ctx)
{
  if (!ctx)
    return;

  if (ctx->obuf && ctx->obuf != ctx->ibuf)
    {
      alignfree (ctx->obuf);
      ctx->obuf = NULL;
    }
  if (ctx->ibuf)
    {
      alignfree (ctx->ibuf);
      ctx->ibuf = NULL;
    }
}
