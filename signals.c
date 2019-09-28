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
  // 做一些拦截处理，
  switch(how) {
  case SIG_SETMASK:
    // 需要屏蔽restart信号
    sigaddset(&mask, PTHREAD_SIG_RESTART);
    // 不能屏蔽cancel信号
    sigdelset(&mask, PTHREAD_SIG_CANCEL);
    break;
  case SIG_BLOCK:
    // 不能屏蔽cancel
    sigdelset(&mask, PTHREAD_SIG_CANCEL);
    break;
  case SIG_UNBLOCK:
    // 需要屏蔽restart
    sigdelset(&mask, PTHREAD_SIG_RESTART);
    break;
  }
  // 调系统函数
  if (sigprocmask(how, &mask, oldmask) == -1)
    return errno;
  else
    return 0;
}
// 给线程发信号，即给进程发信号
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
  // 全1，阻塞全部信号
  sigfillset(&mask);
  // 不屏蔽取消信号
  sigdelset(&mask, PTHREAD_SIG_CANCEL);
  /* Signals in set are assumed blocked on entrance */
  /* Install our signal handler on all signals in set,
     and unblock them in mask */
  for (s = 0; s < NSIG; s++) {
    if (sigismember(set, s) && s != PTHREAD_SIG_CANCEL) {
      sigdelset(&mask, s);
      action.sa_handler = __pthread_sighandler;
      // 在信号处理函数里阻塞所有的信号
      sigfillset(&action.sa_mask); /* block all signals in the handler */
      action.sa_flags = 0;
      // 注册该信号对应的handler
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
