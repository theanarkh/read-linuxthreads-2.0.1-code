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

#include <signal.h>

/* Primitives for controlling thread execution */
// 给pid进程发送信号
static inline void restart(pthread_t th)
{
  kill(th->p_pid, PTHREAD_SIG_RESTART);
}

static inline void suspend(pthread_t self)
{
  sigset_t mask;
  // 获取旧的信号掩码
  sigprocmask(SIG_SETMASK, NULL, &mask); /* Get current signal mask */
  // 清除对应的信号掩码，即可以处理该信号了
  sigdelset(&mask, PTHREAD_SIG_RESTART); /* Unblock the restart signal */
  do {
    // 挂起直到指定的被restart信号发生
    sigsuspend(&mask);                   /* Wait for signal */
  } while (self->p_signal != PTHREAD_SIG_RESTART);
}

static inline void suspend_with_cancellation(pthread_t self)
{
  sigset_t mask;
  sigjmp_buf jmpbuf;
  // 获取当前的信号掩码
  sigprocmask(SIG_SETMASK, NULL, &mask); /* Get current signal mask */
  // 清除PTHREAD_SIG_RESTART的信号掩码，即可以处理该信号
  sigdelset(&mask, PTHREAD_SIG_RESTART); /* Unblock the restart signal */
  /* No need to save the signal mask, we'll restore it ourselves */
  // 直接调用返回0，从siglongjump回来返回非0
  if (sigsetjmp(jmpbuf, 0) == 0) {
    self->p_cancel_jmp = &jmpbuf;
    if (! (self->p_canceled && self->p_cancelstate == PTHREAD_CANCEL_ENABLE)) {
      do {
        sigsuspend(&mask);               /* Wait for a signal */
      } while (self->p_signal != PTHREAD_SIG_RESTART);
    }
    self->p_cancel_jmp = NULL;
  } else {
    // 重新设置信号掩码
    sigaddset(&mask, PTHREAD_SIG_RESTART); /* Reblock the restart signal */
    sigprocmask(SIG_SETMASK, &mask, NULL);
  }
}
