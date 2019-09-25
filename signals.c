/* Linuxthreads - a simple clone()-based implementation of Posix        */
/* threads for Linux.                                                   */
/* Copyright (C) 1996 Xavier Leroy (Xavier.Leroy@inria.fr)              */
/*                                                                      */
/* This program is free software; you can redistribute it and/or        */
/* modify it under the terms of the GNU Library General Public License  */
/* as published by the Free Software Foundation; either version 2       */
/* of the License, or (at your option) any later version.               */
/*                                                                      */
/* This program is distributed in the hope that it will be useful,      */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of       */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        */
/* GNU Library General Public License for more details.                 */

/* Handling of signals */

#include <errno.h>
#include <signal.h>
#include "pthread.h"
#include "internals.h"

int pthread_sigmask(int how, const sigset_t * newmask, sigset_t * oldmask)
{
  sigset_t mask;
  mask = *newmask;
  /* Don't allow PTHREAD_SIG_RESTART to be unmasked.
     Don't allow PTHREAD_SIG_CANCEL to be masked. */
  switch(how) {
  case SIG_SETMASK:
    sigaddset(&mask, PTHREAD_SIG_RESTART);
    sigdelset(&mask, PTHREAD_SIG_CANCEL);
    break;
  case SIG_BLOCK:
    sigdelset(&mask, PTHREAD_SIG_CANCEL);
    break;
  case SIG_UNBLOCK:
    sigdelset(&mask, PTHREAD_SIG_RESTART);
    break;
  }
  if (sigprocmask(how, &mask, oldmask) == -1)
    return errno;
  else
    return 0;
}

int pthread_kill(pthread_t thread, int signo)
{
  if (kill(thread->p_pid, signo) == -1)
    return errno;
  else
    return 0;
}

int sigwait(const sigset_t * set, int * sig)
{
  volatile pthread_t self = thread_self();
  sigset_t mask;
  int s;
  struct sigaction action, saved_signals[NSIG];
  sigjmp_buf jmpbuf;

  /* Get ready to block all signals except those in set
     and the cancellation signal */
  sigfillset(&mask);
  sigdelset(&mask, PTHREAD_SIG_CANCEL);
  /* Signals in set are assumed blocked on entrance */
  /* Install our signal handler on all signals in set,
     and unblock them in mask */
  for (s = 0; s < NSIG; s++) {
    if (sigismember(set, s) && s != PTHREAD_SIG_CANCEL) {
      sigdelset(&mask, s);
      action.sa_handler = __pthread_sighandler;
      sigfillset(&action.sa_mask); /* block all signals in the handler */
      action.sa_flags = 0;
      sigaction(s, &action, &(saved_signals[s]));
    }
  }
  /* Test for cancellation */
  if (sigsetjmp(jmpbuf, 1) == 0) {
    self->p_cancel_jmp = &jmpbuf;
    if (! (self->p_canceled && self->p_cancelstate == PTHREAD_CANCEL_ENABLE)) {
      /* Reset the signal count */
      self->p_signal = 0;
      /* Unblock the signals and wait for them */
      sigsuspend(&mask);
    }
  }
  self->p_cancel_jmp = NULL;
  /* The signals are now reblocked. Restore the sighandlers. */
  for (s = 0; s < NSIG; s++) {
    if (sigismember(set, s) && s != PTHREAD_SIG_CANCEL)
      sigaction(s, &(saved_signals[s]), NULL);
  }
  /* Check for cancellation */
  pthread_testcancel();
  /* We should have self->p_signal != 0 and equal to the signal received */
  *sig = self->p_signal;
  return 0;
}
