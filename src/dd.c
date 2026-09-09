/* dd -- convert a file while copying it.
   Copyright (C) 1985-2024 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* Written by Paul Rubin, David MacKenzie, and Stuart Kemp. */

#include <config.h>

#include <ctype.h>
#include <sys/types.h>
#include <signal.h>

#include "system.h"
#include "alignalloc.h"
#include "close-stream.h"
#include "fd-reopen.h"
#include "gethrxtime.h"
#include "human.h"
#include "ioblksize.h"
#include "long-options.h"
#include "quote.h"
#include "verror.h"
#include "xstrtol.h"
#include "xtime.h"

/* The official name of this program (e.g., no 'g' prefix).  */
#define PROGRAM_NAME "dd"

#define AUTHORS \
  proper_name ("Paul Rubin"), \
  proper_name ("David MacKenzie"), \
  proper_name ("Stuart Kemp")

/* Use SA_NOCLDSTOP as a proxy for whether the sigaction machinery is
   present.  */
#ifndef SA_NOCLDSTOP
# define SA_NOCLDSTOP 0
# define sigprocmask(How, Set, Oset) /* empty */
# define sigset_t int
# if ! HAVE_SIGINTERRUPT
#  define siginterrupt(sig, flag) /* empty */
# endif
#endif

/* NonStop circa 2011 lacks SA_RESETHAND; see Bug#9076.  */
#ifndef SA_RESETHAND
# define SA_RESETHAND 0
#endif

#ifndef SIGINFO
# define SIGINFO SIGUSR1
#endif

/* This may belong in GNULIB's fcntl module instead.
   Define O_CIO to 0 if it is not supported by this OS. */
#ifndef O_CIO
# define O_CIO 0
#endif

/* On AIX 5.1 and AIX 5.2, O_NOCACHE is defined via <fcntl.h>
   and would interfere with our use of that name, below.  */
#undef O_NOCACHE

#define output_char(c)				\
  do						\
    {						\
      obuf[oc++] = (c);				\
      if (oc >= output_blocksize)		\
        write_output ();			\
    }						\
  while (0)

/* Default input and output blocksize. */
#define DEFAULT_BLOCKSIZE 512

#include "dd_config.h"

#include "args.h"
#include "conversions.h"
#include "stats.h"
#include "signals.h"

/* Global context holding all runtime state, config, buffers, and stats */
static dd_context_t dd_ctx;

#define input_file (dd_ctx.cfg.input_file)
#define output_file (dd_ctx.cfg.output_file)
#define input_blocksize (dd_ctx.cfg.input_blocksize)
#define output_blocksize (dd_ctx.cfg.output_blocksize)
#define conversion_blocksize (dd_ctx.cfg.conversion_blocksize)
#define skip_records (dd_ctx.cfg.skip_records)
#define skip_bytes (dd_ctx.cfg.skip_bytes)
#define seek_records (dd_ctx.cfg.seek_records)
#define seek_bytes (dd_ctx.cfg.seek_bytes)
#define max_records (dd_ctx.cfg.max_records)
#define max_bytes (dd_ctx.cfg.max_bytes)
#define conversions_mask (dd_ctx.cfg.conversions_mask)
#define input_flags (dd_ctx.cfg.input_flags)
#define output_flags (dd_ctx.cfg.output_flags)
#define status_level (dd_ctx.cfg.status_level)
#define i_nocache (dd_ctx.cfg.i_nocache)
#define o_nocache (dd_ctx.cfg.o_nocache)
#define i_nocache_eof (dd_ctx.cfg.i_nocache_eof)
#define o_nocache_eof (dd_ctx.cfg.o_nocache_eof)

#define r_full (dd_ctx.stats.r_full)
#define r_partial (dd_ctx.stats.r_partial)
#define r_truncate (dd_ctx.stats.r_truncate)
#define w_full (dd_ctx.stats.w_full)
#define w_partial (dd_ctx.stats.w_partial)
#define w_bytes (dd_ctx.stats.w_bytes)
#define reported_w_bytes (dd_ctx.stats.reported_w_bytes)
#define start_time (dd_ctx.stats.start_time)
#define next_time (dd_ctx.stats.next_time)
#define progress_len (dd_ctx.stats.progress_len)

#define page_size (dd_ctx.page_size)
#define ibuf (dd_ctx.ibuf)
#define obuf (dd_ctx.obuf)
#define oc (dd_ctx.oc)
#define col (dd_ctx.col)
#define input_seekable (dd_ctx.input_seekable)
#define input_seek_errno (dd_ctx.input_seek_errno)
#define input_offset (dd_ctx.input_offset)
#define final_op_was_seek (dd_ctx.final_op_was_seek)
#define warn_partial_read (dd_ctx.warn_partial_read)
#define translation_needed (dd_ctx.translation_needed)
#define newline_character (dd_ctx.newline_character)
#define space_character (dd_ctx.space_character)
#define iread_fnc (dd_ctx.iread_fnc)

#define interrupt_signal dd_interrupt_signal
#define info_signal_count dd_info_signal_count
#define caught_signals dd_caught_signals

/* True if we need to close the standard output *stream*.  */
static bool close_stdout_required = true;

/* The only reason to close the standard output *stream* is if
   parse_long_options fails (as it does for --help or --version).
   In any other case, dd uses only the STDOUT_FILENO file descriptor,
   and the "cleanup" function calls "close (STDOUT_FILENO)".
   Closing the file descriptor and then letting the usual atexit-run
   close_stdout function call "fclose (stdout)" would result in a
   harmless failure of the close syscall (with errno EBADF).
   This function serves solely to avoid the unnecessary close_stdout
   call, once parse_long_options has succeeded.
   Meanwhile, we guarantee that the standard error stream is flushed,
   by inlining the last half of close_stdout as needed.  */
static void
maybe_close_stdout (void)
{
  if (close_stdout_required)
    close_stdout ();
  else if (close_stream (stderr) != 0)
    _exit (EXIT_FAILURE);
}

/* Like the 'error' function but handle any pending newline,
   and do not exit.  */

ATTRIBUTE_FORMAT ((__printf__, 2, 3))
static void
diagnose (int errnum, char const *fmt, ...)
{
  if (0 < progress_len)
    {
      fputc ('\n', stderr);
      progress_len = 0;
    }

  va_list ap;
  va_start (ap, fmt);
  verror (0, errnum, fmt, ap);
  va_end (ap);
}

void
usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("\
Usage: %s [OPERAND]...\n\
  or:  %s OPTION\n\
"),
              program_name, program_name);
      fputs (_("\
Copy a file, converting and formatting according to the operands.\n\
\n\
  bs=BYTES        read and write up to BYTES bytes at a time (default: 512);\n\
                  overrides ibs and obs\n\
  cbs=BYTES       convert BYTES bytes at a time\n\
  conv=CONVS      convert the file as per the comma separated symbol list\n\
  count=N         copy only N input blocks\n\
  ibs=BYTES       read up to BYTES bytes at a time (default: 512)\n\
"), stdout);
      fputs (_("\
  if=FILE         read from FILE instead of stdin\n\
  iflag=FLAGS     read as per the comma separated symbol list\n\
  obs=BYTES       write BYTES bytes at a time (default: 512)\n\
  of=FILE         write to FILE instead of stdout\n\
  oflag=FLAGS     write as per the comma separated symbol list\n\
  seek=N          (or oseek=N) skip N obs-sized output blocks\n\
  skip=N          (or iseek=N) skip N ibs-sized input blocks\n\
  status=LEVEL    The LEVEL of information to print to stderr;\n\
                  'none' suppresses everything but error messages,\n\
                  'noxfer' suppresses the final transfer statistics,\n\
                  'progress' shows periodic transfer statistics\n\
"), stdout);
      fputs (_("\
\n\
N and BYTES may be followed by the following multiplicative suffixes:\n\
c=1, w=2, b=512, kB=1000, K=1024, MB=1000*1000, M=1024*1024, xM=M,\n\
GB=1000*1000*1000, G=1024*1024*1024, and so on for T, P, E, Z, Y, R, Q.\n\
Binary prefixes can be used, too: KiB=K, MiB=M, and so on.\n\
If N ends in 'B', it counts bytes not blocks.\n\
\n\
Each CONV symbol may be:\n\
\n\
"), stdout);
      fputs (_("\
  ascii     from EBCDIC to ASCII\n\
  ebcdic    from ASCII to EBCDIC\n\
  ibm       from ASCII to alternate EBCDIC\n\
  block     pad newline-terminated records with spaces to cbs-size\n\
  unblock   replace trailing spaces in cbs-size records with newline\n\
  lcase     change upper case to lower case\n\
  ucase     change lower case to upper case\n\
  sparse    try to seek rather than write all-NUL output blocks\n\
  swab      swap every pair of input bytes\n\
  sync      pad every input block with NULs to ibs-size; when used\n\
            with block or unblock, pad with spaces rather than NULs\n\
"), stdout);
      fputs (_("\
  excl      fail if the output file already exists\n\
  nocreat   do not create the output file\n\
  notrunc   do not truncate the output file\n\
  noerror   continue after read errors\n\
  fdatasync  physically write output file data before finishing\n\
  fsync     likewise, but also write metadata\n\
"), stdout);
      fputs (_("\
\n\
Each FLAG symbol may be:\n\
\n\
  append    append mode (makes sense only for output; conv=notrunc suggested)\n\
"), stdout);
      if (O_CIO)
        fputs (_("  cio       use concurrent I/O for data\n"), stdout);
      if (O_DIRECT)
        fputs (_("  direct    use direct I/O for data\n"), stdout);
      if (O_DIRECTORY)
        fputs (_("  directory  fail unless a directory\n"), stdout);
      if (O_DSYNC)
        fputs (_("  dsync     use synchronized I/O for data\n"), stdout);
      if (O_SYNC)
        fputs (_("  sync      likewise, but also for metadata\n"), stdout);
      fputs (_("  fullblock  accumulate full blocks of input (iflag only)\n"),
             stdout);
      if (O_NONBLOCK)
        fputs (_("  nonblock  use non-blocking I/O\n"), stdout);
      if (O_NOATIME)
        fputs (_("  noatime   do not update access time\n"), stdout);
#if HAVE_POSIX_FADVISE
      if (O_NOCACHE)
        fputs (_("  nocache   Request to drop cache.  See also oflag=sync\n"),
               stdout);
#endif
      if (O_NOCTTY)
        fputs (_("  noctty    do not assign controlling terminal from file\n"),
               stdout);
      if (HAVE_WORKING_O_NOFOLLOW)
        fputs (_("  nofollow  do not follow symlinks\n"), stdout);
      if (O_NOLINKS)
        fputs (_("  nolinks   fail if multiply-linked\n"), stdout);
      if (O_BINARY)
        fputs (_("  binary    use binary I/O for data\n"), stdout);
      if (O_TEXT)
        fputs (_("  text      use text I/O for data\n"), stdout);

      {
        printf (_("\
\n\
Sending a %s signal to a running 'dd' process makes it\n\
print I/O statistics to standard error and then resume copying.\n\
\n\
Options are:\n\
\n\
"), SIGINFO == SIGUSR1 ? "USR1" : "INFO");
      }

      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}

/* Common options to use when displaying sizes and rates.  */

enum { human_opts = (human_autoscale | human_round_to_nearest
                     | human_space_before_unit | human_SI | human_B) };

/* Ensure input buffer IBUF is allocated.  */

static void
alloc_ibuf (void)
{
  if (ibuf)
    return;

  bool extra_byte_for_swab = !!(conversions_mask & C_SWAB);
  ibuf = alignalloc (page_size, input_blocksize + extra_byte_for_swab);
  if (!ibuf)
    {
      char hbuf[LONGEST_HUMAN_READABLE + 1];
      error (EXIT_FAILURE, 0,
             _("memory exhausted by input buffer of size %td bytes (%s)"),
             input_blocksize,
             human_readable (input_blocksize, hbuf,
                             human_opts | human_base_1024, 1, 1));
    }
}

/* Ensure output buffer OBUF is allocated/initialized.  */

static void
alloc_obuf (void)
{
  if (obuf)
    return;

  if (conversions_mask & C_TWOBUFS)
    {
      obuf = alignalloc (page_size, output_blocksize);
      if (!obuf)
        {
          char hbuf[LONGEST_HUMAN_READABLE + 1];
          error (EXIT_FAILURE, 0,
                 _("memory exhausted by output buffer of size %td"
                   " bytes (%s)"),
                 output_blocksize,
                 human_readable (output_blocksize, hbuf,
                                 human_opts | human_base_1024, 1, 1));
        }
    }
  else
    {
      alloc_ibuf ();
      obuf = ibuf;
    }
}

/* Return true if I has more than one bit set.  I must be nonnegative.  */

static inline bool
multiple_bits_set (int i)
{
  return MULTIPLE_BITS_SET (i);
}

/* Print transfer statistics.  */

static void
print_xfer_stats (xtime_t progress_time)
{
  dd_print_xfer_stats (&dd_ctx.stats, &progress_len, progress_time);
}

static void
print_stats (void)
{
  dd_print_stats (&dd_ctx.stats, status_level, &progress_len);
}

/* Install the signal handlers.  */

static void
install_signal_handlers (void)
{
  dd_install_signal_handlers ();
}

/* Close FD.  Return 0 if successful, -1 (setting errno) otherwise.
   If close fails with errno == EINTR, POSIX says the file descriptor
   is in an unspecified state, so keep trying to close FD but do not
   consider EBADF to be an error.  Do not process signals.  This all
   differs somewhat from functions like ifdatasync and ifsync.  */
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

static int synchronize_output (void);

void
dd_cleanup (void)
{
  if (!dd_interrupt_signal)
    {
      int sync_status = synchronize_output ();
      if (sync_status)
        exit (sync_status);
    }

  if (iclose (STDIN_FILENO) != 0)
    error (EXIT_FAILURE, errno, _("closing input file %s"),
           quoteaf (input_file));

  /* Don't remove this call to close, even though close_stdout
     closes standard output.  This close is necessary when cleanup
     is called as a consequence of signal handling.  */
  if (iclose (STDOUT_FILENO) != 0)
    error (EXIT_FAILURE, errno,
           _("closing output file %s"), quoteaf (output_file));
}

static void
cleanup (void)
{
  dd_cleanup ();
}

/* Process any pending signals.  If signals are caught, this function
   should be called periodically.  Ideally there should never be an
   unbounded amount of time when signals are not being processed.  */

static void
process_signals (void)
{
  dd_process_signals (&dd_ctx);
}

static void
finish_up (void)
{
  /* Process signals first, so that cleanup is called at most once.  */
  process_signals ();
  cleanup ();
  print_stats ();
}

static void
quit (int code)
{
  finish_up ();
  exit (code);
}

/* Return LEN rounded down to a multiple of IO_BUFSIZE
   (to minimize calls to the expensive posix_fadvise (,POSIX_FADV_DONTNEED),
   while storing the remainder internally per FD.
   Pass LEN == 0 to get the current remainder.  */

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

/* Discard the cache from the current offset of either
   STDIN_FILENO or STDOUT_FILENO.
   Return true on success.  */

static bool
invalidate_cache (int fd, off_t len)
{
  int adv_ret = -1;
  off_t offset;
  bool nocache_eof = (fd == STDIN_FILENO ? i_nocache_eof : o_nocache_eof);

  /* Minimize syscalls.  */
  off_t clen = cache_round (fd, len);
  if (len && !clen)
    return true; /* Don't advise this time.  */
  else if (! len && ! clen && ! nocache_eof)
    return true;
  off_t pending = len ? cache_round (fd, 0) : 0;

  if (fd == STDIN_FILENO)
    {
      if (input_seekable)
        offset = input_offset;
      else
        {
          offset = -1;
          errno = ESPIPE;
        }
    }
  else
    {
      static off_t output_offset = -2;

      if (output_offset != -1)
        {
          if (output_offset < 0)
            output_offset = lseek (fd, 0, SEEK_CUR);
          else if (len)
            output_offset += clen + pending;
        }

      offset = output_offset;
    }

  if (0 <= offset)
   {
     if (! len && clen && nocache_eof)
       {
         pending = clen;
         clen = 0;
       }

     /* Note we're being careful here to only invalidate what
        we've read, so as not to dump any read ahead cache.
        Note also the kernel is conservative and only invalidates
        full pages in the specified range.  */
#if HAVE_POSIX_FADVISE
     offset = offset - clen - pending;
     /* ensure full page specified when invalidating to eof.  */
     if (clen == 0)
       offset -= offset % page_size;
     adv_ret = posix_fadvise (fd, offset, clen, POSIX_FADV_DONTNEED);
#else
     errno = ENOTSUP;
#endif
   }

  return adv_ret != -1 ? true : false;
}

/* Read from FD into the buffer BUF of size SIZE, processing any
   signals that arrive before bytes are read.  Return the number of
   bytes read if successful, -1 (setting errno) on failure.  */

static ssize_t
iread (int fd, char *buf, idx_t size)
{
  ssize_t nread;
  static ssize_t prev_nread;

  do
    {
      process_signals ();
      nread = read (fd, buf, size);
      /* Ignore final read error with iflag=direct as that
         returns EINVAL due to the non aligned file offset.  */
      if (nread == -1 && errno == EINVAL
          && 0 < prev_nread && prev_nread < size
          && (input_flags & O_DIRECT))
        {
          errno = 0;
          nread = 0;
        }
    }
  while (nread < 0 && errno == EINTR);

  /* Short read may be due to received signal.  */
  if (0 < nread && nread < size)
    process_signals ();

  if (0 < nread && warn_partial_read)
    {
      if (0 < prev_nread && prev_nread < size)
        {
          idx_t prev = prev_nread;
          if (status_level != STATUS_NONE)
            diagnose (0, ngettext (("warning: partial read (%td byte); "
                                    "suggest iflag=fullblock"),
                                   ("warning: partial read (%td bytes); "
                                    "suggest iflag=fullblock"),
                                   select_plural (prev)),
                      prev);
          warn_partial_read = false;
        }
    }

  prev_nread = nread;
  return nread;
}

/* Wrapper around iread function to accumulate full blocks.  */
static ssize_t
iread_fullblock (int fd, char *buf, idx_t size)
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
      buf   += ncurr;
      size  -= ncurr;
    }

  return nread;
}

/* Write to FD the buffer BUF of size SIZE, processing any signals
   that arrive.  Return the number of bytes written, setting errno if
   this is less than SIZE.  Keep trying if there are partial
   writes.  */

static idx_t
iwrite (int fd, char const *buf, idx_t size)
{
  idx_t total_written = 0;

  if ((output_flags & O_DIRECT) && size < output_blocksize)
    {
      int old_flags = fcntl (STDOUT_FILENO, F_GETFL);
      if (fcntl (STDOUT_FILENO, F_SETFL, old_flags & ~O_DIRECT) != 0
          && status_level != STATUS_NONE)
        diagnose (errno, _("failed to turn off O_DIRECT: %s"),
                  quotef (output_file));

      /* Since we have just turned off O_DIRECT for the final write,
         we try to preserve some of its semantics.  */

      /* Call invalidate_cache to setup the appropriate offsets
         for subsequent calls.  */
      o_nocache_eof = true;
      invalidate_cache (STDOUT_FILENO, 0);

      /* Attempt to ensure that that final block is committed
         to stable storage as quickly as possible.  */
      conversions_mask |= C_FSYNC;

      /* After the subsequent fsync we'll call invalidate_cache
         to attempt to clear all data from the page cache.  */
    }

  while (total_written < size)
    {
      ssize_t nwritten = 0;
      process_signals ();

      /* Perform a seek for a NUL block if sparse output is enabled.  */
      final_op_was_seek = false;
      if ((conversions_mask & C_SPARSE) && is_nul (buf, size))
        {
          if (lseek (fd, size, SEEK_CUR) < 0)
            {
              conversions_mask &= ~C_SPARSE;
              /* Don't warn about the advisory sparse request.  */
            }
          else
            {
              final_op_was_seek = true;
              nwritten = size;
            }
        }

      if (!nwritten)
        nwritten = write (fd, buf + total_written, size - total_written);

      if (nwritten < 0)
        {
          if (errno != EINTR)
            break;
        }
      else if (nwritten == 0)
        {
          /* Some buggy drivers return 0 when one tries to write beyond
             a device's end.  (Example: Linux kernel 1.2.13 on /dev/fd0.)
             Set errno to ENOSPC so they get a sensible diagnostic.  */
          errno = ENOSPC;
          break;
        }
      else
        total_written += nwritten;
    }

  if (o_nocache && total_written)
    invalidate_cache (fd, total_written);

  return total_written;
}

/* Write, then empty, the output buffer 'obuf'. */

static void
write_output (void)
{
  idx_t nwritten = iwrite (STDOUT_FILENO, obuf, output_blocksize);
  w_bytes += nwritten;
  if (nwritten != output_blocksize)
    {
      diagnose (errno, _("writing to %s"), quoteaf (output_file));
      if (nwritten != 0)
        w_partial++;
      quit (EXIT_FAILURE);
    }
  else
    w_full++;
  oc = 0;
}

/* Restart on EINTR from fdatasync.  */

static int
ifdatasync (int fd)
{
  int ret;

  do
    {
      process_signals ();
      ret = fdatasync (fd);
    }
  while (ret < 0 && errno == EINTR);

  return ret;
}

/* Restart on EINTR from fd_reopen.  */

static int
ifd_reopen (int desired_fd, char const *file, int flag, mode_t mode)
{
  int ret;

  do
    {
      process_signals ();
      ret = fd_reopen (desired_fd, file, flag, mode);
    }
  while (ret < 0 && errno == EINTR);

  return ret;
}

/* Restart on EINTR from fstat.  */

static int
ifstat (int fd, struct stat *st)
{
  int ret;

  do
    {
      process_signals ();
      ret = fstat (fd, st);
    }
  while (ret < 0 && errno == EINTR);

  return ret;
}

/* Restart on EINTR from fsync.  */

static int
ifsync (int fd)
{
  int ret;

  do
    {
      process_signals ();
      ret = fsync (fd);
    }
  while (ret < 0 && errno == EINTR);

  return ret;
}

/* Restart on EINTR from ftruncate.  */

static int
iftruncate (int fd, off_t length)
{
  int ret;

  do
    {
      process_signals ();
      ret = ftruncate (fd, length);
    }
  while (ret < 0 && errno == EINTR);

  return ret;
}
static void
scanargs (int argc, char *const *argv)
{
  dd_init_default_config (&dd_ctx.cfg);
  bool use_fullblock = false;

  dd_scanargs (argc, argv, &dd_ctx.cfg, &warn_partial_read, &use_fullblock);

  iread_fnc = use_fullblock ? iread_fullblock : iread;
}

/* Fix up translation table. */

static void
apply_translations (void)
{
  dd_apply_translations (conversions_mask,
                         &newline_character,
                         &space_character,
                         &translation_needed);
}

/* Apply the character-set translations specified by the user
   to the NREAD bytes in BUF.  */

static void
translate_buffer (char *buf, idx_t nread)
{
  dd_translate_buffer (buf, nread);
}

static char *
swab_buffer (char *buf, idx_t *nread, int *saved_byte)
{
  return dd_swab_buffer (buf, nread, saved_byte);
}

/* Add OFFSET to the input offset, setting the overflow flag if
   necessary.  */

static void
advance_input_offset (intmax_t offset)
{
  if (0 <= input_offset && ckd_add (&input_offset, input_offset, offset))
    input_offset = -1;
}

/* Throw away RECORDS blocks of BLOCKSIZE bytes plus BYTES bytes on
   file descriptor FDESC, which is open with read permission for FILE.
   Store up to BLOCKSIZE bytes of the data at a time in IBUF or OBUF, if
   necessary. RECORDS or BYTES must be nonzero. If FDESC is
   STDIN_FILENO, advance the input offset. Return the number of
   records remaining, i.e., that were not skipped because EOF was
   reached.  If FDESC is STDOUT_FILENO, on return, BYTES is the
   remaining bytes in addition to the remaining records.  */

static intmax_t
skip (int fdesc, char const *file, intmax_t records, idx_t blocksize,
      idx_t *bytes)
{
  /* Try lseek and if an error indicates it was an inappropriate operation --
     or if the file offset is not representable as an off_t --
     fall back on using read.  */

  errno = 0;
  off_t offset;
  if (! ckd_mul (&offset, records, blocksize)
      && ! ckd_add (&offset, offset, *bytes)
      && 0 <= lseek (fdesc, offset, SEEK_CUR))
    {
      if (fdesc == STDIN_FILENO)
        {
           struct stat st;
           if (ifstat (STDIN_FILENO, &st) != 0)
             error (EXIT_FAILURE, errno, _("cannot fstat %s"), quoteaf (file));
           if (usable_st_size (&st) && 0 <= input_offset
               && st.st_size - input_offset < offset)
             {
               /* When skipping past EOF, return the number of _full_ blocks
                * that are not skipped, and set offset to EOF, so the caller
                * can determine the requested skip was not satisfied.  */
               records = ( offset - st.st_size ) / blocksize;
               offset = st.st_size - input_offset;
             }
           else
             records = 0;
           advance_input_offset (offset);
        }
      else
        {
          records = 0;
          *bytes = 0;
        }
      return records;
    }
  else
    {
      int lseek_errno = errno;

      /* The seek request may have failed above if it was too big
         (> device size, > max file size, etc.)
         Or it may not have been done at all (> OFF_T_MAX).
         Therefore try to seek to the end of the file,
         to avoid redundant reading.  */
      if (lseek (fdesc, 0, SEEK_END) >= 0)
        {
          /* File is seekable, and we're at the end of it, and
             size <= OFF_T_MAX. So there's no point using read to advance.  */

          if (!lseek_errno)
            {
              /* The original seek was not attempted as offset > OFF_T_MAX.
                 We should error for write as can't get to the desired
                 location, even if OFF_T_MAX < max file size.
                 For read we're not going to read any data anyway,
                 so we should error for consistency.
                 It would be nice to not error for /dev/{zero,null}
                 for any offset, but that's not a significant issue.  */
              lseek_errno = EOVERFLOW;
            }

          diagnose (lseek_errno,
                    gettext (fdesc == STDIN_FILENO
                             ? N_("%s: cannot skip")
                             : N_("%s: cannot seek")),
                    quotef (file));
          /* If the file has a specific size and we've asked
             to skip/seek beyond the max allowable, then quit.  */
          quit (EXIT_FAILURE);
        }
      /* else file_size && offset > OFF_T_MAX or file ! seekable */

      char *buf;
      if (fdesc == STDIN_FILENO)
        {
          alloc_ibuf ();
          buf = ibuf;
        }
      else
        {
          alloc_obuf ();
          buf = obuf;
        }

      do
        {
          ssize_t nread = iread_fnc (fdesc, buf, records ? blocksize : *bytes);
          if (nread < 0)
            {
              if (fdesc == STDIN_FILENO)
                {
                  diagnose (errno, _("error reading %s"), quoteaf (file));
                  if (conversions_mask & C_NOERROR)
                    print_stats ();
                }
              else
                diagnose (lseek_errno, _("%s: cannot seek"), quotef (file));
              quit (EXIT_FAILURE);
            }
          else if (nread == 0)
            break;
          else if (fdesc == STDIN_FILENO)
            advance_input_offset (nread);

          if (records != 0)
            records--;
          else
            *bytes = 0;
        }
      while (records || *bytes);

      return records;
    }
}

/* Advance the input by NBYTES if possible, after a read error.
   The input file offset may or may not have advanced after the failed
   read; adjust it to point just after the bad record regardless.
   Return true if successful, or if the input is already known to not
   be seekable.  */

static bool
advance_input_after_read_error (idx_t nbytes)
{
  if (! input_seekable)
    {
      if (input_seek_errno == ESPIPE)
        return true;
      errno = input_seek_errno;
    }
  else
    {
      off_t offset;
      advance_input_offset (nbytes);
      if (input_offset < 0)
        {
          diagnose (0, _("offset overflow while reading file %s"),
                    quoteaf (input_file));
          return false;
        }
      offset = lseek (STDIN_FILENO, 0, SEEK_CUR);
      if (0 <= offset)
        {
          off_t diff;
          if (offset == input_offset)
            return true;
          diff = input_offset - offset;
          if (! (0 <= diff && diff <= nbytes) && status_level != STATUS_NONE)
            diagnose (0, _("warning: invalid file offset after failed read"));
          if (0 <= lseek (STDIN_FILENO, diff, SEEK_CUR))
            return true;
          if (errno == 0)
            diagnose (0, _("cannot work around kernel bug after all"));
        }
    }

  diagnose (errno, _("%s: cannot seek"), quotef (input_file));
  return false;
}

/* Copy NREAD bytes of BUF, with no conversions.  */

static void
copy_simple (char const *buf, idx_t nread)
{
  char const *start = buf;	/* First uncopied char in BUF.  */

  do
    {
      idx_t nfree = MIN (nread, output_blocksize - oc);

      memcpy (obuf + oc, start, nfree);

      nread -= nfree;		/* Update the number of bytes left to copy. */
      start += nfree;
      oc += nfree;
      if (oc >= output_blocksize)
        write_output ();
    }
  while (nread != 0);
}

/* Copy NREAD bytes of BUF, doing conv=block
   (pad newline-terminated records to 'conversion_blocksize',
   replacing the newline with trailing spaces).  */

static void
copy_with_block (char const *buf, idx_t nread)
{
  for (idx_t i = nread; i; i--, buf++)
    {
      if (*buf == newline_character)
        {
          if (col < conversion_blocksize)
            {
              idx_t j;
              for (j = col; j < conversion_blocksize; j++)
                output_char (space_character);
            }
          col = 0;
        }
      else
        {
          if (col == conversion_blocksize)
            r_truncate++;
          else if (col < conversion_blocksize)
            output_char (*buf);
          col++;
        }
    }
}

/* Copy NREAD bytes of BUF, doing conv=unblock
   (replace trailing spaces in 'conversion_blocksize'-sized records
   with a newline).  */

static void
copy_with_unblock (char const *buf, idx_t nread)
{
  static idx_t pending_spaces = 0;

  for (idx_t i = 0; i < nread; i++)
    {
      char c = buf[i];

      if (col++ >= conversion_blocksize)
        {
          col = pending_spaces = 0; /* Wipe out any pending spaces.  */
          i--;			/* Push the char back; get it later. */
          output_char (newline_character);
        }
      else if (c == space_character)
        pending_spaces++;
      else
        {
          /* 'c' is the character after a run of spaces that were not
             at the end of the conversion buffer.  Output them.  */
          while (pending_spaces)
            {
              output_char (space_character);
              --pending_spaces;
            }
          output_char (c);
        }
    }
}

/* Set the file descriptor flags for FD that correspond to the nonzero bits
   in ADD_FLAGS.  The file's name is NAME.  */

static void
set_fd_flags (int fd, int add_flags, char const *name)
{
  /* Ignore file creation flags that are no-ops on file descriptors.  */
  add_flags &= ~ (O_NOCTTY | O_NOFOLLOW);

  if (add_flags)
    {
      int old_flags = fcntl (fd, F_GETFL);
      int new_flags = old_flags | add_flags;
      bool ok = true;
      if (old_flags < 0)
        ok = false;
      else if (old_flags != new_flags)
        {
          if (new_flags & (O_DIRECTORY | O_NOLINKS))
            {
              /* NEW_FLAGS contains at least one file creation flag that
                 requires some checking of the open file descriptor.  */
              struct stat st;
              if (ifstat (fd, &st) != 0)
                ok = false;
              else if ((new_flags & O_DIRECTORY) && ! S_ISDIR (st.st_mode))
                {
                  errno = ENOTDIR;
                  ok = false;
                }
              else if ((new_flags & O_NOLINKS) && 1 < st.st_nlink)
                {
                  errno = EMLINK;
                  ok = false;
                }
              new_flags &= ~ (O_DIRECTORY | O_NOLINKS);
            }

          if (ok && old_flags != new_flags
              && fcntl (fd, F_SETFL, new_flags) == -1)
            ok = false;
        }

      if (!ok)
        error (EXIT_FAILURE, errno, _("setting flags for %s"), quoteaf (name));
    }
}

/* The main loop.  */

static int
dd_copy (void)
{
  char *bufstart;		/* Input buffer. */
  ssize_t nread;		/* Bytes read in the current block.  */

  /* If nonzero, then the previously read block was partial and
     PARTREAD was its size.  */
  idx_t partread = 0;

  int exit_status = EXIT_SUCCESS;
  idx_t n_bytes_read;

  if (skip_records != 0 || skip_bytes != 0)
    {
      intmax_t us_bytes;
      bool us_bytes_overflow =
        (ckd_mul (&us_bytes, skip_records, input_blocksize)
         || ckd_add (&us_bytes, skip_bytes, us_bytes));
      off_t input_offset0 = input_offset;
      intmax_t us_blocks = skip (STDIN_FILENO, input_file,
                                 skip_records, input_blocksize, &skip_bytes);

      /* POSIX doesn't say what to do when dd detects it has been
         asked to skip past EOF, so I assume it's non-fatal.
         There are 3 reasons why there might be unskipped blocks/bytes:
             1. file is too small
             2. pipe has not enough data
             3. partial reads  */
      if ((us_blocks
           || (0 <= input_offset
               && (us_bytes_overflow
                   || us_bytes != input_offset - input_offset0)))
          && status_level != STATUS_NONE)
        {
          diagnose (0, _("%s: cannot skip to specified offset"),
                    quotef (input_file));
        }
    }

  if (seek_records != 0 || seek_bytes != 0)
    {
      idx_t bytes = seek_bytes;
      intmax_t write_records = skip (STDOUT_FILENO, output_file,
                                      seek_records, output_blocksize, &bytes);

      if (write_records != 0 || bytes != 0)
        {
          memset (obuf, 0, write_records ? output_blocksize : bytes);

          do
            {
              idx_t size = write_records ? output_blocksize : bytes;
              if (iwrite (STDOUT_FILENO, obuf, size) != size)
                {
                  diagnose (errno, _("writing to %s"), quoteaf (output_file));
                  quit (EXIT_FAILURE);
                }

              if (write_records != 0)
                write_records--;
              else
                bytes = 0;
            }
          while (write_records || bytes);
        }
    }

  if (max_records == 0 && max_bytes == 0)
    return exit_status;

  alloc_ibuf ();
  alloc_obuf ();
  int saved_byte = -1;

  while (true)
    {
      if (status_level == STATUS_PROGRESS)
        {
          xtime_t progress_time = gethrxtime ();
          if (next_time <= progress_time)
            {
              print_xfer_stats (progress_time);
              next_time += XTIME_PRECISION;
            }
        }

      if (r_partial + r_full >= max_records + !!max_bytes)
        break;

      /* Zero the buffer before reading, so that if we get a read error,
         whatever data we are able to read is followed by zeros.
         This minimizes data loss. */
      if ((conversions_mask & C_SYNC) && (conversions_mask & C_NOERROR))
        memset (ibuf,
                (conversions_mask & (C_BLOCK | C_UNBLOCK)) ? ' ' : '\0',
                input_blocksize);

      if (r_partial + r_full >= max_records)
        nread = iread_fnc (STDIN_FILENO, ibuf, max_bytes);
      else
        nread = iread_fnc (STDIN_FILENO, ibuf, input_blocksize);

      if (nread > 0)
        {
          advance_input_offset (nread);
          if (i_nocache)
            invalidate_cache (STDIN_FILENO, nread);
        }
      else if (nread == 0)
        {
          i_nocache_eof |= i_nocache;
          o_nocache_eof |= o_nocache && ! (conversions_mask & C_NOTRUNC);
          break;			/* EOF.  */
        }
      else
        {
          if (!(conversions_mask & C_NOERROR) || status_level != STATUS_NONE)
            diagnose (errno, _("error reading %s"), quoteaf (input_file));

          if (conversions_mask & C_NOERROR)
            {
              print_stats ();
              idx_t bad_portion = input_blocksize - partread;

              /* We already know this data is not cached,
                 but call this so that correct offsets are maintained.  */
              invalidate_cache (STDIN_FILENO, bad_portion);

              /* Seek past the bad block if possible. */
              if (!advance_input_after_read_error (bad_portion))
                {
                  exit_status = EXIT_FAILURE;

                  /* Suppress duplicate diagnostics.  */
                  input_seekable = false;
                  input_seek_errno = ESPIPE;
                }
              if ((conversions_mask & C_SYNC) && !partread)
                /* Replace the missing input with null bytes and
                   proceed normally.  */
                nread = 0;
              else
                continue;
            }
          else
            {
              /* Write any partial block. */
              exit_status = EXIT_FAILURE;
              break;
            }
        }

      n_bytes_read = nread;

      if (n_bytes_read < input_blocksize)
        {
          r_partial++;
          partread = n_bytes_read;
          if (conversions_mask & C_SYNC)
            {
              if (!(conversions_mask & C_NOERROR))
                /* If C_NOERROR, we zeroed the block before reading. */
                memset (ibuf + n_bytes_read,
                        (conversions_mask & (C_BLOCK | C_UNBLOCK)) ? ' ' : '\0',
                        input_blocksize - n_bytes_read);
              n_bytes_read = input_blocksize;
            }
        }
      else
        {
          r_full++;
          partread = 0;
        }

      if (ibuf == obuf)		/* If not C_TWOBUFS. */
        {
          idx_t nwritten = iwrite (STDOUT_FILENO, obuf, n_bytes_read);
          w_bytes += nwritten;
          if (nwritten != n_bytes_read)
            {
              diagnose (errno, _("error writing %s"), quoteaf (output_file));
              return EXIT_FAILURE;
            }
          else if (n_bytes_read == input_blocksize)
            w_full++;
          else
            w_partial++;
          continue;
        }

      /* Do any translations on the whole buffer at once.  */

      if (translation_needed)
        translate_buffer (ibuf, n_bytes_read);

      if (conversions_mask & C_SWAB)
        bufstart = swab_buffer (ibuf, &n_bytes_read, &saved_byte);
      else
        bufstart = ibuf;

      if (conversions_mask & C_BLOCK)
        copy_with_block (bufstart, n_bytes_read);
      else if (conversions_mask & C_UNBLOCK)
        copy_with_unblock (bufstart, n_bytes_read);
      else
        copy_simple (bufstart, n_bytes_read);
    }

  /* If we have a char left as a result of conv=swab, output it.  */
  if (0 <= saved_byte)
    {
      char saved_char = saved_byte;
      if (conversions_mask & C_BLOCK)
        copy_with_block (&saved_char, 1);
      else if (conversions_mask & C_UNBLOCK)
        copy_with_unblock (&saved_char, 1);
      else
        output_char (saved_char);
    }

  if ((conversions_mask & C_BLOCK) && col > 0)
    {
      /* If the final input line didn't end with a '\n', pad
         the output block to 'conversion_blocksize' chars.  */
      for (idx_t i = col; i < conversion_blocksize; i++)
        output_char (space_character);
    }

  if (col && (conversions_mask & C_UNBLOCK))
    {
      /* If there was any output, add a final '\n'.  */
      output_char (newline_character);
    }

  /* Write out the last block. */
  if (oc != 0)
    {
      idx_t nwritten = iwrite (STDOUT_FILENO, obuf, oc);
      w_bytes += nwritten;
      if (nwritten != 0)
        w_partial++;
      if (nwritten != oc)
        {
          diagnose (errno, _("error writing %s"), quoteaf (output_file));
          return EXIT_FAILURE;
        }
    }

  /* If the last write was converted to a seek, then for a regular file
     or shared memory object, ftruncate to extend the size.  */
  if (final_op_was_seek)
    {
      struct stat stdout_stat;
      if (ifstat (STDOUT_FILENO, &stdout_stat) != 0)
        {
          diagnose (errno, _("cannot fstat %s"), quoteaf (output_file));
          return EXIT_FAILURE;
        }
      if (S_ISREG (stdout_stat.st_mode) || S_TYPEISSHM (&stdout_stat))
        {
          off_t output_offset = lseek (STDOUT_FILENO, 0, SEEK_CUR);
          if (0 <= output_offset && stdout_stat.st_size < output_offset)
            {
              if (iftruncate (STDOUT_FILENO, output_offset) != 0)
                {
                  diagnose (errno, _("failed to truncate to %jd bytes"
                                     " in output file %s"),
                            (intmax_t) output_offset, quoteaf (output_file));
                  return EXIT_FAILURE;
                }
            }
        }
    }

  /* fdatasync/fsync can take a long time, so issue a final progress
     indication now if progress has been made since the previous indication.  */
  if (conversions_mask & (C_FDATASYNC | C_FSYNC)
      && status_level == STATUS_PROGRESS
      && 0 <= reported_w_bytes && reported_w_bytes < w_bytes)
    print_xfer_stats (0);

  return exit_status;
}

/* Synchronize output according to conversions_mask.
   Do this even if w_bytes is zero, as fsync and fdatasync
   flush out write requests from other processes too.
   Clear bits in conversions_mask so that synchronization is done only once.
   Return zero if successful, an exit status otherwise.  */

static int
synchronize_output (void)
{
  int exit_status = 0;
  int mask = conversions_mask;
  conversions_mask &= ~ (C_FDATASYNC | C_FSYNC);

  if ((mask & C_FDATASYNC) && ifdatasync (STDOUT_FILENO) != 0)
    {
      if (errno != ENOSYS && errno != EINVAL)
        {
          diagnose (errno, _("fdatasync failed for %s"), quoteaf (output_file));
          exit_status = EXIT_FAILURE;
        }
      mask |= C_FSYNC;
    }

  if ((mask & C_FSYNC) && ifsync (STDOUT_FILENO) != 0)
    {
      diagnose (errno, _("fsync failed for %s"), quoteaf (output_file));
      return EXIT_FAILURE;
    }

  return exit_status;
}

int
main (int argc, char **argv)
{
  int i;
  int exit_status;
  off_t offset;

  install_signal_handlers ();

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  /* Arrange to close stdout if parse_long_options exits.  */
  atexit (maybe_close_stdout);

  page_size = getpagesize ();

  parse_gnu_standard_options_only (argc, argv, PROGRAM_NAME, PACKAGE, Version,
                                   true, usage, AUTHORS,
                                   (char const *) nullptr);
  close_stdout_required = false;

  /* Initialize translation table to identity translation. */
  dd_init_translations ();

  /* Decode arguments. */
  scanargs (argc, argv);

  apply_translations ();

  if (input_file == nullptr)
    {
      input_file = _("standard input");
      set_fd_flags (STDIN_FILENO, input_flags, input_file);
    }
  else
    {
      if (ifd_reopen (STDIN_FILENO, input_file, O_RDONLY | input_flags, 0) < 0)
        error (EXIT_FAILURE, errno, _("failed to open %s"),
               quoteaf (input_file));
    }

  offset = lseek (STDIN_FILENO, 0, SEEK_CUR);
  input_seekable = (0 <= offset);
  input_offset = MAX (0, offset);
  input_seek_errno = errno;

  if (output_file == nullptr)
    {
      output_file = _("standard output");
      set_fd_flags (STDOUT_FILENO, output_flags, output_file);
    }
  else
    {
      mode_t perms = MODE_RW_UGO;
      int opts
        = (output_flags
           | (conversions_mask & C_NOCREAT ? 0 : O_CREAT)
           | (conversions_mask & C_EXCL ? O_EXCL : 0)
           | (seek_records || (conversions_mask & C_NOTRUNC) ? 0 : O_TRUNC));

      off_t size;
      if ((ckd_mul (&size, seek_records, output_blocksize)
           || ckd_add (&size, seek_bytes, size))
          && !(conversions_mask & C_NOTRUNC))
        error (EXIT_FAILURE, 0,
               _("offset too large: "
                 "cannot truncate to a length of seek=%jd"
                 " (%td-byte) blocks"),
               seek_records, output_blocksize);

      /* Open the output file with *read* access only if we might
         need to read to satisfy a 'seek=' request.  If we can't read
         the file, go ahead with write-only access; it might work.  */
      if ((! seek_records
           || ifd_reopen (STDOUT_FILENO, output_file, O_RDWR | opts, perms) < 0)
          && (ifd_reopen (STDOUT_FILENO, output_file, O_WRONLY | opts, perms)
              < 0))
        error (EXIT_FAILURE, errno, _("failed to open %s"),
               quoteaf (output_file));

      if (seek_records != 0 && !(conversions_mask & C_NOTRUNC))
        {
          if (iftruncate (STDOUT_FILENO, size) != 0)
            {
              /* Complain only when ftruncate fails on a regular file, a
                 directory, or a shared memory object, as POSIX 1003.1-2004
                 specifies ftruncate's behavior only for these file types.
                 For example, do not complain when Linux kernel 2.4 ftruncate
                 fails on /dev/fd0.  */
              int ftruncate_errno = errno;
              struct stat stdout_stat;
              if (ifstat (STDOUT_FILENO, &stdout_stat) != 0)
                {
                  diagnose (errno, _("cannot fstat %s"), quoteaf (output_file));
                  exit_status = EXIT_FAILURE;
                }
              else if (S_ISREG (stdout_stat.st_mode)
                       || S_ISDIR (stdout_stat.st_mode)
                       || S_TYPEISSHM (&stdout_stat))
                {
                  intmax_t isize = size;
                  diagnose (ftruncate_errno,
                            _("failed to truncate to %jd bytes"
                              " in output file %s"),
                            isize, quoteaf (output_file));
                  exit_status = EXIT_FAILURE;
                }
            }
        }
    }

  start_time = gethrxtime ();
  next_time = start_time + XTIME_PRECISION;

  exit_status = dd_copy ();

  int sync_status = synchronize_output ();
  if (sync_status)
    exit_status = sync_status;

  if (max_records == 0 && max_bytes == 0)
    {
      /* Special case to invalidate cache to end of file.  */
      if (i_nocache && !invalidate_cache (STDIN_FILENO, 0))
        {
          diagnose (errno, _("failed to discard cache for: %s"),
                    quotef (input_file));
          exit_status = EXIT_FAILURE;
        }
      if (o_nocache && !invalidate_cache (STDOUT_FILENO, 0))
        {
          diagnose (errno, _("failed to discard cache for: %s"),
                    quotef (output_file));
          exit_status = EXIT_FAILURE;
        }
    }
  else
    {
      /* Invalidate any pending region or to EOF if appropriate.  */
      if (i_nocache || i_nocache_eof)
        invalidate_cache (STDIN_FILENO, 0);
      if (o_nocache || o_nocache_eof)
        invalidate_cache (STDOUT_FILENO, 0);
    }

  finish_up ();
  main_exit (exit_status);
}
