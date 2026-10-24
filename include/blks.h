/*
 * blks.h - High-Performance 384-Bit Cryptographic Tree-Hash C API
 *
 * Part of the blks project. Provides direct C-FFI compatibility for blkcp.
 */

#ifndef BLKS_H
#define BLKS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLKS_DIGEST_BYTES 48
#define BLKS_BASE64_CHARS 64
#define BLKS_HEX_CHARS    96

typedef struct BlksHasher BlksHasher;
typedef struct BlksHasher blks_hasher_t;

/**
 * Compute blks-384 hash over a memory buffer into a 48-byte buffer.
 * Uses multithreaded tree reduction for large buffers.
 * Returns 0 on success, -1 on invalid arguments.
 */
int blks_hash_buffer(const uint8_t *data, size_t len, uint8_t out_digest[BLKS_DIGEST_BYTES]);

/**
 * Allocate a new streaming blks hasher.
 */
BlksHasher *blks_hasher_new(void);

/**
 * Feed data into a streaming blks hasher.
 * Returns 0 on success, -1 on error.
 */
int blks_hasher_update(BlksHasher *hasher, const uint8_t *data, size_t len);

/**
 * Finalize streaming hasher, write 48 bytes to out_digest, and free hasher memory.
 * Returns 0 on success, -1 on error.
 */
int blks_hasher_finalize(BlksHasher *hasher, uint8_t out_digest[BLKS_DIGEST_BYTES]);

/**
 * Free a streaming hasher without finalizing.
 */
void blks_hasher_free(BlksHasher *hasher);

/**
 * Encode 48-byte binary digest into a null-terminated 64-character Base64 string.
 * out_str must have at least 65 bytes of capacity (64 chars + '\0').
 * Returns 0 on success, -1 on error.
 */
int blks_digest_to_base64(const uint8_t in_digest[BLKS_DIGEST_BYTES], char *out_str, size_t out_str_size);

#ifdef __cplusplus
}
#endif

#endif /* BLKS_H */
