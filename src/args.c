/**
 * @file args.c
 * @brief Modern POSIX/GNU CLI argument parser for blkcp.
 *
 * Implements standard getopt_long options (-i, -o, -b, -e, -l, -p, -q, -f,
 * --direct, --autotune, --hash, --sparse, --sync, etc.) and intuitive
 * positional operands (blkcp INPUT OUTPUT).
 */

#include <config.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "system.h"
#include "version.h"
#include "version-etc.h"
#include "error.h"
#include "quote.h"
#include "quotearg.h"
#include "xstrtol.h"

#define DEFAULT_BLOCKSIZE 512

extern void usage (int status);

enum
{
  OPT_AUTOTUNE = 1000,
  OPT_HASH,
  OPT_DIRECT,
  OPT_SPARSE,
  OPT_SYNC,
  OPT_SWAB,
  OPT_SKIP,
  OPT_SEEK,
  OPT_FDATASYNC,
  OPT_FSYNC,
  OPT_NOERROR,
  OPT_NOTRUNC,
  OPT_NOCACHE,
  OPT_JSON,
  OPT_QUEUE_DEPTH
};

static struct option const modern_long_options[] =
{
  {"input",        required_argument, NULL, 'i'},
  {"output",       required_argument, NULL, 'o'},
  {"block-size",   required_argument, NULL, 'b'},
  {"bs",           required_argument, NULL, 'b'},
  {"limit",        required_argument, NULL, 'l'},
  {"size",         required_argument, NULL, 'l'},
  {"bytes",        required_argument, NULL, 'l'},
  {"count",        required_argument, NULL, 'c'},
  {"engine",       required_argument, NULL, 'e'},
  {"progress",     no_argument,       NULL, 'p'},
  {"json",         no_argument,       NULL, OPT_JSON},
  {"queue-depth",  required_argument, NULL, OPT_QUEUE_DEPTH},
  {"async-queue",  required_argument, NULL, OPT_QUEUE_DEPTH},
  {"quiet",        no_argument,       NULL, 'q'},
  {"force",        no_argument,       NULL, 'f'},
  {"dry-run",      no_argument,       NULL, 'n'},
  {"autotune",     no_argument,       NULL, OPT_AUTOTUNE},
  {"hash",         no_argument,       NULL, OPT_HASH},
  {"sha256",       no_argument,       NULL, OPT_HASH},
  {"direct",       no_argument,       NULL, OPT_DIRECT},
  {"nocache",      no_argument,       NULL, OPT_NOCACHE},
  {"sparse",       no_argument,       NULL, OPT_SPARSE},
  {"sync",         no_argument,       NULL, OPT_SYNC},
  {"swab",         no_argument,       NULL, OPT_SWAB},
  {"skip",         required_argument, NULL, OPT_SKIP},
  {"seek",         required_argument, NULL, OPT_SEEK},
  {"fdatasync",    no_argument,       NULL, OPT_FDATASYNC},
  {"fsync",        no_argument,       NULL, OPT_FSYNC},
  {"noerror",      no_argument,       NULL, OPT_NOERROR},
  {"notrunc",      no_argument,       NULL, OPT_NOTRUNC},
  {"help",         no_argument,       NULL, 'h'},
  {"version",      no_argument,       NULL, 'v'},
  {NULL, 0, NULL, 0}
};

static intmax_t
parse_integer (char const *str, strtol_error *invalid)
{
  int indeterminate = 0;
  uintmax_t n = indeterminate;
  char *suffix;
  static char const suffixes[] = "EGkKMPQRTwYZ0";
  strtol_error e = xstrtoumax (str, &suffix, 10, &n, suffixes);
  intmax_t result;

  if ((e & ~LONGINT_OVERFLOW) == LONGINT_INVALID_SUFFIX_CHAR
      && *suffix == 'B' && str < suffix && suffix[-1] != 'B')
    {
      suffix++;
      if (!*suffix)
        e &= ~LONGINT_INVALID_SUFFIX_CHAR;
    }

  if (n <= INTMAX_MAX)
    result = (intmax_t) n;
  else
    {
      e = LONGINT_OVERFLOW;
      result = INTMAX_MAX;
    }

  *invalid = e;
  return result;
}

void
dd_init_default_config (dd_config_t *cfg)
{
  memset (cfg, 0, sizeof *cfg);
  cfg->input_blocksize = 0;
  cfg->output_blocksize = 0;
  cfg->skip_records = 0;
  cfg->skip_bytes = 0;
  cfg->seek_records = 0;
  cfg->seek_bytes = 0;
  cfg->max_records = INTMAX_MAX;
  cfg->max_bytes = 0;
  cfg->bytes_to_copy = -1;
  cfg->status_level = STATUS_DEFAULT;
}

void
dd_scanargs (int argc, char *const *argv, dd_config_t *cfg, bool *warn_partial_read, bool *use_fullblock)
{
  idx_t blocksize = 0;
  intmax_t count = INTMAX_MAX;
  intmax_t skip = 0;
  intmax_t seek = 0;

  optind = 1;
  int c;
  while ((c = getopt_long (argc, (char **) argv, "i:o:b:e:l:s:c:pqfnhv", modern_long_options, NULL)) != -1)
    {
      switch (c)
        {
        case 'i':
          cfg->input_file = optarg;
          break;
        case 'o':
          cfg->output_file = optarg;
          break;
        case 'b':
          if (strcmp (optarg, "auto") == 0 || strcmp (optarg, "autotune") == 0)
            {
              cfg->conversions_mask |= C_AUTOTUNE;
            }
          else
            {
              strtol_error invalid = LONGINT_OK;
              blocksize = (idx_t) parse_integer (optarg, &invalid);
              if (invalid != LONGINT_OK || blocksize <= 0)
                error (EXIT_FAILURE, invalid == LONGINT_OVERFLOW ? EOVERFLOW : 0,
                       "%s: %s", _("invalid block size"), quoteaf (optarg));
            }
          break;
        case 'e':
          if (strcmp (optarg, "uring") == 0 || strcmp (optarg, "io_uring") == 0)
            {
              cfg->engine = ENGINE_URING;
              cfg->conversions_mask |= C_URING;
            }
          else if (strcmp (optarg, "async") == 0 || strcmp (optarg, "pipeline") == 0)
            {
              cfg->engine = ENGINE_ASYNC;
              cfg->conversions_mask |= C_ASYNC;
            }
          else if (strcmp (optarg, "reflink") == 0 || strcmp (optarg, "cfr") == 0
                   || strcmp (optarg, "zero-copy") == 0)
            {
              cfg->engine = ENGINE_REFLINK;
              cfg->conversions_mask |= C_REFLINK;
            }
          else if (strcmp (optarg, "splice") == 0 || strcmp (optarg, "pipe") == 0)
            {
              cfg->engine = ENGINE_SPLICE;
              cfg->conversions_mask |= C_SPLICE;
            }
          else if (strcmp (optarg, "sync") == 0)
            {
              cfg->engine = ENGINE_SYNC;
            }
          else if (strcmp (optarg, "auto") == 0)
            {
              cfg->engine = ENGINE_AUTO;
            }
          else
            {
              error (EXIT_FAILURE, 0, _("unrecognized engine: %s (valid: sync, async, reflink, uring, splice, auto)"), quoteaf (optarg));
            }
          break;
        case 'l':
        case 's':
          {
            strtol_error invalid = LONGINT_OK;
            cfg->bytes_to_copy = parse_integer (optarg, &invalid);
            if (invalid != LONGINT_OK || cfg->bytes_to_copy < 0)
              error (EXIT_FAILURE, invalid == LONGINT_OVERFLOW ? EOVERFLOW : 0,
                     "%s: %s", _("invalid byte limit"), quoteaf (optarg));
            cfg->input_flags |= O_COUNT_BYTES;
          }
          break;
        case 'c':
          {
            strtol_error invalid = LONGINT_OK;
            count = parse_integer (optarg, &invalid);
            if (invalid != LONGINT_OK || count < 0)
              error (EXIT_FAILURE, invalid == LONGINT_OVERFLOW ? EOVERFLOW : 0,
                     "%s: %s", _("invalid count"), quoteaf (optarg));
          }
          break;
        case 'p':
          cfg->status_level = STATUS_PROGRESS;
          break;
        case OPT_JSON:
          cfg->status_level = STATUS_JSON;
          cfg->json_output = true;
          break;
        case OPT_QUEUE_DEPTH:
          {
            strtol_error invalid = LONGINT_OK;
            intmax_t qd = parse_integer (optarg, &invalid);
            if (invalid != LONGINT_OK || qd < 2 || qd > 1024)
              error (EXIT_FAILURE, invalid == LONGINT_OVERFLOW ? EOVERFLOW : 0,
                     "%s: %s (must be between 2 and 1024)",
                     _("invalid queue depth"), quoteaf (optarg));
            cfg->async_queue_depth = (size_t) qd;
          }
          break;
        case 'q':
          cfg->status_level = STATUS_NONE;
          break;
        case 'f':
          cfg->output_flags |= O_FORCE;
          break;
        case 'n':
          cfg->dry_run = true;
          break;
        case OPT_AUTOTUNE:
          cfg->conversions_mask |= C_AUTOTUNE;
          break;
        case OPT_HASH:
          cfg->conversions_mask |= C_SHA256;
          break;
        case OPT_DIRECT:
          cfg->input_flags |= O_DIRECT;
          cfg->output_flags |= O_DIRECT;
          break;
        case OPT_NOCACHE:
          cfg->i_nocache = true;
          cfg->o_nocache = true;
          break;
        case OPT_SPARSE:
          cfg->conversions_mask |= C_SPARSE;
          break;
        case OPT_SYNC:
          cfg->conversions_mask |= C_SYNC;
          break;
        case OPT_SWAB:
          cfg->conversions_mask |= C_SWAB;
          break;
        case OPT_SKIP:
          {
            strtol_error invalid = LONGINT_OK;
            skip = parse_integer (optarg, &invalid);
            if (invalid != LONGINT_OK || skip < 0)
              error (EXIT_FAILURE, invalid == LONGINT_OVERFLOW ? EOVERFLOW : 0,
                     "%s: %s", _("invalid skip offset"), quoteaf (optarg));
          }
          break;
        case OPT_SEEK:
          {
            strtol_error invalid = LONGINT_OK;
            seek = parse_integer (optarg, &invalid);
            if (invalid != LONGINT_OK || seek < 0)
              error (EXIT_FAILURE, invalid == LONGINT_OVERFLOW ? EOVERFLOW : 0,
                     "%s: %s", _("invalid seek offset"), quoteaf (optarg));
          }
          break;
        case OPT_FDATASYNC:
          cfg->conversions_mask |= C_FDATASYNC;
          break;
        case OPT_FSYNC:
          cfg->conversions_mask |= C_FSYNC;
          break;
        case OPT_NOERROR:
          cfg->conversions_mask |= C_NOERROR;
          break;
        case OPT_NOTRUNC:
          cfg->conversions_mask |= C_NOTRUNC;
          break;
        case 'h':
          usage (EXIT_SUCCESS);
          break;
        case 'v':
          version_etc (stdout, "blkcp", PACKAGE_NAME, Version, "Meik", (char *) NULL);
          exit (EXIT_SUCCESS);
          break;
        default:
          usage (EXIT_FAILURE);
        }
    }

  /* Unambiguous positional arguments: blkcp [INPUT] [OUTPUT] */
  for (int i = optind; i < argc; i++)
    {
      if (cfg->input_file == NULL)
        cfg->input_file = argv[i];
      else if (cfg->output_file == NULL)
        cfg->output_file = argv[i];
      else
        {
          error (0, 0, _("extra operand %s"), quoteaf (argv[i]));
          usage (EXIT_FAILURE);
        }
    }

  /* Assign final resolved block size */
  if (blocksize <= 0)
    blocksize = DEFAULT_BLOCKSIZE;

  cfg->input_blocksize = blocksize;
  cfg->output_blocksize = blocksize;

  /* Apply byte offsets */
  if (skip > 0)
    cfg->skip_bytes = (idx_t) skip;
  if (seek > 0)
    cfg->seek_bytes = seek;

  /* Apply block count if specified */
  if (count != INTMAX_MAX)
    {
      cfg->max_records = count;
      if (cfg->bytes_to_copy < 0)
        {
          intmax_t total_bytes;
          if (!ckd_mul (&total_bytes, count, blocksize))
            {
              cfg->bytes_to_copy = total_bytes;
              cfg->input_flags |= O_COUNT_BYTES;
            }
        }
    }

  *warn_partial_read = false;
  *use_fullblock = false;
}
