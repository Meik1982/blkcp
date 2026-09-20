#ifndef BLKCP_CONFIG_H
#define BLKCP_CONFIG_H

#include <config.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include "system.h"
#include "idx.h"
#include "xtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status levels */
enum dd_status_level
{
  STATUS_NONE = 1,
  STATUS_NOXFER = 2,
  STATUS_DEFAULT = 3,
  STATUS_PROGRESS = 4,
  STATUS_JSON = 5
};

/* Conversion and operational flags */
enum dd_conversions
{
  C_SWAB        = 1 << 0,   /**< Byte-pair swapping (AVX2/SSSE3-accelerated) */
  C_NOERROR     = 1 << 1,   /**< Continue operation across read errors */
  C_NOTRUNC     = 1 << 2,   /**< Do not truncate output file */
  C_SYNC        = 1 << 3,   /**< Pad short reads with zero bytes */
  C_FDATASYNC   = 1 << 4,   /**< fdatasync output before completion */
  C_FSYNC       = 1 << 5,   /**< fsync output before completion */
  C_SPARSE      = 1 << 6,   /**< Detect zero blocks and create sparse file */
  C_AUTOTUNE    = 1 << 7,   /**< Dynamic blocksize autotuning */
  C_FORCE       = 1 << 8,   /**< Bypass safety guard */
  C_SHA256      = 1 << 9,   /**< Real-time streaming SHA-256 calculation */
  C_ASYNC       = 1 << 10,  /**< Pthread double-buffering pipeline */
  C_REFLINK     = 1 << 11,  /**< CoW copy_file_range */
  C_URING       = 1 << 12,  /**< io_uring asynchronous backend */
  C_SPLICE      = 1 << 13,  /**< splice zero-copy pipe backend */
  C_NOCREAT     = 1 << 14,  /**< Do not create output file if missing */
  C_EXCL        = 1 << 15   /**< Fail if output file already exists */
};

/**
 * @brief Execution backend engine selection for data transfer.
 */
typedef enum blkcp_engine
{
  ENGINE_AUTO = 0,    /**< Automatic backend detection (Reflink -> Uring/Async -> Sync) */
  ENGINE_SYNC,        /**< Standard synchronous block I/O engine */
  ENGINE_ASYNC,       /**< Multi-threaded ringbuffer pipeline */
  ENGINE_REFLINK,     /**< Linux Kernel zero-copy copy_file_range */
  ENGINE_URING,       /**< Linux io_uring asynchronous execution */
  ENGINE_SPLICE       /**< Linux Kernel zero-copy splice engine */
} blkcp_engine_t;

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

    O_FORCE = FFS_MASK (v6_mask),
    v7_mask = v6_mask ^ O_FORCE,

    O_ASYNC_PIPELINE = FFS_MASK (v7_mask)
  };

/**
 * @brief Configuration parsed from CLI operands (if=, of=, bs=, count=, conv=, etc.)
 */
typedef struct dd_config
{
  char const *input_file;         /**< Path to input file or device (NULL for stdin) */
  char const *output_file;        /**< Path to output file or device (NULL for stdout) */
  idx_t blocksize;                /**< Canonical block size for read/write operations (-b / --block-size) */
#define input_blocksize blocksize
#define output_blocksize blocksize
  intmax_t skip_records;          /**< Input blocks to skip before copying */
  idx_t skip_bytes;               /**< Additional bytes to skip */
  intmax_t seek_records;          /**< Output blocks to seek before writing */
  intmax_t seek_bytes;            /**< Additional bytes to seek */
  intmax_t max_records;           /**< Max records to copy (from -c / --count) */
  idx_t max_bytes;                /**< Remaining bytes to copy */
  intmax_t bytes_to_copy;         /**< Exact byte count limit (-l / --limit; -1 = unbounded) */
  blkcp_engine_t engine;          /**< Explicitly chosen I/O execution backend engine */
  int conversions_mask;           /**< Bitmask of active conversions (enum dd_conversions) */
  int input_flags;                /**< Bitmask of input flags (O_DIRECT, O_NONBLOCK, etc.) */
  int output_flags;               /**< Bitmask of output flags (O_APPEND, O_FORCE, etc.) */
  int status_level;               /**< Telemetry verbosity (none, noxfer, progress, json, default) */
  bool json_output;               /**< Emit machine-readable NDJSON telemetry */
  size_t async_queue_depth;       /**< Ringbuffer queue capacity for async engine (0 = dynamic auto) */
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
  intmax_t r_truncate;            /**< Unused placeholder */
  intmax_t w_full;                /**< Complete output blocks written */
  intmax_t w_partial;             /**< Partial output blocks written */
  intmax_t w_bytes;               /**< Cumulative bytes written to output */
  intmax_t reported_w_bytes;      /**< Bytes reported in previous progress update */
  xtime_t start_time;             /**< Transfer start timestamp (nanoseconds via TSC) */
  xtime_t next_time;              /**< Next scheduled periodic progress report time */
  int progress_len;               /**< Character length of last printed progress line */
  uint32_t progress_check_counter; /**< Fast counter to throttle vDSO clock polls in hot loop */
  intmax_t last_clock_check_bytes; /**< Last transferred bytes count when clock was sampled */

  /* Async pipeline telemetry (when engine == ENGINE_ASYNC) */
  size_t async_capacity;          /**< Active ringbuffer capacity */
  uint64_t async_reader_stalls;   /**< Reader waits due to full ringbuffer */
  uint64_t async_writer_stalls;   /**< Writer waits due to empty ringbuffer */
} dd_stats_t;

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

  /* I/O descriptors and status */
  bool input_seekable;            /**< True if input supports lseek(SEEK_CUR) */
  int input_seek_errno;           /**< Errno captured when initial lseek failed */
  off_t input_offset;             /**< Byte offset in input file */
  bool final_op_was_seek;         /**< True if last write was sparse lseek forward */
  bool warn_partial_read;         /**< Warn on short reads before EOF */

  /* Signal state */
  sig_atomic_t volatile interrupt_signal; /**< SIGINT/SIGTERM cancellation flag */
  sig_atomic_t volatile info_signal_count;/**< Pending SIGINFO/SIGUSR1 request count */

  /* Dynamic function pointers */
  ssize_t (*iread_fnc) (int fd, char *buf, idx_t size); /**< Custom reader routine */

  /* Streaming cache eviction state (--nocache chunking) */
  off_t i_nocache_pending;        /**< Un-evicted input bytes pending chunked fadvise */
  off_t o_nocache_pending;        /**< Un-evicted output bytes pending chunked fadvise */

  /* On-the-fly checksumming state (Hardware-accelerated OpenSSL EVP) */
  EVP_MD_CTX *sha_evp_ctx;        /**< Streaming EVP SHA-256 computation state */
  unsigned char sha_digest[32];   /**< Final 256-bit binary hash digest */
  bool sha_computed;              /**< Set to true when hash computation finalized */

  /* Estimated or measured total input size for progress/ETA */
  intmax_t total_input_size;      /**< Source size in bytes (-1 if unknown/pipe) */
} dd_context_t;

/* Global or thread-local active context pointer for signal handling */
extern dd_context_t *current_dd_ctx;

#ifdef __cplusplus
}
#endif

#endif /* BLKCP_CONFIG_H */
