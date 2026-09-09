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
void dd_init_translations (void);

/* Apply selected conversions (ASCII, EBCDIC, IBM, UCASE, LCASE) */
void dd_apply_translations (int conversions_mask,
                            char *newline_char,
                            char *space_char,
                            bool *translation_needed);

/* Translate characters in buffer */
void dd_translate_buffer (char *buf, idx_t nread);

/* Byte-swapping (conv=swab) */
char *dd_swab_buffer (char *buf, idx_t *nread, int *saved_byte);

#ifdef __cplusplus
}
#endif

#endif /* DD_CONVERSIONS_H */
