#ifndef DD_CONFIG_H
#define DD_CONFIG_H

#include <config.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include "system.h"
#include "idx.h"
#include "xtime.h"
#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status levels */
enum dd_status_level
{
  STATUS_NONE = 1,
  STATUS_NOXFER = 2,
  STATUS_DEFAULT = 3,
  STATUS_PROGRESS = 4
};

/* Conversion flags */
enum dd_conversions
{
  C_ASCII = 000001,
  C_EBCDIC = 000002,
  C_IBM = 000004,
  C_BLOCK = 000010,
  C_UNBLOCK = 000020,
  C_LCASE = 000040,
  C_UCASE = 000100,
  C_SWAB = 000200,
  C_NOERROR = 000400,
  C_NOTRUNC = 001000,
  C_SYNC = 002000,
  C_TWOBUFS = 004000,
  C_NOCREAT = 010000,
  C_EXCL = 020000,
  C_FDATASYNC = 040000,
  C_FSYNC = 0100000,
  C_SPARSE = 0200000,
  C_AUTOTUNE = 0400000,
  C_FORCE = 01000000,
  C_SHA256 = 02000000
};

#define FFS_MASK(x) ((x) ^ ((x) & ((x) - 1)))
#define MULTIPLE_BITS_SET(i) (((i) & ((i) - 1)) != 0)

#ifndef O_CIO
# define O_CIO 0
#endif

enum dd_private_flags
  {
    v_mask = ~(0
          | O_APPEND
          | O_BINARY
          | O_CIO
          | O_DIRECT
          | O_DIRECTORY
          | O_DSYNC
          | O_NOATIME
          | O_NOCTTY
          | O_NOFOLLOW
          | O_NOLINKS
          | O_NONBLOCK
          | O_SYNC
          | O_TEXT
          ),

    O_FULLBLOCK = FFS_MASK (v_mask),
    v2_mask = v_mask ^ O_FULLBLOCK,

    O_NOCACHE = FFS_MASK (v2_mask),
    v3_mask = v2_mask ^ O_NOCACHE,

    O_COUNT_BYTES = FFS_MASK (v3_mask),
    v4_mask = v3_mask ^ O_COUNT_BYTES,

    O_SKIP_BYTES = FFS_MASK (v4_mask),
    v5_mask = v4_mask ^ O_SKIP_BYTES,

    O_SEEK_BYTES = FFS_MASK (v5_mask),
    v6_mask = v5_mask ^ O_SEEK_BYTES,

    O_FORCE = FFS_MASK (v6_mask)
  };

/* Configuration parsed from CLI operands */
typedef struct dd_config
{
  char const *input_file;
  char const *output_file;
  idx_t input_blocksize;
  idx_t output_blocksize;
  idx_t conversion_blocksize;
  intmax_t skip_records;
  idx_t skip_bytes;
  intmax_t seek_records;
  intmax_t seek_bytes;
  intmax_t max_records;
  idx_t max_bytes;
  int conversions_mask;
  int input_flags;
  int output_flags;
  int status_level;
  bool i_nocache;
  bool o_nocache;
  bool i_nocache_eof;
  bool o_nocache_eof;
} dd_config_t;

/* Transfer statistics & telemetry */
typedef struct dd_stats
{
  intmax_t r_full;
  intmax_t r_partial;
  intmax_t r_truncate;
  intmax_t w_full;
  intmax_t w_partial;
  intmax_t w_bytes;
  intmax_t reported_w_bytes;
  xtime_t start_time;
  xtime_t next_time;
  int progress_len;
} dd_stats_t;

enum dd_trans_mode
{
  TRANS_MODE_NONE = 0,
  TRANS_MODE_TABLE,
  TRANS_MODE_FAST_UCASE,
  TRANS_MODE_FAST_LCASE
};

/* Complete runtime context */
typedef struct dd_context
{
  dd_config_t cfg;
  dd_stats_t stats;

  /* Runtime buffer state */
  idx_t page_size;
  char *ibuf;
  char *obuf;
  idx_t oc;
  idx_t col;

  /* I/O descriptors and status */
  bool input_seekable;
  int input_seek_errno;
  off_t input_offset;
  bool final_op_was_seek;
  bool warn_partial_read;
  bool translation_needed;
  int trans_mode;
  char newline_character;
  char space_character;
  unsigned char trans_table[256];
  idx_t pending_spaces;

  /* Signal state */
  sig_atomic_t volatile interrupt_signal;
  sig_atomic_t volatile info_signal_count;

  /* Dynamic function pointers */
  ssize_t (*iread_fnc) (int fd, char *buf, idx_t size);

  /* On-the-fly checksumming state */
  struct sha256_ctx sha_ctx;
  unsigned char sha_digest[32];
  bool sha_computed;
} dd_context_t;

/* Global or thread-local active context pointer for signal handling */
extern dd_context_t *current_dd_ctx;

#ifdef __cplusplus
}
#endif

#endif /* DD_CONFIG_H */
