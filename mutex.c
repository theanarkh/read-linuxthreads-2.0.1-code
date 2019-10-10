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

/* Mutexes */

#include <errno.h>
#include <sched.h>
#include <stddef.h>
#include "pthread.h"
#include "internals.h"
#include "spinlock.h"
#include "queue.h"
#include "restart.h"

// 利用属性结构体初始化mutex节点
int __pthread_mutex_init(pthread_mutex_t * mutex,
                       const pthread_mutexattr_t * mutex_attr)
{
  mutex->m_spinlock = 0;
  mutex->m_count = 0;
  mutex->m_owner = NULL;
  mutex->m_kind =
    mutex_attr == NULL ? PTHREAD_MUTEX_FAST_NP : mutex_attr->mutexkind;
  queue_init(&mutex->m_waiting);
  return 0;
}
weak_alias (__pthread_mutex_init, pthread_mutex_init)

// 销毁互斥锁
int __pthread_mutex_destroy(pthread_mutex_t * mutex)
{
  int count;
  acquire(&mutex->m_spinlock);
  count = mutex->m_count;
  release(&mutex->m_spinlock);
  // 正在被使用
  if (count > 0) return EBUSY;
  return 0;
}
weak_alias (__pthread_mutex_destroy, pthread_mutex_destroy)

// 非阻塞式获取锁
int __pthread_mutex_trylock(pthread_mutex_t * mutex)
{
  pthread_t self;

  acquire(&mutex->m_spinlock);
  switch(mutex->m_kind) {
  case PTHREAD_MUTEX_FAST_NP:
    // 还没有被使用，则使用数加一，返回成功
    if (mutex->m_count == 0) {
      mutex->m_count = 1;
      release(&mutex->m_spinlock);
      return 0;
    }
    break;
  // 递归获取互斥变量
  case PTHREAD_MUTEX_RECURSIVE_NP:
    self = thread_self();
    // 等于0则说明还没有被获取过，可以直接获取，或者已经被当前线程获取了，则次数加一
    if (mutex->m_count == 0 || mutex->m_owner == self) {
      mutex->m_count++;
      mutex->m_owner = self;
      release(&mutex->m_spinlock);
      return 0;
    }
    break;
  default:
    return EINVAL;
  }
  release(&mutex->m_spinlock);
  return EBUSY;
}
weak_alias (__pthread_mutex_trylock, pthread_mutex_trylock)

// 阻塞式获取互斥变量
int __pthread_mutex_lock(pthread_mutex_t * mutex)
{
  pthread_t self;

  while(1) {
    acquire(&mutex->m_spinlock);
    switch(mutex->m_kind) {
    case PTHREAD_MUTEX_FAST_NP:
      if (mutex->m_count == 0) {
        mutex->m_count = 1;
        release(&mutex->m_spinlock);
        return 0;
      }
      self = thread_self();
      break;
    case PTHREAD_MUTEX_RECURSIVE_NP:
      self = thread_self();
      // 等于0或者本线程已经获得过该互斥锁，则可以重复获得，m_count累加
      if (mutex->m_count == 0 || mutex->m_owner == self) {
        mutex->m_count++;
        // 标记该互斥锁已经被本线程获取
        mutex->m_owner = self;
        release(&mutex->m_spinlock);
        return 0;
      }
      break;
    default:
      return EINVAL;
    }
    /* Suspend ourselves, then try again */
    // 获取失败，需要阻塞，把当前线程插入该互斥锁的等待队列
    enqueue(&mutex->m_waiting, self);
    release(&mutex->m_spinlock);
    // 挂起等待唤醒
    suspend(self); /* This is not a cancellation point */
  }
}
weak_alias (__pthread_mutex_lock, pthread_mutex_lock)

int __pthread_mutex_unlock(pthread_mutex_t * mutex)
{
  pthread_t th;

  acquire(&mutex->m_spinlock);
  switch (mutex->m_kind) {
  case PTHREAD_MUTEX_FAST_NP:
    mutex->m_count = 0;
    break;
  case PTHREAD_MUTEX_RECURSIVE_NP:
    mutex->m_count--;
    if (mutex->m_count > 0) {
      release(&mutex->m_spinlock);
      return 0;
    }
    mutex->m_count = 0; /* so that excess unlocks do not break everything */
    break;
  default:
    return EINVAL;
  }
  // 取出一个被阻塞的线程（如果有的话），唤醒他
  th = dequeue(&mutex->m_waiting);
  release(&mutex->m_spinlock);
  if (th != NULL) restart(th);
  return 0;
}
weak_alias (__pthread_mutex_unlock, pthread_mutex_unlock)

int __pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
  attr->mutexkind = PTHREAD_MUTEX_FAST_NP;
  return 0;
}
weak_alias (__pthread_mutexattr_init, pthread_mutexattr_init)

int __pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
  return 0;
}
weak_alias (__pthread_mutexattr_destroy, pthread_mutexattr_destroy)

int __pthread_mutexattr_setkind_np(pthread_mutexattr_t *attr, int kind)
{
  if (kind != PTHREAD_MUTEX_FAST_NP && kind != PTHREAD_MUTEX_RECURSIVE_NP)
    return EINVAL;
  attr->mutexkind = kind;
  return 0;
}
weak_alias (__pthread_mutexattr_setkind_np, pthread_mutexattr_setkind_np)

int __pthread_mutexattr_getkind_np(const pthread_mutexattr_t *attr, int *kind)
{
  *kind = attr->mutexkind;
  return 0;
}
weak_alias (__pthread_mutexattr_getkind_np, pthread_mutexattr_getkind_np)
// 保存init_routine只执行一次
int pthread_once(pthread_once_t * once_control, void (*init_routine)(void))
{
  if (testandset(once_control) == 0) init_routine();
  return 0;
}
