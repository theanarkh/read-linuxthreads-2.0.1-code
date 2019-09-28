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

/* Thread cancellation */

#include <errno.h>
#include "pthread.h"
#include "internals.h"
#include "restart.h"

/*
 修改线程的可取消属性。有一个取消点
 取消状态分为可取消，不可取消
    不可取消的时候，收到取消信号，忽略
    可取消的时候，收到取消信号的时候，根据取消类型做处理。
      立即处理
      不立刻处理，到下一个取消点，判定线程的状态的取消类型再处理
 */
int pthread_setcancelstate(int state, int * oldstate)
{
  pthread_t self = thread_self();
  if (state < PTHREAD_CANCEL_ENABLE || state > PTHREAD_CANCEL_DISABLE)
    return EINVAL;
  // 保存旧的状态
  if (oldstate != NULL) *oldstate = self->p_cancelstate;
  // 设置新的状态
  self->p_cancelstate = state;
  // 判断线程是否被取消了，并且当前被设置成可取消状态，并且是需要马上处理的,则直接退出
  if (self->p_canceled &&
      self->p_cancelstate == PTHREAD_CANCEL_ENABLE &&
      self->p_canceltype == PTHREAD_CANCEL_ASYNCHRONOUS)
    pthread_exit(PTHREAD_CANCELED);
  return 0;
}
// 见上一个函数
int pthread_setcanceltype(int type, int * oldtype)
{
  pthread_t self = thread_self();
  if (type < PTHREAD_CANCEL_DEFERRED || type > PTHREAD_CANCEL_ASYNCHRONOUS)
    return EINVAL;
  if (oldtype != NULL) *oldtype = self->p_canceltype;
  self->p_canceltype = type;
  if (self->p_canceled &&
      self->p_cancelstate == PTHREAD_CANCEL_ENABLE &&
      self->p_canceltype == PTHREAD_CANCEL_ASYNCHRONOUS)
    pthread_exit(PTHREAD_CANCELED);
  return 0;
}
// 给线程发送取消请求，线程收到该信号是否处理，怎么处理取决于线程本身对于取消的相关配置
int pthread_cancel(pthread_t thread)
{
  thread->p_canceled = 1;
  kill(thread->p_pid, PTHREAD_SIG_CANCEL);
  return 0;
}
// 设置一个取消点
void pthread_testcancel(void)
{
  pthread_t self = thread_self();
  // 判断线程是不是已经被取消，并且是可取消的，则退出
  if (self->p_canceled && self->p_cancelstate == PTHREAD_CANCEL_ENABLE)
    pthread_exit(PTHREAD_CANCELED);
}
// 链表中新增一个clean函数
void _pthread_cleanup_push(struct _pthread_cleanup_buffer * buffer,
			   void (*routine)(void *), void * arg)
{
  pthread_t self = thread_self();
  buffer->routine = routine;
  buffer->arg = arg;
  buffer->prev = self->p_cleanup;
  self->p_cleanup = buffer;
}
// 删除一个clean节点，execute判断是否需要执行
void _pthread_cleanup_pop(struct _pthread_cleanup_buffer * buffer,
			  int execute)
{
  pthread_t self = thread_self();
  if (execute) buffer->routine(buffer->arg);
  self->p_cleanup = buffer->prev;
}
// 新增一个clean节点，保存旧的取消类型，设置新的取消类型为PTHREAD_CANCEL_DEFERRED
void _pthread_cleanup_push_defer(struct _pthread_cleanup_buffer * buffer,
				 void (*routine)(void *), void * arg)
{
  pthread_t self = thread_self();
  buffer->routine = routine;
  buffer->arg = arg;
  buffer->canceltype = self->p_canceltype;
  buffer->prev = self->p_cleanup;
  self->p_canceltype = PTHREAD_CANCEL_DEFERRED;
  self->p_cleanup = buffer;
}

// 和上面的函数配套。删除一个clean节点，execute控制是否需要执行删除的这个节点，恢复线程的取消类型，是一个有取消点的函数
void _pthread_cleanup_pop_restore(struct _pthread_cleanup_buffer * buffer,
				  int execute)
{
  pthread_t self = thread_self();
  if (execute) buffer->routine(buffer->arg);
  self->p_cleanup = buffer->prev;
  self->p_canceltype = buffer->canceltype;
  if (self->p_canceled &&
      self->p_cancelstate == PTHREAD_CANCEL_ENABLE &&
      self->p_canceltype == PTHREAD_CANCEL_ASYNCHRONOUS)
    pthread_exit(PTHREAD_CANCELED);
}
// 线程退出的时候(pthread_exit)调用执行clean链表的节点
void __pthread_perform_cleanup(void)
{
  pthread_t self = thread_self();
  struct _pthread_cleanup_buffer * c;
  for (c = self->p_cleanup; c != NULL; c = c->prev) c->routine(c->arg);
}

#ifndef PIC
/* We need a hook to force the cancelation wrappers to be linked in when
   static libpthread is used.  */
extern const int __pthread_provide_wrappers;
static const int * const __pthread_require_wrappers =
  &__pthread_provide_wrappers;
#endif
