#ifndef DD_CONVERSIONS_H
#define DD_CONVERSIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "idx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize translation table */
void dd_init_translations (unsigned char *trans_table);

/* Apply selected conversions (ASCII, EBCDIC, IBM, UCASE, LCASE) */
void dd_apply_translations (unsigned char *trans_table,
                            int conversions_mask,
                            char *newline_char,
                            char *space_char,
                            bool *translation_needed,
                            int *trans_mode);

/* Fast SIMD-vectorizable branchless case conversions */
void dd_vector_ucase (char *buf, idx_t nread);
void dd_vector_lcase (char *buf, idx_t nread);

/* Translate characters in buffer */
void dd_translate_buffer (unsigned char const *trans_table, char *buf, idx_t nread);

/* Byte-swapping (conv=swab) */
char *dd_swab_buffer (char *buf, idx_t *nread, int *saved_byte);

#ifdef __cplusplus
}
#endif

#endif /* DD_CONVERSIONS_H */
