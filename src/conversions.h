/**
 * @file conversions.h
 * @brief Character set translations and SIMD-accelerated case modifications.
 *
 * Implements EBCDIC/ASCII/IBM table translations, branchless SIMD-vectorized
 * ASCII case conversions (ucase, lcase), and byte-pair swapping (swab).
 */

#ifndef DD_CONVERSIONS_H
#define DD_CONVERSIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "idx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize identity 256-byte translation lookup table.
 *
 * @param trans_table Pointer to 256-byte table to initialize.
 */
void dd_init_translations (unsigned char *trans_table);

/**
 * @brief Configure translation mode and lookup table from active conversion flags.
 *
 * @param trans_table Active 256-byte lookup table.
 * @param conversions_mask Bitmask of conversions requested.
 * @param newline_char Output pointer for newline character representation.
 * @param space_char Output pointer for space character representation.
 * @param translation_needed Output flag set to true if character translation is active.
 * @param trans_mode Output translation execution mode (enum dd_trans_mode).
 */
void dd_apply_translations (unsigned char *trans_table,
                            int conversions_mask,
                            char *newline_char,
                            char *space_char,
                            bool *translation_needed,
                            int *trans_mode);

/**
 * @brief SIMD-vectorizable branchless ASCII upper-case transformation.
 *
 * @param buf Data buffer.
 * @param nread Byte length of data to process.
 */
void dd_vector_ucase (char *buf, idx_t nread);

/**
 * @brief SIMD-vectorizable branchless ASCII lower-case transformation.
 *
 * @param buf Data buffer.
 * @param nread Byte length of data to process.
 */
void dd_vector_lcase (char *buf, idx_t nread);

/**
 * @brief Translate buffer bytes using reentrant 256-byte table.
 *
 * @param trans_table Active 256-byte translation table.
 * @param buf Data buffer to mutate in place.
 * @param nread Number of bytes in buffer.
 */
void dd_translate_buffer (unsigned char const *trans_table, char *buf, idx_t nread);

/**
 * @brief Swap adjacent bytes (swab) with preservation of odd trailing byte.
 *
 * @param buf Data buffer.
 * @param nread In/out pointer to byte length.
 * @param saved_byte In/out tracker for odd-boundary byte carry-over.
 * @return Pointer to adjusted buffer base.
 */
char *dd_swab_buffer (char *buf, idx_t *nread, int *saved_byte);

#ifdef __cplusplus
}
#endif

#endif /* DD_CONVERSIONS_H */
