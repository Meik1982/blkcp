#ifndef DD_SIGNALS_H
#define DD_SIGNALS_H

#include <signal.h>
#include <stdbool.h>
#include "dd_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Global signal state accessible from async handlers */
extern sig_atomic_t volatile dd_interrupt_signal;
extern sig_atomic_t volatile dd_info_signal_count;
extern sigset_t dd_caught_signals;

/* Install standard signal handlers for SIGINT, SIGINFO / SIGUSR1 */
void dd_install_signal_handlers (void);

/* Check and process pending signals (SIGINT termination or SIGINFO output) */
void dd_process_signals (dd_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DD_SIGNALS_H */
