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

/**
 * @brief Configuration parsed from CLI operands (if=, of=, bs=, count=, conv=, etc.)
 */
typedef struct dd_config
{
  char const *input_file;         /**< Path to input file or device (NULL for stdin) */
  char const *output_file;        /**< Path to output file or device (NULL for stdout) */
  idx_t input_blocksize;          /**< Block size for read operations (ibs) */
  idx_t output_blocksize;         /**< Block size for write operations (obs) */
  idx_t conversion_blocksize;     /**< Record length for block/unblock conversion (cbs) */
  intmax_t skip_records;          /**< Input blocks to skip before copying */
  idx_t skip_bytes;               /**< Additional bytes to skip when iflag=skip_bytes */
  intmax_t seek_records;          /**< Output blocks to seek before writing */
  intmax_t seek_bytes;            /**< Additional bytes to seek when oflag=seek_bytes */
  intmax_t max_records;           /**< Max records to copy (from count=N) */
  idx_t max_bytes;                /**< Remaining bytes to copy when iflag=count_bytes */
  int conversions_mask;           /**< Bitmask of active conversions (enum dd_conversions) */
  int input_flags;                /**< Bitmask of input flags (O_DIRECT, O_NONBLOCK, etc.) */
  int output_flags;               /**< Bitmask of output flags (O_APPEND, O_FORCE, etc.) */
  int status_level;               /**< Telemetry verbosity (none, noxfer, progress, default) */
  bool i_nocache;                 /**< Discard input cache after every block read */
  bool o_nocache;                 /**< Discard output cache after every block write */
  bool i_nocache_eof;             /**< Discard entire input cache at EOF */
  bool o_nocache_eof;             /**< Discard entire output cache at completion */
} dd_config_t;

/**
 * @brief Transfer statistics, timing and progress telemetry
 */
typedef struct dd_stats
{
  intmax_t r_full;                /**< Complete input blocks read */
  intmax_t r_partial;             /**< Partial input blocks read */
  intmax_t r_truncate;            /**< Records truncated by cbs limit */
  intmax_t w_full;                /**< Complete output blocks written */
  intmax_t w_partial;             /**< Partial output blocks written */
  intmax_t w_bytes;               /**< Cumulative bytes written to output */
  intmax_t reported_w_bytes;      /**< Bytes reported in previous progress update */
  xtime_t start_time;             /**< Transfer start timestamp (nanoseconds via TSC) */
  xtime_t next_time;              /**< Next scheduled periodic progress report time */
  int progress_len;               /**< Character length of last printed progress line */
} dd_stats_t;

/**
 * @brief Translation execution modes for optimized character transformation
 */
enum dd_trans_mode
{
  TRANS_MODE_NONE = 0,            /**< No translation needed */
  TRANS_MODE_TABLE,               /**< Standard 256-byte lookup table translation */
  TRANS_MODE_FAST_UCASE,          /**< Branchless SIMD-vectorized ASCII upper-casing */
  TRANS_MODE_FAST_LCASE           /**< Branchless SIMD-vectorized ASCII lower-casing */
};

/**
 * @brief Complete reentrant runtime context for execution, state, and signals
 */
typedef struct dd_context
{
  dd_config_t cfg;                /**< Parsed command line options and switches */
  dd_stats_t stats;               /**< Transfer statistics and execution timing */

  /* Runtime buffer state */
  idx_t page_size;                /**< OS memory page size for buffer alignment */
  char *ibuf;                     /**< Input read buffer (aligned via alignalloc) */
  char *obuf;                     /**< Output write buffer (shares ibuf on fast path) */
  idx_t oc;                       /**< Current byte count accumulated in obuf */
  idx_t col;                      /**< Current column index for block/unblock conversion */

  /* I/O descriptors and status */
  bool input_seekable;            /**< True if input supports lseek(SEEK_CUR) */
  int input_seek_errno;           /**< Errno captured when initial lseek failed */
  off_t input_offset;             /**< Byte offset in input file */
  bool final_op_was_seek;         /**< True if last write was sparse lseek forward */
  bool warn_partial_read;         /**< Warn on short reads before EOF */
  bool translation_needed;        /**< True if character translation is active */
  int trans_mode;                 /**< Active transformation mode (enum dd_trans_mode) */
  char newline_character;         /**< Active newline character representation */
  char space_character;           /**< Active space character representation */
  unsigned char trans_table[256]; /**< Reentrant 256-byte charset translation table */
  idx_t pending_spaces;           /**< State tracker for unblock space expansion */

  /* Signal state */
  sig_atomic_t volatile interrupt_signal; /**< SIGINT/SIGTERM cancellation flag */
  sig_atomic_t volatile info_signal_count;/**< Pending SIGINFO/SIGUSR1 request count */

  /* Dynamic function pointers */
  ssize_t (*iread_fnc) (int fd, char *buf, idx_t size); /**< Custom reader routine */

  /* On-the-fly checksumming state */
  struct sha256_ctx sha_ctx;      /**< Streaming SHA-256 computation state */
  unsigned char sha_digest[32];   /**< Final 256-bit binary hash digest */
  bool sha_computed;              /**< Set to true when hash computation finalized */
} dd_context_t;

/* Global or thread-local active context pointer for signal handling */
extern dd_context_t *current_dd_ctx;

#ifdef __cplusplus
}
#endif

#endif /* DD_CONFIG_H */
