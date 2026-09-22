/**
 * @file io_engine.c
 * @brief Main I/O orchestration engine, stream lifecycle and backend driver dispatcher.
 *
 * Coordinates stream initialization, target safety guards, skip/seek offsets,
 * and executes the central transfer loop via pluggable backend drivers
 * (sync block I/O, async ringbuffer, Linux kernel zero-copy reflink).
 */

#include <config.h>
#include <sys/types.h>
#include <sys/stat.h>
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
#include "xalloc.h"

#include "blkcp_config.h"
#include "stats.h"
#include "signals.h"
#include "conversions.h"
#include "io_engine.h"
#include "io_driver.h"
#include "io_engine_internal.h"

#ifdef __linux__
# include <sys/ioctl.h>
# include <linux/fs.h>
# include <sys/sysmacros.h>
#endif

/* Global pointer for signal delivery & diagnosis telemetry clearing */
dd_context_t *active_ctx = NULL;

/**
 * @brief Signal-safe diagnostic reporting that clears interactive \r status lines.
 */
void
dd_diagnose (int errnum, char const *fmt, ...)
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

/* -------------------------------------------------------------------------- */
/*                           Hardware & Device Helpers                        */
/* -------------------------------------------------------------------------- */

#if defined __linux__
# ifndef BLKSSZGET
#  define BLKSSZGET _IO(0x12,104)
# endif
# ifndef BLKPBSZGET
#  define BLKPBSZGET _IO(0x12,123)
# endif
#endif

idx_t
dd_detect_sector_size (int fd)
{
  struct stat st;
  if (fstat (fd, &st) != 0)
    return 512;

#if defined __linux__ && defined BLKSSZGET
  if (S_ISBLK (st.st_mode))
    {
      unsigned int logical_sector = 0;
      if (ioctl (fd, BLKSSZGET, &logical_sector) == 0 && logical_sector > 0)
        return logical_sector;
    }
#endif

  return 512;
}

idx_t
dd_detect_optimal_blocksize (int fd)
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

#if defined __linux__
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
      if (next == 'p' && isdigit ((unsigned char) dev_name[dlen + 1]))
        return true;
      return false;
    }
  else
    {
      if (isdigit ((unsigned char) next))
        return true;
      return false;
    }
}

typedef enum target_safety_status {
  TARGET_SAFETY_OK = 0,
  TARGET_SAFETY_MOUNTED_SYSTEM,
  TARGET_SAFETY_ACTIVE_SWAP
} target_safety_status_t;

static target_safety_status_t
evaluate_target_safety (char const *output_file, char *detail, size_t detail_size)
{
  if (!output_file)
    return TARGET_SAFETY_OK;

  struct stat target_st;
  if (stat (output_file, &target_st) != 0)
    return TARGET_SAFETY_OK;

  if (!S_ISBLK (target_st.st_mode))
    return TARGET_SAFETY_OK;

  FILE *fp = fopen ("/proc/mounts", "r");
  if (fp)
    {
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
                      char const *tgt_base = strrchr (output_file, '/');
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
                      if (detail && detail_size > 0)
                        snprintf (detail, detail_size, "%s", mountpoint);
                      return TARGET_SAFETY_MOUNTED_SYSTEM;
                    }
                }
            }
        }
      fclose (fp);
    }

  /* Check /proc/swaps to protect active system swap partitions */
  FILE *sfp = fopen ("/proc/swaps", "r");
  if (sfp)
    {
      char sline[1024];
      char swap_dev[512];
      /* Skip header line */
      if (fgets (sline, sizeof sline, sfp))
        {
          while (fgets (sline, sizeof sline, sfp))
            {
              if (sscanf (sline, "%511s", swap_dev) != 1)
                continue;

              struct stat sw_st;
              if (stat (swap_dev, &sw_st) == 0 && S_ISBLK (sw_st.st_mode))
                {
                  bool match = false;
                  if (sw_st.st_rdev == target_st.st_rdev)
                    match = true;
                  else if (major (sw_st.st_rdev) == major (target_st.st_rdev))
                    {
                      char const *tgt_base = strrchr (output_file, '/');
                      char const *dev_base = strrchr (swap_dev, '/');
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
                      fclose (sfp);
                      if (detail && detail_size > 0)
                        snprintf (detail, detail_size, "%s", swap_dev);
                      return TARGET_SAFETY_ACTIVE_SWAP;
                    }
                }
            }
        }
      fclose (sfp);
    }

  return TARGET_SAFETY_OK;
}

/* Verifies that the target block device is not a mounted system partition unless forced */
static void
check_target_safety (dd_context_t *ctx)
{
  if (!ctx->cfg.output_file)
    return;

  if ((ctx->cfg.output_flags & O_FORCE) || (ctx->cfg.conversions_mask & C_FORCE))
    return;

  char detail[512] = {0};
  target_safety_status_t status = evaluate_target_safety (ctx->cfg.output_file, detail, sizeof detail);
  if (status == TARGET_SAFETY_MOUNTED_SYSTEM)
    {
      error (EXIT_FAILURE, 0,
             _("SAFETY GUARD: refusing to overwrite '%s' which contains mounted system path '%s'.\n"
               "Use '-f' or '--force' to override if intentional."),
             quoteaf (ctx->cfg.output_file), quoteaf (detail));
    }
  else if (status == TARGET_SAFETY_ACTIVE_SWAP)
    {
      error (EXIT_FAILURE, 0,
             _("SAFETY GUARD: refusing to overwrite '%s' which is an active system swap device (%s).\n"
               "Use '-f' or '--force' to override if intentional."),
             quoteaf (ctx->cfg.output_file), quoteaf (detail));
    }
}
#endif

/* -------------------------------------------------------------------------- */
/*                           Buffer Memory Lifecycle                          */
/* -------------------------------------------------------------------------- */

void
dd_alloc_ibuf (dd_context_t *ctx)
{
  if (ctx->ibuf)
    return;

  idx_t bs = ctx->cfg.input_blocksize;
  if (ctx->cfg.conversions_mask & C_AUTOTUNE)
    {
      /* Ensure buffer accommodates the maximum autotune stage (4 MiB) */
      if (bs < (idx_t) 4 * 1024 * 1024)
        bs = (idx_t) 4 * 1024 * 1024;
    }

  idx_t alloc_size = bs + 2;
  if (ctx->page_size > 0 && (alloc_size % ctx->page_size) != 0)
    alloc_size = ((alloc_size + ctx->page_size - 1) / ctx->page_size) * ctx->page_size;

  ctx->ibuf = alignalloc (ctx->page_size, alloc_size);
  if (!ctx->ibuf)
    xalloc_die ();
}

void
dd_alloc_obuf (dd_context_t *ctx)
{
  if (ctx->obuf)
    return;

  idx_t bs = ctx->cfg.output_blocksize;
  if (ctx->cfg.conversions_mask & C_AUTOTUNE)
    {
      if (bs < (idx_t) 4 * 1024 * 1024)
        bs = (idx_t) 4 * 1024 * 1024;
    }

  if (ctx->cfg.conversions_mask & C_SWAB)
    {
      idx_t alloc_size = bs + 2;
      if (ctx->page_size > 0 && (alloc_size % ctx->page_size) != 0)
        alloc_size = ((alloc_size + ctx->page_size - 1) / ctx->page_size) * ctx->page_size;
      ctx->obuf = alignalloc (ctx->page_size, alloc_size);
    }
  else if (ctx->ibuf)
    {
      /* Single unified buffer for input and output */
      ctx->obuf = ctx->ibuf;
      return;
    }
  else
    {
      idx_t alloc_size = bs;
      if (ctx->page_size > 0 && (alloc_size % ctx->page_size) != 0)
        alloc_size = ((alloc_size + ctx->page_size - 1) / ctx->page_size) * ctx->page_size;
      ctx->obuf = alignalloc (ctx->page_size, alloc_size);
    }

  if (!ctx->obuf)
    xalloc_die ();
}

/* -------------------------------------------------------------------------- */
/*                           Low-Level I/O Primitives                         */
/* -------------------------------------------------------------------------- */

void
dd_advance_input_offset (dd_context_t *ctx, off_t nread)
{
  if (0 <= ctx->input_offset)
    {
      if (ckd_add (&ctx->input_offset, ctx->input_offset, nread))
        ctx->input_offset = -1;
    }
}

void
dd_invalidate_cache (int fd, off_t len)
{
#if HAVE_POSIX_FADVISE
  off_t pos = lseek (fd, 0, SEEK_CUR);
  if (0 <= pos)
    {
      off_t off = len < pos ? pos - len : 0;
      posix_fadvise (fd, off, len, POSIX_FADV_DONTNEED);
    }
#else
  (void) fd;
  (void) len;
#endif
}

#define NOCACHE_CHUNK_THRESHOLD (32 * 1024 * 1024)

void
dd_invalidate_cache_chunked (int fd, off_t bytes, off_t *pending, bool force_flush)
{
#if HAVE_POSIX_FADVISE
  if (pending)
    *pending += bytes;

  off_t to_evict = pending ? *pending : bytes;

  if (to_evict >= NOCACHE_CHUNK_THRESHOLD || (force_flush && to_evict > 0))
    {
      off_t pos = lseek (fd, 0, SEEK_CUR);
      if (0 <= pos)
        {
          off_t off = to_evict < pos ? pos - to_evict : 0;
          posix_fadvise (fd, off, to_evict, POSIX_FADV_DONTNEED);
        }
      if (pending)
        *pending = 0;
    }
#else
  (void) fd;
  (void) bytes;
  (void) pending;
  (void) force_flush;
#endif
}

static void
set_fd_flags (int fd, int add_flags, char const *name)
{
  if (add_flags)
    {
      int old_flags = fcntl (fd, F_GETFL);
      if (old_flags < 0 || fcntl (fd, F_SETFL, old_flags | add_flags) < 0)
        dd_diagnose (errno, _("failed to turn on flags for %s"), quoteaf (name));
    }
}

ssize_t
dd_iread (int fd, char *buf, idx_t size)
{
  while (true)
    {
      ssize_t ret = read (fd, buf, size);
      if (ret < 0)
        {
          if (errno == EINTR)
            continue;
#if defined __linux__ && defined O_DIRECT
          if (errno == EINVAL)
            {
              int cur_flags = fcntl (fd, F_GETFL);
              if (cur_flags >= 0 && (cur_flags & O_DIRECT))
                {
                  /* Drop O_DIRECT for unaligned tail or unsupported read */
                  fcntl (fd, F_SETFL, cur_flags & ~O_DIRECT);
                  ret = read (fd, buf, size);
                  if (ret > 0)
                    dd_invalidate_cache (fd, ret);
                  fcntl (fd, F_SETFL, cur_flags);
                  return ret;
                }
            }
#endif
        }
      return ret;
    }
}

ssize_t
dd_iread_fullblock (int fd, char *buf, idx_t size)
{
  ssize_t nread = 0;
  while (0 < size)
    {
      ssize_t ncurr = dd_iread (fd, buf, size);
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

idx_t
dd_iwrite (dd_context_t *ctx, int fd, char const *buf, idx_t size)
{
  idx_t total_written = 0;

  if ((ctx->cfg.conversions_mask & C_SHA256) && size > 0 && ctx->sha_evp_ctx)
    EVP_DigestUpdate (ctx->sha_evp_ctx, buf, (size_t) size);

  if ((ctx->cfg.conversions_mask & C_SPARSE) && is_nul (buf, size))
    {
      off_t offset = lseek (fd, size, SEEK_CUR);
      if (0 <= offset)
        return size;
    }

  while (total_written < size)
    {
      ssize_t nwritten = write (fd, buf + total_written, size - total_written);
      if (nwritten < 0)
        {
          if (errno == EINTR)
            continue;
#if defined __linux__ && defined O_DIRECT
          if (errno == EINVAL && (ctx->cfg.output_flags & O_DIRECT))
            {
              /* O_DIRECT alignment trap: unaligned partial trailing block */
              int cur_flags = fcntl (fd, F_GETFL);
              if (cur_flags >= 0 && (cur_flags & O_DIRECT))
                {
                  /* Drop O_DIRECT for this unaligned tail write */
                  fcntl (fd, F_SETFL, cur_flags & ~O_DIRECT);
                  nwritten = write (fd, buf + total_written, size - total_written);
                  if (nwritten > 0)
                    dd_invalidate_cache (fd, nwritten);
                  fcntl (fd, F_SETFL, cur_flags);
                  if (nwritten > 0)
                    {
                      total_written += nwritten;
                      continue;
                    }
                }
            }
#endif
          break;
        }
      total_written += nwritten;
    }

  if (ctx->cfg.o_nocache && total_written > 0)
    dd_invalidate_cache_chunked (fd, total_written, &ctx->o_nocache_pending, false);

  return total_written;
}

void
dd_write_output (dd_context_t *ctx)
{
  idx_t nwritten = dd_iwrite (ctx, STDOUT_FILENO, ctx->obuf, ctx->cfg.output_blocksize);
  ctx->stats.w_bytes += nwritten;
  if (nwritten != ctx->cfg.output_blocksize)
    {
      dd_diagnose (errno, _("error writing %s"), quoteaf (ctx->cfg.output_file));
      if (nwritten != 0)
        ctx->stats.w_partial++;
    }
  else
    {
      ctx->stats.w_full++;
    }
  ctx->oc = 0;
  dd_check_progress (ctx);
}

void
dd_copy_simple (dd_context_t *ctx, char const *buf, idx_t nbytes)
{
  if (ctx->oc == 0 && nbytes == ctx->cfg.output_blocksize)
    {
      idx_t nwritten = dd_iwrite (ctx, STDOUT_FILENO, buf, nbytes);
      ctx->stats.w_bytes += nwritten;
      if (nwritten != nbytes)
        {
          dd_diagnose (errno, _("error writing %s"), quoteaf (ctx->cfg.output_file));
          if (nwritten != 0)
            ctx->stats.w_partial++;
        }
      else
        {
          ctx->stats.w_full++;
        }
      dd_check_progress (ctx);
      return;
    }

  while (nbytes > 0)
    {
      idx_t copy_now = ctx->cfg.output_blocksize - ctx->oc;
      if (copy_now > nbytes)
        copy_now = nbytes;

      memcpy (ctx->obuf + ctx->oc, buf, copy_now);
      ctx->oc += copy_now;
      buf += copy_now;
      nbytes -= copy_now;

      if (ctx->oc >= ctx->cfg.output_blocksize)
        dd_write_output (ctx);
    }
}

/* -------------------------------------------------------------------------- */
/*                           Driver Selection & Orchestration                 */
/* -------------------------------------------------------------------------- */

const dd_io_driver_t *
dd_select_io_driver (dd_context_t *ctx)
{
#if defined __linux__
  /* 1. Explicit io_uring request */
  if (ctx->cfg.engine == ENGINE_URING || (ctx->cfg.conversions_mask & C_URING))
    return &uring_io_driver;

  /* 2. Explicit reflink / zero-copy request */
  if (ctx->cfg.engine == ENGINE_REFLINK || (ctx->cfg.conversions_mask & C_REFLINK))
    return &reflink_io_driver;

  /* 2b. Explicit splice zero-copy request */
  if (ctx->cfg.engine == ENGINE_SPLICE || (ctx->cfg.conversions_mask & C_SPLICE))
    return &splice_io_driver;
#endif

  /* 3. Explicit Async Pipeline or flag */
  if (ctx->cfg.engine == ENGINE_ASYNC || (ctx->cfg.conversions_mask & C_ASYNC) || (ctx->cfg.output_flags & O_ASYNC_PIPELINE))
    return &async_io_driver;

  /* 4. Explicit Sync block driver */
  if (ctx->cfg.engine == ENGINE_SYNC)
    return &sync_io_driver;

#if defined __linux__
  /* 5. Opportunistic zero-copy reflink via copy_file_range when engine == ENGINE_AUTO */
  const int incompatible = C_SWAB | C_SYNC | C_SHA256 | C_SPARSE | C_AUTOTUNE;
  if (!(ctx->cfg.conversions_mask & incompatible)
      && !ctx->iread_fnc && !ctx->cfg.i_nocache && !ctx->cfg.o_nocache
      && !(ctx->cfg.input_flags & (O_DIRECT | O_NOCACHE))
      && !(ctx->cfg.output_flags & (O_DIRECT | O_NOCACHE)))
    {
      struct stat st_in, st_out;
      if (fstat (STDIN_FILENO, &st_in) == 0 && S_ISREG (st_in.st_mode)
          && fstat (STDOUT_FILENO, &st_out) == 0 && S_ISREG (st_out.st_mode))
        return &reflink_io_driver;
    }

  /* 6. Opportunistic io_uring pipeline when engine == ENGINE_AUTO */
  /* If either stream is a block device or direct I/O is requested */
  if (!(ctx->cfg.conversions_mask & (C_SWAB | C_AUTOTUNE)))
    {
      struct stat st_in, st_out;
      bool is_in_blk = (fstat (STDIN_FILENO, &st_in) == 0 && S_ISBLK (st_in.st_mode));
      bool is_out_blk = (fstat (STDOUT_FILENO, &st_out) == 0 && S_ISBLK (st_out.st_mode));
      bool is_direct = (ctx->cfg.input_flags & O_DIRECT) || (ctx->cfg.output_flags & O_DIRECT);

      if (is_in_blk || is_out_blk || is_direct)
        return &uring_io_driver;
    }

  /* 7. Opportunistic zero-copy kernel splice when either end is a FIFO/pipe */
  if (!(ctx->cfg.conversions_mask & incompatible)
      && !ctx->iread_fnc && !ctx->cfg.i_nocache && !ctx->cfg.o_nocache
      && !(ctx->cfg.input_flags & (O_DIRECT | O_NOCACHE))
      && !(ctx->cfg.output_flags & (O_DIRECT | O_NOCACHE)))
    {
      struct stat st_in, st_out;
      bool in_pipe = (fstat (STDIN_FILENO, &st_in) == 0 && S_ISFIFO (st_in.st_mode));
      bool out_pipe = (fstat (STDOUT_FILENO, &st_out) == 0 && S_ISFIFO (st_out.st_mode));
      if (in_pipe || out_pipe)
        return &splice_io_driver;
    }
#endif

  /* 7. Default standard synchronous driver */
  return &sync_io_driver;
}

/**
 * @brief Generic skip / seek utility for advancing file position.
 */
static intmax_t
skip (dd_context_t *ctx, int fdesc, char const *file, intmax_t records,
      idx_t blocksize, idx_t *bytes)
{
  off_t offset = records * blocksize + *bytes;
  if (0 <= lseek (fdesc, offset, SEEK_CUR))
    {
      *bytes = 0;
      return 0;
    }

  /* Non-seekable fallback */
  char *buf = xmalloc (blocksize);
  while (records > 0)
    {
      ssize_t nread = dd_iread (fdesc, buf, blocksize);
      if (nread <= 0)
        {
          free (buf);
          return records;
        }
      records--;
    }

  if (*bytes > 0)
    {
      ssize_t nread = dd_iread (fdesc, buf, *bytes);
      if (nread > 0)
        *bytes -= nread;
    }

  free (buf);
  (void) ctx;
  (void) file;
  return records;
}

/**
 * @brief Central, unified data transfer orchestration loop.
 */
static int
dd_copy (dd_context_t *ctx)
{
  int exit_status = EXIT_SUCCESS;

  /* Handle input skip */
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
          dd_diagnose (0, _("%s: cannot skip to specified offset"),
                       quotef (ctx->cfg.input_file));
        }
    }

  /* Handle output seek */
  if (ctx->cfg.seek_records != 0 || ctx->cfg.seek_bytes != 0)
    {
      intmax_t write_records = skip (ctx, STDOUT_FILENO, ctx->cfg.output_file,
                                     ctx->cfg.seek_records, ctx->cfg.output_blocksize, &ctx->cfg.seek_bytes);
      if (write_records != 0)
        {
          dd_alloc_obuf (ctx);
          memset (ctx->obuf, 0, ctx->cfg.output_blocksize);
          do
            {
              idx_t size = ctx->cfg.output_blocksize;
              if (write_records == 1 && ctx->cfg.seek_bytes != 0)
                size = ctx->cfg.seek_bytes;
              if (dd_iwrite (ctx, STDOUT_FILENO, ctx->obuf, size) != size)
                {
                  dd_diagnose (errno, _("error writing %s"), quoteaf (ctx->cfg.output_file));
                  return EXIT_FAILURE;
                }
            }
          while (--write_records != 0);
        }
    }

  if (ctx->cfg.max_records == 0 && ctx->cfg.max_bytes == 0)
    return exit_status;

  if (ctx->cfg.conversions_mask & C_SHA256)
    {
      ctx->sha_evp_ctx = EVP_MD_CTX_new ();
      if (ctx->sha_evp_ctx)
        EVP_DigestInit_ex (ctx->sha_evp_ctx, EVP_sha256 (), NULL);
    }

  /* Select and initialize backend driver */
  const dd_io_driver_t *driver = dd_select_io_driver (ctx);
  void *driver_state = NULL;

  if (driver->init (ctx, &driver_state) != EXIT_SUCCESS)
    {
      /* Seamless fallback to synchronous driver */
      driver = &sync_io_driver;
      if (driver->init (ctx, &driver_state) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    }

  bool eof = false;

  /* ======================================================================== */
  /*                  THE SINGLE CENTRAL ORCHESTRATION LOOP                   */
  /* ======================================================================== */
  while (!eof)
    {
      if (active_ctx)
        dd_check_signals (active_ctx);

      /* Global limit reached check */
      if (ctx->cfg.bytes_to_copy >= 0)
        {
          if (ctx->stats.w_bytes >= ctx->cfg.bytes_to_copy)
            break;
        }
      else if (ctx->stats.r_partial + ctx->stats.r_full >= ctx->cfg.max_records + !!ctx->cfg.max_bytes)
        break;

      bool fallback = false;
      int rc = driver->step (ctx, driver_state, &eof, &fallback);

      if (fallback)
        {
          driver->cleanup (ctx, driver_state);
          driver = &sync_io_driver;
          if (driver->init (ctx, &driver_state) != EXIT_SUCCESS)
            return EXIT_FAILURE;
          continue;
        }

      if (rc != EXIT_SUCCESS)
        {
          exit_status = EXIT_FAILURE;
          break;
        }

      /* Single point of truth for interactive live telemetry */
      dd_check_progress (ctx);
    }

  /* Flush pending buffers and release driver resources */
  driver->flush (ctx, driver_state);
  driver->cleanup (ctx, driver_state);

  /* Finalize SHA-256 digest if active */
  if (ctx->cfg.conversions_mask & C_SHA256)
    {
      if (ctx->sha_evp_ctx)
        {
          unsigned int digest_len = 0;
          EVP_DigestFinal_ex (ctx->sha_evp_ctx, ctx->sha_digest, &digest_len);
          EVP_MD_CTX_free (ctx->sha_evp_ctx);
          ctx->sha_evp_ctx = NULL;
        }
      ctx->sha_computed = true;
    }

  return exit_status;
}

/* -------------------------------------------------------------------------- */
/*                           Setup & Stream Lifecycle                         */
/* -------------------------------------------------------------------------- */

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
        {
#if defined __linux__ && defined O_DIRECT
          if (errno == EINVAL && (ctx->cfg.input_flags & O_DIRECT))
            {
              /* Filesystem does not support O_DIRECT (e.g. tmpfs). Transparent fallback to cache eviction */
              ctx->cfg.input_flags &= ~O_DIRECT;
              ctx->cfg.i_nocache = true;
              if (ifd_reopen (STDIN_FILENO, ctx->cfg.input_file, O_RDONLY | ctx->cfg.input_flags, 0) < 0)
                error (EXIT_FAILURE, errno, _("failed to open %s"), quoteaf (ctx->cfg.input_file));
              if (ctx->cfg.status_level != STATUS_NONE)
                error (0, 0, _("warning: '%s' does not support direct I/O; falling back to cache eviction"), quoteaf (ctx->cfg.input_file));
            }
          else
#endif
            error (EXIT_FAILURE, errno, _("failed to open %s"), quoteaf (ctx->cfg.input_file));
        }
    }

  ctx->total_input_size = -1;
  struct stat st;
  if (fstat (STDIN_FILENO, &st) == 0)
    {
      if (S_ISREG (st.st_mode))
        ctx->total_input_size = st.st_size;
#if defined __linux__ && defined BLKGETSIZE64
      else if (S_ISBLK (st.st_mode))
        {
          uint64_t bytes = 0;
          if (ioctl (STDIN_FILENO, BLKGETSIZE64, &bytes) == 0 && bytes > 0)
            ctx->total_input_size = (intmax_t) bytes;
        }
#endif
    }

  off_t offset = lseek (STDIN_FILENO, 0, SEEK_CUR);
  ctx->input_seekable = (0 <= offset);
  ctx->input_offset = (offset > 0 ? offset : 0);

#if HAVE_POSIX_FADVISE
  if (ctx->cfg.i_nocache)
    posix_fadvise (STDIN_FILENO, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
}

static void
setup_output_stream (dd_context_t *ctx)
{
  if (ctx->cfg.output_file == NULL)
    {
      ctx->cfg.output_file = _("standard output");
      set_fd_flags (STDOUT_FILENO, ctx->cfg.output_flags, ctx->cfg.output_file);
    }
  else
    {
      mode_t perms = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
      int opts = ctx->cfg.output_flags
                 | (ctx->cfg.conversions_mask & C_NOCREAT ? 0 : O_CREAT)
                 | (ctx->cfg.conversions_mask & C_EXCL ? O_EXCL : 0)
                 | (ctx->cfg.seek_records || ctx->cfg.seek_bytes || (ctx->cfg.conversions_mask & C_NOTRUNC) ? 0 : O_TRUNC);

#if defined __linux__
      check_target_safety (ctx);
#endif

      if (ofd_reopen (STDOUT_FILENO, ctx->cfg.output_file, O_WRONLY | opts, perms) < 0)
        {
#if defined __linux__ && defined O_DIRECT
          if (errno == EINVAL && (opts & O_DIRECT))
            {
              /* Filesystem does not support O_DIRECT (e.g. tmpfs). Transparent fallback to cache eviction */
              opts &= ~O_DIRECT;
              ctx->cfg.output_flags &= ~O_DIRECT;
              ctx->cfg.o_nocache = true;
              if (ofd_reopen (STDOUT_FILENO, ctx->cfg.output_file, O_WRONLY | opts, perms) < 0)
                error (EXIT_FAILURE, errno, _("failed to open %s"), quoteaf (ctx->cfg.output_file));
              if (ctx->cfg.status_level != STATUS_NONE)
                error (0, 0, _("warning: '%s' does not support direct I/O; falling back to cache eviction"), quoteaf (ctx->cfg.output_file));
            }
          else
#endif
            error (EXIT_FAILURE, errno, _("failed to open %s"), quoteaf (ctx->cfg.output_file));
        }
    }

#if HAVE_POSIX_FADVISE
  if (ctx->cfg.o_nocache)
    posix_fadvise (STDOUT_FILENO, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
}

int
dd_synchronize_output (dd_context_t *ctx)
{
  int exit_status = EXIT_SUCCESS;
  int mask = ctx->cfg.conversions_mask;

  if ((mask & C_FDATASYNC) && ifdatasync (STDOUT_FILENO) != 0)
    {
      if (errno != ENOSYS && errno != EINVAL)
        {
          dd_diagnose (errno, _("fdatasync failed for %s"), quoteaf (ctx->cfg.output_file));
          exit_status = EXIT_FAILURE;
        }
      mask |= C_FSYNC;
    }

  if ((mask & C_FSYNC) && ifsync (STDOUT_FILENO) != 0)
    {
      dd_diagnose (errno, _("fsync failed for %s"), quoteaf (ctx->cfg.output_file));
      return EXIT_FAILURE;
    }

  return exit_status;
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

static int
dd_execute_dry_run (dd_context_t *ctx)
{
  /* 1. Inspect input stream */
  char const *in_path = ctx->cfg.input_file ? ctx->cfg.input_file : "<standard input>";
  char const *in_type = "stream";
  intmax_t in_size = -1;
  bool in_exists = true;

  if (ctx->cfg.input_file)
    {
      struct stat st_in;
      if (stat (ctx->cfg.input_file, &st_in) == 0)
        {
          if (S_ISREG (st_in.st_mode))
            {
              in_type = "regular_file";
              in_size = st_in.st_size;
            }
          else if (S_ISBLK (st_in.st_mode))
            {
              in_type = "block_device";
#if defined __linux__ && defined BLKGETSIZE64
              int fd = open (ctx->cfg.input_file, O_RDONLY | O_CLOEXEC);
              if (fd >= 0)
                {
                  uint64_t bytes = 0;
                  if (ioctl (fd, BLKGETSIZE64, &bytes) == 0)
                    in_size = (intmax_t) bytes;
                  close (fd);
                }
#endif
            }
          else if (S_ISCHR (st_in.st_mode))
            in_type = "character_device";
          else if (S_ISFIFO (st_in.st_mode))
            in_type = "fifo_pipe";
          else if (S_ISDIR (st_in.st_mode))
            in_type = "directory";
          else
            in_type = "special_file";
        }
      else
        {
          in_exists = false;
          in_type = "not_found";
        }
    }

  /* 2. Inspect output stream */
  char const *out_path = ctx->cfg.output_file ? ctx->cfg.output_file : "<standard output>";
  char const *out_type = "stream";
  intmax_t out_size = -1;
  bool out_exists = false;

  if (ctx->cfg.output_file)
    {
      struct stat st_out;
      if (stat (ctx->cfg.output_file, &st_out) == 0)
        {
          out_exists = true;
          if (S_ISREG (st_out.st_mode))
            {
              out_type = "regular_file";
              out_size = st_out.st_size;
            }
          else if (S_ISBLK (st_out.st_mode))
            {
              out_type = "block_device";
#if defined __linux__ && defined BLKGETSIZE64
              int fd = open (ctx->cfg.output_file, O_RDONLY | O_CLOEXEC);
              if (fd >= 0)
                {
                  uint64_t bytes = 0;
                  if (ioctl (fd, BLKGETSIZE64, &bytes) == 0)
                    out_size = (intmax_t) bytes;
                  close (fd);
                }
#endif
            }
          else if (S_ISCHR (st_out.st_mode))
            out_type = "character_device";
          else if (S_ISFIFO (st_out.st_mode))
            out_type = "fifo_pipe";
          else if (S_ISDIR (st_out.st_mode))
            out_type = "directory";
          else
            out_type = "special_file";
        }
      else
        {
          out_exists = false;
          out_type = "new_file";
        }
    }

  /* 3. Safety Guard Evaluation */
  char guard_detail[512] = {0};
  char const *guard_status = "ok";
  char const *guard_msg = "OK (no target restrictions violated)";
  bool forced = (ctx->cfg.output_flags & O_FORCE) || (ctx->cfg.conversions_mask & C_FORCE);

#if defined __linux__
  target_safety_status_t safety = evaluate_target_safety (ctx->cfg.output_file, guard_detail, sizeof guard_detail);
  if (safety == TARGET_SAFETY_MOUNTED_SYSTEM)
    {
      if (forced)
        {
          guard_status = "overridden";
          guard_msg = "OVERRIDDEN via -f/--force (target contains mounted system partition)";
        }
      else
        {
          guard_status = "blocked";
          guard_msg = "BLOCKED: would refuse to write (target contains mounted system partition; requires -f)";
        }
    }
  else if (safety == TARGET_SAFETY_ACTIVE_SWAP)
    {
      if (forced)
        {
          guard_status = "overridden";
          guard_msg = "OVERRIDDEN via -f/--force (target is active system swap device)";
        }
      else
        {
          guard_status = "blocked";
          guard_msg = "BLOCKED: would refuse to write (target is active system swap device; requires -f)";
        }
    }
#endif

  /* 4. Determine Engine */
  char const *engine_name = "sync";
  char const *engine_desc = "Classic synchronous block I/O";

  if (ctx->cfg.engine == ENGINE_URING || (ctx->cfg.conversions_mask & C_URING))
    {
      engine_name = "io_uring";
      engine_desc = "Linux io_uring asynchronous execution";
    }
  else if (ctx->cfg.engine == ENGINE_REFLINK || (ctx->cfg.conversions_mask & C_REFLINK))
    {
      engine_name = "reflink";
      engine_desc = "Linux Kernel zero-copy copy_file_range";
    }
  else if (ctx->cfg.engine == ENGINE_SPLICE || (ctx->cfg.conversions_mask & C_SPLICE))
    {
      engine_name = "splice";
      engine_desc = "Linux Kernel zero-copy pipe splicing";
    }
  else if (ctx->cfg.engine == ENGINE_ASYNC || (ctx->cfg.conversions_mask & C_ASYNC) || (ctx->cfg.output_flags & O_ASYNC_PIPELINE))
    {
      engine_name = "async";
      engine_desc = "Multi-threaded ringbuffer pipeline";
    }
  else if (ctx->cfg.engine == ENGINE_SYNC)
    {
      engine_name = "sync";
      engine_desc = "Classic synchronous block I/O";
    }
  else /* ENGINE_AUTO */
    {
#if defined __linux__
      const int incompatible = C_SWAB | C_SYNC | C_SHA256 | C_SPARSE | C_AUTOTUNE;
      bool can_reflink = !(ctx->cfg.conversions_mask & incompatible)
                         && !ctx->iread_fnc && !ctx->cfg.i_nocache && !ctx->cfg.o_nocache
                         && !(ctx->cfg.input_flags & (O_DIRECT | O_NOCACHE))
                         && !(ctx->cfg.output_flags & (O_DIRECT | O_NOCACHE))
                         && strcmp (in_type, "regular_file") == 0
                         && (strcmp (out_type, "regular_file") == 0 || strcmp (out_type, "new_file") == 0);

      bool is_in_blk = (strcmp (in_type, "block_device") == 0);
      bool is_out_blk = (strcmp (out_type, "block_device") == 0);
      bool is_direct = (ctx->cfg.input_flags & O_DIRECT) || (ctx->cfg.output_flags & O_DIRECT);
      bool can_uring = !(ctx->cfg.conversions_mask & (C_SWAB | C_AUTOTUNE))
                       && (is_in_blk || is_out_blk || is_direct);

      bool in_pipe = (strcmp (in_type, "fifo_pipe") == 0 || !ctx->cfg.input_file);
      bool out_pipe = (strcmp (out_type, "fifo_pipe") == 0 || !ctx->cfg.output_file);
      bool can_splice = !(ctx->cfg.conversions_mask & incompatible)
                        && !ctx->iread_fnc && !ctx->cfg.i_nocache && !ctx->cfg.o_nocache
                        && !(ctx->cfg.input_flags & (O_DIRECT | O_NOCACHE))
                        && !(ctx->cfg.output_flags & (O_DIRECT | O_NOCACHE))
                        && (in_pipe || out_pipe);

      if (can_reflink)
        {
          engine_name = "reflink";
          engine_desc = "Linux Kernel zero-copy copy_file_range (auto-detected)";
        }
      else if (can_uring)
        {
          engine_name = "io_uring";
          engine_desc = "Linux io_uring asynchronous execution (auto-detected)";
        }
      else if (can_splice)
        {
          engine_name = "splice";
          engine_desc = "Linux Kernel zero-copy pipe splicing (auto-detected)";
        }
      else
        {
          engine_name = "sync";
          engine_desc = "Classic synchronous block I/O (default)";
        }
#else
      engine_name = "sync";
      engine_desc = "Classic synchronous block I/O (default)";
#endif
    }

  /* 5. Transfer block size */
  idx_t effective_bs = ctx->cfg.blocksize > 0 ? ctx->cfg.blocksize : 512;
  bool autotune = (ctx->cfg.conversions_mask & C_AUTOTUNE) != 0;

  /* 6. Formatting */
  if (ctx->cfg.json_output || ctx->cfg.status_level == STATUS_JSON)
    {
      printf ("{\"event\":\"dry_run\",\"dry_run\":true,"
              "\"input\":{\"path\":\"%s\",\"type\":\"%s\",\"exists\":%s,\"size_bytes\":%jd},"
              "\"output\":{\"path\":\"%s\",\"type\":\"%s\",\"exists\":%s,\"size_bytes\":%jd,"
              "\"safety_guard\":\"%s\",\"safety_guard_detail\":\"%s\"},"
              "\"plan\":{\"engine\":\"%s\",\"blocksize\":%zu,\"autotune\":%s,"
              "\"direct_io\":%s,\"nocache\":%s,\"sha256\":%s,"
              "\"limit_bytes\":%jd,\"skip_bytes\":%zu,\"seek_bytes\":%zu}}\n",
              in_path, in_type, in_exists ? "true" : "false", in_size,
              out_path, out_type, out_exists ? "true" : "false", out_size,
              guard_status, guard_detail,
              engine_name, (size_t) effective_bs, autotune ? "true" : "false",
              (ctx->cfg.input_flags & O_DIRECT) || (ctx->cfg.output_flags & O_DIRECT) ? "true" : "false",
              ctx->cfg.i_nocache || ctx->cfg.o_nocache ? "true" : "false",
              ctx->cfg.conversions_mask & C_SHA256 ? "true" : "false",
              (intmax_t) ctx->cfg.bytes_to_copy,
              (size_t) (ctx->cfg.skip_records * effective_bs + ctx->cfg.skip_bytes),
              (size_t) (ctx->cfg.seek_records * effective_bs + ctx->cfg.seek_bytes));
    }
  else
    {
      printf (_("=== blkcp Dry-Run Execution Plan ===\n"));
      if (in_size >= 0)
        printf (_("Input:        %s (%s, %jd bytes)\n"), in_path, in_type, in_size);
      else
        printf (_("Input:        %s (%s)\n"), in_path, in_type);

      if (out_size >= 0)
        printf (_("Output:       %s (%s, %jd bytes)\n"), out_path, out_type, out_size);
      else
        printf (_("Output:       %s (%s)\n"), out_path, out_type);

      if (guard_detail[0] != '\0')
        printf (_("Safety Guard: %s [%s]\n"), guard_msg, guard_detail);
      else
        printf (_("Safety Guard: %s\n"), guard_msg);

      printf (_("Engine:       %s (%s)\n"), engine_name, engine_desc);

      if (autotune)
        printf (_("Block Size:   dynamic (autotune active)\n"));
      else
        printf (_("Block Size:   %zu bytes\n"), (size_t) effective_bs);

      printf (_("Direct I/O:   %s\n"),
              (ctx->cfg.input_flags & O_DIRECT) || (ctx->cfg.output_flags & O_DIRECT) ? _("enabled (O_DIRECT)") : _("disabled"));
      printf (_("Cache Policy: %s\n"),
              ctx->cfg.i_nocache || ctx->cfg.o_nocache ? _("nocache (chunked eviction)") : _("kernel page cache"));
      printf (_("Checksum:     %s\n"),
              ctx->cfg.conversions_mask & C_SHA256 ? _("SHA-256 (in-flight)") : _("disabled"));

      if (ctx->cfg.bytes_to_copy >= 0)
        printf (_("Byte Limit:   %jd bytes\n"), (intmax_t) ctx->cfg.bytes_to_copy);
      else
        printf (_("Byte Limit:   unbounded\n"));

      if (ctx->cfg.skip_records > 0 || ctx->cfg.skip_bytes > 0)
        printf (_("Skip Offset:  %zu bytes\n"), (size_t) (ctx->cfg.skip_records * effective_bs + ctx->cfg.skip_bytes));
      if (ctx->cfg.seek_records > 0 || ctx->cfg.seek_bytes > 0)
        printf (_("Seek Offset:  %zu bytes\n"), (size_t) (ctx->cfg.seek_records * effective_bs + ctx->cfg.seek_bytes));

      printf (_("Note: Dry-run simulation mode (-n / --dry-run) active. No data was transferred or modified.\n"));
    }

  return EXIT_SUCCESS;
}

int
dd_execute (dd_context_t *ctx)
{
  active_ctx = ctx;

  if (ctx->cfg.dry_run)
    {
      int status = dd_execute_dry_run (ctx);
      active_ctx = NULL;
      return status;
    }

  setup_input_stream (ctx);
  setup_output_stream (ctx);

  ctx->stats.start_time = gethrxtime ();
  ctx->stats.next_time = ctx->stats.start_time + XTIME_PRECISION;

  int status = dd_copy (ctx);

  if (ctx->cfg.i_nocache || ctx->cfg.i_nocache_eof)
    dd_invalidate_cache_chunked (STDIN_FILENO, 0, &ctx->i_nocache_pending, true);
  if (ctx->cfg.o_nocache || ctx->cfg.o_nocache_eof)
    dd_invalidate_cache_chunked (STDOUT_FILENO, 0, &ctx->o_nocache_pending, true);

  dd_engine_cleanup (ctx);
  dd_print_stats (ctx);

  active_ctx = NULL;
  return status;
}

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
