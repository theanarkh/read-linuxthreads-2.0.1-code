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

/* Thread termination and joining */

#include <errno.h>
#include <sched.h>
#include <unistd.h>
#include "pthread.h"
#include "internals.h"
#include "spinlock.h"
#include "restart.h"

void pthread_exit(void * retval)
{
  pthread_t self = thread_self();
  pthread_t joining;
  struct pthread_request request;

  /* Reset the cancellation flag to avoid looping if the cleanup handlers
     contain cancellation points */
  self->p_canceled = 0;
  /* Call cleanup functions and destroy the thread-specific data */
  __pthread_perform_cleanup();
  __pthread_destroy_specifics();
  /* Store return value */
  acquire(&self->p_spinlock);
  self->p_retval = retval;
  /* Say that we've terminated */
  self->p_terminated = 1;
  /* See if someone is joining on us */
  joining = self->p_joining;
  release(&self->p_spinlock);
  /* Restart joining thread if any */
  if (joining != NULL) restart(joining);
  /* If this is the initial thread, block until all threads have terminated.
     If another thread calls exit, we'll be terminated from our signal
     handler. */
  if (self == __pthread_main_thread && __pthread_manager_request >= 0) {
    request.req_thread = self;
    request.req_kind = REQ_MAIN_THREAD_EXIT;
    __libc_write(__pthread_manager_request, (char *)&request, sizeof(request));
    suspend(self);
  }
  /* Exit the process (but don't flush stdio streams, and don't run
     atexit functions). */
  _exit(0);
}
// 调用该函数的线程会等待th线程结束
int pthread_join(pthread_t th, void ** thread_return)
{
  volatile pthread_t self = thread_self();
  struct pthread_request request;
  // 不能等待自己结束，否则会死锁，即自己无法结束
  if (th == self) return EDEADLK;
  acquire(&th->p_spinlock);
  /* If detached or already joined, error */
  // th线程已经是detach状态，即不是joinable的，或者已经被jion过了
  if (th->p_detached || th->p_joining != NULL) {
    release(&th->p_spinlock);
    return EINVAL;
  }
  /* If not terminated yet, suspend ourselves. */
  // 线程还在运行
  if (! th->p_terminated) {
    th->p_joining = self;
    release(&th->p_spinlock);
    suspend_with_cancellation(self);
    acquire(&th->p_spinlock);
    /* This is a cancellation point */
    if (self->p_canceled && self->p_cancelstate == PTHREAD_CANCEL_ENABLE) {
      th->p_joining = NULL;
      release(&th->p_spinlock);
      pthread_exit(PTHREAD_CANCELED);
    }
  }
  /* Get return value */
  // 线程已经结束，设置线程的返回值
  if (thread_return != NULL) *thread_return = th->p_retval;
  release(&th->p_spinlock);
  /* Send notification to thread manager */
  // 管道的写端
  if (__pthread_manager_request >= 0) {
    // 发送线程结束通知到给manager线程
    request.req_thread = self;
    request.req_kind = REQ_FREE;
    request.req_args.free.thread = th;
    __libc_write(__pthread_manager_request,
		 (char *) &request, sizeof(request));
  }
  return 0;
}

int pthread_detach(pthread_t th)
{
  int terminated;
  struct pthread_request request;

  acquire(&th->p_spinlock);
  /* If already detached, error */
  if (th->p_detached) {
    release(&th->p_spinlock);
    return EINVAL;
  }
  /* If already joining, don't do anything. */
  if (th->p_joining != NULL) {
    release(&th->p_spinlock);
    return 0;
  }
  /* Mark as detached */
  th->p_detached = 1;
  terminated = th->p_terminated;
  release(&th->p_spinlock);
  /* If already terminated, notify thread manager to reclaim resources */
  if (terminated && __pthread_manager_request >= 0) {
    request.req_thread = thread_self();
    request.req_kind = REQ_FREE;
    request.req_args.free.thread = th;
    __libc_write(__pthread_manager_request,
		 (char *) &request, sizeof(request));
  }
  return 0;
}
