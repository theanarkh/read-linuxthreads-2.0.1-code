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

// 线程退出
void pthread_exit(void * retval)
{
  // 获取当前线程的结构体
  pthread_t self = thread_self();
  pthread_t joining;
  struct pthread_request request;

  /* Reset the cancellation flag to avoid looping if the cleanup handlers
     contain cancellation points */
  // 设置成0，避免其他函数里判断是cancel状态，然后再调pthread_exit函数
  self->p_canceled = 0;
  /* Call cleanup functions and destroy the thread-specific data */
  // 执行clean节点的函数
  __pthread_perform_cleanup();
  // 遍历pthread_keys数组，销毁线程中的specifics数据
  __pthread_destroy_specifics();
  /* Store return value */
  // 加锁
  acquire(&self->p_spinlock);
  // 退出值,可以在join中返回给其他线程
  self->p_retval = retval;
  /* Say that we've terminated */
  // 已终止
  self->p_terminated = 1;
  /* See if someone is joining on us */
  // 判断有没有其他线程在等待该线程退出
  joining = self->p_joining;
  release(&self->p_spinlock);
  /* Restart joining thread if any */
  // 唤醒他
  if (joining != NULL) restart(joining);
  /* If this is the initial thread, block until all threads have terminated.
     If another thread calls exit, we'll be terminated from our signal
     handler. */
  // 如果是主线程退出，通知manage线程，如果是一般线程则直接执行exit退出
  if (self == __pthread_main_thread && __pthread_manager_request >= 0) {
    request.req_thread = self;
    request.req_kind = REQ_MAIN_THREAD_EXIT;
    // 写入管道
    __libc_write(__pthread_manager_request, (char *)&request, sizeof(request));
    // 挂起等待唤醒，全部子线程都退出后了才唤醒主线程，然后主线程也退出,见manager.c的__pthread_manager函数
    suspend(self);
  }
  /* Exit the process (but don't flush stdio streams, and don't run
     atexit functions). */
  // 线程退出，见操作系统实现
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
  // join的线程还在运行，则需要等待
  if (! th->p_terminated) {
    // 记录谁在join th
    th->p_joining = self;
    release(&th->p_spinlock);
    // 挂起等待唤醒，th退出的时候才会唤醒self线程，见pthread_exit的restart
    suspend_with_cancellation(self);
    acquire(&th->p_spinlock);
    /* This is a cancellation point */
    // 取消点
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
  // 管道的写端，join的线程已经退出，通知manage线程回收退出线程的资源，见REQ_FREE的处理
  if (__pthread_manager_request >= 0) {
    // 发送th线程已经结束的通知给manager线程，self是发送者
    request.req_thread = self;
    request.req_kind = REQ_FREE;
    request.req_args.free.thread = th;
    // 写入管道
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
  // detach过了
  if (th->p_detached) {
    release(&th->p_spinlock);
    return EINVAL;
  }
  /* If already joining, don't do anything. */
  // 有线程join了该线程，不能detach
  if (th->p_joining != NULL) {
    release(&th->p_spinlock);
    return 0;
  }
  /* Mark as detached */
  // 标记已经detach
  th->p_detached = 1;
  terminated = th->p_terminated;
  release(&th->p_spinlock);
  /* If already terminated, notify thread manager to reclaim resources */
  // 线程已经退出了，detach的时候，通知manager,__pthread_manager_request是管道写端
  if (terminated && __pthread_manager_request >= 0) {
    request.req_thread = thread_self();
    request.req_kind = REQ_FREE;
    request.req_args.free.thread = th;
    __libc_write(__pthread_manager_request,
		 (char *) &request, sizeof(request));
  }
  return 0;
}
