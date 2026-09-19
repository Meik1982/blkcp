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

#ifndef O_CIO
# define O_CIO 0
#endif

#define LONGEST_SYMBOL "count_bytes"

struct symbol_value
{
  char symbol[sizeof LONGEST_SYMBOL];
  int value;
};

static struct symbol_value const conversions[] =
{
  {"ascii", C_ASCII | C_UNBLOCK | C_TWOBUFS},
  {"ebcdic", C_EBCDIC | C_BLOCK | C_TWOBUFS},
  {"ibm", C_IBM | C_BLOCK | C_TWOBUFS},
  {"block", C_BLOCK | C_TWOBUFS},
  {"unblock", C_UNBLOCK | C_TWOBUFS},
  {"lcase", C_LCASE | C_TWOBUFS},
  {"ucase", C_UCASE | C_TWOBUFS},
  {"sparse", C_SPARSE},
  {"autotune", C_AUTOTUNE},
  {"swab", C_SWAB | C_TWOBUFS},
  {"noerror", C_NOERROR},
  {"nocreat", C_NOCREAT},
  {"excl", C_EXCL},
  {"notrunc", C_NOTRUNC},
  {"sync", C_SYNC},
  {"fdatasync", C_FDATASYNC},
  {"fsync", C_FSYNC},
  {"force", C_FORCE},
  {"sha256", C_SHA256},
  {"hash", C_SHA256},
  {"async", C_ASYNC},
  {"reflink", C_REFLINK},
  {"cfr", C_REFLINK},
  {"zero-copy", C_REFLINK},
  {"zerocopy", C_REFLINK},
  {"uring", C_URING},
  {"io_uring", C_URING},
  {"", 0}
};

static struct symbol_value const flags[] =
{
  {"cio", O_CIO},
  {"direct", O_DIRECT},
  {"directory", O_DIRECTORY},
  {"dsync", O_DSYNC},
  {"sync", O_SYNC},
  {"nocache", O_NOCACHE},
  {"nonblock", O_NONBLOCK},
  {"noatime", O_NOATIME},
  {"noctty", O_NOCTTY},
  {"nofollow", O_NOFOLLOW},
  {"nolinks", O_NOLINKS},
  {"binary", O_BINARY},
  {"text", O_TEXT},
  {"fullblock", O_FULLBLOCK},
  {"count_bytes", O_COUNT_BYTES},
  {"skip_bytes", O_SKIP_BYTES},
  {"seek_bytes", O_SEEK_BYTES},
  {"force", O_FORCE},
  {"async", O_ASYNC_PIPELINE},
  {"", 0}
};

static struct symbol_value const statuses[] =
{
  {"none", STATUS_NONE},
  {"noxfer", STATUS_NOXFER},
  {"progress", STATUS_PROGRESS},
  {"", 0}
};

extern void usage (int status);

ATTRIBUTE_PURE
static bool
operand_matches (char const *str, char const *pattern, char delim)
{
  while (*pattern)
    if (*str++ != *pattern++)
      return false;
  return !*str || *str == delim;
}

ATTRIBUTE_PURE
static bool
operand_is (char const *operand, char const *name)
{
  return operand_matches (operand, name, '=');
}

static inline bool
multiple_bits_set (int i)
{
  return MULTIPLE_BITS_SET (i);
}

static int
parse_symbols (char const *str, struct symbol_value const *table,
               bool exclusive, char const *error_msgid)
{
  int value = 0;

  while (true)
    {
      char const *strcomma = strchr (str, ',');
      struct symbol_value const *entry;

      for (entry = table;
           ! (operand_matches (str, entry->symbol, ',') && entry->value);
           entry++)
        {
          if (! entry->symbol[0])
            {
              idx_t slen = strcomma ? (idx_t)(strcomma - str) : (idx_t)strlen (str);
              error (0, 0, "%s: %s", _(error_msgid),
                     quotearg_n_style_mem (0, locale_quoting_style,
                                           str, slen));
              usage (EXIT_FAILURE);
            }
        }

      if (exclusive)
        value = entry->value;
      else
        value |= entry->value;

      if (!strcomma)
        break;
      str = strcomma + 1;
    }

  return value;
}

static intmax_t
parse_integer (char const *str, strtol_error *invalid)
{
  int indeterminate = 0;
  uintmax_t n = indeterminate;
  char *suffix;
  static char const suffixes[] = "bcEGkKMPQRTwYZ0";
  strtol_error e = xstrtoumax (str, &suffix, 10, &n, suffixes);
  intmax_t result;

  if ((e & ~LONGINT_OVERFLOW) == LONGINT_INVALID_SUFFIX_CHAR
      && *suffix == 'B' && str < suffix && suffix[-1] != 'B')
    {
      suffix++;
      if (!*suffix)
        e &= ~LONGINT_INVALID_SUFFIX_CHAR;
    }

  if ((e & ~LONGINT_OVERFLOW) == LONGINT_INVALID_SUFFIX_CHAR
      && *suffix == 'x')
    {
      strtol_error f = LONGINT_OK;
      intmax_t o = parse_integer (suffix + 1, &f);
      if ((f & ~LONGINT_OVERFLOW) != LONGINT_OK)
        {
          e = f;
          result = indeterminate;
        }
      else if (ckd_mul (&result, n, o)
               || (result != 0 && ((e | f) & LONGINT_OVERFLOW)))
        {
          e = LONGINT_OVERFLOW;
          result = INTMAX_MAX;
        }
      else
        {
          if (result == 0 && STRPREFIX (str, "0x"))
            error (0, 0, _("warning: %s is a zero multiplier; "
                           "use %s if that is intended"),
                   quote_n (0, "0x"), quote_n (1, "00x"));
          e = LONGINT_OK;
        }
    }
  else if (n <= INTMAX_MAX)
    result = n;
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
  cfg->conversion_blocksize = 0;
  cfg->skip_records = 0;
  cfg->skip_bytes = 0;
  cfg->seek_records = 0;
  cfg->seek_bytes = 0;
  cfg->max_records = INTMAX_MAX;
  cfg->max_bytes = 0;
  cfg->bytes_to_copy = -1;
  cfg->status_level = STATUS_DEFAULT;
}

/* Helper to parse and assign numeric CLI operands (ibs, obs, bs, cbs, skip, seek, count) */
static void
parse_numeric_operand (char const *name, char const *val, dd_config_t *cfg,
                       idx_t *blocksize, intmax_t *count, bool *count_B,
                       intmax_t *skip, bool *skip_B, intmax_t *seek, bool *seek_B)
{
  strtol_error invalid = LONGINT_OK;
  intmax_t n = parse_integer (val, &invalid);
  bool has_B = !!strchr (val, 'B');
  intmax_t n_min = 0;
  intmax_t n_max = INTMAX_MAX;
  idx_t *converted_idx = nullptr;

  idx_t max_blocksize = MIN (IDX_MAX - 1, MIN (SSIZE_MAX, OFF_T_MAX));

  if (operand_is (name, "ibs"))
    {
      n_min = 1;
      n_max = max_blocksize;
      converted_idx = &cfg->input_blocksize;
    }
  else if (operand_is (name, "obs"))
    {
      n_min = 1;
      n_max = max_blocksize;
      converted_idx = &cfg->output_blocksize;
    }
  else if (operand_is (name, "bs"))
    {
      n_min = 1;
      n_max = max_blocksize;
      converted_idx = blocksize;
    }
  else if (operand_is (name, "cbs"))
    {
      n_min = 1;
      n_max = MIN (SIZE_MAX, IDX_MAX);
      converted_idx = &cfg->conversion_blocksize;
    }
  else if (operand_is (name, "skip") || operand_is (name, "iseek"))
    {
      *skip = n;
      *skip_B = has_B;
    }
  else if (operand_is (name + (*name == 'o'), "seek"))
    {
      *seek = n;
      *seek_B = has_B;
    }
  else if (operand_is (name, "count"))
    {
      *count = n;
      *count_B = has_B;
    }
  else if (operand_is (name, "bytes") || operand_is (name, "tocopy") || operand_is (name, "tc"))
    {
      cfg->bytes_to_copy = n;
    }
  else
    {
      error (0, 0, _("unrecognized operand %s"), quoteaf (name));
      usage (EXIT_FAILURE);
    }

  if (n < n_min)
    invalid = LONGINT_INVALID;
  else if (n_max < n)
    invalid = LONGINT_OVERFLOW;

  if (invalid != LONGINT_OK)
    error (EXIT_FAILURE, invalid == LONGINT_OVERFLOW ? EOVERFLOW : 0,
           "%s: %s", _("invalid number"), quoteaf (val));
  else if (converted_idx)
    *converted_idx = n;
}

enum
{
  OPT_AUTOTUNE = 1000,
  OPT_HASH,
  OPT_DIRECT,
  OPT_SPARSE,
  OPT_SYNC,
  OPT_SKIP,
  OPT_SEEK,
  OPT_FDATASYNC,
  OPT_FSYNC,
  OPT_NOERROR,
  OPT_NOTRUNC
};

static struct option const modern_long_options[] =
{
  {"input", required_argument, NULL, 'i'},
  {"output", required_argument, NULL, 'o'},
  {"block-size", required_argument, NULL, 'b'},
  {"bs", required_argument, NULL, 'b'},
  {"limit", required_argument, NULL, 'l'},
  {"size", required_argument, NULL, 'l'},
  {"bytes", required_argument, NULL, 'l'},
  {"count", required_argument, NULL, 'c'},
  {"engine", required_argument, NULL, 'e'},
  {"progress", no_argument, NULL, 'p'},
  {"quiet", no_argument, NULL, 'q'},
  {"force", no_argument, NULL, 'f'},
  {"autotune", no_argument, NULL, OPT_AUTOTUNE},
  {"hash", no_argument, NULL, OPT_HASH},
  {"sha256", no_argument, NULL, OPT_HASH},
  {"direct", no_argument, NULL, OPT_DIRECT},
  {"sparse", no_argument, NULL, OPT_SPARSE},
  {"sync", no_argument, NULL, OPT_SYNC},
  {"skip", required_argument, NULL, OPT_SKIP},
  {"seek", required_argument, NULL, OPT_SEEK},
  {"fdatasync", no_argument, NULL, OPT_FDATASYNC},
  {"fsync", no_argument, NULL, OPT_FSYNC},
  {"noerror", no_argument, NULL, OPT_NOERROR},
  {"notrunc", no_argument, NULL, OPT_NOTRUNC},
  {"help", no_argument, NULL, 'h'},
  {"version", no_argument, NULL, 'v'},
  {NULL, 0, NULL, 0}
};

void
dd_scanargs (int argc, char *const *argv, dd_config_t *cfg, bool *warn_partial_read, bool *use_fullblock)
{
  idx_t blocksize = 0;
  intmax_t count = INTMAX_MAX;
  intmax_t skip = 0;
  intmax_t seek = 0;
  bool count_B = false, skip_B = false, seek_B = false;

  optind = 1;
  int c;
  while ((c = getopt_long (argc, (char **) argv, "i:o:b:e:l:s:c:pqfhv", modern_long_options, NULL)) != -1)
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
          if (operand_matches (optarg, "auto", 0) || operand_matches (optarg, "autotune", 0))
            {
              cfg->conversions_mask |= C_AUTOTUNE;
            }
          else
            {
              strtol_error invalid = LONGINT_OK;
              blocksize = parse_integer (optarg, &invalid);
              if (invalid != LONGINT_OK || blocksize <= 0)
                error (EXIT_FAILURE, invalid == LONGINT_OVERFLOW ? EOVERFLOW : 0,
                       "%s: %s", _("invalid block size"), quoteaf (optarg));
            }
          break;
        case 'e':
          if (operand_matches (optarg, "uring", 0) || operand_matches (optarg, "io_uring", 0))
            {
              cfg->engine = ENGINE_URING;
              cfg->conversions_mask |= C_URING;
            }
          else if (operand_matches (optarg, "async", 0) || operand_matches (optarg, "pipeline", 0))
            {
              cfg->engine = ENGINE_ASYNC;
              cfg->conversions_mask |= C_ASYNC;
            }
          else if (operand_matches (optarg, "reflink", 0) || operand_matches (optarg, "cfr", 0)
                   || operand_matches (optarg, "zero-copy", 0))
            {
              cfg->engine = ENGINE_REFLINK;
              cfg->conversions_mask |= C_REFLINK;
            }
          else if (operand_matches (optarg, "sync", 0))
            {
              cfg->engine = ENGINE_SYNC;
            }
          else if (operand_matches (optarg, "auto", 0))
            {
              cfg->engine = ENGINE_AUTO;
            }
          else
            {
              error (EXIT_FAILURE, 0, _("unrecognized engine: %s (valid: sync, async, reflink, uring, auto)"), quoteaf (optarg));
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
            count_B = !!strchr (optarg, 'B');
            if (invalid != LONGINT_OK || count < 0)
              error (EXIT_FAILURE, invalid == LONGINT_OVERFLOW ? EOVERFLOW : 0,
                     "%s: %s", _("invalid count"), quoteaf (optarg));
          }
          break;
        case 'p':
          cfg->status_level = STATUS_PROGRESS;
          break;
        case 'q':
          cfg->status_level = STATUS_NONE;
          break;
        case 'f':
          cfg->output_flags |= O_FORCE;
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
        case OPT_SPARSE:
          cfg->conversions_mask |= C_SPARSE;
          break;
        case OPT_SYNC:
          cfg->conversions_mask |= C_SYNC;
          break;
        case OPT_SKIP:
          {
            strtol_error invalid = LONGINT_OK;
            skip = parse_integer (optarg, &invalid);
            skip_B = true;
            if (invalid != LONGINT_OK || skip < 0)
              error (EXIT_FAILURE, invalid == LONGINT_OVERFLOW ? EOVERFLOW : 0,
                     "%s: %s", _("invalid skip offset"), quoteaf (optarg));
          }
          break;
        case OPT_SEEK:
          {
            strtol_error invalid = LONGINT_OK;
            seek = parse_integer (optarg, &invalid);
            seek_B = true;
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

  for (int i = optind; i < argc; i++)
    {
      char const *name = argv[i];
      char const *val = strchr (name, '=');

      if (val == nullptr)
        {
          /* Positional arguments: blkcp [INPUT] [OUTPUT] */
          if (cfg->input_file == NULL)
            cfg->input_file = name;
          else if (cfg->output_file == NULL)
            cfg->output_file = name;
          else
            {
              error (0, 0, _("unrecognized operand %s"), quoteaf (name));
              usage (EXIT_FAILURE);
            }
          continue;
        }
      val++;

      if (operand_is (name, "if"))
        cfg->input_file = val;
      else if (operand_is (name, "of"))
        cfg->output_file = val;
      else if (operand_is (name, "conv"))
        cfg->conversions_mask |= parse_symbols (val, conversions, false,
                                               N_("invalid conversion"));
      else if (operand_is (name, "iflag"))
        cfg->input_flags |= parse_symbols (val, flags, false,
                                          N_("invalid input flag"));
      else if (operand_is (name, "oflag"))
        cfg->output_flags |= parse_symbols (val, flags, false,
                                           N_("invalid output flag"));
      else if (operand_is (name, "status"))
        cfg->status_level = parse_symbols (val, statuses, true,
                                          N_("invalid status level"));
      else if (operand_is (name, "opt"))
        {
          char *opts = xstrdup (val);
          char *saveptr = NULL;
          for (char *tok = strtok_r (opts, ",", &saveptr); tok; tok = strtok_r (NULL, ",", &saveptr))
            {
              if (operand_matches (tok, "auto", 0) || operand_matches (tok, "autotune", 0))
                cfg->conversions_mask |= C_AUTOTUNE;
              else if (operand_matches (tok, "force", 0))
                cfg->output_flags |= O_FORCE;
              else if (operand_matches (tok, "sha256", 0) || operand_matches (tok, "hash", 0))
                cfg->conversions_mask |= C_SHA256;
              else if (operand_matches (tok, "async", 0) || operand_matches (tok, "pipeline", 0))
                cfg->conversions_mask |= C_ASYNC;
              else if (operand_matches (tok, "reflink", 0) || operand_matches (tok, "cfr", 0)
                       || operand_matches (tok, "zero-copy", 0) || operand_matches (tok, "zerocopy", 0))
                cfg->conversions_mask |= C_REFLINK;
              else if (operand_matches (tok, "uring", 0) || operand_matches (tok, "io_uring", 0)
                       || operand_matches (tok, "io-uring", 0))
                {
                  cfg->conversions_mask |= C_URING;
                  cfg->engine = ENGINE_URING;
                }
              else
                {
                  error (0, 0, _("unrecognized option in opt: %s"), quoteaf (tok));
                  free (opts);
                  usage (EXIT_FAILURE);
                }
            }
          free (opts);
        }
      else if (operand_is (name, "bs") && (operand_matches (val, "auto", 0) || operand_matches (val, "autotune", 0)))
        {
          cfg->conversions_mask |= C_AUTOTUNE;
        }
      else
        {
          parse_numeric_operand (name, val, cfg, &blocksize, &count, &count_B,
                                 &skip, &skip_B, &seek, &seek_B);
        }
    }

  if (blocksize)
    cfg->input_blocksize = cfg->output_blocksize = blocksize;
  else
    cfg->conversions_mask |= C_TWOBUFS;

  if (cfg->input_blocksize == 0)
    cfg->input_blocksize = DEFAULT_BLOCKSIZE;
  if (cfg->output_blocksize == 0)
    cfg->output_blocksize = DEFAULT_BLOCKSIZE;
  if (cfg->conversion_blocksize == 0)
    cfg->conversions_mask &= ~(C_BLOCK | C_UNBLOCK);

  if (cfg->input_flags & (O_DSYNC | O_SYNC))
    cfg->input_flags |= O_RSYNC;

  if (cfg->output_flags & O_FULLBLOCK)
    {
      error (0, 0, "%s: %s", _("invalid output flag"), quote ("fullblock"));
      usage (EXIT_FAILURE);
    }

  if (skip_B)
    cfg->input_flags |= O_SKIP_BYTES;
  if (cfg->input_flags & O_SKIP_BYTES && skip != 0)
    {
      cfg->skip_records = skip / cfg->input_blocksize;
      cfg->skip_bytes = skip % cfg->input_blocksize;
    }
  else if (skip != 0)
    cfg->skip_records = skip;

  if (cfg->bytes_to_copy >= 0)
    {
      cfg->input_flags |= O_COUNT_BYTES;
      cfg->max_records = cfg->bytes_to_copy / cfg->input_blocksize;
      cfg->max_bytes = cfg->bytes_to_copy % cfg->input_blocksize;
    }
  else if (count_B)
    cfg->input_flags |= O_COUNT_BYTES;
  if (!(cfg->bytes_to_copy >= 0) && (cfg->input_flags & O_COUNT_BYTES) && count != INTMAX_MAX)
    {
      cfg->max_records = count / cfg->input_blocksize;
      cfg->max_bytes = count % cfg->input_blocksize;
    }
  else if (!(cfg->bytes_to_copy >= 0) && count != INTMAX_MAX)
    cfg->max_records = count;

  if (seek_B)
    cfg->output_flags |= O_SEEK_BYTES;
  if (cfg->output_flags & O_SEEK_BYTES && seek != 0)
    {
      cfg->seek_records = seek / cfg->output_blocksize;
      cfg->seek_bytes = seek % cfg->output_blocksize;
    }
  else if (seek != 0)
    cfg->seek_records = seek;

  if (warn_partial_read)
    *warn_partial_read =
      (! (cfg->conversions_mask & C_TWOBUFS) && ! (cfg->input_flags & O_FULLBLOCK)
       && (cfg->skip_records
           || (0 < cfg->max_records && cfg->max_records < INTMAX_MAX)
           || (cfg->input_flags | cfg->output_flags) & O_DIRECT));

  if (use_fullblock)
    *use_fullblock = !!(cfg->input_flags & O_FULLBLOCK);
  cfg->input_flags &= ~O_FULLBLOCK;

  if (multiple_bits_set (cfg->conversions_mask & (C_ASCII | C_EBCDIC | C_IBM)))
    error (EXIT_FAILURE, 0, _("cannot combine any two of {ascii,ebcdic,ibm}"));
  if (multiple_bits_set (cfg->conversions_mask & (C_BLOCK | C_UNBLOCK)))
    error (EXIT_FAILURE, 0, _("cannot combine block and unblock"));
  if (multiple_bits_set (cfg->conversions_mask & (C_LCASE | C_UCASE)))
    error (EXIT_FAILURE, 0, _("cannot combine lcase and ucase"));
  if (multiple_bits_set (cfg->conversions_mask & (C_EXCL | C_NOCREAT)))
    error (EXIT_FAILURE, 0, _("cannot combine excl and nocreat"));
  if (multiple_bits_set (cfg->input_flags & (O_DIRECT | O_NOCACHE))
      || multiple_bits_set (cfg->output_flags & (O_DIRECT | O_NOCACHE)))
    error (EXIT_FAILURE, 0, _("cannot combine direct and nocache"));

  if (cfg->input_flags & O_NOCACHE)
    {
      cfg->i_nocache = true;
      cfg->i_nocache_eof = (cfg->max_records == 0 && cfg->max_bytes == 0);
      cfg->input_flags &= ~O_NOCACHE;
    }
  if (cfg->output_flags & O_NOCACHE)
    {
      cfg->o_nocache = true;
      cfg->o_nocache_eof = (cfg->max_records == 0 && cfg->max_bytes == 0);
      cfg->output_flags &= ~O_NOCACHE;
    }
}
