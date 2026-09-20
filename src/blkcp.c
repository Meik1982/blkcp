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

#include "blkcp_config.h"
#include "args.h"
#include "conversions.h"
#include "stats.h"
#include "signals.h"
#include "io_engine.h"

#define PROGRAM_NAME "blkcp"
#define AUTHORS \
  proper_name ("Meik"), \
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
Usage: %s [OPTIONS] [INPUT] [OUTPUT]\n\
   or: %s -i INPUT -o OUTPUT [OPTIONS]\n\
"),
              program_name, program_name);
      fputs (_("\
Next-Gen Block Copy Tool (blkcp) for fast, safe and modern stream/disk replication.\n\
\n\
Core Options:\n\
  -i, --input=FILE         Input file or block device (default: stdin)\n\
  -o, --output=FILE        Output file or block device (default: stdout)\n\
  -b, --block-size=SIZE    Block size for transfer (e.g. 64K, 4M, 1G);\n\
                           use '-b auto' or '--autotune' for dynamic I/O tuning\n\
  -e, --engine=NAME        Execution backend engine:\n\
                             'sync'     Classic synchronous block I/O\n\
                             'async'    Multi-threaded ringbuffer pipeline\n\
                             'reflink'  Linux Kernel Zero-Copy (copy_file_range)\n\
                             'uring'    Linux io_uring asynchronous execution\n\
                             'splice'   Linux Kernel Zero-Copy pipe splicing\n\
                             'auto'     Intelligent auto-detection (default)\n\
  -l, --limit=BYTES        Limit copy to exactly BYTES bytes (aliases: -s, --size)\n\
  -c, --count=N            Copy only N input blocks\n\
  -p, --progress           Show periodic real-time transfer telemetry and speed\n\
      --json               Emit machine-readable NDJSON telemetry on stderr\n\
      --queue-depth=N      Async ringbuffer queue depth (2..1024 slots; default: auto)\n\
  -q, --quiet              Suppress all output except fatal error messages\n\
  -f, --force              Override Target Safety Guard (e.g. write to mounted disks)\n\
      --hash, --sha256     Compute on-the-fly streaming SHA-256 checksum\n\
      --autotune           Enable dynamic in-flight throughput autotuning\n\
      --direct             Use direct I/O (O_DIRECT) bypassing kernel page cache\n\
      --skip=BYTES         Skip BYTES at input before copying\n\
      --seek=BYTES         Seek BYTES at output before writing\n\
      --sparse             Punch holes / create sparse file for blocks of zeros\n\
      --sync               Pad short reads with zero bytes\n\
      --notrunc            Do not truncate the output file\n\
      --noerror            Continue operation across read errors\n\
      --fdatasync          Flush output data to disk before completion\n\
      --fsync              Flush output data and metadata before completion\n\
\n\
Multiplicative Suffixes (SI / IEC):\n\
  c=1, w=2, b=512, kB=1000, K=1024, MB=1000*1000, M=1024*1024,\n\
  GB=1000^3, G=1024^3, T, P, E, Z, Y, KiB, MiB, GiB, TiB.\n\
\n\
Sending SIGUSR1 to a running blkcp process prints instantaneous transfer\n\
statistics to stderr and continues execution.\n\
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
