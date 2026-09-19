/**
 * @file signals.h
 * @brief Async-signal-safe interrupt and progress telemetery dispatching.
 *
 * Provides atomic signal counters and flags for SIGINT/SIGTERM termination
 * and SIGUSR1/SIGINFO instant transfer progress telemetry.
 */

#ifndef DD_SIGNALS_H
#define DD_SIGNALS_H

#include <signal.h>
#include <stdbool.h>
#include "blkcp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Global signal flag indicating pending termination (SIGINT, SIGTERM).
 */
extern sig_atomic_t volatile dd_interrupt_signal;

/**
 * @brief Global atomic counter tracking pending asynchronous SIGUSR1 / SIGINFO requests.
 */
extern sig_atomic_t volatile dd_info_signal_count;

/**
 * @brief Mask of all signals intercepted and tracked by blkcp.
 */
extern sigset_t dd_caught_signals;

/**
 * @brief Install dedicated signal handlers for SIGINT, SIGTERM, and SIGUSR1.
 */
void dd_install_signal_handlers (void);

/**
 * @brief Evaluate and process pending signals synchronously outside signal context.
 *
 * @param ctx Active runtime context.
 */
void dd_process_signals (dd_context_t *ctx);

/**
 * @brief Fast branch-predicted inline check avoiding function calls in the transfer hot loop.
 *
 * @param ctx Active runtime context.
 */
static inline void
dd_check_signals (dd_context_t *ctx)
{
  if (__builtin_expect (dd_interrupt_signal || dd_info_signal_count, 0))
    dd_process_signals (ctx);
}

#ifdef __cplusplus
}
#endif

#endif /* DD_SIGNALS_H */
