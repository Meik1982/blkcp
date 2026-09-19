#ifndef DD_IO_ENGINE_H
#define DD_IO_ENGINE_H

#include "blkcp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Synchronize output file data/metadata to disk */
int dd_synchronize_output (dd_context_t *ctx);

/* Clean up descriptors and pending buffers */
void dd_engine_cleanup (dd_context_t *ctx);

/* Fullblock reader function */
ssize_t dd_iread_fullblock (int fd, char *buf, idx_t size);

/* Execute the full I/O copy pipeline */
int dd_execute (dd_context_t *ctx);

/* Explicitly free allocated buffers */
void dd_context_free (dd_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DD_IO_ENGINE_H */
