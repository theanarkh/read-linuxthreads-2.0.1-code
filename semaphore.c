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

/* Semaphores a la POSIX 1003.1b */

#include "pthread.h"
#include "semaphore.h"
#include "internals.h"
#include "restart.h"


#ifndef HAS_COMPARE_AND_SWAP
/* If we have no atomic compare and swap, fake it using an extra spinlock.  */

#include "spinlock.h"
// 等于旧值则更新sem_status为新值，如果一样则不更新，更新则返回1
static inline int compare_and_swap(sem_t *sem, long oldval, long newval)
{
  int ret;
  acquire(&sem->sem_spinlock);
  if ((ret = sem->sem_status == oldval) != 0)
    sem->sem_status = newval;
  release(&sem->sem_spinlock);
  return ret;
}

#else
/* But if we do have an atomic compare and swap, use it!  */

#define compare_and_swap(sem,old,new) \
  __compare_and_swap(&(sem)->sem_status, (old), (new))

#endif


/* The state of a semaphore is represented by a long int encoding
   either the semaphore count if >= 0 and no thread is waiting on it,
   or the head of the list of threads waiting for the semaphore.
   To distinguish the two cases, we encode the semaphore count N
   as 2N+1, so that it has the lowest bit set.

   A sequence of sem_wait operations on a semaphore initialized to N
   result in the following successive states:
     2N+1, 2N-1, ..., 3, 1, &first_waiting_thread, &second_waiting_thread, ...
*/

int sem_init(sem_t *sem, int pshared, unsigned int value)
{
  if (value > SEM_VALUE_MAX) {
    errno = EINVAL;
    return -1;
  }
  // 还没实现
  if (pshared) {
    errno = ENOSYS;
    return -1;
  }
  // 记录资源数,N=>2N+1
  sem->sem_status = ((long)value << 1) + 1;
  return 0;
}

int sem_wait(sem_t * sem)
{
  long oldstatus, newstatus;
  volatile pthread_t self = thread_self();
  pthread_t * th;

  while (1) {
    do {
      // oldstatus可能是线程或者资源数
      oldstatus = sem->sem_status;
      // 大于1说明有资源，等于1说着0说明没有资源或没有资源并且有线程阻塞
      if ((oldstatus & 1) && (oldstatus != 1))
        newstatus = oldstatus - 2;
      else {
        // 没有可用资源，需要阻塞
        newstatus = (long) self;
        // 保存这时候的资源数或者上一个被阻塞的线程
        self->p_nextwaiting = (pthread_t) oldstatus;
      }
    }
    // sem_status可能指向资源数或者被阻塞的线程链表。赋值成功则返回1，否则返回0
    while (! compare_and_swap(sem, oldstatus, newstatus));
    // self是按偶数地址对齐的，低位为1说明是还有可用资源
    if (newstatus & 1)
      /* We got the semaphore. */
      return 0;
    /* Wait for sem_post or cancellation */
    // 等待被restart或者cancel信号唤醒
    suspend_with_cancellation(self);
    /* This is a cancellation point */
    // 判断是否被取消了，即是被cancel信号唤醒的，不是的话重新判断是否有资源，即回到上面的while(1)
    if (self->p_canceled && self->p_cancelstate == PTHREAD_CANCEL_ENABLE) {
      /* Remove ourselves from the waiting list if we're still on it */
      /* First check if we're at the head of the list. */
      do {
        // 得到被阻塞的第一个线程
        oldstatus = sem->sem_status;
        // 相等说明当前线程是最后一个被阻塞的线程
        if (oldstatus != (long) self) break;
        // 得到该线程被阻塞时的资源数或下一个被阻塞的线程
        newstatus = (long) self->p_nextwaiting;
      }
      // sem_status指向资源数或者下一个被阻塞的线程
      while (! compare_and_swap(sem, oldstatus, newstatus));
      /* Now, check if we're somewhere in the list.
         There's a race condition with sem_post here, but it does not matter:
         the net result is that at the time pthread_exit is called,
         self is no longer reachable from sem->sem_status. */
      // 可能是break或者while为true，不是当前线程并且不是资源数，即oldstatus指向一个其他线程
      if (oldstatus != (long) self && (oldstatus & 1) == 0) {
        th = &(((pthread_t) oldstatus)->p_nextwaiting);
        // 不是资源数，即是线程，从等待的线程链表中删除self线程，因为他即将退出
        while (*th != (pthread_t) 1 && *th != NULL) {
          if (*th == self) {
            *th = self->p_nextwaiting;
            break;
          }
          th = &((*th)->p_nextwaiting);
        }
      }
      // 当前线程退出
      pthread_exit(PTHREAD_CANCELED);
    }
  }
}
// 非阻塞获取信号量
int sem_trywait(sem_t * sem)
{
  long oldstatus, newstatus;

  do {
    oldstatus = sem->sem_status;
    // oldstatus & 1等于0说明是线程，即有线程在等待，或者等于1，都说明没有可用资源，直接返回
    if ((oldstatus & 1) == 0 || (oldstatus == 1)) {
      errno = EAGAIN;
      return -1;
    }
    // 更新资源数
    newstatus = oldstatus - 2;
  }
  // 更新资源数，如果失败说明被其他线程拿到锁了，则重新执行do里面的逻辑，因为数据可能被修改了
  while (! compare_and_swap(sem, oldstatus, newstatus));
  return 0;
}

int sem_post(sem_t * sem)
{
  long oldstatus, newstatus;
  pthread_t th, next_th;

  do {
    oldstatus = sem->sem_status;
    // 说明原来的资源数是0，并且有线程在等待，则更新为1，2n+1即3
    if ((oldstatus & 1) == 0)
      newstatus = 3;
    else {
      if (oldstatus >= SEM_VALUE_MAX) {
        /* Overflow */
        errno = ERANGE;
        return -1;
      }
      // 否则加2，即资源数加一
      newstatus = oldstatus + 2;
    }
  }
  // 更新资源数
  while (! compare_and_swap(sem, oldstatus, newstatus));
  // 如果之前有线程阻塞，则唤醒所有线程，再次竞争获得信号量
  if ((oldstatus & 1) == 0) {
    th = (pthread_t) oldstatus;
    do {
      next_th = th->p_nextwaiting;
      th->p_nextwaiting = NULL;
      restart(th);
      th = next_th;
    } while(th != (pthread_t) 1);
  }
  return 0;
}
// 获取资源数
int sem_getvalue(sem_t * sem, int * sval)
{
  long status = sem->sem_status;
  // 有资源
  if (status & 1)
    // 除以2
    *sval = (int)((unsigned long) status >> 1);
  else
    *sval = 0;
  return 0;
}

int sem_destroy(sem_t * sem)
{
  // 有线程在等待
  if ((sem->sem_status & 1) == 0) {
    errno = EBUSY;
    return -1;
  }
  return 0;
}
