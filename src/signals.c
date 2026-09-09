#include <config.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include "signals.h"
#include "stats.h"
#include "system.h"

#ifndef SA_RESETHAND
# define SA_RESETHAND 0
#endif

#ifndef SIGINFO
# define SIGINFO SIGUSR1
#endif

sig_atomic_t volatile dd_interrupt_signal = 0;
sig_atomic_t volatile dd_info_signal_count = 0;
sigset_t dd_caught_signals;

/* Internal handler for termination signals (SIGINT) */
static void
interrupt_handler (int sig)
{
  if (! SA_RESETHAND)
    signal (sig, SIG_DFL);
  dd_interrupt_signal = sig;
}

/* Internal handler for info signals (SIGINFO / SIGUSR1) */
static void
siginfo_handler (int sig)
{
  if (! SA_NOCLDSTOP)
    signal (sig, siginfo_handler);
  dd_info_signal_count++;
}

void
dd_install_signal_handlers (void)
{
  bool catch_siginfo = ! (SIGINFO == SIGUSR1 && getenv ("POSIXLY_CORRECT"));

#if SA_NOCLDSTOP
  struct sigaction act;
  sigemptyset (&dd_caught_signals);
  if (catch_siginfo)
    sigaddset (&dd_caught_signals, SIGINFO);
  sigaction (SIGINT, nullptr, &act);
  if (act.sa_handler != SIG_IGN)
    sigaddset (&dd_caught_signals, SIGINT);
  act.sa_mask = dd_caught_signals;

  if (sigismember (&dd_caught_signals, SIGINFO))
    {
      act.sa_handler = siginfo_handler;
      act.sa_flags = 0;
      sigaction (SIGINFO, &act, nullptr);
    }

  if (sigismember (&dd_caught_signals, SIGINT))
    {
      act.sa_handler = interrupt_handler;
      act.sa_flags = SA_NODEFER | SA_RESETHAND;
      sigaction (SIGINT, &act, nullptr);
    }
#else
  if (catch_siginfo)
    {
      signal (SIGINFO, siginfo_handler);
      siginterrupt (SIGINFO, 1);
    }
  if (signal (SIGINT, SIG_IGN) != SIG_IGN)
    {
      signal (SIGINT, interrupt_handler);
      siginterrupt (SIGINT, 1);
    }
#endif
}

extern void dd_cleanup (void);

void
dd_process_signals (dd_context_t *ctx)
{
  while (dd_interrupt_signal || dd_info_signal_count)
    {
      int interrupt;
      int infos;
      sigset_t oldset;

      sigprocmask (SIG_BLOCK, &dd_caught_signals, &oldset);

      interrupt = dd_interrupt_signal;
      infos = dd_info_signal_count;

      if (infos)
        dd_info_signal_count = infos - 1;

      sigprocmask (SIG_SETMASK, &oldset, nullptr);

      if (interrupt)
        dd_cleanup ();

      if (ctx)
        dd_print_stats (&ctx->stats, ctx->cfg.status_level, &ctx->stats.progress_len);

      if (interrupt)
        raise (interrupt);
    }
}
