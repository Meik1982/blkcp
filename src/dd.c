#include <config.h>
#include <sys/types.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "system.h"
#include "close-stream.h"
#include "closeout.h"
#include "long-options.h"
#include "progname.h"
#include "version.h"

#include "dd_config.h"
#include "args.h"
#include "conversions.h"
#include "stats.h"
#include "signals.h"
#include "io_engine.h"

#define PROGRAM_NAME "dd"
#define AUTHORS \
  proper_name ("Paul Rubin"), \
  proper_name ("David MacKenzie"), \
  proper_name ("Stuart Kemp")

static bool close_stdout_required = true;

static void
maybe_close_stdout (void)
{
  if (close_stdout_required)
    close_stdout ();
  else if (close_stream (stderr) != 0)
    _exit (EXIT_FAILURE);
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
                  overrides ibs and obs; 'bs=auto' autotunes block size\n\
  cbs=BYTES       convert BYTES bytes at a time\n\
  conv=CONVS      convert the file as per the comma separated symbol list\n\
  count=N         copy only N input blocks\n\
  ibs=BYTES       read up to BYTES bytes at a time (default: 512)\n\
  if=FILE         read from FILE instead of stdin\n\
  iflag=FLAGS     read as per the comma separated symbol list\n\
  obs=BYTES       write BYTES bytes at a time (default: 512)\n\
  of=FILE         write to FILE instead of stdout\n\
  oflag=FLAGS     write as per the comma separated symbol list\n\
  opt=FEATURE     enable modern extensions ('auto', 'force', 'sha256')\n\
  seek=N          (or oseek=N) skip N obs-sized output blocks\n\
  skip=N          (or iseek=N) skip N ibs-sized input blocks\n\
  status=LEVEL    The LEVEL of information to print to stderr;\n\
                  'none' suppresses everything but error messages,\n\
                  'noxfer' suppresses the final transfer statistics,\n\
                  'progress' shows periodic transfer statistics\n\
\n\
N and BYTES may be followed by the following multiplicative suffixes:\n\
c=1, w=2, b=512, kB=1000, K=1024, MB=1000*1000, M=1024*1024, xM=M,\n\
GB=1000*1000*1000, G=1024*1024*1024, and so on for T, P, E, Z, Y, R, Q.\n\
Binary prefixes can be used, too: KiB=K, MiB=M, and so on.\n\
If N ends in 'B', it counts bytes not blocks.\n\
\n\
Each CONV symbol may be:\n\
\n\
  ascii     from EBCDIC to ASCII\n\
  ebcdic    from ASCII to EBCDIC\n\
  ibm       from ASCII to alternate EBCDIC\n\
  block     pad newline-terminated records with spaces to cbs-size\n\
  unblock   replace trailing spaces in cbs-size records with newline\n\
  lcase     change upper case to lower case\n\
  ucase     change lower case to upper case\n\
  sparse    try to seek rather than write the output for NUL input blocks\n\
  autotune  dynamically benchmark and select optimal block size while copying\n\
  sha256    compute streaming SHA-256 digest on-the-fly while copying\n\
  force     override safety guard protection when writing to system devices\n\
  swab      swap every pair of input bytes\n\
  sync      pad every input block with NULs to ibs-size; when used\n\
            with block or unblock, pad with spaces rather than NULs\n\
  excl      fail if the output file already exists\n\
  nocreat   do not create the output file\n\
  notrunc   do not truncate the output file\n\
  noerror   continue after read errors\n\
  fdatasync physically write output file data before finishing\n\
  fsync     likewise, but also write metadata\n\
\n\
Each FLAG symbol may be:\n\
\n\
  append    append mode (makes sense only for output; conv=notrunc suggested)\n\
  direct    use direct I/O for data\n\
  directory fail unless a directory\n\
  dsync     use synchronized I/O for data\n\
  sync      likewise, but also for metadata\n\
  nonblock  use non-blocking I/O\n\
  noatime   do not update access time\n\
  nocache   Request to discard cache.  See also oflag=sync\n\
  noctty    do not assign controlling terminal from file\n\
  nofollow  do not follow symlinks\n\
  count_bytes  treat 'count=N' as a byte count (iflag only)\n\
  skip_bytes   treat 'skip=N' as a byte count (iflag only)\n\
  seek_bytes   treat 'seek=N' as a byte count (oflag only)\n\
  force        override safety guard check against overwriting mounted system roots\n\
\n\
Sending a USR1 signal to a running 'dd' process makes it\n\
print I/O statistics to standard error and then resume copying.\n\
\n\
"), stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}

int
main (int argc, char **argv)
{
  static dd_context_t ctx;

  dd_install_signal_handlers ();

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (maybe_close_stdout);

  ctx.page_size = getpagesize ();

  parse_gnu_standard_options_only (argc, argv, PROGRAM_NAME, PACKAGE, Version,
                                   true, usage, AUTHORS,
                                   (char const *) nullptr);
  close_stdout_required = false;

  dd_init_translations (ctx.trans_table);
  dd_init_default_config (&ctx.cfg);
  ctx.newline_character = '\n';
  ctx.space_character = ' ';
  ctx.pending_spaces = 0;

  bool use_fullblock = false;
  dd_scanargs (argc, argv, &ctx.cfg, &ctx.warn_partial_read, &use_fullblock);
  if (use_fullblock)
    ctx.iread_fnc = dd_iread_fullblock;

  dd_apply_translations (ctx.trans_table,
                         ctx.cfg.conversions_mask,
                         &ctx.newline_character,
                         &ctx.space_character,
                         &ctx.translation_needed,
                         &ctx.trans_mode);

  int exit_status = dd_execute (&ctx);

  dd_context_free (&ctx);

  return exit_status;
}
